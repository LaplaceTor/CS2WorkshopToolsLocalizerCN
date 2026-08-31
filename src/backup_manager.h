#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct GameVersionSignature {
    std::string cs2ProductVersion;
    std::string qt5CoreSha256;
    std::string qt5WidgetsSha256;
    std::string backupTimestamp;
    int64_t timestampEpoch = 0;
};

enum class BackupMatchStatus {
    Matches,             // 备份与当前游戏版本完全匹配，可以安全恢复
    NoBackup,            // 无备份
    MissingManifest,     // 存在备份文件但缺少版本清单
    GameUpdated,         // CS2 已更新（cs2.exe / Qt5Core 哈希或版本变化），拒绝使用旧备份直接覆盖
    ReadError            // 读取当前游戏或备份信息失败
};

struct BackupValidationResult {
    BackupMatchStatus status = BackupMatchStatus::NoBackup;
    GameVersionSignature backupSig;
    GameVersionSignature currentSig;
    std::wstring reason;
};

class BackupManager {
public:
    // 检查 backupDir 目录是否存在且包含有效的备份文件
    static bool HasBackup(const std::wstring& backupDir);

    // 获取当前 CS2 游戏环境的版本签名信息 (cs2.exe ProductVersion, Qt5Core SHA256, Qt5Widgets SHA256)
    static bool GetCurrentGameSignature(const std::wstring& cs2Root, GameVersionSignature& outSig, std::wstring& outError);

    // 读取 backupDir/backup_manifest.json 的版本签名
    static bool ReadBackupManifest(const std::wstring& backupDir, GameVersionSignature& outSig, std::wstring& outError);

    // 写入/更新 backupDir/backup_manifest.json
    static bool WriteBackupManifest(const std::wstring& backupDir, const GameVersionSignature& sig, std::wstring& outError);

    // 严格校验备份是否与当前 CS2 游戏版本匹配
    static BackupValidationResult BackupMatchesCurrentGame(const std::wstring& cs2Root, const std::wstring& backupDir);

    // 创建或更新完整备份（包含 FGD 文件、Qt5Core.dll 以及绑定的 backup_manifest.json）
    static bool CreateOrUpdateBackup(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError, bool forceRecreate = false);

    // 递归备份 CS2 目录下的所有 FGD 文件到 backupDir（按相对路径）
    static bool BackupFgdFiles(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError);

    // 备份 Qt5Core.dll 到 backupDir/game/bin/win64/Qt5Core.dll
    static bool BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);

    // 从 backupDir 完整恢复所有 FGD 和 Qt5Core.dll 到 CS2 目录，并清理临时注入文件（默认带版本匹配检查）
    static bool RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError, bool force = false);

    // 检查 CS2 目录中是否存在未还原的补丁文件（如 qtcore_qm.dll / qt_translations.json）
    static bool IsPatchDeployed(const std::wstring& cs2Root);

    // 检查工作目录下是否存在未正常结束的会话状态记录
    static bool HasUnrestoredSession(const std::wstring& workingDir);

    // 保存运行会话状态 (isPatched = true 表示补丁已部署且未还原)
    static void SaveSessionState(const std::wstring& workingDir, bool isPatched);

    // 清除运行会话状态记录
    static void ClearSessionState(const std::wstring& workingDir);

    // 计算文件 SHA-256 哈希值
    static std::string ComputeFileSha256(const std::wstring& filePath);

    // 获取文件 ProductVersion 字符串 (如 "1.40.8.2")
    static std::string GetFileProductVersion(const std::wstring& filePath);
};


