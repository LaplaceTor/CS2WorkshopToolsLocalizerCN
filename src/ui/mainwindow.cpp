#include "ui/mainwindow.h"
#include "core/cs2_detector.h"
#include "core/path_constants.h"
#include "core/dictionary_paths.h"
#include "core/launcher_config.h"
#include "service/localization_service.h"
#include "core/hammer_ipc.h"
#include "core/process_monitor.h"
#include "core/fgd_translator.h"
#include "core/pe_patcher.h"
#include "core/backup_manager.h"
#include "core/dictionary_compiler.h"
#include "ui/debug_window.h"

#include <windows.h>
#include <psapi.h>
#include <thread>
#include <fstream>
#include <QFileSystemWatcher>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QStatusBar>
#include <QMessageBox>
#include <QDateTime>
#include <QCloseEvent>
#include <QApplication>
#include <QThread>
#include <QTimer>
#include <QFileInfo>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QFile>
#include <QSaveFile>
#include <QCheckBox>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QEventLoop>
#include <memory>
#include <filesystem>

namespace fs = std::filesystem;


MainWindow::MainWindow(const std::wstring& cs2Root, QWidget *parent)
    // 注意：初始化顺序必须与 mainwindow.h 中的声明顺序一致，
    // 否则 MSVC 会报 C5038（-Wreorder）。调整成员声明位置时请同步改这里。
    : QMainWindow(parent)
    , m_cs2Root(cs2Root)
    , m_useMachineTransCheck(nullptr)
    , m_toggleLangBtn(nullptr)
    , m_hotReloadBtn(nullptr)
    , m_debugBtn(nullptr)
    , m_fileWatcher(nullptr)
    , m_hotReloadDebounceTimer(nullptr)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_hammerProcess(new QProcess(this))
    , m_monitorTimer(new QTimer(this))
        , m_isHammerRunning(false)
    , m_hammerPid(0)
    , m_hammerProcessHandle(nullptr)
{
    // 获取程序所在目录作为工作目录
    QString appDir = QApplication::applicationDirPath();

    if (!appDir.isEmpty()) {
        m_workingDir = appDir.toStdWString();
    } else {
        try {
            m_workingDir = fs::current_path().wstring();
        } catch (...) {
            wchar_t curDir[MAX_PATH] = {0};
            GetCurrentDirectoryW(MAX_PATH, curDir);
            m_workingDir = curDir;
        }
    }

    setupUi();
    populateAddons();
    loadSettings();

    connect(m_injectBtn, &QPushButton::clicked,
            this, &MainWindow::onInjectClicked);

    connect(m_launchBtn, &QPushButton::clicked,
            this, &MainWindow::onLaunchClicked);

    connect(m_updateBtn, &QPushButton::clicked,
            this, &MainWindow::onUpdateTranslationsClicked);

    connect(m_restoreBtn, &QPushButton::clicked,
            this, &MainWindow::onRestoreClicked);

    connect(m_helpBtn, &QPushButton::clicked,
            this, &MainWindow::onHelpClicked);

    if (m_toggleLangBtn) {
        connect(m_toggleLangBtn, &QPushButton::clicked,
                this, &MainWindow::onToggleLangClicked);
    }

    if (m_hotReloadBtn) {
        connect(m_hotReloadBtn, &QPushButton::clicked,
                this, &MainWindow::onHotReloadClicked);
    }

    if (m_debugBtn) {
        connect(m_debugBtn, &QPushButton::clicked,
                this, &MainWindow::onDebugClicked);
    }

    connect(
        m_addonCombo,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::saveSettings
    );

    connect(
        m_argsEdit,
        &QLineEdit::textChanged,
        this,
        &MainWindow::saveSettings
    );

    if (m_useMachineTransCheck) {
        connect(
            m_useMachineTransCheck,
            &QCheckBox::toggled,
            this,
            [this](bool) {
                saveSettings();
                writeAppDirPointer();
                if (m_isHammerRunning) {
                    performHotReload(false);
                }
            }
        );
    }

    connect(
        m_hammerProcess,
        &QProcess::started,
        this,
        &MainWindow::onHammerStarted
    );

    connect(
        m_hammerProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        &MainWindow::onHammerFinished
    );

    connect(
        m_hammerProcess,
        &QProcess::errorOccurred,
        this,
        &MainWindow::onHammerError
    );

    connect(
        m_monitorTimer,
        &QTimer::timeout,
        this,
        &MainWindow::onCheckProcessState
    );

    appendLog(
        "==================================================",
        "#66d9ef"
    );

    appendLog(
        " CS2 Workshop Tools Localizer CN 汉化启动器已就绪",
        "#a6e22e"
    );

    appendLog(
        "==================================================",
        "#66d9ef"
    );

    appendLog(
        QString("已锁定 CS2 安装目录: %1")
            .arg(QString::fromStdWString(m_cs2Root)),
        "#f8f8f2"
    );

    // 检查并生成翻译字典文件
    fs::path fgdPath = ResolveDictionaryPath(m_workingDir, L"fgd_translations.jsonc");
    fs::path fgdOverridePath = ResolveDictionaryPath(m_workingDir, L"fgd_override.jsonc");
    fs::path qtPath = ResolveDictionaryPath(m_workingDir, L"qt_translations.jsonc");

    std::wstring notice;

    if (FgdTranslator::EnsureFgdDictionaryExists(
            fgdPath.wstring(),
            L"",
            notice)) {

        appendLog(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    if (FgdTranslator::EnsureFgdOverrideDictionaryExists(
            fgdOverridePath.wstring(),
            L"",
            notice)) {

        appendLog(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    if (FgdTranslator::EnsureQtDictionaryExists(
            qtPath.wstring(),
            L"",
            notice)) {

        appendLog(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    // 检查上一次是否异常退出并执行安全恢复
    checkAndRecoverAbnormalExit();

    // 写入程序目录指针供注入模块直读，并挂载词典文件自动热重载监听
    writeAppDirPointer();
    setupFileWatcher();

    // 根据恢复后的实际状态刷新按钮
    updateActionButtonState();
}

MainWindow::~MainWindow() {
    if (m_hammerProcessHandle != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(m_hammerProcessHandle)
        );

        m_hammerProcessHandle = nullptr;
    }
}

void MainWindow::setupUi() {
    setWindowTitle(
        "CS2 Workshop Tools 汉化启动器 - CS2 Workshop Tools Localizer CN"
    );

    resize(480, 450);
    setMinimumSize(480, 450);

    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout =
        new QVBoxLayout(centralWidget);

    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // 1. 顶部 CS2 路径信息卡片
    QGroupBox* pathGroup =
        new QGroupBox("CS2 路径信息", centralWidget);

    QHBoxLayout* pathLayout =
        new QHBoxLayout(pathGroup);

    pathLayout->setContentsMargins(8, 6, 8, 6);

    m_cs2PathLabel =
        new QLabel(
            QString::fromStdWString(m_cs2Root),
            pathGroup
        );

    m_cs2PathLabel->setStyleSheet(
        "font-weight: bold; "
        "color: #4ec9b0; "
        "font-size: 12px;"
    );

    m_cs2PathLabel->setTextInteractionFlags(
        Qt::TextSelectableByMouse
    );

    pathLayout->addWidget(m_cs2PathLabel);

    mainLayout->addWidget(pathGroup);

    // 2. 参数与 Addon 选择卡片
    QGroupBox* configGroup =
        new QGroupBox("启动配置", centralWidget);

    QGridLayout* configLayout =
        new QGridLayout(configGroup);

    configLayout->setContentsMargins(8, 6, 8, 6);
    configLayout->setHorizontalSpacing(8);
    configLayout->setVerticalSpacing(6);

    QLabel* addonLabel =
        new QLabel(
            "目标 Addon 模组:",
            configGroup
        );

    m_addonCombo =
        new QComboBox(configGroup);

    m_addonCombo->setMinimumHeight(28);

    configLayout->addWidget(
        addonLabel,
        0,
        0
    );

    configLayout->addWidget(
        m_addonCombo,
        0,
        1
    );

    QLabel* argsLabel =
        new QLabel(
            "附加启动参数:",
            configGroup
        );

    m_argsEdit =
        new QLineEdit(configGroup);

    m_argsEdit->setMinimumHeight(28);

    m_argsEdit->setPlaceholderText(
        "例如: -gpuraytracing"
    );

    configLayout->addWidget(
        argsLabel,
        1,
        0
    );

    configLayout->addWidget(
        m_argsEdit,
        1,
        1
    );

    m_useMachineTransCheck =
        new QCheckBox("使用机翻", configGroup);

    m_useMachineTransCheck->setChecked(true);

    m_useMachineTransCheck->setToolTip(
        "勾选后将自动引入 fgd_fallback.jsonc 与 qt_fallback.jsonc 作为兜底词典。\n"
        "未人工精翻的词条将自动显示机翻结果，已精翻词条保持最高优先级覆盖。"
    );

    configLayout->addWidget(
        m_useMachineTransCheck,
        2,
        0,
        1,
        2
    );

    mainLayout->addWidget(configGroup);

    // 3. 操作按钮栏
    QVBoxLayout* btnLayout =
        new QVBoxLayout();

    btnLayout->setSpacing(6);

    // 第一行：仅注入 / 启动 HAMMER / 还原
    QHBoxLayout* mainActionLayout =
        new QHBoxLayout();

    mainActionLayout->setSpacing(6);

    // 仅注入
    m_injectBtn =
        new QPushButton(
            "仅注入",
            centralWidget
        );

    m_injectBtn->setMinimumHeight(36);

    m_injectBtn->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Preferred
    );

    m_injectBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #8957e5;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 5px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #a371f7;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #6e40b8;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    // 启动 HAMMER
    m_launchBtn =
        new QPushButton(
            "启动 HAMMER 汉化版",
            centralWidget
        );

    m_launchBtn->setMinimumHeight(36);

    m_launchBtn->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Preferred
    );

    m_launchBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #2ea043;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 5px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #3fb950;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #238636;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    // 还原
    m_restoreBtn =
        new QPushButton(
            "还原",
            centralWidget
        );

    m_restoreBtn->setMinimumHeight(36);

    m_restoreBtn->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Preferred
    );

    m_restoreBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #444c56;"
        "  color: #adbac7;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 5px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #545d68;"
        "  color: white;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #373e47;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    mainActionLayout->addWidget(m_injectBtn);
    mainActionLayout->addWidget(m_launchBtn);
    mainActionLayout->addWidget(m_restoreBtn);

    btnLayout->addLayout(mainActionLayout);

    // 第二行：在线更新 / 字典指南
    QHBoxLayout* subBtnLayout =
        new QHBoxLayout();

    subBtnLayout->setSpacing(6);

    m_updateBtn =
        new QPushButton(
            "🌐 更新在线翻译",
            centralWidget
        );

    m_updateBtn->setMinimumHeight(28);

    m_updateBtn->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Preferred
    );

    m_updateBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #0969da;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #218bff;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #0550ae;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    m_helpBtn =
        new QPushButton(
            "📖 字典指南",
            centralWidget
        );

    m_helpBtn->setMinimumHeight(28);

    m_helpBtn->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Preferred
    );

    m_helpBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #1f6feb;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #388bfd;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #1158c7;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    m_debugBtn = new QPushButton("🐞 调试监控", centralWidget);
    m_debugBtn->setMinimumHeight(28);
    m_debugBtn->setToolTip("打开专用调试监控窗口，查看实时日志流、内存诊断、崩溃事件与快捷控制");
    m_debugBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #21262d;"
        "  color: #58a6ff;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 4px;"
        "  border: 1px solid #30363d;"
        "}"
        "QPushButton:hover {"
        "  background-color: #30363d;"
        "  border-color: #58a6ff;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #161b22;"
        "}"
    );

    subBtnLayout->addWidget(m_updateBtn);
    subBtnLayout->addWidget(m_helpBtn);
    subBtnLayout->addWidget(m_debugBtn);

    btnLayout->addLayout(subBtnLayout);

    // 第三行：运行中实时联动控制（一键切换中英 / 免重启热重载）
    QHBoxLayout* liveControlLayout = new QHBoxLayout();
    liveControlLayout->setSpacing(6);

    m_toggleLangBtn = new QPushButton("🔀 切换原文 / 翻译", centralWidget);
    m_toggleLangBtn->setMinimumHeight(28);
    m_toggleLangBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toggleLangBtn->setEnabled(false);
    m_toggleLangBtn->setToolTip("需在 Hammer 运行中时使用（一键切换原文/翻译）");
    m_toggleLangBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #d29922;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #e3b341;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #bb8009;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    m_hotReloadBtn = new QPushButton("⚡ 热重载词典", centralWidget);
    m_hotReloadBtn->setMinimumHeight(28);
    m_hotReloadBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_hotReloadBtn->setEnabled(false);
    m_hotReloadBtn->setToolTip("需在 Hammer 运行中时使用（重新从磁盘读取词典，无需重启 Hammer）");
    m_hotReloadBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #8957e5;"
        "  color: white;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #a371f7;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #6e40b8;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #2d333b;"
        "  color: #636e7b;"
        "}"
    );

    liveControlLayout->addWidget(m_toggleLangBtn);
    liveControlLayout->addWidget(m_hotReloadBtn);

    btnLayout->addLayout(liveControlLayout);

    mainLayout->addLayout(btnLayout);

    // 4. 实时日志视窗
    QGroupBox* logGroup =
        new QGroupBox(
            "执行日志与状态",
            centralWidget
        );

    QVBoxLayout* logLayout =
        new QVBoxLayout(logGroup);

    logLayout->setContentsMargins(
        6,
        6,
        6,
        6
    );

    m_logEdit =
        new QTextEdit(logGroup);

    m_logEdit->setReadOnly(true);

    m_logEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  font-family: 'Consolas', 'Courier New', monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #333333;"
        "  border-radius: 4px;"
        "}"
    );

    logLayout->addWidget(m_logEdit);

    mainLayout->addWidget(
        logGroup,
        1
    );

    // 5. 底部状态栏
    m_statusLabel =
        new QLabel(
            "状态: 就绪",
            this
        );

    m_statusLabel->setStyleSheet(
        "color: #8b949e;"
        "padding-left: 4px;"
        "font-size: 11px;"
    );

    statusBar()->addWidget(
        m_statusLabel
    );
}

void MainWindow::populateAddons() {
    m_addonCombo->clear();

    std::vector<std::wstring> addons =
        Cs2Detector::GetAvailableAddons(
            m_cs2Root
        );

    if (addons.empty()) {
        m_addonCombo->addItem(
            "addon_template (默认模组)"
        );
    } else {
        for (const auto& addon : addons) {
            m_addonCombo->addItem(
                QString::fromStdWString(addon)
            );
        }
    }
}

void MainWindow::loadSettings() {
    const LauncherSettings saved = LauncherConfig::Load(m_workingDir);

    // 恢复附加启动参数
    m_argsEdit->setText(saved.launchArgs);

    // 恢复使用机翻设置
    if (m_useMachineTransCheck) {
        m_useMachineTransCheck->setChecked(saved.useMachineTrans);
    }

    // 恢复选择的目标 Addon：先精确匹配，再退化为「名称 + 空格」前缀匹配
    int index = -1;
    if (!saved.selectedAddon.isEmpty()) {
        index = m_addonCombo->findText(saved.selectedAddon);
        if (index == -1) {
            const QString prefix = saved.selectedAddon + " ";
            for (int i = 0; i < m_addonCombo->count(); ++i) {
                const QString itemText = m_addonCombo->itemText(i);
                if (itemText == saved.selectedAddon || itemText.startsWith(prefix)) {
                    index = i;
                    break;
                }
            }
        }
    }

    if (index != -1) {
        m_addonCombo->setCurrentIndex(index);
    } else if (m_addonCombo->count() > 0) {
        m_addonCombo->setCurrentIndex(0);
    }
}

void MainWindow::saveSettings() {
    LauncherSettings settings;
    settings.selectedAddon   = LauncherConfig::NormalizeAddonName(m_addonCombo->currentText());
    settings.launchArgs      = m_argsEdit->text();
    settings.useMachineTrans = m_useMachineTransCheck ? m_useMachineTransCheck->isChecked() : true;

    LauncherConfig::Save(m_workingDir, settings);
}

void MainWindow::appendLog(
    const QString& msg,
    const QString& color
) {
    if (QThread::currentThread() != this->thread()) {
        // worker 线程调用：封送至 UI 线程执行（若窗口已销毁则自动丢弃）
        QMetaObject::invokeMethod(
            this,
            [this, msg, color]() {
                appendLog(msg, color);
            },
            Qt::QueuedConnection
        );
        return;
    }

    QString timeStr =
        QDateTime::currentDateTime()
            .toString("HH:mm:ss");

    QString formattedMsg =
        QString(
            "<span style='color: #6a9955;'>[%1]</span> "
            "<span style='color: %2;'>%3</span>"
        )
            .arg(
                timeStr,
                color,
                msg.toHtmlEscaped()
            );

    m_logEdit->append(formattedMsg);
}

bool MainWindow::runHeavyInWorker(const std::function<bool()>& task) {
    if (m_workerBusy) {
        appendLog(
            "[-] 已有后台操作正在进行，已忽略并发请求",
            "#f92672"
        );
        return false;
    }

    m_workerBusy = true;

    // QtConcurrent + QFutureWatcher + QEventLoop：
    // 任务在全局线程池执行，UI 线程事件循环保持响应，完成后再取回结果
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    QObject::connect(
        &watcher,
        &QFutureWatcher<bool>::finished,
        &loop,
        &QEventLoop::quit
    );

    watcher.setFuture(QtConcurrent::run(task));
    loop.exec();

    m_workerBusy = false;

    return watcher.future().result();
}

void MainWindow::setUiBusy(bool busy) {
    m_updateBtn->setEnabled(!busy);
    m_addonCombo->setEnabled(!busy);
    m_argsEdit->setEnabled(!busy);

    if (busy) {
        m_injectBtn->setEnabled(false);
        m_launchBtn->setEnabled(false);
        m_restoreBtn->setEnabled(false);
    } else {
        updateActionButtonState();
    }
}

bool MainWindow::isPatchDeployedAndValid() {
    fs::path backupDir =
        fs::path(m_workingDir) / paths::kBackupDir;

    // 没有备份，不认为当前处于有效的已注入状态
    if (!BackupManager::HasBackup(
            backupDir.wstring())) {

        return false;
    }

    // 当前目录是否存在实际补丁文件
    bool patchFilesPresent =
        BackupManager::IsPatchDeployed(
            m_cs2Root
        );

    // session_state 是否标记为已注入
    bool sessionPatched =
        BackupManager::HasUnrestoredSession(
            m_workingDir
        );

    /*
     * 正常状态：
     *
     * session_state.json:
     *   "is_patched": true
     *
     * 并且 CS2 目录中存在实际补丁文件。
     *
     * 对于异常关闭后残留的情况，
     * 即使 session_state 被破坏，只要补丁文件仍存在，
     * 也继续认为当前目录处于注入状态。
     */
    if (!sessionPatched && !patchFilesPresent) {
        return false;
    }

    /*
     * BackupMatchesCurrentGame 内含 4 次全文件 SHA256（cs2.exe / Qt5Core / 备份 Qt5Core），
     * 属于重 IO：这里仅做轻量判断，重量级校验放后台异步执行并缓存结果（15 秒有效期），
     * 校验完成后再刷新一次按钮状态。校验未到达期间沿用缓存值（初始为 false，保守视为未注入）。
     */
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_validationPending &&
        (m_lastValidationMs == 0 || nowMs - m_lastValidationMs > 15000)) {

        m_validationPending = true;

        std::wstring cs2Root = m_cs2Root;
        std::wstring workingDir = m_workingDir;

        auto* watcher = new QFutureWatcher<bool>(this);
        QObject::connect(
            watcher,
            &QFutureWatcher<bool>::finished,
            this,
            [this, watcher]() {
                m_cachedValidationValid = watcher->result();
                m_validationPending = false;
                m_lastValidationMs = QDateTime::currentMSecsSinceEpoch();
                watcher->deleteLater();

                // 校验结果到达后刷新按钮状态
                updateActionButtonState();
            }
        );

        watcher->setFuture(
            QtConcurrent::run([cs2Root, workingDir]() -> bool {
                auto validation = BackupManager::BackupMatchesCurrentGame(
                    cs2Root,
                    (fs::path(workingDir) / paths::kBackupDir).wstring());
                return validation.status == BackupMatchStatus::Matches;
            })
        );
    }

    return (sessionPatched || patchFilesPresent) && m_cachedValidationValid;
}

void MainWindow::updateActionButtonState() {
    if (m_toggleLangBtn) {
        m_toggleLangBtn->setEnabled(m_isHammerRunning);
        m_toggleLangBtn->setToolTip(m_isHammerRunning ?
            "一键向运行中的 Hammer 发送【切换原文 / 翻译】指令" :
            "需在 Hammer 运行中时使用（一键切换原文/翻译）");
    }
    if (m_hotReloadBtn) {
        m_hotReloadBtn->setEnabled(m_isHammerRunning);
        m_hotReloadBtn->setToolTip(m_isHammerRunning ?
            "重新从磁盘读取翻译词典并即时生效，无需重启 Hammer" :
            "需在 Hammer 运行中时使用（免重启热重载词典）");
    }

    if (m_isHammerRunning) {
        // HAMMER 运行中：三个核心按钮全部禁用
        m_injectBtn->setEnabled(false);
        m_launchBtn->setEnabled(false);
        m_restoreBtn->setEnabled(false);

        m_injectBtn->setToolTip(
            "Hammer 正在运行中，无法注入"
        );

        m_launchBtn->setToolTip(
            "Hammer 已经在运行中"
        );

        m_restoreBtn->setToolTip(
            "Hammer 正在运行中，无法还原"
        );

        return;
    }

    bool injected =
        isPatchDeployedAndValid();

    // 已注入 -> 禁止再次注入
    m_injectBtn->setEnabled(!injected);

    if (injected) {
        m_injectBtn->setToolTip(
            "当前已注入，无需重复注入"
        );
    } else {
        m_injectBtn->setToolTip(
            "仅部署汉化补丁，不启动 HAMMER"
        );
    }

    // 启动 HAMMER：
    // 无论是否已经注入都允许点击
    m_launchBtn->setEnabled(true);

    if (injected) {
        m_launchBtn->setToolTip(
            "当前已注入，将跳过注入并直接启动 HAMMER"
        );
    } else {
        m_launchBtn->setToolTip(
            "当前未注入，启动 HAMMER 前会自动完成注入"
        );
    }

    // 未注入 -> 禁止还原
    m_restoreBtn->setEnabled(injected);

    if (injected) {
        m_restoreBtn->setToolTip(
            "还原当前汉化补丁，恢复 backup 中的原版文件"
        );
    } else {
        m_restoreBtn->setToolTip(
            "当前未注入，无需还原"
        );
    }
}

void MainWindow::updateRestoreButtonState() {
    updateActionButtonState();
}

bool MainWindow::injectLocalization() {
    // 在 UI 线程读取控件状态，随后转入后台线程执行核心流程（核心流程禁止访问任何 UI 控件）
    bool useMachineTrans =
        (m_useMachineTransCheck != nullptr) ? m_useMachineTransCheck->isChecked() : true;

    return runHeavyInWorker([this, useMachineTrans]() {
        return injectLocalizationCore(useMachineTrans);
    });
}

bool MainWindow::injectLocalizationCore(bool useMachineTrans) {
    const LocalizationService::Context ctx{m_cs2Root, m_workingDir};
    return LocalizationService::Inject(
        ctx,
        useMachineTrans,
        [this](const QString& msg, const QString& color) { appendLog(msg, color); });
}

void MainWindow::onInjectClicked() {
    if (m_isHammerRunning) {
        QMessageBox::warning(
            this,
            "警告",
            "Hammer 正在运行中，不能进行注入！"
        );
        return;
    }

    if (m_workerBusy) {
        QMessageBox::information(
            this,
            "提示",
            "后台操作正在进行中，请稍候再试。"
        );
        return;
    }

    if (Cs2Detector::IsCs2ProcessRunning()) {
        QMessageBox::warning(
            this,
            "警告",
            "检测到 CS2 进程正在运行。\n\n"
            "请先退出 CS2 / Hammer 后再执行注入。"
        );
        return;
    }

    // 二次状态校验，避免状态变化后重复注入
    if (isPatchDeployedAndValid()) {
        QMessageBox::information(
            this,
            "提示",
            "当前已经是“已注入”状态，无需重复注入。"
        );

        updateActionButtonState();
        return;
    }

    saveSettings();

    setUiBusy(true);

    m_statusLabel->setText(
        "状态: 正在注入汉化补丁..."
    );

    appendLog(
        "==================================================",
        "#66d9ef"
    );

    appendLog(
        "[*] 开始执行“仅注入”操作，不启动 HAMMER。",
        "#66d9ef"
    );

    bool ok =
        injectLocalization();

    setUiBusy(false);

    if (ok) {
        m_statusLabel->setText(
            "状态: 已注入，等待启动 HAMMER"
        );

        QMessageBox::information(
            this,
            "注入成功",
            "汉化补丁已经成功注入。\n\n"
            "当前 CS2 仍处于“已注入 / HAMMER 未启动”状态。\n"
            "现在可以点击“启动 HAMMER”，启动时将跳过重复注入。"
        );
    } else {
        m_statusLabel->setText(
            "状态: 注入失败"
        );

        QMessageBox::critical(
            this,
            "错误",
            "汉化补丁注入失败，已自动回滚，请查看执行日志了解详细原因。"
        );
    }

    updateActionButtonState();
}

bool MainWindow::startHammerProcess() {
    fs::path cs2Bin = paths::Win64Bin(m_cs2Root);

    QString selectedAddon =
        m_addonCombo->currentText().trimmed();

    if (selectedAddon.contains(" ")) {
        selectedAddon =
            selectedAddon.split(" ").first();
    }

    if (selectedAddon.isEmpty()) {
        selectedAddon = "addon_template";
    }

    QString cs2ExePath =
        QString::fromStdWString(
            (cs2Bin / paths::kCs2Exe).wstring()
        );

    if (!QFileInfo::exists(cs2ExePath)) {
        QMessageBox::critical(
            this,
            "启动错误",
            "找不到 cs2.exe：\n" +
            cs2ExePath
        );

        return false;
    }

    QStringList processArgs;

    processArgs
        << "-addon"
        << selectedAddon
        << "-tools";

    QString customArgs =
        m_argsEdit->text().trimmed();

    if (!customArgs.isEmpty()) {
        QStringList userTokens =
            QProcess::splitCommand(
                customArgs
            );

        processArgs.append(
            userTokens
        );
    }

    m_hammerProcess->setProgram(
        cs2ExePath
    );

    m_hammerProcess->setArguments(
        processArgs
    );

    m_hammerProcess->setWorkingDirectory(
        QString::fromStdWString(
            cs2Bin.wstring()
        )
    );

    m_hammerProcess->setProcessChannelMode(
        QProcess::ForwardedChannels
    );

    appendLog(
        QString(
            "[*] 执行 HAMMER 命令: %1 %2"
        )
            .arg(
                cs2ExePath,
                processArgs.join(" ")
            ),
        "#75715e"
    );

    // 保持 session_state 为已注入
    if (!BackupManager::SaveSessionState(
            m_workingDir,
            true
        )) {
        appendLog(
            "[-] 会话状态写入失败 (session_state.json)，异常退出后的自动恢复可能失效",
            "#f92672"
        );
    }

    m_isHammerRunning = true;
    m_processMonitor.Reset();

    m_statusLabel->setText(
        "状态: Hammer 编辑器正在启动..."
    );

    m_launchBtn->setText(
        "HAMMER 运行中..."
    );

    m_injectBtn->setEnabled(false);
    m_launchBtn->setEnabled(false);
    m_restoreBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);

    m_hammerProcess->start();

    m_monitorTimer->start(1000);

    return true;
}

void MainWindow::onLaunchClicked() {
    if (m_isHammerRunning) {
        QMessageBox::information(
            this,
            "提示",
            "Hammer 已经在运行中，请勿重复启动！"
        );

        return;
    }

    if (m_workerBusy) {
        QMessageBox::information(
            this,
            "提示",
            "后台操作正在进行中，请稍候再试。"
        );

        return;
    }

    if (Cs2Detector::IsCs2ProcessRunning()) {
        QMessageBox::warning(
            this,
            "提示",
            "检测到系统中已有 CS2 进程正在运行！\n\n"
            "为防止文件冲突与补丁写入受阻，请先退出当前运行的 CS2 游戏或编辑器，然后再启动 HAMMER。"
        );

        return;
    }

    saveSettings();

    bool injected =
        isPatchDeployedAndValid();

    setUiBusy(true);

    if (!injected) {
        appendLog(
            "[*] 当前状态为“未注入”，启动 HAMMER 前自动执行注入。",
            "#66d9ef"
        );

        m_statusLabel->setText(
            "状态: 正在注入汉化补丁..."
        );

        if (!injectLocalization()) {
            setUiBusy(false);
            updateActionButtonState();
            return;
        }

        appendLog(
            "[+] 自动注入完成，准备启动 HAMMER。",
            "#a6e22e"
        );
    } else {
        appendLog(
            "[+] 检测到当前已经处于“已注入”状态。",
            "#a6e22e"
        );

        appendLog(
            "[+] 跳过重复注入，直接启动 HAMMER。",
            "#66d9ef"
        );
    }

    m_statusLabel->setText(
        "状态: 正在启动 HAMMER..."
    );

    if (!startHammerProcess()) {
        m_isHammerRunning = false;

        setUiBusy(false);

        updateActionButtonState();

        return;
    }
}

void MainWindow::onHammerStarted() {
    m_isHammerRunning = true;
    m_processMonitor.Reset();

    m_hammerPid =
        m_hammerProcess->processId();

    if (m_hammerProcessHandle != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                m_hammerProcessHandle
            )
        );

        m_hammerProcessHandle = nullptr;
    }

    if (m_hammerPid > 0) {
        m_hammerProcessHandle =
            OpenProcess(
                SYNCHRONIZE |
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                static_cast<DWORD>(
                    m_hammerPid
                )
            );
    }

    m_statusLabel->setText(
        "状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)"
    );

    appendLog(
        QString(
            "[+] CS2 Hammer 进程已成功启动 (PID: %1)！正在监听运行生命周期..."
        )
            .arg(m_hammerPid),
        "#a6e22e"
    );

    updateActionButtonState();
}

void MainWindow::onHammerFinished(
    int exitCode,
    QProcess::ExitStatus exitStatus
) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    handleHammerProcessTerminated();
}

void MainWindow::onCheckProcessState() {
    if (!m_isHammerRunning) {
        m_monitorTimer->stop();
        return;
    }

    // 三路探测来源按优先级由 ProcessMonitor 判定（QProcess → 原生句柄 → PID）
    ProcessMonitor::Source src;
    src.qProcessRunning = (m_hammerProcess != nullptr) &&
                          (m_hammerProcess->state() == QProcess::Running);
    src.nativeHandle    = m_hammerProcessHandle;
    src.pid             = static_cast<unsigned long>(m_hammerPid);

    if (m_processMonitor.Sample(src)) {
        m_statusLabel->setText(
            "状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)"
        );
    } else if (m_processMonitor.ReachedTerminationThreshold()) {
        handleHammerProcessTerminated();
    }
}

void MainWindow::handleHammerProcessTerminated() {
    if (!m_isHammerRunning) {
        return;
    }

    m_monitorTimer->stop();

    m_isHammerRunning = false;

    if (m_hammerProcessHandle != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                m_hammerProcessHandle
            )
        );

        m_hammerProcessHandle = nullptr;
    }

    m_hammerPid = 0;

    appendLog(
        "[*] 检测到本程序启动的 CS2 与 Hammer 已退出，正在执行自动安全还原...",
        "#66d9ef"
    );

    m_statusLabel->setText(
        "状态: 正在恢复原版文件..."
    );

    bool restoreOk =
        runHeavyInWorker(
            [this]() {
                return doRestore(true);
            }
        );

    if (restoreOk) {
        if (!BackupManager::ClearSessionState(
                m_workingDir
            )) {
            appendLog(
                "[-] 清除会话状态失败 (session_state.json)，下次启动可能重复执行自动恢复",
                "#f92672"
            );
        }

        m_statusLabel->setText(
            "状态: 就绪（未注入）"
        );

    } else {

        m_statusLabel->setText(
            "状态: 恢复备份遇到占用，请手动点击【还原】"
        );

        appendLog(
            "[-] 部分原版文件还原失败（可能仍被其他程序占用），未清除会话状态以保护原版备份。",
            "#f92672"
        );

        QMessageBox::warning(
            this,
            "恢复提示",
            "自动恢复原版文件时遇到部分文件被占用或写入受阻！\n\n"
            "请确认 CS2 与 Hammer 是否已完全退出，然后可手动点击【还原】。\n"
            "（启动器已安全保留会话状态与原版备份，下次启动也会自动再次尝试恢复）。"
        );
    }

    m_launchBtn->setText(
        "启动 HAMMER"
    );

    setUiBusy(false);

    updateActionButtonState();
}

void MainWindow::onHammerError(
    QProcess::ProcessError error
) {
    if (error == QProcess::FailedToStart) {
        // startHammerProcess 在 start() 前已置 m_isHammerRunning = true，
        // 而 FailedToStart 只触发 errorOccurred、不触发 finished，
        // 必须在此立即回滚，否则只能依赖 1 秒轮询兜底
        m_isHammerRunning = false;
        m_monitorTimer->stop();
        m_hammerPid = 0;

        appendLog(
            QString(
                "[-] 无法启动 Hammer 进程 (cs2.exe)，错误码: %1"
            )
                .arg(error),
            "#f92672"
        );

        QMessageBox::critical(
            this,
            "启动错误",
            "无法启动 cs2.exe 进程，请检查 CS2 路径与游戏完整性。"
        );

        bool restored =
            runHeavyInWorker(
                [this]() {
                    return doRestore(true);
                }
            );

        if (restored) {
            if (!BackupManager::ClearSessionState(
                    m_workingDir
                )) {
                appendLog(
                    "[-] 清除会话状态失败 (session_state.json)，下次启动可能重复执行自动恢复",
                    "#f92672"
                );
            }
        }

        setUiBusy(false);

        m_launchBtn->setText(
            "启动 HAMMER"
        );

        m_statusLabel->setText(
            "状态: 启动出错"
        );

        updateActionButtonState();
        return;
    }

    // 其他错误 (Crashed / TimedError 等)：交由 finished / 监控定时器路径统一处理，避免双重恢复
}

bool MainWindow::doRestore(bool showLog) {
    const LocalizationService::Context ctx{m_cs2Root, m_workingDir};
    return LocalizationService::Restore(
        ctx,
        showLog,
        [this](const QString& msg, const QString& color) { appendLog(msg, color); });
}

void MainWindow::fetchUrlCandidates(
    const QStringList& urls,
    std::function<void(bool success, const QByteArray& data)> callback
) {
    if (urls.isEmpty()) {
        callback(
            false,
            QByteArray()
        );

        return;
    }

    auto fetchNext =
        std::make_shared<std::function<void(int)>>();

    *fetchNext =
        [this, urls, callback, fetchNext](int index) {

        if (index >= urls.size()) {
            callback(
                false,
                QByteArray()
            );

            return;
        }

        QUrl url(
            urls[index]
        );

        QNetworkRequest request(url);

        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy
        );

        request.setHeader(
            QNetworkRequest::UserAgentHeader,
            "CS2WorkshopToolsLocalizerCN"
        );

        request.setTransferTimeout(
            10000
        );

        QNetworkReply* reply =
            m_networkManager->get(
                request
            );

        connect(
            reply,
            &QNetworkReply::finished,
            this,
            [reply, index, callback, fetchNext]() {

                reply->deleteLater();

                int statusCode =
                    reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute
                    ).toInt();

                if (
                    reply->error() ==
                        QNetworkReply::NoError &&
                    statusCode == 200
                ) {

                    QByteArray data =
                        reply->readAll();

                    if (!data.isEmpty()) {
                        callback(
                            true,
                            data
                        );

                        return;
                    }
                }

                (*fetchNext)(index + 1);
            }
        );
    };

    (*fetchNext)(0);
}

void MainWindow::reportDictionaryFetchFailure(const QString& dictName) {
    appendLog(
        QString("[-] 获取 %1 失败：%2").arg(dictName,"所有节点连接超时或不可达，请检查网络或代理设置。"),
        "#f92672"
    );

    QMessageBox::critical(
        this,
        "更新失败",
        QString("获取 %1 失败！\n%2").arg(dictName,"无法连接到 GitHub 仓库，请检查您的网络连接或代理设置。")
    );

    setUiBusy(false);
    m_statusLabel->setText("状态: 词典更新失败");
}

void MainWindow::reportDictionaryParseFailure(
    const QString& dictName,
    const QString& parseErrorText
) {
    appendLog(
        QString("[-] 解析 %1 失败: %2").arg(dictName, parseErrorText),
        "#f92672"
    );

    QMessageBox::critical(
        this,
        "更新失败",
        QString("下载的 %1 格式异常或内容为空，已放弃更新。").arg(dictName)
    );

    setUiBusy(false);
    m_statusLabel->setText("状态: 词典校验失败");
}

// 剥离 JSONC 注释，使其可被 QJsonDocument 解析（含尾随逗号移除）
static QByteArray StripJsonc(const QByteArray& input) {
    const std::string stripped =
        DictionaryCompiler::StripJsonComments(input.constData(), (size_t)input.size());
    return QByteArray(stripped.data(), (qsizetype)stripped.size());
}

// 统计 fgd_override 词典的有效规则数：
// properties / io / classes 三个子对象的键数之和，再加上其余非下划线开头的顶层键；
// 三者合计为 0 时退化为顶层键总数。
static qsizetype CountOverrideRules(const QJsonObject& obj) {
    qsizetype count = 0;

    for (const char* section : {"properties", "io", "classes"}) {
        if (obj.contains(section) && obj[section].isObject()) {
            count += obj[section].toObject().keys().size();
        }
    }

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() != "properties" && it.key() != "io" && it.key() != "classes" &&
            !it.key().startsWith("_")) {
            ++count;
        }
    }

    return (count == 0) ? obj.keys().size() : count;
}

void MainWindow::fetchOnlineDictionary(
    const QStringList& urls,
    const QString& stepLabel,
    const QString& dictName,
    const QString& desc,
    std::function<qsizetype(const QJsonDocument&)> countOf,
    std::function<void(const OnlineDictionary&)> onSuccess
) {
    appendLog(
        QString("%1 正在获取 %2 (%3)...").arg(stepLabel, dictName, desc),
        "#e6db74"
    );

    fetchUrlCandidates(
        urls,
        [this, dictName, countOf, onSuccess](bool ok, const QByteArray& data) {
            if (!ok) {
                reportDictionaryFetchFailure(dictName);
                return;
            }

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(StripJsonc(data), &parseErr);
            if (parseErr.error != QJsonParseError::NoError ||
                !doc.isObject() || doc.object().isEmpty()) {
                reportDictionaryParseFailure(dictName, parseErr.errorString());
                return;
            }

            OnlineDictionary result;
            result.raw   = data;
            result.doc   = doc;
            result.count = countOf(doc);
            onSuccess(result);
        }
    );
}

void MainWindow::onUpdateTranslationsClicked() {
    if (m_isHammerRunning) {
        QMessageBox::warning(
            this,
            "警告",
            "Hammer 正在运行中，无法在运行时更新词典！"
        );

        return;
    }

    int ret =
        QMessageBox::question(
            this,
            "确认更新在线翻译",
            "是否从 GitHub 仓库获取并更新最新的汉化词典文件？\n\n"
            "提示：此操作将使用在线最新词典覆盖本地的 qt_translations.jsonc、fgd_translations.jsonc 与 fgd_override.jsonc。\n"
            "若您之前手动自定义过本地词典，请注意备份。",
            QMessageBox::Yes |
            QMessageBox::No,
            QMessageBox::No
        );

    if (ret != QMessageBox::Yes) {
        return;
    }

    setUiBusy(true);

    m_statusLabel->setText(
        "状态: 正在从 GitHub 获取最新翻译词典..."
    );

    appendLog(
        "[*] 正在从 GitHub 官方仓库下载最新词典文件...",
        "#66d9ef"
    );

    QStringList qtUrls = {
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/qt_translations.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/qt_translations.jsonc",
    };

    QStringList fgdUrls = {
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_translations.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_translations.jsonc",
    };

    QStringList overrideUrls = {
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_override.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_override.jsonc",
    };


    // 依次拉取三个在线词典：任一步失败都会中止（失败收尾由 fetchOnlineDictionary 统一处理）
    fetchOnlineDictionary(
        qtUrls,
        "[1/3]",
        "qt_translations.jsonc",
        "界面词典",
        [](const QJsonDocument& doc) { return doc.object().keys().size(); },
        [this, fgdUrls, overrideUrls](const OnlineDictionary& qt) {
            appendLog(
                QString("[+] qt_translations.jsonc 获取成功，有效词条: %1 条").arg(qt.count),
                "#a6e22e"
            );

            fetchOnlineDictionary(
                fgdUrls,
                "[2/3]",
                "fgd_translations.jsonc",
                "实体定义词典",
                [](const QJsonDocument& doc) { return doc.object().keys().size(); },
                [this, overrideUrls, qt](const OnlineDictionary& fgd) {
                    appendLog(
                        QString("[+] fgd_translations.jsonc 获取成功，有效词条: %1 条").arg(fgd.count),
                        "#a6e22e"
                    );

                    fetchOnlineDictionary(
                        overrideUrls,
                        "[3/3]",
                        "fgd_override.jsonc",
                        "实体覆盖词典",
                        [](const QJsonDocument& doc) { return CountOverrideRules(doc.object()); },
                        [this, qt, fgd](const OnlineDictionary& ovr) {
                            appendLog(
                                QString("[+] fgd_override.jsonc 获取成功，有效规则: %1 条").arg(ovr.count),
                                "#a6e22e"
                            );

                            // 原子保存
                            auto atomicSave =
                                [this](
                                    const QString& localPath,
                                    const QByteArray& data,
                                    const QString& name
                                ) -> bool {

                                std::string clean =
                                    DictionaryCompiler::StripJsonComments(
                                        data.constData(),
                                        data.size()
                                    );

                                QJsonParseError parseErr;
                                QJsonDocument doc =
                                    QJsonDocument::fromJson(
                                        QByteArray(clean.data(), static_cast<qsizetype>(clean.size())),
                                        &parseErr
                                    );

                                if (parseErr.error != QJsonParseError::NoError || doc.isNull() || !doc.isObject()) {
                                    appendLog(
                                        QString("[-] 词典数据校验未通过，已放弃写入 %1: %2").arg(name, parseErr.errorString()),
                                        "#f92672"
                                    );
                                    return false;
                                }

                                QSaveFile saveFile(localPath);
                                if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                                    appendLog(
                                        QString("[-] 无法以安全原子模式打开文件 %1: %2").arg(localPath, saveFile.errorString()),
                                        "#f92672"
                                    );
                                    return false;
                                }

                                qint64 written = saveFile.write(data);
                                if (written != data.size()) {
                                    saveFile.cancelWriting();
                                    appendLog(
                                        QString("[-] 写入数据不完整 (%1): 预期 %2 字节，实际写入 %3 字节").arg(localPath).arg(data.size()).arg(written),
                                        "#f92672"
                                    );
                                    return false;
                                }

                                if (!saveFile.commit()) {
                                    appendLog(
                                        QString("[-] 提交安全写入失败 (%1): %2").arg(localPath, saveFile.errorString()),
                                        "#f92672"
                                    );
                                    return false;
                                }

                                return true;
                            };

                            fs::path transDir = fs::path(m_workingDir) / paths::kTranslationsDir;
                            std::error_code ec;
                            fs::create_directories(transDir, ec);

                            QString qtLocalPath =
                                QString::fromStdWString(
                                    (transDir / L"qt_translations.jsonc").wstring()
                                );

                            QString fgdLocalPath =
                                QString::fromStdWString(
                                    (transDir / L"fgd_translations.jsonc").wstring()
                                );

                            QString overrideLocalPath =
                                QString::fromStdWString(
                                    (transDir / L"fgd_override.jsonc").wstring()
                                );

                            if (
                                !atomicSave(
                                    qtLocalPath,
                                    qt.raw,
                                    "qt_translations.jsonc"
                                ) ||
                                !atomicSave(
                                    fgdLocalPath,
                                    fgd.raw,
                                    "fgd_translations.jsonc"
                                ) ||
                                !atomicSave(
                                    overrideLocalPath,
                                    ovr.raw,
                                    "fgd_override.jsonc"
                                )
                            ) {

                                QMessageBox::critical(
                                    this,
                                    "写入失败",
                                    "保存或校验更新词典失败，请检查文件写入权限。"
                                );

                                setUiBusy(false);

                                m_statusLabel->setText(
                                    "状态: 写入失败"
                                );

                                return;
                            }

                            appendLog(
                                QString(
                                    "[SUCCESS] 翻译词典原子更新成功！(界面: %1, 实体: %2, 覆盖: %3)"
                                )
                                    .arg(
                                        qt.count
                                    )
                                    .arg(
                                        fgd.count
                                    )
                                    .arg(
                                        ovr.count
                                    ),
                                "#a6e22e"
                            );

                            m_statusLabel->setText(
                                "状态: 在线词典更新成功"
                            );

                            setUiBusy(false);

                            QMessageBox::information(
                                this,
                                "更新成功",
                                QString(
                                    "已成功从 GitHub 获取并更新最新汉化词典！\n\n"
                                    "- 界面词典 (qt_translations.jsonc): %1 条\n"
                                    "- 实体词典 (fgd_translations.jsonc): %2 条\n"
                                    "- 覆盖词典 (fgd_override.jsonc): %3 条\n\n"
                                    "当前可使用“仅注入”或“启动 HAMMER”应用最新汉化。"
                                )
                                    .arg(qt.count)
                                    .arg(fgd.count)
                                    .arg(ovr.count)
                            );

                            updateActionButtonState();

                        }
                    );
                }
            );
        }
    );
}

void MainWindow::onRestoreClicked() {
    if (m_isHammerRunning) {
        QMessageBox::warning(
            this,
            "警告",
            "Hammer 正在运行中，无法执行还原！"
        );

        return;
    }

    if (Cs2Detector::IsCs2ProcessRunning()) {
        QMessageBox::warning(
            this,
            "警告",
            "检测到 CS2 正在运行中，无法执行还原！\n\n"
            "请先退出 CS2 / Hammer。"
        );

        return;
    }

    // 必须处于有效的“已注入”状态
    if (!isPatchDeployedAndValid()) {
        QMessageBox::information(
            this,
            "提示",
            "当前没有检测到有效的已注入状态，无需还原。"
        );

        updateActionButtonState();
        return;
    }

    int ret =
        QMessageBox::question(
            this,
            "确认还原",
            "是否确认将 backup 中的所有原始文件覆盖恢复到 CS2 目录？\n\n"
            "还原成功后，当前汉化补丁将被完全移除。",
            QMessageBox::Yes |
            QMessageBox::No,
            QMessageBox::No
        );

    if (ret != QMessageBox::Yes) {
        return;
    }

    setUiBusy(true);

    m_statusLabel->setText(
        "状态: 正在还原原版文件..."
    );

    // 版本校验（含多次全文件 SHA256）与还原一并放入后台线程，避免 UI 冻结；
    // 校验被拒绝时通过 rejectReason/rejected 回传具体原因
    QString rejectReason;
    bool rejected = false;
    bool restoreOk =
        runHeavyInWorker(
            [this, &rejectReason, &rejected]() -> bool {
                auto val =
                    BackupManager::BackupMatchesCurrentGame(
                        m_cs2Root,
                        (fs::path(m_workingDir) / paths::kBackupDir).wstring()
                    );

                if (val.status != BackupMatchStatus::Matches) {
                    rejectReason = QString::fromStdWString(val.reason);
                    rejected = (val.status == BackupMatchStatus::GameUpdated);
                    return false;
                }

                return doRestore(true);
            }
        );

    if (!restoreOk && rejected) {
        m_statusLabel->setText(
            "状态: 还原被拒绝"
        );

        QMessageBox::warning(
            this,
            "拒绝恢复旧版备份",
            "检测到 CS2 游戏已经更新，备份版本与当前游戏不一致！\n\n" +
            rejectReason +
            "\n\n"
            "为防止旧版本文件覆盖破坏新版 CS2，已拒绝还原。\n"
            "请先处理当前游戏版本，再重新建立对应版本的备份。"
        );

        updateActionButtonState();
        setUiBusy(false);
        return;
    }

    if (!restoreOk && !rejectReason.isEmpty()) {
        // 备份状态不允许安全还原（无清单/读取失败等）
        QMessageBox::warning(
            this,
            "无法还原",
            QString(
                "当前备份状态不允许安全还原。\n\n%1"
            )
                .arg(rejectReason)
        );

        updateActionButtonState();
        setUiBusy(false);
        return;
    }

    if (restoreOk) {

        if (!BackupManager::ClearSessionState(
                m_workingDir
            )) {
            appendLog(
                "[-] 清除会话状态失败 (session_state.json)，下次启动可能重复执行自动恢复",
                "#f92672"
            );
        }

        m_statusLabel->setText(
            "状态: 就绪（未注入）"
        );

        appendLog(
            "[SUCCESS] 原版文件还原成功，当前恢复为未注入状态。",
            "#a6e22e"
        );

        QMessageBox::information(
            this,
            "还原成功",
            "所有原始文件已成功恢复。\n\n"
            "当前状态：未注入。"
        );

    } else {

        m_statusLabel->setText(
            "状态: 还原失败"
        );

        QMessageBox::critical(
            this,
            "还原失败",
            "还原过程中遇到错误，请查看执行日志。"
        );
    }

    setUiBusy(false);

    updateActionButtonState();
}

void MainWindow::checkAndRecoverAbnormalExit() {
    fs::path workPath(m_workingDir);

    fs::path backupDir =
        workPath / paths::kBackupDir;

    bool hasUnrestored =
        BackupManager::HasUnrestoredSession(
            m_workingDir
        ) ||
        (
            BackupManager::HasBackup(
                backupDir.wstring()
            ) &&
            BackupManager::IsPatchDeployed(
                m_cs2Root
            )
        );

    if (!hasUnrestored) {
        return;
    }

    appendLog(
        "[!] 检测到上一次程序未正常结束或存在未还原的补丁文件，正在进行安全恢复检查...",
        "#fd971f"
    );

    // 检查 CS2 是否仍在运行
    while (
        Cs2Detector::IsCs2ProcessRunning()
    ) {

        int ret =
            QMessageBox::warning(
                this,
                "检测到 CS2 正在运行",
                "检测到上一次程序未正常结束，且 CS2 (cs2.exe) 目前仍在运行中！\n\n"
                "为确保原版文件能够被安全恢复，请先退出 CS2 游戏或 Hammer 编辑器，然后点击【重试】。\n"
                "若点击【取消】，将推迟恢复（注意：未还原状态下直接启动可能导致原版备份异常）。",
                QMessageBox::Retry |
                QMessageBox::Cancel,
                QMessageBox::Retry
            );

        if (ret ==
            QMessageBox::Cancel) {

            appendLog(
                "[-] 用户推迟了异常退出的自动还原流程",
                "#f92672"
            );

            updateActionButtonState();

            return;
        }
    }

    appendLog(
        "[*] 正在自动从 backup 目录恢复纯净原版文件...",
        "#66d9ef"
    );

    if (runHeavyInWorker(
            [this]() {
                return doRestore(true);
            }
        )) {

        if (!BackupManager::ClearSessionState(
                m_workingDir
            )) {
            appendLog(
                "[-] 清除会话状态失败 (session_state.json)，下次启动可能重复执行自动恢复",
                "#f92672"
            );
        }

        appendLog(
            "[SUCCESS] 上次异常退出遗留的文件已成功还原为纯净原版备份！",
            "#a6e22e"
        );

        updateActionButtonState();

        QMessageBox::information(
            this,
            "自动恢复成功",
            "检测到上一次程序未正常结束（如意外关闭、断电或崩溃）。\n\n"
            "启动器已自动将 CS2 原始文件完整恢复，以确保您的游戏文件纯净无损。"
        );

    } else {

        appendLog(
            "[-] 自动恢复备份失败，请检查文件占用或稍后点击【还原】",
            "#f92672"
        );

        updateActionButtonState();
    }
}

void MainWindow::onHelpClicked() {
    QMessageBox msgBox(this);

    msgBox.setWindowTitle(
        "翻译字典格式与填写指南"
    );

    msgBox.setTextFormat(
        Qt::RichText
    );

    msgBox.setText(
        "<h3>📚 CS2 Workshop Tools 汉化字典格式与编写指南</h3>"

        "<p>所有翻译字典均采用标准 "
        "<b>UTF-8 JSONC</b> 键值对格式："
        "<code>\"英文原词\": \"中文翻译\"</code></p>"

        "<hr/>"

        "<h4>1. 实体定义翻译字典 "
        "(<code>fgd_translations.jsonc</code>)</h4>"

        "<ul>"

        "<li><b>作用</b>：精准匹配替换 FGD 实体定义中已有的英文字符串。</li>"

        "<li><b>实体类说明</b>：例如 "
        "<code>\"Omnidirectional point light\": \"全向点光源\"</code></li>"

        "<li><b>属性显示名</b>：例如 "
        "<code>\"Light Source\": \"光源\"</code>、"
        "<code>\"Name\": \"名称\"</code></li>"

        "<li><b>属性悬停描述</b>：例如 "
        "<code>\"The name that other entities use...\": \"其他实体用于引用的名称。\"</code></li>"

        "<li><b>选项与标记</b>：例如 "
        "<code>\"Enabled\": \"已启用\"</code>、"
        "<code>\"Disabled\": \"已禁用\"</code></li>"

        "<li><b>输入/输出 (I/O) 说明</b>：例如 "
        "<code>\"Removes this entity from the world.\": \"从世界中移除此实体。\"</code></li>"

        "</ul>"

        "<hr/>"

        "<h4>2. 实体键值描述补充与覆盖字典 "
        "(<code>fgd_override.jsonc</code>)</h4>"

        "<ul>"

        "<li><b>作用</b>：针对特定属性名 (Key)、实体类或 I/O "
        "<b>补充缺失描述</b>或<b>强制覆盖说明</b>。</li>"

        "<li><b>属性描述补充</b>：在 "
        "<code>properties</code> 分组中配置，如 "
        "<code>\"bodygroups\": \"设置模型的子部件与可选网格组合。\"</code></li>"

        "<li><b>I/O 说明补充</b>：在 "
        "<code>io</code> 分组中配置，如 "
        "<code>\"SetParent\": \"设置该实体的父级层级对象。\"</code></li>"

        "<li><b>实体类说明</b>：在 "
        "<code>classes</code> 分组中配置，如 "
        "<code>\"info_node\": \"AI 地面导航节点。\"</code></li>"

        "</ul>"

        "<hr/>"

        "<h4>3. 界面核心字典 "
        "(<code>qt_translations.jsonc</code>)</h4>"

        "<ul>"

        "<li><b>主菜单与工具栏</b>：例如 "
        "<code>\"File\": \"文件\"</code>、"
        "<code>\"Clipping Tool\": \"剪切工具\"</code></li>"

        "<li><b>通用属性面板</b>：例如 "
        "<code>\"Transform Locked\": \"变换锁定\"</code>、"
        "<code>\"Pinned To\": \"固定至\"</code></li>"

        "<li><b>快捷键自动适配</b>：对于带有快捷键后缀的文本（如 "
        "<code>[Shift+X]</code>、"
        "<code>(Ctrl+Z)</code>、"
        "<code>\\tCtrl+S</code>、"
        "<code>...</code>、"
        "<code>:</code>），"
        "<b>只需翻译基础英文</b>，快捷键后缀会被引擎自动保留和拼接，"
        "无需手动输入！</li>"

        "</ul>"

        "<hr/>"

        "<p>💡 <b>修改即生效</b>："
        "直接用文本编辑器编辑上述 JSONC 文件并保存，"
        "之后点击“仅注入”或“启动 HAMMER”即可应用最新汉化。</p>"
    );

    msgBox.setIcon(
        QMessageBox::Information
    );

    msgBox.exec();
}

void MainWindow::closeEvent(
    QCloseEvent *event
) {
    if (m_isHammerRunning) {

        QMessageBox::warning(
            this,
            "禁止关闭",
            "本启动器启动的 CS2 / Hammer 编辑器正在运行中！\n\n"
            "为防止 CS2 原版文件损坏或丢失，在运行期间严禁关闭本启动器。\n"
            "请先在 CS2 / Hammer 中正常退出，启动器将在退出后自动恢复原版文件并允许安全关闭。"
        );

        event->ignore();

        return;
    }

    if (m_workerBusy) {
        // 后台正在执行备份/还原/注入等 IO 操作，此时销毁窗口会导致任务中断
        QMessageBox::warning(
            this,
            "请稍候",
            "后台操作正在进行中，请等待其完成后再关闭启动器。"
        );

        event->ignore();

        return;
    }

    // 保存当前用户配置
    saveSettings();

    /*
     * 注意：
     *
     * 现在“仅注入”是一个独立操作。
     *
     * 因此关闭启动器时，不自动还原。
     *
     * 只有：
     *
     * 1. 手动点击“还原”
     * 2. HAMMER 正常退出后的自动还原
     * 3. 下次启动时的异常退出恢复
     *
     * 才会执行 RestoreAll。
     *
     * 这样用户可以：
     *
     * 仅注入 -> 关闭启动器 -> 保持补丁处于已注入状态。
     */
    event->accept();
}


bool MainWindow::sendIpcCommandToHammer(unsigned int msgId) {
    return HammerIpc::Send(msgId);
}

void MainWindow::onToggleLangClicked() {
    if (HammerIpc::Send(HammerIpc::kMsgToggleLang)) {
        appendLog("[⚡] 已向运行中的 Hammer 发送【切换原文 / 翻译】指令", "#a6e22e");
    } else {
        appendLog("[!] 未检测到运行中的 Hammer 汉化模块 IPC 窗口（请确保 Hammer 正在运行）", "#f92672");
    }
}

void MainWindow::writeAppDirPointer() {
    if (m_cs2Root.empty()) return;
    fs::path cs2Bin = paths::Win64Bin(m_cs2Root);
    if (!fs::exists(cs2Bin)) return;
    fs::path pointerFile = cs2Bin / L"localizer_appdir.txt";
    bool useMachineTrans = (m_useMachineTransCheck != nullptr) ? m_useMachineTransCheck->isChecked() : true;
    try {
        std::wofstream ofs(pointerFile, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << m_workingDir << L"\n";
            ofs << L"use_machine_trans=" << (useMachineTrans ? 1 : 0) << L"\n";
        }
    } catch (...) {}
}

void MainWindow::setupFileWatcher() {
    m_fileWatcher = new QFileSystemWatcher(this);
    m_hotReloadDebounceTimer = new QTimer(this);
    m_hotReloadDebounceTimer->setSingleShot(true);
    m_hotReloadDebounceTimer->setInterval(300); // 300ms 防抖

    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, &MainWindow::onWatchedFileChanged);
    connect(m_hotReloadDebounceTimer, &QTimer::timeout,
            this, &MainWindow::onDebouncedHotReload);

    fs::path transDir = fs::path(m_workingDir) / paths::kTranslationsDir;
    fs::path parentTransDir = fs::path(m_workingDir) / L".." / paths::kTranslationsDir;

    QStringList filesToWatch;
    auto addDictFiles = [&](const fs::path& dir) {
        filesToWatch << QString::fromStdWString((dir / L"qt_translations.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"qt_fallback.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_translations.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_override.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_fallback.jsonc").wstring());
    };
    addDictFiles(transDir);
    if (fs::exists(parentTransDir)) {
        addDictFiles(parentTransDir);
    }

    for (const QString& f : filesToWatch) {
        if (QFile::exists(f)) {
            m_fileWatcher->addPath(f);
        }
    }
}

void MainWindow::onWatchedFileChanged(const QString& path) {
    // 编辑器（如 VSCode/Notepad++）保存时可能采用原子替换机制，重新挂载监视路径
    if (m_fileWatcher && !m_fileWatcher->files().contains(path) && QFile::exists(path)) {
        m_fileWatcher->addPath(path);
    }
    if (m_hotReloadDebounceTimer) {
        m_hotReloadDebounceTimer->start(); // 重启 300ms 防抖计时
    }
}

void MainWindow::onDebouncedHotReload() {
    fs::path transDir = fs::path(m_workingDir) / paths::kTranslationsDir;
    fs::path parentTransDir = fs::path(m_workingDir) / L".." / paths::kTranslationsDir;

    // 若在源码/开发目录中编辑了上层 translations，自动同步至当前程序运行目录
    if (fs::exists(parentTransDir)) {
        for (const auto& name : { L"qt_translations.jsonc", L"qt_fallback.jsonc", L"fgd_translations.jsonc", L"fgd_override.jsonc", L"fgd_fallback.jsonc" }) {
            fs::path pSrc = parentTransDir / name;
            fs::path pDst = transDir / name;
            if (fs::exists(pSrc)) {
                try {
                    if (!fs::exists(pDst) || fs::last_write_time(pSrc) > fs::last_write_time(pDst)) {
                        fs::copy_file(pSrc, pDst, fs::copy_options::overwrite_existing);
                    }
                } catch (...) {}
            }
        }
    }

    // 重新挂载可能因原子写入丢失的监视路径
    QStringList filesToWatch;
    auto addDictFiles = [&](const fs::path& dir) {
        filesToWatch << QString::fromStdWString((dir / L"qt_translations.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"qt_fallback.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_translations.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_override.jsonc").wstring());
        filesToWatch << QString::fromStdWString((dir / L"fgd_fallback.jsonc").wstring());
    };
    addDictFiles(transDir);
    if (fs::exists(parentTransDir)) {
        addDictFiles(parentTransDir);
    }

    for (const QString& f : filesToWatch) {
        if (m_fileWatcher && !m_fileWatcher->files().contains(f) && QFile::exists(f)) {
            m_fileWatcher->addPath(f);
        }
    }

    HWND hWnd = HammerIpc::FindIpcWindow();

    if (!m_isHammerRunning && !hWnd) {
        appendLog("[📝] 检测到程序目录词典保存更新（已就绪，将在 Hammer 运行时即刻生效）", "#8b949e");
        return;
    }

    appendLog("[⚡] 检测到程序目录词典更新，自动执行 FGD + Qt 全量热重载...", "#58a6ff");
    performHotReload(true);
}

bool MainWindow::performHotReload(bool silent) {
    // 1. 刷新路径指针（同步机翻兜底标志）
    writeAppDirPointer();

    bool useMachineTrans = (m_useMachineTransCheck != nullptr) ? m_useMachineTransCheck->isChecked() : true;

    // 2. 镜像同步与合并 qt_translations.jsonc 到游戏目录（保障本地 fallback 完整）
    fs::path srcQtJson = ResolveDictionaryPath(m_workingDir, L"qt_translations.jsonc");
    fs::path srcQtFallback = ResolveDictionaryPath(m_workingDir, L"qt_fallback.jsonc");
    fs::path cs2Bin = paths::Win64Bin(m_cs2Root);
    fs::path destQtJson = cs2Bin / L"qt_translations.jsonc";
    fs::path destQtFallback = cs2Bin / L"qt_fallback.jsonc";

    if (fs::exists(cs2Bin)) {
        // 同步 fallback 字典到游戏目录备份（如果存在）
        if (fs::exists(srcQtFallback)) {
            try {
                fs::copy_file(srcQtFallback, destQtFallback, fs::copy_options::overwrite_existing);
            } catch (...) {}
        }
        // 优先合并主词典与机翻兜底词典部署到游戏目录
        std::wstring qtFallbackParam = (useMachineTrans && fs::exists(srcQtFallback)) ? srcQtFallback.wstring() : L"";
        bool merged = false;
        if (!qtFallbackParam.empty() && fs::exists(srcQtJson)) {
            std::wstring mergeErr;
            merged = DictionaryCompiler::MergeJsonFiles(srcQtJson.wstring(), qtFallbackParam, destQtJson.wstring(), mergeErr);
        }
        if (!merged && fs::exists(srcQtJson)) {
            try {
                fs::copy_file(srcQtJson, destQtJson, fs::copy_options::overwrite_existing);
            } catch (...) {}
        }
    }

    // 3. 联动重新编译并部署 FGD（引入 fgd_fallback 兜底）
    fs::path transDir = fs::path(m_workingDir) / paths::kTranslationsDir;
    fs::path backupDir = fs::path(m_workingDir) / paths::kBackupDir;
    fs::path fgdDictPath = ResolveDictionaryPath(m_workingDir, L"fgd_translations.jsonc");
    fs::path fgdOverridePath = ResolveDictionaryPath(m_workingDir, L"fgd_override.jsonc");
    fs::path fgdFallbackPath = ResolveDictionaryPath(m_workingDir, L"fgd_fallback.jsonc");

    std::wstring fgdFallbackParam = (useMachineTrans && fs::exists(fgdFallbackPath)) ? fgdFallbackPath.wstring() : L"";

    std::vector<std::wstring> transFgd;
    std::wstring err;
    bool fgdOk = FgdTranslator::TranslateAndDeployAll(
        m_cs2Root,
        backupDir.wstring(),
        transDir.wstring(),
        fgdDictPath.wstring(),
        fgdOverridePath.wstring(),
        transFgd,
        err,
        fgdFallbackParam
    );

    // 4. 发送 IPC 消息给 Hammer
    bool ipcOk = HammerIpc::Send(HammerIpc::kMsgReloadDict);

    if (ipcOk) {
        if (fgdOk) {
            appendLog(QString("[⚡] 全量热重载成功！已重新编译覆盖 %1 个 FGD 实体文件，并刷新 Hammer 界面翻译%2")
                .arg(transFgd.size())
                .arg(useMachineTrans ? " (已载入机翻兜底)" : ""), "#a6e22e");
        } else {
            appendLog(QString("[⚡] Qt 界面翻译已热重载生效（FGD 重新部署提示: %1）").arg(QString::fromStdWString(err)), "#e6db74");
        }
        return true;
    } else {
        if (!silent) {
            appendLog("[!] 未检测到运行中的 Hammer 汉化模块 IPC 窗口（已在磁盘完成 FGD 重新编译与词典同步）", "#d29922");
        }
        return false;
    }
}

void MainWindow::onHotReloadClicked() {
    performHotReload(false);
}

void MainWindow::onDebugClicked() {
    openDebugWindow();
}

void MainWindow::openDebugWindow() {
    DebugWindow* dbg = new DebugWindow(m_cs2Root, this);
    dbg->setAttribute(Qt::WA_DeleteOnClose);
    dbg->show();
    dbg->raise();
    dbg->activateWindow();
}