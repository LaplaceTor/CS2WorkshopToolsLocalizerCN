#include "mainwindow.h"
#include "cs2_detector.h"
#include "fgd_translator.h"
#include "pe_patcher.h"
#include "backup_manager.h"
#include "dictionary_compiler.h"

#include <windows.h>
#include <psapi.h>
#include <thread>

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
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QFile>
#include <QSaveFile>

#include <memory>
#include <filesystem>

namespace fs = std::filesystem;

MainWindow::MainWindow(const std::wstring& cs2Root, QWidget *parent)
    : QMainWindow(parent)
    , m_cs2Root(cs2Root)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_hammerProcess(new QProcess(this))
    , m_monitorTimer(new QTimer(this))
    , m_notRunningCount(0)
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
    fs::path fgdPath =
        fs::path(m_workingDir) / L"fgd_translations.json";

    fs::path fgdOverridePath =
        fs::path(m_workingDir) / L"fgd_override.json";

    fs::path qtPath =
        fs::path(m_workingDir) / L"qt_translations.json";

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

    resize(460, 420);
    setMinimumSize(460, 420);

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

    subBtnLayout->addWidget(m_updateBtn);
    subBtnLayout->addWidget(m_helpBtn);

    btnLayout->addLayout(subBtnLayout);

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
    fs::path configPath =
        fs::path(m_workingDir) / L"config.ini";

    QSettings settings(
        QString::fromStdWString(configPath.wstring()),
        QSettings::IniFormat
    );

    QString savedAddon =
        settings.value(
            "Launcher/SelectedAddon",
            ""
        ).toString().trimmed();

    QString savedArgs =
        settings.value(
            "Launcher/LaunchArgs",
            ""
        ).toString();

    // 恢复附加启动参数
    m_argsEdit->setText(savedArgs);

    // 恢复选择的目标 Addon
    if (!savedAddon.isEmpty()) {
        int index =
            m_addonCombo->findText(savedAddon);

        if (index == -1) {
            for (int i = 0;
                 i < m_addonCombo->count();
                 ++i) {

                QString itemText =
                    m_addonCombo->itemText(i);

                if (itemText == savedAddon ||
                    itemText.startsWith(
                        savedAddon + " "
                    )) {

                    index = i;
                    break;
                }
            }
        }

        if (index != -1) {
            m_addonCombo->setCurrentIndex(index);
        } else {
            if (m_addonCombo->count() > 0) {
                m_addonCombo->setCurrentIndex(0);
            }
        }
    } else {
        if (m_addonCombo->count() > 0) {
            m_addonCombo->setCurrentIndex(0);
        }
    }
}

void MainWindow::saveSettings() {
    fs::path configPath =
        fs::path(m_workingDir) / L"config.ini";

    QSettings settings(
        QString::fromStdWString(configPath.wstring()),
        QSettings::IniFormat
    );

    QString selectedAddon =
        m_addonCombo->currentText().trimmed();

    if (selectedAddon.contains(" ")) {
        selectedAddon =
            selectedAddon.split(" ").first();
    }

    settings.setValue(
        "Launcher/SelectedAddon",
        selectedAddon
    );

    settings.setValue(
        "Launcher/LaunchArgs",
        m_argsEdit->text()
    );

    settings.sync();
}

void MainWindow::appendLog(
    const QString& msg,
    const QString& color
) {
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
        fs::path(m_workingDir) / L"backup";

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

    // backup_manifest 必须匹配当前 CS2
    auto validation =
        BackupManager::BackupMatchesCurrentGame(
            m_cs2Root,
            backupDir.wstring()
        );

    if (validation.status != BackupMatchStatus::Matches) {
        return false;
    }

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
    return sessionPatched || patchFilesPresent;
}

void MainWindow::updateActionButtonState() {
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
    fs::path workPath(m_workingDir);

    fs::path backupDir =
        workPath / L"backup";

    fs::path transDir =
        workPath / L"translations";

    fs::path cs2Bin =
        fs::path(m_cs2Root) /
        L"game" /
        L"bin" /
        L"win64";

    fs::path fgdDictPath =
        workPath / L"fgd_translations.json";

    fs::path fgdOverridePath =
        workPath / L"fgd_override.json";

    fs::path qtDictPath =
        workPath / L"qt_translations.json";

    fs::path qmDllSrc =
        workPath / L"qtcore_qm.dll";

    std::wstring notice;

    if (FgdTranslator::EnsureFgdDictionaryExists(
            fgdDictPath.wstring(),
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
            qtDictPath.wstring(),
            L"",
            notice)) {

        appendLog(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    // ==========================================
    // STEP 1: 原版备份
    // ==========================================
    appendLog(
        "[1/3] 正在校验游戏版本并准备原版备份...",
        "#e6db74"
    );

    auto matchResult =
        BackupManager::BackupMatchesCurrentGame(
            m_cs2Root,
            backupDir.wstring()
        );

    bool forceRecreate = false;

    if (matchResult.status ==
        BackupMatchStatus::GameUpdated) {

        appendLog(
            QString(
                "[!] 检测到 CS2 游戏版本发生变化: %1"
            )
                .arg(
                    QString::fromStdWString(
                        matchResult.reason
                    )
                ),
            "#fd971f"
        );

        appendLog(
            "[*] 当前旧备份已失效，将重新建立当前版本原版备份...",
            "#66d9ef"
        );

        forceRecreate = true;
    }

    std::vector<std::wstring> backedFgd;
    std::wstring err;

    if (!BackupManager::CreateOrUpdateBackup(
            m_cs2Root,
            backupDir.wstring(),
            backedFgd,
            err,
            forceRecreate)) {

        appendLog(
            QString(
                "[-] 备份原版文件失败: %1"
            )
                .arg(
                    QString::fromStdWString(err)
                ),
            "#f92672"
        );

        QMessageBox::critical(
            this,
            "错误",
            "备份 CS2 原版文件失败，已中止注入！\n" +
            QString::fromStdWString(err)
        );

        return false;
    }

    appendLog(
        QString(
            "[+] 成功捕获并绑定 %1 个原版 FGD 与 Qt5Core.dll"
        )
            .arg(backedFgd.size()),
        "#a6e22e"
    );

    // ==========================================
    // STEP 2: FGD 汉化
    // ==========================================
    appendLog(
        "[2/3] 正在部署 FGD 汉化...",
        "#e6db74"
    );

    std::vector<std::wstring> transFgd;

    if (!FgdTranslator::TranslateAndDeployAll(
            m_cs2Root,
            backupDir.wstring(),
            transDir.wstring(),
            fgdDictPath.wstring(),
            fgdOverridePath.wstring(),
            transFgd,
            err)) {

        appendLog(
            QString(
                "[-] 汉化 FGD 失败: %1"
            )
                .arg(
                    QString::fromStdWString(err)
                ),
            "#f92672"
        );

        QMessageBox::critical(
            this,
            "错误",
            "汉化 FGD 文件失败，注入已中止！\n" +
            QString::fromStdWString(err)
        );

        doRestore(false);
        return false;
    }

    appendLog(
        QString(
            "[+] 成功汉化并部署 %1 个 FGD 文件"
        )
            .arg(transFgd.size()),
        "#a6e22e"
    );

    // ==========================================
    // STEP 3: Qt Patch
    // ==========================================
    appendLog(
        "[3/3] 正在部署 Qt 汉化模块并修补 Qt5Core.dll...",
        "#e6db74"
    );

    try {
        fs::path destQtJson =
            cs2Bin / L"qt_translations.json";

        fs::path destQmDll =
            cs2Bin / L"qtcore_qm.dll";

        if (!fs::exists(qtDictPath)) {
            appendLog(
                "[-] 找不到 qt_translations.json",
                "#f92672"
            );

            QMessageBox::critical(
                this,
                "错误",
                "找不到 qt_translations.json！"
            );

            doRestore(false);
            return false;
        }

        if (!fs::exists(qmDllSrc)) {
            appendLog(
                "[-] 找不到 qtcore_qm.dll",
                "#f92672"
            );

            QMessageBox::critical(
                this,
                "错误",
                "找不到 qtcore_qm.dll！"
            );

            doRestore(false);
            return false;
        }

        fs::copy_file(
            qtDictPath,
            destQtJson,
            fs::copy_options::overwrite_existing
        );

        fs::copy_file(
            qmDllSrc,
            destQmDll,
            fs::copy_options::overwrite_existing
        );

        // 预编译纯 C 二进制字典 qt_translations.lcld
        fs::path destQtLcld =
            cs2Bin / L"qt_translations.lcld";

        std::wstring compileErr;

        if (DictionaryCompiler::CompileJsonFileToLcld(
                qtDictPath.wstring(),
                destQtLcld.wstring(),
                compileErr)) {

            appendLog(
                "[+] 成功编译并部署纯二进制字典 qt_translations.lcld (LCLD v1)",
                "#a6e22e"
            );

        } else {

            appendLog(
                QString(
                    "[!] 二进制字典编译提示: %1 (将回退至纯文本 JSON 模式)"
                )
                    .arg(
                        QString::fromStdWString(
                            compileErr
                        )
                    ),
                "#fd971f"
            );
        }

        // 修补 Qt5Core.dll
        fs::path backupQtCore =
            backupDir /
            L"game" /
            L"bin" /
            L"win64" /
            L"Qt5Core.dll";

        fs::path targetQtCore =
            cs2Bin / L"Qt5Core.dll";

        if (!PePatcher::PatchQtCore(
                backupQtCore.wstring(),
                targetQtCore.wstring(),
                err)) {

            appendLog(
                QString(
                    "[-] 修补 Qt5Core.dll 失败: %1"
                )
                    .arg(
                        QString::fromStdWString(err)
                    ),
                "#f92672"
            );

            QMessageBox::critical(
                this,
                "错误",
                "修补 Qt5Core.dll 失败！\n" +
                QString::fromStdWString(err)
            );

            doRestore(false);
            return false;
        }

        appendLog(
            "[+] Qt5Core.dll PE Code Cave 注入与重定向修补成功",
            "#a6e22e"
        );

    } catch (const std::exception& e) {

        appendLog(
            QString(
                "[-] 部署补丁异常: %1"
            )
                .arg(e.what()),
            "#f92672"
        );

        QMessageBox::critical(
            this,
            "错误",
            QString(
                "部署补丁异常: %1"
            )
                .arg(e.what())
        );

        doRestore(false);
        return false;
    }

    /*
     * 注入成功后记录 session_state。
     *
     * 这样即使用户之后直接关闭启动器，
     * 下次启动依然可以知道当前目录没有被还原。
     */
    BackupManager::SaveSessionState(
        m_workingDir,
        true
    );

    appendLog(
        "[SUCCESS] 汉化补丁注入完成，当前处于“已注入”状态。",
        "#a6e22e"
    );

    m_statusLabel->setText(
        "状态: 已注入，等待启动 HAMMER"
    );

    updateActionButtonState();

    return true;
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
    }

    updateActionButtonState();
}

bool MainWindow::startHammerProcess() {
    fs::path cs2Bin =
        fs::path(m_cs2Root) /
        L"game" /
        L"bin" /
        L"win64";

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
            (cs2Bin / L"cs2.exe").wstring()
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
    BackupManager::SaveSessionState(
        m_workingDir,
        true
    );

    m_isHammerRunning = true;
    m_notRunningCount = 0;

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
    m_notRunningCount = 0;

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

    bool isRunning = false;

    if (m_hammerProcess &&
        m_hammerProcess->state() ==
            QProcess::Running) {

        isRunning = true;

    } else if (
        m_hammerProcessHandle != nullptr
    ) {

        DWORD waitRes =
            WaitForSingleObject(
                static_cast<HANDLE>(
                    m_hammerProcessHandle
                ),
                0
            );

        if (waitRes == WAIT_TIMEOUT) {
            isRunning = true;
        }

    } else if (m_hammerPid > 0) {

        isRunning =
            Cs2Detector::IsProcessRunning(
                static_cast<DWORD>(
                    m_hammerPid
                )
            );
    }

    if (isRunning) {
        m_notRunningCount = 0;

        m_statusLabel->setText(
            "状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)"
        );

    } else {

        m_notRunningCount++;

        if (m_notRunningCount >= 2) {
            handleHammerProcessTerminated();
        }
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
        doRestore(true);

    if (restoreOk) {
        BackupManager::ClearSessionState(
            m_workingDir
        );

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
    if (!m_isHammerRunning) {
        appendLog(
            QString(
                "[-] 启动 Hammer 进程出错 (错误码: %1)"
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
            doRestore(true);

        if (restored) {
            BackupManager::ClearSessionState(
                m_workingDir
            );
        }

        setUiBusy(false);

        m_statusLabel->setText(
            "状态: 启动出错"
        );

        updateActionButtonState();
    }
}

bool MainWindow::doRestore(bool showLog) {
    fs::path workPath(m_workingDir);

    fs::path backupDir =
        workPath / L"backup";

    if (!BackupManager::HasBackup(
            backupDir.wstring())) {

        return true;
    }

    if (showLog) {
        appendLog(
            "[*] 正在还原原版 FGD 实体定义及核心二进制...",
            "#66d9ef"
        );
    }

    std::wstring err;

    if (!BackupManager::RestoreAll(
            m_cs2Root,
            backupDir.wstring(),
            err)) {

        if (showLog) {
            appendLog(
                QString(
                    "[-] 还原备份失败: %1"
                )
                    .arg(
                        QString::fromStdWString(err)
                    ),
                "#f92672"
            );
        }

        return false;
    }

    if (showLog) {
        appendLog(
            "[SUCCESS] 还原操作完成！所有原版 FGD 实体定义及 Qt5Core.dll 已恢复原样。",
            "#a6e22e"
        );
    }

    return true;
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
            "提示：此操作将使用在线最新词典覆盖本地的 qt_translations.json、fgd_translations.json 与 fgd_override.json。\n"
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
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/qt_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/qt_translations.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/master/qt_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@master/qt_translations.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2HammerTranslateCN/main/qt_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2HammerTranslateCN@main/qt_translations.json"
    };

    QStringList fgdUrls = {
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/fgd_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/fgd_translations.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/master/fgd_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@master/fgd_translations.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2HammerTranslateCN/main/fgd_translations.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2HammerTranslateCN@main/fgd_translations.json"
    };

    QStringList overrideUrls = {
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/fgd_override.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/fgd_override.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/master/fgd_override.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@master/fgd_override.json",
        "https://raw.githubusercontent.com/LaplaceTor/CS2HammerTranslateCN/main/fgd_override.json",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2HammerTranslateCN@main/fgd_override.json"
    };

    auto stripJsonc =
        [](const QByteArray& input) -> QByteArray {

        QByteArray output;

        output.reserve(
            input.size()
        );

        const char* p =
            input.constData();

        const char* end =
            p + input.size();

        while (p < end) {

            if (*p == '"') {

                output.append(*p++);

                while (p < end) {

                    char c =
                        *p++;

                    output.append(c);

                    if (c == '"') {
                        break;
                    }

                    if (
                        c == '\\' &&
                        p < end
                    ) {
                        output.append(
                            *p++
                        );
                    }
                }

            } else if (
                *p == '/' &&
                (p + 1 < end)
            ) {

                if (*(p + 1) == '/') {

                    p += 2;

                    while (
                        p < end &&
                        *p != '\n' &&
                        *p != '\r'
                    ) {
                        p++;
                    }

                } else if (*(p + 1) == '*') {

                    p += 2;

                    while (
                        p + 1 < end &&
                        !(
                            *p == '*' &&
                            *(p + 1) == '/'
                        )
                    ) {
                        p++;
                    }

                    if (p + 1 < end) {
                        p += 2;
                    } else {
                        p = end;
                    }

                } else {

                    output.append(
                        *p++
                    );
                }

            } else {

                output.append(
                    *p++
                );
            }
        }

        return output;
    };

    // 1. Fetch qt_translations.json
    appendLog(
        "[1/3] 正在获取 qt_translations.json (界面词典)...",
        "#e6db74"
    );

    fetchUrlCandidates(
        qtUrls,
        [
            this,
            fgdUrls,
            overrideUrls,
            stripJsonc
        ]
        (
            bool qtOk,
            const QByteArray& qtData
        ) {

        if (!qtOk) {

            appendLog(
                "[-] 获取 qt_translations.json 失败：所有节点连接超时或不可达，请检查网络或代理设置。",
                "#f92672"
            );

            QMessageBox::critical(
                this,
                "更新失败",
                "获取 qt_translations.json 失败！\n"
                "无法连接到 GitHub 仓库，请检查您的网络连接或代理设置。"
            );

            setUiBusy(false);

            m_statusLabel->setText(
                "状态: 词典更新失败"
            );

            return;
        }

        QJsonParseError qtParseErr;

        QJsonDocument qtDoc =
            QJsonDocument::fromJson(
                stripJsonc(qtData),
                &qtParseErr
            );

        if (
            qtParseErr.error !=
                QJsonParseError::NoError ||
            !qtDoc.isObject() ||
            qtDoc.object().isEmpty()
        ) {

            appendLog(
                QString(
                    "[-] 解析 qt_translations.json 失败: %1"
                )
                    .arg(
                        qtParseErr.errorString()
                    ),
                "#f92672"
            );

            QMessageBox::critical(
                this,
                "更新失败",
                "下载的 qt_translations.json 格式异常或内容为空，已放弃更新。"
            );

            setUiBusy(false);

            m_statusLabel->setText(
                "状态: 词典校验失败"
            );

            return;
        }

        qsizetype qtCount =
            qtDoc.object().keys().size();

        appendLog(
            QString(
                "[+] qt_translations.json 获取成功，有效词条: %1 条"
            )
                .arg(qtCount),
            "#a6e22e"
        );

        // 2. Fetch fgd_translations.json
        appendLog(
            "[2/3] 正在获取 fgd_translations.json (实体定义词典)...",
            "#e6db74"
        );

        fetchUrlCandidates(
            fgdUrls,
            [
                this,
                overrideUrls,
                qtData,
                qtCount,
                stripJsonc
            ]
            (
                bool fgdOk,
                const QByteArray& fgdData
            ) {

            if (!fgdOk) {

                appendLog(
                    "[-] 获取 fgd_translations.json 失败：所有节点连接超时或不可达，请检查网络设置。",
                    "#f92672"
                );

                QMessageBox::critical(
                    this,
                    "更新失败",
                    "获取 fgd_translations.json 失败！\n"
                    "无法连接到 GitHub 仓库，请检查网络连接。"
                );

                setUiBusy(false);

                m_statusLabel->setText(
                    "状态: 词典更新失败"
                );

                return;
            }

            QJsonParseError fgdParseErr;

            QJsonDocument fgdDoc =
                QJsonDocument::fromJson(
                    stripJsonc(fgdData),
                    &fgdParseErr
                );

            if (
                fgdParseErr.error !=
                    QJsonParseError::NoError ||
                !fgdDoc.isObject() ||
                fgdDoc.object().isEmpty()
            ) {

                appendLog(
                    QString(
                        "[-] 解析 fgd_translations.json 失败: %1"
                    )
                        .arg(
                            fgdParseErr.errorString()
                        ),
                    "#f92672"
                );

                QMessageBox::critical(
                    this,
                    "更新失败",
                    "下载的 fgd_translations.json 格式异常或内容为空，已放弃更新。"
                );

                setUiBusy(false);

                m_statusLabel->setText(
                    "状态: 词典校验失败"
                );

                return;
            }

            qsizetype fgdCount =
                fgdDoc.object().keys().size();

            appendLog(
                QString(
                    "[+] fgd_translations.json 获取成功，有效词条: %1 条"
                )
                    .arg(fgdCount),
                "#a6e22e"
            );

            // 3. Fetch fgd_override.json
            appendLog(
                "[3/3] 正在获取 fgd_override.json (实体覆盖词典)...",
                "#e6db74"
            );

            fetchUrlCandidates(
                overrideUrls,
                [
                    this,
                    qtData,
                    fgdData,
                    qtCount,
                    fgdCount,
                    stripJsonc
                ]
                (
                    bool overrideOk,
                    const QByteArray& overrideData
                ) {

                if (!overrideOk) {

                    appendLog(
                        "[-] 获取 fgd_override.json 失败：所有节点连接超时或不可达，请检查网络设置。",
                        "#f92672"
                    );

                    QMessageBox::critical(
                        this,
                        "更新失败",
                        "获取 fgd_override.json 失败！\n"
                        "无法连接到 GitHub 仓库，请检查网络设置。"
                    );

                    setUiBusy(false);

                    m_statusLabel->setText(
                        "状态: 词典更新失败"
                    );

                    return;
                }

                QJsonParseError overrideParseErr;

                QJsonDocument overrideDoc =
                    QJsonDocument::fromJson(
                        stripJsonc(overrideData),
                        &overrideParseErr
                    );

                if (
                    overrideParseErr.error !=
                        QJsonParseError::NoError ||
                    !overrideDoc.isObject() ||
                    overrideDoc.object().isEmpty()
                ) {

                    appendLog(
                        QString(
                            "[-] 解析 fgd_override.json 失败: %1"
                        )
                            .arg(
                                overrideParseErr.errorString()
                            ),
                        "#f92672"
                    );

                    QMessageBox::critical(
                        this,
                        "更新失败",
                        "下载的 fgd_override.json 格式异常或内容为空，已放弃更新。"
                    );

                    setUiBusy(false);

                    m_statusLabel->setText(
                        "状态: 词典校验失败"
                    );

                    return;
                }

                qsizetype overrideCount = 0;

                QJsonObject overrideObj =
                    overrideDoc.object();

                if (
                    overrideObj.contains("properties") &&
                    overrideObj["properties"].isObject()
                ) {
                    overrideCount +=
                        overrideObj["properties"]
                            .toObject()
                            .keys()
                            .size();
                }

                if (
                    overrideObj.contains("io") &&
                    overrideObj["io"].isObject()
                ) {
                    overrideCount +=
                        overrideObj["io"]
                            .toObject()
                            .keys()
                            .size();
                }

                if (
                    overrideObj.contains("classes") &&
                    overrideObj["classes"].isObject()
                ) {
                    overrideCount +=
                        overrideObj["classes"]
                            .toObject()
                            .keys()
                            .size();
                }

                for (
                    auto it = overrideObj.begin();
                    it != overrideObj.end();
                    ++it
                ) {

                    if (
                        it.key() != "properties" &&
                        it.key() != "io" &&
                        it.key() != "classes" &&
                        !it.key().startsWith("_")
                    ) {
                        overrideCount++;
                    }
                }

                if (overrideCount == 0) {
                    overrideCount =
                        overrideObj.keys().size();
                }

                appendLog(
                    QString(
                        "[+] fgd_override.json 获取成功，有效规则: %1 条"
                    )
                        .arg(overrideCount),
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
                            QByteArray::fromRawData(
                                clean.c_str(),
                                clean.size()
                            ),
                            &parseErr
                        );

                    if (
                        parseErr.error !=
                            QJsonParseError::NoError ||
                        !doc.isObject()
                    ) {

                        appendLog(
                            QString(
                                "[-] %1 校验失败: %2"
                            )
                                .arg(
                                    name,
                                    parseErr.errorString()
                                ),
                            "#f92672"
                        );

                        return false;
                    }

                    QSaveFile saveFile(localPath);

                    if (!saveFile.open(
                            QIODevice::WriteOnly
                        )) {

                        appendLog(
                            QString(
                                "[-] 无法创建文件 %1: %2"
                            )
                                .arg(
                                    name,
                                    saveFile.errorString()
                                ),
                            "#f92672"
                        );

                        return false;
                    }

                    if (
                        saveFile.write(data) !=
                        data.size()
                    ) {

                        saveFile.cancelWriting();

                        appendLog(
                            QString(
                                "[-] 写入 %1 数据不完整"
                            )
                                .arg(name),
                            "#f92672"
                        );

                        return false;
                    }

                    if (!saveFile.commit()) {

                        appendLog(
                            QString(
                                "[-] 原子提交 %1 失败: %2"
                            )
                                .arg(
                                    name,
                                    saveFile.errorString()
                                ),
                            "#f92672"
                        );

                        return false;
                    }

                    return true;
                };

                QString qtLocalPath =
                    QString::fromStdWString(
                        (
                            fs::path(m_workingDir) /
                            L"qt_translations.json"
                        ).wstring()
                    );

                QString fgdLocalPath =
                    QString::fromStdWString(
                        (
                            fs::path(m_workingDir) /
                            L"fgd_translations.json"
                        ).wstring()
                    );

                QString overrideLocalPath =
                    QString::fromStdWString(
                        (
                            fs::path(m_workingDir) /
                            L"fgd_override.json"
                        ).wstring()
                    );

                if (
                    !atomicSave(
                        qtLocalPath,
                        qtData,
                        "qt_translations.json"
                    ) ||
                    !atomicSave(
                        fgdLocalPath,
                        fgdData,
                        "fgd_translations.json"
                    ) ||
                    !atomicSave(
                        overrideLocalPath,
                        overrideData,
                        "fgd_override.json"
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
                            qtCount
                        )
                        .arg(
                            fgdCount
                        )
                        .arg(
                            overrideCount
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
                        "- 界面词典 (qt_translations.json): %1 条\n"
                        "- 实体词典 (fgd_translations.json): %2 条\n"
                        "- 覆盖词典 (fgd_override.json): %3 条\n\n"
                        "当前可使用“仅注入”或“启动 HAMMER”应用最新汉化。"
                    )
                        .arg(qtCount)
                        .arg(fgdCount)
                        .arg(overrideCount)
                );

                updateActionButtonState();
            });
        });
    });
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

    fs::path backupDir =
        fs::path(m_workingDir) / L"backup";

    auto val =
        BackupManager::BackupMatchesCurrentGame(
            m_cs2Root,
            backupDir.wstring()
        );

    if (val.status ==
        BackupMatchStatus::GameUpdated) {

        QMessageBox::warning(
            this,
            "拒绝恢复旧版备份",
            "检测到 CS2 游戏已经更新，备份版本与当前游戏不一致！\n\n" +
            QString::fromStdWString(
                val.reason
            ) +
            "\n\n"
            "为防止旧版本文件覆盖破坏新版 CS2，已拒绝还原。\n"
            "请先处理当前游戏版本，再重新建立对应版本的备份。"
        );

        updateActionButtonState();
        return;
    }

    if (
        val.status != BackupMatchStatus::Matches
    ) {

        QMessageBox::warning(
            this,
            "无法还原",
            QString(
                "当前备份状态不允许安全还原。\n\n%1"
            )
                .arg(
                    QString::fromStdWString(
                        val.reason
                    )
                )
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

    bool restoreOk =
        doRestore(true);

    if (restoreOk) {

        BackupManager::ClearSessionState(
            m_workingDir
        );

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
        workPath / L"backup";

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

    if (doRestore(true)) {

        BackupManager::ClearSessionState(
            m_workingDir
        );

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
        "<b>UTF-8 JSON / JSONC</b> 键值对格式："
        "<code>\"英文原词\": \"中文翻译\"</code></p>"

        "<hr/>"

        "<h4>1. 实体定义翻译字典 "
        "(<code>fgd_translations.json</code>)</h4>"

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
        "(<code>fgd_override.json</code>)</h4>"

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
        "(<code>qt_translations.json</code>)</h4>"

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
        "直接用文本编辑器编辑上述 JSON 文件并保存，"
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