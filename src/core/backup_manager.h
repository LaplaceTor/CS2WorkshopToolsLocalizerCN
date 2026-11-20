#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

struct GameVersionSignature {
    std::string cs2ProductVersion;
    std::string cs2ExeSha256;
    std::string qt5CoreSha256;
    std::string qt5WidgetsSha256;
    std::string backupTimestamp;
    int64_t timestampEpoch = 0;
};

enum class BackupMatchStatus {
    Matches,
    NoBackup,
    MissingManifest,
    GameUpdated,
    ReadError
};

struct BackupValidationResult {
    BackupMatchStatus status = BackupMatchStatus::NoBackup;
    GameVersionSignature backupSig;
    GameVersionSignature currentSig;
    std::wstring reason;
};

class BackupManager {
public:
    static bool HasBackup(const std::wstring& backupDir);

    static bool GetCurrentGameSignature(const std::wstring& cs2Root, GameVersionSignature& outSig, std::wstring& outError);

    static bool ReadBackupManifest(const std::wstring& backupDir, GameVersionSignature& outSig, std::wstring& outError);

    static bool WriteBackupManifest(const std::wstring& backupDir, const GameVersionSignature& sig, std::wstring& outError);

    static BackupValidationResult BackupMatchesCurrentGame(const std::wstring& cs2Root, const std::wstring& backupDir);

    static bool CreateOrUpdateBackup(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError, bool forceRecreate = false);

    static bool BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);

    static bool RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError, bool force = false);

    static bool IsPatchDeployed(const std::wstring& cs2Root);

    static bool HasUnrestoredSession(const std::wstring& workingDir);

    static bool SaveSessionState(const std::wstring& workingDir, bool isPatched);

    static bool ClearSessionState(const std::wstring& workingDir);

    static std::string ComputeFileSha256(const std::wstring& filePath);

    static std::string GetFileProductVersion(const std::wstring& filePath);

    static bool SafeCopyFileWithRetry(const std::filesystem::path& src, const std::filesystem::path& dst, int maxRetries = 25, int sleepMs = 150);
    static bool SafeRemoveFileWithRetry(const std::filesystem::path& path, int maxRetries = 15, int sleepMs = 100);
};

