#include "core/backup_manager.h"
#include "core/pe_patcher.h"
#include "core/path_constants.h"
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
#include <QDebug>

namespace fs = std::filesystem;

namespace {

bool IsPatchedDll(const fs::path& dllPath) {
    PatchInfo info;
    std::wstring ignoredError;
    return PePatcher::GetPatchInfo(dllPath.wstring(), info, ignoredError) && info.isPatched;
}

}

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

bool BackupManager::SafeCopyFileWithRetry(const fs::path& src, const fs::path& dst, int maxRetries, int sleepMs) {
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

bool BackupManager::SafeRemoveFileWithRetry(const fs::path& path, int maxRetries, int sleepMs) {
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
        fs::path win64Bin = paths::Win64Bin(cs2Root);
        fs::path cs2Exe = paths::Cs2Exe(cs2Root);
        fs::path qt5Core = paths::Qt5Core(cs2Root);
        fs::path qt5Widgets = paths::Qt5Widgets(cs2Root);

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
        outError = L"获取当前游戏版本签名异常: " + QString::fromUtf8(e.what()).toStdWString();
        return false;
    }
}

bool BackupManager::ReadBackupManifest(const std::wstring& backupDir, GameVersionSignature& outSig, std::wstring& outError) {
    try {
        fs::path manifestPath = paths::Manifest(backupDir);
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
        outError = L"解析备份元数据异常: " + QString::fromUtf8(e.what()).toStdWString();
        return false;
    }
}

bool BackupManager::WriteBackupManifest(const std::wstring& backupDir, const GameVersionSignature& sig, std::wstring& outError) {
    try {
        fs::path manifestPath = paths::Manifest(backupDir);
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
        outError = L"写入备份元数据异常: " + QString::fromUtf8(e.what()).toStdWString();
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

    if (res.backupSig.cs2ProductVersion != res.currentSig.cs2ProductVersion) {
        res.status = BackupMatchStatus::GameUpdated;
        res.reason = L"检测到 CS2 版本已更新！备份版本为 [" +
            QString::fromStdString(res.backupSig.cs2ProductVersion).toStdWString() +
            L"]，当前游戏版本为 [" +
            QString::fromStdString(res.currentSig.cs2ProductVersion).toStdWString() +
            L"]。";
        return res;
    }

    if (!res.backupSig.cs2ExeSha256.empty() && !res.currentSig.cs2ExeSha256.empty()) {
        if (res.backupSig.cs2ExeSha256 != res.currentSig.cs2ExeSha256) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"检测到 cs2.exe 二进制哈希发生变动 (Steam depot 更新或完整性修复)。";
            return res;
        }
    }

    if (!res.backupSig.qt5WidgetsSha256.empty() && !res.currentSig.qt5WidgetsSha256.empty()) {
        if (res.backupSig.qt5WidgetsSha256 != res.currentSig.qt5WidgetsSha256) {
            res.status = BackupMatchStatus::GameUpdated;
            res.reason = L"检测到 Qt5Widgets.dll 哈希发生变动，游戏可能已经历热更新或完整性修复。";
            return res;
        }
    }

    fs::path backupQtCore = paths::Qt5Core(backupDir);
    if (fs::exists(backupQtCore)) {
        if (IsPatchedDll(backupQtCore)) {
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

    fs::path gameQtCore = paths::Qt5Core(cs2Root);
    if (fs::exists(gameQtCore)) {
        if (!IsPatchedDll(gameQtCore)) {
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
    fs::path manifestPath = paths::Manifest(targetBackupPath);

    if (!forceRecreate && HasBackup(backupDir) && fs::exists(manifestPath)) {
        GameVersionSignature existingSig;
        std::wstring mErr;
        if (ReadBackupManifest(backupDir, existingSig, mErr)) {
            return true;
        }
    }

    fs::path stagingPath = targetBackupPath.parent_path() / (targetBackupPath.filename().wstring() + L".staging");
    fs::path oldBackupPath = targetBackupPath.parent_path() / (targetBackupPath.filename().wstring() + L".old");

    std::error_code ec;
    fs::remove_all(stagingPath, ec);
    fs::remove_all(oldBackupPath, ec);

    std::vector<std::wstring> stagedFiles;
    if (!BackupQtCore(cs2Root, stagingPath.wstring(), outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }
    stagedFiles.push_back(paths::Qt5Core(cs2Root).wstring());

    fs::path backupQtCore = paths::Qt5Core(stagingPath);
    if (!fs::exists(backupQtCore)) {
        fs::remove_all(stagingPath, ec);
        outError = L"备份临时目录未找到 Qt5Core.dll";
        return false;
    }

    if (IsPatchedDll(backupQtCore)) {
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

    QDateTime now = QDateTime::currentDateTimeUtc();
    sig.timestampEpoch = now.toSecsSinceEpoch();
    sig.backupTimestamp = now.toString(Qt::ISODate).toStdString();

    if (!WriteBackupManifest(stagingPath.wstring(), sig, outError)) {
        fs::remove_all(stagingPath, ec);
        return false;
    }

    if (fs::exists(targetBackupPath)) {
        if (fs::exists(oldBackupPath)) {
            fs::remove_all(oldBackupPath, ec);
        }
        fs::rename(targetBackupPath, oldBackupPath, ec);
        if (ec) {
            fs::remove_all(stagingPath, ec);
            outError = L"无法重命名现有备份目录（可能被占用或受保护）";
            return false;
        }
    }

    fs::rename(stagingPath, targetBackupPath, ec);
    if (ec) {
        if (fs::exists(oldBackupPath)) {
            std::error_code rollEc;
            fs::rename(oldBackupPath, targetBackupPath, rollEc);
        }
        fs::remove_all(stagingPath, ec);
        outError = L"事务提交失败：无法将 staging 目录重命名为备份目录";
        return false;
    }

    fs::remove_all(oldBackupPath, ec);
    outBackedUpFiles = stagedFiles;
    return true;
}

bool BackupManager::BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError) {
    try {
        fs::path srcQtCore = paths::Qt5Core(cs2Root);
        fs::path dstQtCore = paths::Qt5Core(backupDir);

        if (!fs::exists(srcQtCore)) {
            outError = L"未找到源 Qt5Core.dll: " + srcQtCore.wstring();
            return false;
        }

        if (fs::exists(dstQtCore)) {
            if (IsPatchedDll(dstQtCore)) {
                std::error_code ec;
                fs::remove(dstQtCore, ec);
            } else {
                return true;
            }
        }

        if (IsPatchedDll(srcQtCore)) {
            outError = L"当前 CS2 目录下的 Qt5Core.dll 处于已补丁状态 (包含 LCLZ 标记)，严禁将其作为原版备份复制！";
            return false;
        }

        fs::create_directories(dstQtCore.parent_path());
        if (!SafeCopyFileWithRetry(srcQtCore, dstQtCore)) {
            outError = L"复制 Qt5Core.dll 失败 (文件被占用或无写权限): " + dstQtCore.wstring();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        outError = L"备份 Qt5Core.dll 异常: " + QString::fromUtf8(e.what()).toStdWString();
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

        if (!force) {
            auto val = BackupMatchesCurrentGame(cs2Root, backupDir);
            if (val.status == BackupMatchStatus::GameUpdated) {
                outError = L"拒绝恢复！" + val.reason + L"\n请通过启动汉化版重新捕获当前新版本纯净原版备份。";
                return false;
            }
        }

        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file()) {
                if (entry.path().filename() == paths::kBackupManifest) {
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

        fs::path win64Bin = paths::Win64Bin(cs2Path);
        fs::path tempQmDll = win64Bin / paths::kInjectDll;
        fs::path tempPointer = win64Bin / L"localizer_appdir.txt";

        SafeRemoveFileWithRetry(tempQmDll, 15, 100);
        SafeRemoveFileWithRetry(tempPointer, 15, 100);

        return true;
    } catch (const std::exception& e) {
        outError = L"还原文件异常: " + QString::fromUtf8(e.what()).toStdWString();
        return false;
    }
}

bool BackupManager::IsPatchDeployed(const std::wstring& cs2Root) {
    try {
        fs::path win64Bin = paths::Win64Bin(cs2Root);
        fs::path qmDll = win64Bin / paths::kInjectDll;
        return fs::exists(qmDll);
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

        QFile file(QString::fromStdWString(statePath.wstring()));
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }

        return doc.object().value("is_patched").toBool();
    } catch (...) {
        return false;
    }
}

bool BackupManager::SaveSessionState(const std::wstring& workingDir, bool isPatched) {
    try {
        fs::path statePath = fs::path(workingDir) / L"session_state.json";
        if (!isPatched) {
            std::error_code ec;
            fs::remove(statePath, ec);
            if (ec) {
                qWarning() << "SaveSessionState: 移除会话状态文件失败:"
                           << QString::fromStdWString(statePath.wstring());
                return false;
            }
            return true;
        }

        QJsonObject root;
        root["is_patched"] = true;
        QByteArray jsonBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

        QSaveFile saveFile(QString::fromStdWString(statePath.wstring()));
        if (!saveFile.open(QIODevice::WriteOnly)) {
            qWarning() << "SaveSessionState: 无法创建 session_state.json:"
                       << saveFile.errorString();
            return false;
        }

        if (saveFile.write(jsonBytes) != jsonBytes.size()) {
            saveFile.cancelWriting();
            qWarning() << "SaveSessionState: 写入 session_state.json 数据不完整";
            return false;
        }

        if (!saveFile.commit()) {
            qWarning() << "SaveSessionState: 提交 session_state.json 失败:"
                       << saveFile.errorString();
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        qWarning() << "SaveSessionState 异常:" << QString::fromUtf8(e.what());
        return false;
    } catch (...) {
        qWarning() << "SaveSessionState 未知异常";
        return false;
    }
}

bool BackupManager::ClearSessionState(const std::wstring& workingDir) {
    try {
        fs::path statePath = fs::path(workingDir) / L"session_state.json";
        std::error_code ec;
        fs::remove(statePath, ec);
        if (ec) {
            qWarning() << "ClearSessionState: 清除会话状态文件失败:"
                       << QString::fromStdWString(statePath.wstring());
            return false;
        }
        return true;
    } catch (...) {
        qWarning() << "ClearSessionState 未知异常";
        return false;
    }
}

