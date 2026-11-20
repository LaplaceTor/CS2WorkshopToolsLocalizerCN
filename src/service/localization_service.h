#pragma once

#include <functional>
#include <string>

#include <QString>

enum class RestoreCheck {
    Allowed,
    RejectedGameUpdated,
    RejectedOther
};

class LocalizationService {
public:
    using LogSink = std::function<void(const QString& message, const QString& color)>;

    struct Context {
        std::wstring cs2Root;
        std::wstring workingDir;
    };

    static bool Inject(const Context& ctx, bool useMachineTrans, const LogSink& log);

    static bool Restore(const Context& ctx, bool showLog, const LogSink& log);

    static RestoreCheck CheckRestore(const Context& ctx, QString* reason);

    static bool HasPendingRecovery(const Context& ctx);

    static bool IsCs2Running();

    static bool HasBackup(const Context& ctx);

    static bool IsPatchDeployed(const Context& ctx);

    static bool IsBackupMatching(const Context& ctx);

    static void WriteAppDirPointer(const std::wstring& cs2Root,
                                   const std::wstring& workingDir,
                                   bool useMachineTrans);

    static bool ClearSessionState(const std::wstring& workingDir);

    static void SyncDictionariesFromParent(const std::wstring& workingDir);
};
