#pragma once

// HAMMER 编辑器的生命周期编排（原 MainWindow 中的 QProcess / 进程监控 / IPC / 热重载部分）。
//
// 职责边界：
//   - 只负责「启动 → 监控 → 退出 → IPC → 热重载」这条业务流的编排与状态保存；
//   - 不弹对话框、不操作任何 UI 控件，全部结果通过返回值或信号外抛；
//   - 底层能力仍然来自 core 层的 ProcessMonitor / HammerIpc / LauncherConfig。

#include <functional>
#include <string>

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
        QString      addon;      // 目标 Addon 名（已去除附加说明）
        QString      extraArgs;  // 用户附加启动参数
    };

    struct LaunchResult {
        bool    ok    = false;
        QString error;  // 启动失败原因（供上层弹窗展示）
    };

    // 热重载结果：磁盘侧与 IPC 侧分开，便于上层分别提示
    struct ReloadResult {
        bool      ipcOk  = false;  // 是否成功通知到运行中的 Hammer
        bool      fgdOk  = false;  // FGD 重新编译并部署是否成功
        qsizetype fgdFileCount = 0;
        QString   fgdError;
    };

    // 日志回调：(消息, 颜色)
    using LogSink = std::function<void(const QString& message, const QString& color)>;

    void setLogSink(const LogSink& log);

    bool   isRunning() const { return m_isRunning; }
    qint64 processId() const { return m_pid; }

    // 启动 HAMMER（cs2.exe -addon <addon> -tools [用户参数]）。
    // 返回 true 仅代表进程已成功发起启动，后续失败通过 startFailed 信号外抛。
    LaunchResult launch(const LaunchParams& params);

    // 向运行中的 Hammer 发送 IPC 指令
    bool sendToggleLanguage();

    // 重新编译并部署 FGD、同步 Qt 词典，再通过 IPC 通知 Hammer 刷新（免重启热重载）
    ReloadResult reloadDictionaries(const std::wstring& cs2Root,
                                    const std::wstring& workingDir,
                                    bool useMachineTrans);

signals:
    // 进程已启动并拿到 PID
    void started(qint64 pid);

    // 进程确认已退出（finished 或连续多次未探测到）
    void terminated();

    // QProcess 启动失败（FailedToStart）：不会伴随 terminated 信号
    void startFailed(int errorCode);

    // 每秒监控采样仍然探测到进程存活
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
