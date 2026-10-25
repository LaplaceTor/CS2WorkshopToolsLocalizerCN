#include "service/hammer_service.h"

#include <filesystem>
#include <system_error>

#include <QFileInfo>
#include <QTimer>
#include <QFile>
#include <QRegularExpression>

#include <windows.h>

#include "core/backup_manager.h"
#include "core/cs2_detector.h"
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

bool HammerService::isExcludedAddon(const QString& addonName) {
    return Cs2Detector::IsExcludedAddon(addonName);
}

std::vector<std::wstring> HammerService::availableAddons(const std::wstring& cs2Root) {
    return Cs2Detector::GetAvailableAddons(cs2Root);
}

HammerService::CreateAddonResult HammerService::createAddon(const std::wstring& cs2Root, const QString& rawAddonName) {
    CreateAddonResult result;
    const QString addon = rawAddonName.trimmed().toLower();

    if (addon.isEmpty()) {
        result.error = "项目名称不能为空。";
        return result;
    }

    static const QRegularExpression namePattern(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_]*$"));
    if (!namePattern.match(addon).hasMatch()) {
        result.error = "项目名称格式不正确：必须以英文字母开头，且只能包含英文字母、数字和下划线。";
        return result;
    }

    if (isExcludedAddon(addon)) {
        result.error = QString("“%1”为 CS2 官方内置只读模组，不可用作项目名称。").arg(addon);
        return result;
    }

    const fs::path cs2RootPath(cs2Root);
    const fs::path contentAddonPath = paths::Addons(cs2RootPath) / addon.toStdWString();
    const fs::path gameAddonPath = paths::GameAddons(cs2RootPath) / addon.toStdWString();

    if (fs::exists(contentAddonPath) || fs::exists(gameAddonPath)) {
        result.error = QString("项目“%1”在工坊模组目录中已存在，请换用其他名称。").arg(addon);
        return result;
    }

    std::error_code ec;
    fs::create_directories(contentAddonPath, ec);
    if (ec) {
        result.error = QString("创建 content 目录失败：%1").arg(QString::fromStdString(ec.message()));
        return result;
    }

    fs::create_directories(gameAddonPath, ec);
    if (ec) {
        result.error = QString("创建 game 目录失败：%1").arg(QString::fromStdString(ec.message()));
        return result;
    }

    const fs::path contentTemplate = paths::Addons(cs2RootPath) / L"addon_template";
    const fs::path gameTemplate = paths::GameAddons(cs2RootPath) / L"addon_template";
    const fs::path rcExe = paths::ResourceCopyExe(cs2RootPath);

    auto copyFromTemplate = [&](const fs::path& templateDir, const fs::path& targetDir) {
        if (!fs::exists(templateDir)) {
            return;
        }

        bool rcSucceeded = false;
        if (fs::exists(rcExe)) {
            const fs::path rcLog = targetDir / L"resourcecopy.log";
            QStringList rcArgs;
            rcArgs << "-retail"
                   << "-skip" << "readonly_tools_asset_info.bin"
                   << "-skip" << "resourcecopy.log"
                   << "-fulllog" << QString::fromStdWString(rcLog.wstring())
                   << "-rename" << "xxx_mapname_xxx" << addon
                   << QString::fromStdWString((templateDir / "*").wstring())
                   << QString::fromStdWString(targetDir.wstring());

            QProcess p;
            p.setWorkingDirectory(QString::fromStdWString(paths::Win64Bin(cs2RootPath).wstring()));
            p.start(QString::fromStdWString(rcExe.wstring()), rcArgs);
            if (p.waitForFinished(20000) && p.exitCode() == 0) {
                rcSucceeded = true;
            }
            std::error_code ignoreEc;
            fs::remove(rcLog, ignoreEc);
        }

        if (!rcSucceeded) {
            // 兜底：C++ 递归复制并重命名
            try {
                for (const auto& entry : fs::recursive_directory_iterator(templateDir)) {
                    const auto& p = entry.path();
                    if (p.filename() == L"readonly_tools_asset_info.bin" || p.filename() == L"resourcecopy.log") {
                        continue;
                    }
                    auto rel = fs::relative(p, templateDir);
                    std::wstring relStr = rel.wstring();
                    const std::wstring fromTag = L"xxx_mapname_xxx";
                    const std::wstring toTag = addon.toStdWString();
                    size_t pos = 0;
                    while ((pos = relStr.find(fromTag, pos)) != std::wstring::npos) {
                        relStr.replace(pos, fromTag.length(), toTag);
                        pos += toTag.length();
                    }
                    fs::path destPath = targetDir / relStr;
                    if (entry.is_directory()) {
                        fs::create_directories(destPath);
                    } else if (entry.is_regular_file()) {
                        fs::create_directories(destPath.parent_path());
                        fs::copy_file(p, destPath, fs::copy_options::overwrite_existing);
                    }
                }
            } catch (...) {}
        }
    };

    copyFromTemplate(contentTemplate, contentAddonPath);
    copyFromTemplate(gameTemplate, gameAddonPath);

    // 写入干净的标准 KV3 addoninfo.txt
    const fs::path addonInfoPath = gameAddonPath / L"addoninfo.txt";
    QFile infoFile(QString::fromStdWString(addonInfoPath.wstring()));
    if (infoFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        infoFile.write(
            "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n"
            "{\n"
            "}\n"
        );
        infoFile.close();
    }

    // 确保移除任何可能残留的只读标识与日志
    std::error_code ignoreEc;
    fs::remove(gameAddonPath / L"readonly_tools_asset_info.bin", ignoreEc);
    fs::remove(contentAddonPath / L"readonly_tools_asset_info.bin", ignoreEc);
    fs::remove(gameAddonPath / L"resourcecopy.log", ignoreEc);
    fs::remove(contentAddonPath / L"resourcecopy.log", ignoreEc);

    log(QString("[+] 成功创建新 Addon 项目: %1").arg(addon), "#a6e22e");

    result.ok = true;
    result.addonName = addon;
    return result;
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

    const QString selectedAddon = params.addon.trimmed();
    if (selectedAddon.isEmpty() || isExcludedAddon(selectedAddon)) {
        result.error = "未选择有效的目标 Addon 项目，请先选择或新建一个项目。";
        return result;
    }

    QStringList processArgs;
    processArgs << "-addon" << selectedAddon << "-tools" << "-vulkan";

    const QString customArgs = params.extraArgs.trimmed();
    if (!customArgs.isEmpty()) {
        const QStringList customList = QProcess::splitCommand(customArgs);
        for (const QString& arg : customList) {
            if (arg.compare("-vulkan", Qt::CaseInsensitive) != 0) {
                processArgs.append(arg);
            }
        }
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

HammerService::ReloadResult HammerService::reloadDictionaries(const std::wstring& /*cs2Root*/,
                                                              const std::wstring& /*workingDir*/,
                                                              bool /*useMachineTrans*/) {
    ReloadResult result;

    // 纯内存模式：无需向 CS2 目录写入任何 JSON 词典或 FGD 文件
    // 注入模块收到 IPC 消息后，会自动从启动器程序目录直读最新词典，
    // 在内存中重载并预编译最新 FGD，最后触发 Hammer 原生 Reload .FGD Files 并重绘所有窗口
    result.fgdOk = true;
    result.fgdFileCount = 0;

    // 发送 IPC 消息给 Hammer 注入模块
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
