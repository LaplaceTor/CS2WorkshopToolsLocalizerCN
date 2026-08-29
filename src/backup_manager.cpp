#include "backup_manager.h"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

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

                fs::create_directories(dstPath.parent_path());
                fs::copy_file(entry.path(), dstPath, fs::copy_options::overwrite_existing);

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

        fs::create_directories(dstQtCore.parent_path());
        fs::copy_file(srcQtCore, dstQtCore, fs::copy_options::overwrite_existing);
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

        // 1. 还原 backup 目录下的所有文件
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file()) {
                fs::path relPath = fs::relative(entry.path(), backupRoot);
                fs::path dstPath = cs2Path / relPath;

                fs::create_directories(dstPath.parent_path());
                fs::copy_file(entry.path(), dstPath, fs::copy_options::overwrite_existing);
            }
        }

        // 2. 清理临时部署的注入文件
        fs::path win64Bin = cs2Path / L"game" / L"bin" / L"win64";
        fs::path tempQmDll = win64Bin / L"qtcore_qm.dll";
        fs::path tempJson = win64Bin / L"qt_translations.json";
        fs::path tempOldJson = win64Bin / L"translations" / L"hammer.json";

        std::error_code ec;
        if (fs::exists(tempQmDll)) fs::remove(tempQmDll, ec);
        if (fs::exists(tempJson)) fs::remove(tempJson, ec);
        if (fs::exists(tempOldJson)) fs::remove(tempOldJson, ec);

        return true;
    } catch (const std::exception& e) {
        outError = L"还原文件异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

