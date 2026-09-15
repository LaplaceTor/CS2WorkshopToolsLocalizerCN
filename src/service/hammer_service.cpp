#include "service/hammer_service.h"

#include <filesystem>

#include <QFileInfo>
#include <QTimer>

#include <windows.h>

#include "core/backup_manager.h"
#include "core/cs2_detector.h"
#include "core/dictionary_compiler.h"
#include "core/dictionary_paths.h"
#include "core/fgd_translator.h"
#include "core/hammer_ipc.h"
#include "core/path_constants.h"

namespace fs = std::filesystem;

HammerService::HammerService(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_monitorTimer(new QTimer(this))
    , m_isRunning(false)
    , m_pid(0)
    , m_processHandle(nullptr) {
    connect(m_process, &QProcess::started,
            this, &HammerService::onProcessStarted);

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &HammerService::onProcessFinished);

    connect(m_process, &QProcess::errorOccurred,
            this, &HammerService::onProcessError);

    connect(m_monitorTimer, &QTimer::timeout,
            this, &HammerService::onMonitorTimeout);
}

HammerService::~HammerService() {
    if (m_processHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_processHandle));
        m_processHandle = nullptr;
    }
}

void HammerService::setLogSink(const LogSink& log) {
    m_log = log;
}

void HammerService::log(const QString& message, const QString& color) {
    if (m_log) {
        m_log(message, color);
    }
}

HammerService::LaunchResult HammerService::launch(const LaunchParams& params) {
    LaunchResult result;

    const fs::path cs2Bin = paths::Win64Bin(params.cs2Root);

    const QString cs2ExePath =
        QString::fromStdWString((cs2Bin / paths::kCs2Exe).wstring());

    if (!QFileInfo::exists(cs2ExePath)) {
        result.error = "找不到 cs2.exe：\n" + cs2ExePath;
        return result;
    }

    QString selectedAddon = params.addon.trimmed();
    if (selectedAddon.isEmpty()) {
        selectedAddon = "addon_template";
    }

    QStringList processArgs;
    processArgs << "-addon" << selectedAddon << "-tools";

    const QString customArgs = params.extraArgs.trimmed();
    if (!customArgs.isEmpty()) {
        processArgs.append(QProcess::splitCommand(customArgs));
    }

    m_process->setProgram(cs2ExePath);
    m_process->setArguments(processArgs);
    m_process->setWorkingDirectory(QString::fromStdWString(cs2Bin.wstring()));
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);

    log(
        QString("[*] 执行 HAMMER 命令: %1 %2").arg(cs2ExePath, processArgs.join(" ")),
        "#75715e"
    );

    // 保持 session_state 为已注入
    if (!BackupManager::SaveSessionState(params.workingDir, true)) {
        log(
            "[-] 会话状态写入失败 (session_state.json)，异常退出后的自动恢复可能失效",
            "#f92672"
        );
    }

    m_isRunning = true;
    m_monitor.Reset();

    m_process->start();

    m_monitorTimer->start(1000);

    result.ok = true;
    return result;
}

bool HammerService::sendToggleLanguage() {
    return HammerIpc::Send(HammerIpc::kMsgToggleLang);
}

bool HammerService::isIpcAvailable() const {
    return HammerIpc::FindIpcWindow() != nullptr;
}

std::vector<std::wstring> HammerService::availableAddons(const std::wstring& cs2Root) {
    return Cs2Detector::GetAvailableAddons(cs2Root);
}

HammerService::ReloadResult HammerService::reloadDictionaries(const std::wstring& cs2Root,
                                                              const std::wstring& workingDir,
                                                              bool useMachineTrans) {
    ReloadResult result;

    // 1. 镜像同步与合并 qt_translations.jsonc 到游戏目录（保障本地 fallback 完整）
    const fs::path srcQtJson     = ResolveDictionaryPath(workingDir, L"qt_translations.jsonc");
    const fs::path srcQtFallback = ResolveDictionaryPath(workingDir, L"qt_fallback.jsonc");
    const fs::path cs2Bin        = paths::Win64Bin(cs2Root);
    const fs::path destQtJson     = cs2Bin / L"qt_translations.jsonc";
    const fs::path destQtFallback = cs2Bin / L"qt_fallback.jsonc";

    if (fs::exists(cs2Bin)) {
        // 同步 fallback 字典到游戏目录备份（如果存在）
        if (fs::exists(srcQtFallback)) {
            try {
                fs::copy_file(srcQtFallback, destQtFallback, fs::copy_options::overwrite_existing);
            } catch (...) {}
        }

        // 优先合并主词典与机翻兜底词典部署到游戏目录
        const std::wstring qtFallbackParam =
            (useMachineTrans && fs::exists(srcQtFallback)) ? srcQtFallback.wstring() : L"";

        bool merged = false;
        if (!qtFallbackParam.empty() && fs::exists(srcQtJson)) {
            std::wstring mergeErr;
            merged = DictionaryCompiler::MergeJsonFiles(
                srcQtJson.wstring(), qtFallbackParam, destQtJson.wstring(), mergeErr);
        }

        if (!merged && fs::exists(srcQtJson)) {
            try {
                fs::copy_file(srcQtJson, destQtJson, fs::copy_options::overwrite_existing);
            } catch (...) {}
        }
    }

    // 2. 联动重新编译并部署 FGD（引入 fgd_fallback 兜底）
    const fs::path transDir        = fs::path(workingDir) / paths::kTranslationsDir;
    const fs::path backupDir       = fs::path(workingDir) / paths::kBackupDir;
    const fs::path fgdDictPath     = ResolveDictionaryPath(workingDir, L"fgd_translations.jsonc");
    const fs::path fgdOverridePath = ResolveDictionaryPath(workingDir, L"fgd_override.jsonc");
    const fs::path fgdFallbackPath = ResolveDictionaryPath(workingDir, L"fgd_fallback.jsonc");

    const std::wstring fgdFallbackParam =
        (useMachineTrans && fs::exists(fgdFallbackPath)) ? fgdFallbackPath.wstring() : L"";

    std::vector<std::wstring> transFgd;
    std::wstring err;

    result.fgdOk = FgdTranslator::TranslateAndDeployAll(
        cs2Root,
        backupDir.wstring(),
        transDir.wstring(),
        fgdDictPath.wstring(),
        fgdOverridePath.wstring(),
        transFgd,
        err,
        fgdFallbackParam
    );

    result.fgdFileCount = static_cast<qsizetype>(transFgd.size());
    result.fgdError     = QString::fromStdWString(err);

    // 3. 发送 IPC 消息给 Hammer
    result.ipcOk = HammerIpc::Send(HammerIpc::kMsgReloadDict);

    return result;
}

void HammerService::onProcessStarted() {
    m_isRunning = true;
    m_monitor.Reset();

    m_pid = m_process->processId();

    if (m_processHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_processHandle));
        m_processHandle = nullptr;
    }

    if (m_pid > 0) {
        m_processHandle = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(m_pid)
        );
    }

    emit started(m_pid);
}

void HammerService::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    if (!m_isRunning) {
        return;
    }

    resetTerminatedState();

    emit terminated();
}

void HammerService::onProcessError(QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) {
        // 其他错误 (Crashed / TimedError 等)：交由 finished / 监控定时器路径统一处理，避免双重恢复
        return;
    }

    // start() 前已置 m_isRunning = true，而 FailedToStart 只触发 errorOccurred、不触发 finished，
    // 必须在此立即回滚，否则只能依赖 1 秒轮询兜底
    resetTerminatedState();

    log(
        QString("[-] 无法启动 Hammer 进程 (cs2.exe)，错误码: %1").arg(error),
        "#f92672"
    );

    emit startFailed(static_cast<int>(error));
}

void HammerService::onMonitorTimeout() {
    if (!m_isRunning) {
        m_monitorTimer->stop();
        return;
    }

    // 三路探测来源按优先级由 ProcessMonitor 判定（QProcess → 原生句柄 → PID）
    ProcessMonitor::Source src;
    src.qProcessRunning = (m_process != nullptr) && (m_process->state() == QProcess::Running);
    src.nativeHandle    = m_processHandle;
    src.pid             = static_cast<unsigned long>(m_pid);

    if (m_monitor.Sample(src)) {
        emit heartbeat();
    } else if (m_monitor.ReachedTerminationThreshold()) {
        resetTerminatedState();

        emit terminated();
    }
}

void HammerService::resetTerminatedState() {
    m_monitorTimer->stop();

    m_isRunning = false;

    if (m_processHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_processHandle));
        m_processHandle = nullptr;
    }

    m_pid = 0;
}
