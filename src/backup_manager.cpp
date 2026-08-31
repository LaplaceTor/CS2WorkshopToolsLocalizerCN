#include "backup_manager.h"
#include "pe_patcher.h"
#include <windows.h>
#include <winver.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <system_error>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFile>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <QByteArray>
#include <QString>

namespace fs = std::filesystem;

std::string BackupManager::ComputeFileSha256(const std::wstring& filePath) {
    QFile file(QString::fromStdWString(filePath));
    if (!file.open(QIODevice::ReadOnly)) {
        return "";
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return "";
    }
    return hash.result().toHex().toStdString();
}

std::string BackupManager::GetFileProductVersion(const std::wstring& filePath) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(filePath.c_str(), &dummy);
    if (size == 0) return "0.0.0.0";

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(filePath.c_str(), 0, size, data.data())) {
        return "0.0.0.0";
    }

    VS_FIXEDFILEINFO* pFileInfo = nullptr;
    UINT fileInfoLen = 0;
    if (VerQueryValueW(data.data(), L"\\", (LPVOID*)&pFileInfo, &fileInfoLen) && pFileInfo && fileInfoLen >= sizeof(VS_FIXEDFILEINFO)) {
        DWORD major = HIWORD(pFileInfo->dwProductVersionMS);
        DWORD minor = LOWORD(pFileInfo->dwProductVersionMS);
        DWORD build = HIWORD(pFileInfo->dwProductVersionLS);
        DWORD rev   = LOWORD(pFileInfo->dwProductVersionLS);
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", major, minor, build, rev);
        return std::string(buf);
    }
    return "0.0.0.0";
}

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

bool BackupManager::GetCurrentGameSignature(const std::wstring& cs2Root, GameVersionSignature& outSig, std::wstring& outError) {
    try {
        fs::path win64Bin = fs::path(cs2Root) / L"game" / L"bin" / L"win64";
        fs::path cs2Exe = win64Bin / L"cs2.exe";
        fs::path qt5Core = win64Bin / L"Qt5Core.dll";
        fs::path qt5Widgets = win64Bin / L"Qt5Widgets.dll";

        if (!fs::exists(cs2Exe)) {
            outError = L"未找到 cs2.exe: " + cs2Exe.wstring();
            return false;
        }

        outSig.cs2ProductVersion = GetFileProductVersion(cs2Exe.wstring());
        outSig.cs2ExeSha256 = ComputeFileSha256(cs2Exe.wstring());
        outSig.qt5CoreSha256 = fs::exists(qt5Core) ? ComputeFileSha256(qt5Core.wstring()) : "";
        outSig.qt5WidgetsSha256 = fs::exists(qt5Widgets) ? ComputeFileSha256(qt5Widgets.wstring()) : "";
        return true;
    } catch (const std::exception& e) {
        outError = L"获取当前游戏版本签名异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

bool BackupManager::ReadBackupManifest(const std::wstring& backupDir, GameVersionSignature& outSig, std::wstring& outError) {
    try {
        fs::path manifestPath = fs::path(backupDir) / L"backup_manifest.json";
        if (!fs::exists(manifestPath)) {
            outError = L"未找到备份版本元数据清单: " + manifestPath.wstring();
            return false;
        }

        QFile file(QString::fromStdWString(manifestPath.wstring()));
        if (!file.open(QIODevice::ReadOnly)) {
            outError = L"无法读取备份元数据清单文件: " + file.errorString().toStdWString();
            return false;
        }

        QByteArray data = file.readAll();
        file.close();

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            outError = L"解析备份清单 JSON 失败: " + parseError.errorString().toStdWString();
            return false;
        }

        QJsonObject root = doc.object();
        outSig.cs2ProductVersion = root.value("cs2_exe_version").toString().toStdString();
        outSig.cs2ExeSha256 = root.value("cs2_exe_sha256").toString().toStdString();
        outSig.qt5CoreSha256 = root.value("qt5core_sha256").toString().toStdString();
        outSig.qt5WidgetsSha256 = root.value("qt5widgets_sha256").toString().toStdString();
        outSig.backupTimestamp = root.value("timestamp").toString().toStdString();

        if (root.contains("timestamp_epoch")) {
            outSig.timestampEpoch = root.value("timestamp_epoch").toInteger();
        }

        return !outSig.cs2ProductVersion.empty() && !outSig.qt5CoreSha256.empty();
    } catch (const std::exception& e) {
        outError = L"解析备份元数据异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

bool BackupManager::WriteBackupManifest(const std::wstring& backupDir, const GameVersionSignature& sig, std::wstring& outError) {
    try {
        fs::path manifestPath = fs::path(backupDir) / L"backup_manifest.json";
        fs::create_directories(manifestPath.parent_path());

        QJsonObject root;
        root["version"] = 1;
        root["timestamp"] = QString::fromStdString(sig.backupTimestamp);
        root["timestamp_epoch"] = static_cast<qint64>(sig.timestampEpoch);
        root["cs2_exe_version"] = QString::fromStdString(sig.cs2ProductVersion);
        root["cs2_exe_sha256"] = QString::fromStdString(sig.cs2ExeSha256);
        root["qt5core_sha256"] = QString::fromStdString(sig.qt5CoreSha256);
        root["qt5widgets_sha256"] = QString::fromStdString(sig.qt5WidgetsSha256);

        QJsonDocument doc(root);
        QByteArray jsonBytes = doc.toJson(QJsonDocument::Indented);

        // 使用 Qt QSaveFile 进行原生原子写入与事务提交
        QSaveFile saveFile(QString::fromStdWString(manifestPath.wstring()));
        if (!saveFile.open(QIODevice::WriteOnly)) {
            outError = L"无法创建 backup_manifest.json: " + saveFile.errorString().toStdWString();
            return false;
        }

        if (saveFile.write(jsonBytes) != jsonBytes.size()) {
            saveFile.cancelWriting();
            outError = L"写入 backup_manifest.json 数据不完整";
            return false;
        }

        if (!saveFile.commit()) {
            outError = L"提交保存 backup_manifest.json 失败: " + saveFile.errorString().toStdWString();
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        outError = L"写入备份元数据异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

BackupValidationResult BackupManager::BackupMatchesCurrentGame(const std::wstring& cs2Root, const std::wstring& backupDir) {
    BackupValidationResult res;
    res.status = BackupMatchStatus::NoBackup;

    if (!HasBackup(backupDir)) {
        res.reason = L"尚未创建任何原版备份。";
        return res;
    }

    std::wstring err;
    if (!ReadBackupManifest(backupDir, res.backupSig, err)) {
        res.status = BackupMatchStatus::MissingManifest;
        res.reason = L"存在旧版备份文件但缺少版本清单 (backup_manifest.json)。建议重新创建当前版本的纯净备份。";
        return res;
    }

    if (!GetCurrentGameSignature(cs2Root, res.currentSig, err)) {
        res.status = BackupMatchStatus::ReadError;
        res.reason = L"无法读取当前 CS2 游戏目录签名: " + err;
        return res;
    }

    // 1. 严格校验 cs2.exe 的 ProductVersion
    if (res.backupSig.cs2ProductVersion != res.currentSig.cs2ProductVersion) {
        res.status = BackupMatchStatus::GameUpdated;
        res.reason = L"检测到 CS2 版本已更新！备份版本为 [" + 
            std::wstring(res.backupSig.cs2ProductVersion.begin(), res.backupSig.cs2ProductVersion.end()) + 
            L"]，当前游戏版本为 [" + 
            std::wstring(res.currentSig.cs2ProductVersion.begin(), res.currentSig.cs2ProductVersion.end()) + 
            L"]。";
        return res;
    }

    // 2. 严格校验 cs2.exe 的 SHA-256 二进制哈希（捕捉同 ProductVersion 下的 Steam depot 补丁与热更新）
    if (!res.backupSig.cs2ExeSha256.empty() && !res.currentSig.cs2ExeSha256.empty()) {
        if (res.backupSig.cs2ExeSha256 != res.currentSig.cs2ExeSha256) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"检测到 cs2.exe 二进制哈希发生变动 (Steam depot 更新或完整性修复)。";
            return res;
        }
    }

    // 3. 校验 Qt5Widgets.dll SHA-256（如果存在）
    if (!res.backupSig.qt5WidgetsSha256.empty() && !res.currentSig.qt5WidgetsSha256.empty()) {
        if (res.backupSig.qt5WidgetsSha256 != res.currentSig.qt5WidgetsSha256) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"检测到 Qt5Widgets.dll 哈希发生变动，游戏可能已经历热更新或完整性修复。";
            return res;
        }
    }

    // 3. 校验备份中 Qt5Core.dll 与清单记录的一致性
    fs::path backupQtCore = fs::path(backupDir) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    if (fs::exists(backupQtCore)) {
        PatchInfo bInfo;
        std::wstring pErr;
        if (PePatcher::GetPatchInfo(backupQtCore.wstring(), bInfo, pErr) && bInfo.isPatched) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"备份目录内的 Qt5Core.dll 包含 LCLZ 补丁标记（非纯净原版），备份不可用。";
            return res;
        }

        std::string backupQtCoreHash = ComputeFileSha256(backupQtCore.wstring());
        if (backupQtCoreHash != res.backupSig.qt5CoreSha256) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"备份目录内的 Qt5Core.dll 与版本清单记录哈希不一致，备份可能已被篡改。";
            return res;
        }
    }

    // 4. 校验当前游戏目录中未补丁状态的 Qt5Core.dll (若当前游戏文件是纯净状态，比对是否与备份一致)
    fs::path gameQtCore = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    if (fs::exists(gameQtCore)) {
        PatchInfo gInfo;
        std::wstring pErr;
        bool isPatched = (PePatcher::GetPatchInfo(gameQtCore.wstring(), gInfo, pErr) && gInfo.isPatched);
        if (!isPatched) {
            std::string gameQtCoreHash = ComputeFileSha256(gameQtCore.wstring());
            if (gameQtCoreHash != res.backupSig.qt5CoreSha256) {
                res.status = BackupMatchStatus::GameUpdated;
                res.reason = L"检测到 CS2 目录下的原版 Qt5Core.dll 哈希发生变动，游戏可能已经历更新或完整性验证。";
                return res;
            }
        }
    }

    res.status = BackupMatchStatus::Matches;
    res.reason = L"备份文件与当前 CS2 游戏版本完全匹配。";
    return res;
}

bool BackupManager::CreateOrUpdateBackup(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError, bool forceRecreate) {
    fs::path targetBackupPath(backupDir);
    fs::path manifestPath = targetBackupPath / L"backup_manifest.json";

    // 核心安全防线 1：如果不是显式 forceRecreate，且有效备份与 manifest 已存在，绝不重复更新 manifest！
    if (!forceRecreate && HasBackup(backupDir) && fs::exists(manifestPath)) {
        GameVersionSignature existingSig;
        std::wstring mErr;
        if (ReadBackupManifest(backupDir, existingSig, mErr)) {
            return true;
        }
    }

    // 事务式重建：先在 staging 临时目录完成完整备份与校验，成功后再原子替换旧备份
    fs::path stagingPath = targetBackupPath.parent_path() / (targetBackupPath.filename().wstring() + L".staging");
    fs::path oldBackupPath = targetBackupPath.parent_path() / (targetBackupPath.filename().wstring() + L".old");

    std::error_code ec;
    fs::remove_all(stagingPath, ec);
    fs::remove_all(oldBackupPath, ec);

    std::vector<std::wstring> stagedFiles;
    if (!BackupFgdFiles(cs2Root, stagingPath.wstring(), stagedFiles, outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }

    if (!BackupQtCore(cs2Root, stagingPath.wstring(), outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }

    fs::path backupQtCore = stagingPath / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    if (!fs::exists(backupQtCore)) {
        fs::remove_all(stagingPath, ec);
        outError = L"备份临时目录未找到 Qt5Core.dll";
        return false;
    }

    // 严格验证备份目录中的 Qt5Core.dll 绝不能是 patched 的！
    PatchInfo patchInfo;
    std::wstring pErr;
    if (PePatcher::GetPatchInfo(backupQtCore.wstring(), patchInfo, pErr) && patchInfo.isPatched) {
        fs::remove_all(stagingPath, ec);
        outError = L"备份的 Qt5Core.dll 包含 LCLZ 补丁标记，严禁用 patched 文件生成 manifest！";
        return false;
    }

    GameVersionSignature sig;
    if (!GetCurrentGameSignature(cs2Root, sig, outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }

    sig.qt5CoreSha256 = ComputeFileSha256(backupQtCore.wstring());

    // 使用 Qt QDateTime 进行标准 UTC 时间与时间戳生成
    QDateTime now = QDateTime::currentDateTimeUtc();
    sig.timestampEpoch = now.toSecsSinceEpoch();
    sig.backupTimestamp = now.toString(Qt::ISODate).toStdString();

    if (!WriteBackupManifest(stagingPath.wstring(), sig, outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }

    // 事务提交：staging 备份完整成功，原子替换 old backup
    if (fs::exists(targetBackupPath)) {
        fs::rename(targetBackupPath, oldBackupPath, ec);
        if (ec) {
            fs::remove_all(targetBackupPath, ec);
        }
    }

    fs::rename(stagingPath, targetBackupPath, ec);
    if (ec) {
        if (fs::exists(oldBackupPath)) {
            fs::rename(oldBackupPath, targetBackupPath, ec);
        }
        fs::remove_all(stagingPath, ec);
        outError = L"事务提交失败：无法将 staging 目录重命名为备份目录";
        return false;
    }

    // 成功后清理 old 目录
    fs::remove_all(oldBackupPath, ec);
    outBackedUpFiles = stagedFiles;
    return true;
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

        // 安全策略：若备份已存在
        if (fs::exists(dstQtCore)) {
            // 验证已存在的备份是否是未补丁的纯净原版
            PatchInfo bInfo;
            std::wstring pErr;
            if (PePatcher::GetPatchInfo(dstQtCore.wstring(), bInfo, pErr) && bInfo.isPatched) {
                // 备份文件已被污染（包含 LCLZ），必须将其移除
                std::error_code ec;
                fs::remove(dstQtCore, ec);
            } else {
                // 备份中已存在纯净原版，安全保留，绝不覆盖
                return true;
            }
        }

        // 检查源 Qt5Core.dll 是否已被修补 (包含 LCLZ 补丁)
        PatchInfo srcInfo;
        std::wstring pErr;
        if (PePatcher::GetPatchInfo(srcQtCore.wstring(), srcInfo, pErr) && srcInfo.isPatched) {
            outError = L"当前 CS2 目录下的 Qt5Core.dll 处于已补丁状态 (包含 LCLZ 标记)，严禁将其作为原版备份复制！";
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

bool BackupManager::RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError, bool force) {
    try {
        fs::path backupRoot(backupDir);
        fs::path cs2Path(cs2Root);

        if (!fs::exists(backupRoot)) {
            outError = L"备份目录不存在: " + backupDir;
            return false;
        }

        // 版本安全防线：若非强制模式，且检测到游戏已更新，则拒绝直接恢复旧版文件
        if (!force) {
            auto val = BackupMatchesCurrentGame(cs2Root, backupDir);
            if (val.status == BackupMatchStatus::GameUpdated) {
                outError = L"拒绝恢复！" + val.reason + L"\n请通过启动汉化版重新捕获当前新版本纯净原版备份。";
                return false;
            }
        }

        // 1. 还原 backup 目录下的所有文件 (排除 backup_manifest.json，带重试机制)
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file()) {
                if (entry.path().filename() == L"backup_manifest.json") {
                    continue;
                }

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


