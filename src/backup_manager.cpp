#include "backup_manager.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

static bool SafeCopyFileWithRetry(const fs::path& src, const fs::path& dst, int maxRetries = 25, int sleepMs = 150) {
    for (int i = 0; i < maxRetries; ++i) {
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return false;
}

static bool SafeRemoveFileWithRetry(const fs::path& path, int maxRetries = 15, int sleepMs = 100) {
    for (int i = 0; i < maxRetries; ++i) {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return true;
        }
        fs::remove(path, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return false;
}

bool BackupManager::HasBackup(const std::wstring& backupDir) {
    try {
        fs::path backupRoot(backupDir);
        if (!fs::exists(backupRoot) || !fs::is_directory(backupRoot)) {
            return false;
        }
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file()) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool BackupManager::BackupFgdFiles(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError) {
    try {
        fs::path cs2Path(cs2Root);
        fs::path gamePath = cs2Path / L"game";
        fs::path backupPath(backupDir);

        if (!fs::exists(gamePath)) {
            outError = L"未找到 CS2 game 目录: " + gamePath.wstring();
            return false;
        }

        for (const auto& entry : fs::recursive_directory_iterator(gamePath)) {
            if (entry.is_regular_file() && entry.path().extension() == L".fgd") {
                fs::path relPath = fs::relative(entry.path(), cs2Path);
                fs::path dstPath = backupPath / relPath;

                // 安全策略：如果 backup 目录中已有该原版文件，则保留现有备份，防止已汉化文件覆盖纯净原版
                if (!fs::exists(dstPath)) {
                    fs::create_directories(dstPath.parent_path());
                    fs::copy_file(entry.path(), dstPath, fs::copy_options::overwrite_existing);
                }

                outBackedUpFiles.push_back(relPath.wstring());
            }
        }
        return !outBackedUpFiles.empty();
    } catch (const std::exception& e) {
        outError = L"备份 FGD 发生异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

bool BackupManager::BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError) {
    try {
        fs::path srcQtCore = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
        fs::path dstQtCore = fs::path(backupDir) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";

        if (!fs::exists(srcQtCore)) {
            outError = L"未找到源 Qt5Core.dll: " + srcQtCore.wstring();
            return false;
        }

        // 安全策略：若备份已存在则保留，防止覆盖
        if (!fs::exists(dstQtCore)) {
            fs::create_directories(dstQtCore.parent_path());
            fs::copy_file(srcQtCore, dstQtCore, fs::copy_options::overwrite_existing);
        }
        return true;
    } catch (const std::exception& e) {
        outError = L"备份 Qt5Core.dll 异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

bool BackupManager::RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError) {
    try {
        fs::path backupRoot(backupDir);
        fs::path cs2Path(cs2Root);

        if (!fs::exists(backupRoot)) {
            outError = L"备份目录不存在: " + backupDir;
            return false;
        }

        // 1. 还原 backup 目录下的所有文件 (带重试机制，应对句柄延迟释放与文件占用)
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file()) {
                fs::path relPath = fs::relative(entry.path(), backupRoot);
                fs::path dstPath = cs2Path / relPath;

                fs::create_directories(dstPath.parent_path());
                if (!SafeCopyFileWithRetry(entry.path(), dstPath, 30, 150)) {
                    outError = L"还原文件失败 (文件被占用或无写权限): " + dstPath.wstring();
                    return false;
                }
            }
        }

        // 2. 清理临时部署的注入文件 (带重试机制)
        fs::path win64Bin = cs2Path / L"game" / L"bin" / L"win64";
        fs::path tempQmDll = win64Bin / L"qtcore_qm.dll";
        fs::path tempJson = win64Bin / L"qt_translations.json";
        fs::path tempOldJson = win64Bin / L"translations" / L"hammer.json";

        SafeRemoveFileWithRetry(tempQmDll, 15, 100);
        SafeRemoveFileWithRetry(tempJson, 15, 100);
        SafeRemoveFileWithRetry(tempOldJson, 15, 100);

        return true;
    } catch (const std::exception& e) {
        outError = L"还原文件异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

bool BackupManager::IsPatchDeployed(const std::wstring& cs2Root) {
    try {
        fs::path win64Bin = fs::path(cs2Root) / L"game" / L"bin" / L"win64";
        fs::path qmDll = win64Bin / L"qtcore_qm.dll";
        fs::path qtJson = win64Bin / L"qt_translations.json";
        return fs::exists(qmDll) || fs::exists(qtJson);
    } catch (...) {
        return false;
    }
}

bool BackupManager::HasUnrestoredSession(const std::wstring& workingDir) {
    try {
        fs::path statePath = fs::path(workingDir) / L"session_state.json";
        if (!fs::exists(statePath)) {
            return false;
        }
        std::ifstream ifs(statePath);
        if (!ifs.is_open()) return false;
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();
        return content.find("\"is_patched\": true") != std::string::npos;
    } catch (...) {
        return false;
    }
}

void BackupManager::SaveSessionState(const std::wstring& workingDir, bool isPatched) {
    try {
        fs::path statePath = fs::path(workingDir) / L"session_state.json";
        if (!isPatched) {
            std::error_code ec;
            fs::remove(statePath, ec);
            return;
        }
        std::ofstream ofs(statePath);
        if (ofs.is_open()) {
            ofs << "{\n";
            ofs << "  \"is_patched\": true\n";
            ofs << "}\n";
            ofs.flush();
        }
    } catch (...) {}
}

void BackupManager::ClearSessionState(const std::wstring& workingDir) {
    try {
        fs::path statePath = fs::path(workingDir) / L"session_state.json";
        std::error_code ec;
        fs::remove(statePath, ec);
    } catch (...) {}
}

