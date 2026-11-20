#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QObject>
#include <QProcess>
#include <QString>

#include "core/process_monitor.h"

class QTimer;

class HammerService : public QObject {
    Q_OBJECT

public:
    explicit HammerService(QObject* parent = nullptr);
    ~HammerService() override;

    struct LaunchParams {
        std::wstring cs2Root;
        std::wstring workingDir;
        QString      addon;
        QString      extraArgs;
    };

    struct LaunchResult {
        bool    ok    = false;
        QString error;
    };

    struct CreateAddonResult {
        bool    ok    = false;
        QString addonName;
        QString error;
    };

    struct RemoveAddonResult {
        bool    ok    = false;
        QString addonName;
        QString error;
    };

    struct ReloadResult {
        bool      ipcOk  = false;
        bool      fgdOk  = false;
        qsizetype fgdFileCount = 0;
        QString   fgdError;
    };

    using LogSink = std::function<void(const QString& message, const QString& color)>;

    void setLogSink(const LogSink& log);

    bool   isRunning() const { return m_isRunning; }
    qint64 processId() const { return m_pid; }

    bool isIpcAvailable() const;

    static bool isExcludedAddon(const QString& addonName);

    static std::vector<std::wstring> availableAddons(const std::wstring& cs2Root);

    CreateAddonResult createAddon(const std::wstring& cs2Root, const QString& rawAddonName);

    RemoveAddonResult removeAddon(const std::wstring& cs2Root, const QString& rawAddonName);

    LaunchResult launch(const LaunchParams& params);

    bool sendToggleLanguage();

    ReloadResult reloadDictionaries(const std::wstring& cs2Root,
                                    const std::wstring& workingDir,
                                    bool useMachineTrans);

signals:
    void started(qint64 pid);

    void terminated();

    void startFailed(int errorCode);

    void heartbeat();

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onMonitorTimeout();

private:
    void log(const QString& message, const QString& color);
    void resetTerminatedState();

    QProcess*      m_process;
    QTimer*        m_monitorTimer;
    ProcessMonitor m_monitor;
    LogSink        m_log;

    bool   m_isRunning;
    qint64 m_pid;
    void*  m_processHandle;
};
