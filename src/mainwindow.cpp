#include "mainwindow.h"
#include "cs2_detector.h"
#include "fgd_translator.h"
#include "pe_patcher.h"
#include "backup_manager.h"

#include <windows.h>
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
    updateRestoreButtonState();

    connect(m_launchBtn, &QPushButton::clicked, this, &MainWindow::onLaunchClicked);
    connect(m_updateBtn, &QPushButton::clicked, this, &MainWindow::onUpdateTranslationsClicked);
    connect(m_restoreBtn, &QPushButton::clicked, this, &MainWindow::onRestoreClicked);
    connect(m_helpBtn, &QPushButton::clicked, this, &MainWindow::onHelpClicked);
    connect(m_addonCombo, &QComboBox::currentIndexChanged, this, &MainWindow::saveSettings);
    connect(m_argsEdit, &QLineEdit::textChanged, this, &MainWindow::saveSettings);
    connect(m_hammerProcess, &QProcess::started, this, &MainWindow::onHammerStarted);
    connect(m_hammerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::onHammerFinished);
    connect(m_hammerProcess, &QProcess::errorOccurred, this, &MainWindow::onHammerError);
    connect(m_monitorTimer, &QTimer::timeout, this, &MainWindow::onCheckProcessState);

    appendLog("==================================================", "#66d9ef");
    appendLog(" CS2 Workshop Tools Localizer CN 汉化启动器已就绪", "#a6e22e");
    appendLog("==================================================", "#66d9ef");
    appendLog(QString("已锁定 CS2 安装目录: %1").arg(QString::fromStdWString(m_cs2Root)), "#f8f8f2");

    // 检查并生成翻译字典文件
    fs::path fgdPath = fs::path(m_workingDir) / L"fgd_translations.json";
    fs::path fgdOverridePath = fs::path(m_workingDir) / L"fgd_override.json";
    fs::path qtPath = fs::path(m_workingDir) / L"qt_translations.json";

    std::wstring notice;
    if (FgdTranslator::EnsureFgdDictionaryExists(fgdPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
    if (FgdTranslator::EnsureFgdOverrideDictionaryExists(fgdOverridePath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
    if (FgdTranslator::EnsureQtDictionaryExists(qtPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }

    // 检查上一次是否异常退出并执行安全恢复
    checkAndRecoverAbnormalExit();
}

MainWindow::~MainWindow() {
    if (m_hammerProcessHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_hammerProcessHandle));
        m_hammerProcessHandle = nullptr;
    }
}

void MainWindow::setupUi() {
    setWindowTitle("CS2 Workshop Tools 汉化启动器 - CS2 Workshop Tools Localizer CN");
    resize(440, 420);
    setMinimumSize(440, 420);

    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // 1. 顶部 CS2 路径信息卡片
    QGroupBox* pathGroup = new QGroupBox("CS2 路径信息", centralWidget);
    QHBoxLayout* pathLayout = new QHBoxLayout(pathGroup);
    pathLayout->setContentsMargins(8, 6, 8, 6);
    m_cs2PathLabel = new QLabel(QString::fromStdWString(m_cs2Root), pathGroup);
    m_cs2PathLabel->setStyleSheet("font-weight: bold; color: #4ec9b0; font-size: 12px;");
    m_cs2PathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLayout->addWidget(m_cs2PathLabel);
    mainLayout->addWidget(pathGroup);

    // 2. 参数与 Addon 选择卡片
    QGroupBox* configGroup = new QGroupBox("启动配置", centralWidget);
    QGridLayout* configLayout = new QGridLayout(configGroup);
    configLayout->setContentsMargins(8, 6, 8, 6);
    configLayout->setHorizontalSpacing(8);
    configLayout->setVerticalSpacing(6);

    QLabel* addonLabel = new QLabel("目标 Addon 模组:", configGroup);
    m_addonCombo = new QComboBox(configGroup);
    m_addonCombo->setMinimumHeight(28);
    configLayout->addWidget(addonLabel, 0, 0);
    configLayout->addWidget(m_addonCombo, 0, 1);

    QLabel* argsLabel = new QLabel("附加启动参数:", configGroup);
    m_argsEdit = new QLineEdit(configGroup);
    m_argsEdit->setMinimumHeight(28);
    m_argsEdit->setPlaceholderText("例如: -gpuraytracing");
    configLayout->addWidget(argsLabel, 1, 0);
    configLayout->addWidget(m_argsEdit, 1, 1);

    mainLayout->addWidget(configGroup);

    // 3. 操作按钮栏 (分为两行)
    QVBoxLayout* btnLayout = new QVBoxLayout();
    btnLayout->setSpacing(6);

    // 第一行: 主要启动按钮
    m_launchBtn = new QPushButton("🚀 启动 CS2 Workshop Tools (汉化版)", centralWidget);
    m_launchBtn->setMinimumHeight(36);
    m_launchBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #2ea043; color: white; font-weight: bold; font-size: 13px; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #3fb950; }"
        "QPushButton:pressed { background-color: #238636; }"
        "QPushButton:disabled { background-color: #555555; color: #888888; }"
    );
    btnLayout->addWidget(m_launchBtn);

    // 第二行: 功能辅助按钮 (更新、还原、指南)
    QHBoxLayout* subBtnLayout = new QHBoxLayout();
    subBtnLayout->setSpacing(6);

    m_updateBtn = new QPushButton("🌐 更新在线翻译", centralWidget);
    m_updateBtn->setMinimumHeight(28);
    m_updateBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #0969da; color: white; font-weight: bold; font-size: 12px; border-radius: 4px;"
        "}"
        "QPushButton:hover { background-color: #218bff; }"
        "QPushButton:pressed { background-color: #0550ae; }"
        "QPushButton:disabled { background-color: #2d333b; color: #636e7b; }"
    );

    m_restoreBtn = new QPushButton("🔄 还原原版备份", centralWidget);
    m_restoreBtn->setMinimumHeight(28);
    m_restoreBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #444c56; color: #adbac7; font-weight: bold; font-size: 12px; border-radius: 4px;"
        "}"
        "QPushButton:hover { background-color: #545d68; color: white; }"
        "QPushButton:pressed { background-color: #373e47; }"
        "QPushButton:disabled { background-color: #2d333b; color: #636e7b; }"
    );

    m_helpBtn = new QPushButton("📖 字典指南", centralWidget);
    m_helpBtn->setMinimumHeight(28);
    m_helpBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #1f6feb; color: white; font-weight: bold; font-size: 12px; border-radius: 4px;"
        "}"
        "QPushButton:hover { background-color: #388bfd; }"
        "QPushButton:pressed { background-color: #1158c7; }"
    );

    subBtnLayout->addWidget(m_updateBtn);
    subBtnLayout->addWidget(m_restoreBtn);
    subBtnLayout->addWidget(m_helpBtn);
    btnLayout->addLayout(subBtnLayout);

    mainLayout->addLayout(btnLayout);

    // 4. 实时日志视窗
    QGroupBox* logGroup = new QGroupBox("执行日志与状态", centralWidget);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(6, 6, 6, 6);

    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #1e1e1e; color: #d4d4d4; font-family: 'Consolas', 'Courier New', monospace;"
        "  font-size: 11px; border: 1px solid #333333; border-radius: 4px;"
        "}"
    );
    logLayout->addWidget(m_logEdit);
    mainLayout->addWidget(logGroup, 1);

    // 5. 底部状态栏
    m_statusLabel = new QLabel("状态: 就绪", this);
    m_statusLabel->setStyleSheet("color: #8b949e; padding-left: 4px; font-size: 11px;");
    statusBar()->addWidget(m_statusLabel);
}

void MainWindow::populateAddons() {
    m_addonCombo->clear();
    std::vector<std::wstring> addons = Cs2Detector::GetAvailableAddons(m_cs2Root);

    if (addons.empty()) {
        m_addonCombo->addItem("addon_template (默认模组)");
    } else {
        for (const auto& addon : addons) {
            m_addonCombo->addItem(QString::fromStdWString(addon));
        }
    }
}

void MainWindow::loadSettings() {
    fs::path configPath = fs::path(m_workingDir) / L"config.ini";
    QSettings settings(QString::fromStdWString(configPath.wstring()), QSettings::IniFormat);

    QString savedAddon = settings.value("Launcher/SelectedAddon", "").toString().trimmed();
    QString savedArgs = settings.value("Launcher/LaunchArgs", "").toString();

    // 1. 恢复附加启动参数
    m_argsEdit->setText(savedArgs);

    // 2. 恢复选择的目标 Addon
    if (!savedAddon.isEmpty()) {
        int index = m_addonCombo->findText(savedAddon);
        if (index == -1) {
            // 前缀匹配（例如 savedAddon 为 "addon_template"，而列表中为 "addon_template (默认模组)"）
            for (int i = 0; i < m_addonCombo->count(); ++i) {
                QString itemText = m_addonCombo->itemText(i);
                if (itemText == savedAddon || itemText.startsWith(savedAddon + " ")) {
                    index = i;
                    break;
                }
            }
        }

        if (index != -1) {
            m_addonCombo->setCurrentIndex(index);
        } else {
            // 已保存的 ADDON 在当前列表中未找到，自动切换到列表第一个
            if (m_addonCombo->count() > 0) {
                m_addonCombo->setCurrentIndex(0);
            }
        }
    } else {
        // 已保存的 ADDON 为空则自动切换到列表第一个
        if (m_addonCombo->count() > 0) {
            m_addonCombo->setCurrentIndex(0);
        }
    }
}

void MainWindow::saveSettings() {
    fs::path configPath = fs::path(m_workingDir) / L"config.ini";
    QSettings settings(QString::fromStdWString(configPath.wstring()), QSettings::IniFormat);

    QString selectedAddon = m_addonCombo->currentText().trimmed();
    if (selectedAddon.contains(" ")) {
        selectedAddon = selectedAddon.split(" ").first();
    }

    settings.setValue("Launcher/SelectedAddon", selectedAddon);
    settings.setValue("Launcher/LaunchArgs", m_argsEdit->text());
    settings.sync();
}

void MainWindow::appendLog(const QString& msg, const QString& color) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString formattedMsg = QString("<span style='color: #6a9955;'>[%1]</span> <span style='color: %2;'>%3</span>")
                               .arg(timeStr, color, msg.toHtmlEscaped());
    m_logEdit->append(formattedMsg);
}

void MainWindow::setUiBusy(bool busy) {
    m_launchBtn->setEnabled(!busy);
    m_updateBtn->setEnabled(!busy);
    m_addonCombo->setEnabled(!busy);
    m_argsEdit->setEnabled(!busy);
    if (busy) {
        m_restoreBtn->setEnabled(false);
    } else {
        updateRestoreButtonState();
    }
}

void MainWindow::updateRestoreButtonState() {
    if (m_isHammerRunning) {
        m_restoreBtn->setEnabled(false);
        m_restoreBtn->setToolTip("Hammer 正在运行中，无法执行还原");
        return;
    }
    fs::path backupDir = fs::path(m_workingDir) / L"backup";
    bool hasBackup = BackupManager::HasBackup(backupDir.wstring());
    m_restoreBtn->setEnabled(hasBackup);
    if (hasBackup) {
        m_restoreBtn->setToolTip("从 backup 目录恢复所有原版 FGD 实体定义及 Qt5Core.dll");
    } else {
        m_restoreBtn->setToolTip("当前未检测到原版备份文件 (启动汉化版时会自动创建备份)");
    }
}

void MainWindow::onLaunchClicked() {
    if (m_isHammerRunning) {
        QMessageBox::information(this, "提示", "Hammer 已经在运行中，请勿重复启动！");
        return;
    }

    if (Cs2Detector::IsCs2ProcessRunning()) {
        QMessageBox::warning(
            this,
            "提示",
            "检测到系统中已有 CS2 进程正在运行！\n\n"
            "为防止文件冲突与补丁写入受阻，请先退出当前运行的 CS2 游戏或编辑器，然后再启动汉化版。"
        );
        return;
    }

    saveSettings();

    setUiBusy(true);
    m_statusLabel->setText("状态: 正在部署补丁并准备启动...");

    fs::path workPath(m_workingDir);
    fs::path backupDir = workPath / L"backup";
    fs::path transDir = workPath / L"translations";
    fs::path cs2Bin = fs::path(m_cs2Root) / L"game" / L"bin" / L"win64";

    fs::path fgdDictPath = workPath / L"fgd_translations.json";
    fs::path fgdOverridePath = workPath / L"fgd_override.json";
    fs::path qtDictPath = workPath / L"qt_translations.json";

    std::wstring notice;
    if (FgdTranslator::EnsureFgdDictionaryExists(fgdDictPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
    if (FgdTranslator::EnsureFgdOverrideDictionaryExists(fgdOverridePath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
    if (FgdTranslator::EnsureQtDictionaryExists(qtDictPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }

    fs::path qmDllSrc = workPath / L"qtcore_qm.dll";

    // 步骤 1: 将 CS2 文件夹内的 FGD 按相对路径复制到 backup
    appendLog("[1/6] 正在备份 CS2 FGD 实体定义文件至 backup 目录...", "#e6db74");
    std::vector<std::wstring> backedFgd;
    std::wstring err;
    if (!BackupManager::BackupFgdFiles(m_cs2Root, backupDir.wstring(), backedFgd, err)) {
        appendLog(QString("[-] 备份 FGD 失败: %1").arg(QString::fromStdWString(err)), "#f92672");
        QMessageBox::critical(this, "错误", "备份 CS2 FGD 文件失败，已中止启动！\n" + QString::fromStdWString(err));
        setUiBusy(false);
        m_statusLabel->setText("状态: 准备失败");
        return;
    }
    appendLog(QString("[+] 成功备份 %1 个 FGD 文件至 backup 相对路径").arg(backedFgd.size()), "#a6e22e");

    // 步骤 2: 使用 fgd_translations.json 与 fgd_override.json 汉化输出到 translations 文件夹相对路径，并覆盖到 CS2
    appendLog("[2/6] 正在解析字典与覆盖规则并汉化 FGD 实体定义...", "#e6db74");
    std::vector<std::wstring> transFgd;
    if (!FgdTranslator::TranslateAndDeployAll(m_cs2Root, backupDir.wstring(), transDir.wstring(), fgdDictPath.wstring(), fgdOverridePath.wstring(), transFgd, err)) {
        appendLog(QString("[-] 汉化 FGD 失败: %1").arg(QString::fromStdWString(err)), "#f92672");
        QMessageBox::critical(this, "错误", "汉化 FGD 文件失败，已中止启动！\n" + QString::fromStdWString(err));
        doRestore(false);
        setUiBusy(false);
        m_statusLabel->setText("状态: 汉化失败");
        return;
    }
    appendLog(QString("[+] 成功汉化并覆盖部署 %1 个 FGD 文件至 CS2 目录").arg(transFgd.size()), "#a6e22e");

    // 步骤 3: 备份原版 Qt5Core.dll 到 backup 文件夹相对路径
    appendLog("[3/6] 正在备份原版 Qt5Core.dll 至 backup 目录...", "#e6db74");
    if (!BackupManager::BackupQtCore(m_cs2Root, backupDir.wstring(), err)) {
        appendLog(QString("[-] 备份 Qt5Core.dll 失败: %1").arg(QString::fromStdWString(err)), "#f92672");
        QMessageBox::critical(this, "错误", "备份 Qt5Core.dll 失败，已中止启动！\n" + QString::fromStdWString(err));
        doRestore(false);
        setUiBusy(false);
        m_statusLabel->setText("状态: 备份失败");
        return;
    }
    appendLog("[+] 原版 Qt5Core.dll 备份成功", "#a6e22e");

    // 步骤 4: 复制 qt_translations.json 与 qtcore_qm.dll 到 Qt5Core.dll 旁，并修补 Qt5Core.dll
    appendLog("[4/6] 正在部署 qt_translations.json 与 qtcore_qm.dll 并修补 Qt5Core.dll...", "#e6db74");
    try {
        fs::path destQtJson = cs2Bin / L"qt_translations.json";
        fs::path destQmDll = cs2Bin / L"qtcore_qm.dll";

        if (!fs::exists(qtDictPath)) {
            appendLog("[-] 找不到界面字典文件 qt_translations.json", "#f92672");
            QMessageBox::critical(this, "错误", "找不到界面字典文件 qt_translations.json！");
            doRestore(false);
            setUiBusy(false);
            return;
        }

        if (!fs::exists(qmDllSrc)) {
            appendLog("[-] 找不到注入模块 qtcore_qm.dll", "#f92672");
            QMessageBox::critical(this, "错误", "找不到注入模块 qtcore_qm.dll！");
            doRestore(false);
            setUiBusy(false);
            return;
        }

        fs::copy_file(qtDictPath, destQtJson, fs::copy_options::overwrite_existing);
        fs::copy_file(qmDllSrc, destQmDll, fs::copy_options::overwrite_existing);

        // 修补 Qt5Core.dll (源文件读取自 backup 中的原版)
        fs::path backupQtCore = backupDir / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
        fs::path targetQtCore = cs2Bin / L"Qt5Core.dll";

        if (!PePatcher::PatchQtCore(backupQtCore.wstring(), targetQtCore.wstring(), err)) {
            appendLog(QString("[-] 修补 Qt5Core.dll 失败: %1").arg(QString::fromStdWString(err)), "#f92672");
            QMessageBox::critical(this, "错误", "修补 Qt5Core.dll 失败！\n" + QString::fromStdWString(err));
            doRestore(false);
            setUiBusy(false);
            m_statusLabel->setText("状态: PE 修补失败");
            return;
        }
        appendLog("[+] Qt5Core.dll PE Code Cave 注入与重定向修补成功", "#a6e22e");
    } catch (const std::exception& e) {
        appendLog(QString("[-] 部署补丁异常: %1").arg(e.what()), "#f92672");
        QMessageBox::critical(this, "错误", QString("部署补丁异常: %1").arg(e.what()));
        doRestore(false);
        setUiBusy(false);
        return;
    }

    // 步骤 5: 启动 Hammer
    appendLog("[5/6] 正在启动 CS2 Hammer 编辑器...", "#e6db74");

    QString selectedAddon = m_addonCombo->currentText().trimmed();
    if (selectedAddon.contains(" ")) {
        selectedAddon = selectedAddon.split(" ").first();
    }
    if (selectedAddon.isEmpty()) {
        selectedAddon = "addon_template";
    }

    QString cs2ExePath = QString::fromStdWString((cs2Bin / L"cs2.exe").wstring());

    QStringList processArgs;
    processArgs << "-addon" << selectedAddon << "-tools";

    QString customArgs = m_argsEdit->text().trimmed();
    if (!customArgs.isEmpty()) {
        QStringList userTokens = QProcess::splitCommand(customArgs);
        processArgs.append(userTokens);
    }

    m_hammerProcess->setProgram(cs2ExePath);
    m_hammerProcess->setArguments(processArgs);
    m_hammerProcess->setWorkingDirectory(QString::fromStdWString(cs2Bin.wstring()));
    m_hammerProcess->setProcessChannelMode(QProcess::ForwardedChannels);

    appendLog(QString("[*] 执行命令: %1 %2").arg(cs2ExePath, processArgs.join(" ")), "#75715e");

    // 保存运行会话状态，防止运行期间程序被强杀导致下次启动丢失备份
    BackupManager::SaveSessionState(m_workingDir, true);
    m_isHammerRunning = true;
    m_notRunningCount = 0;
    m_statusLabel->setText("状态: Hammer 编辑器正在启动...");
    m_launchBtn->setText("⏳ Hammer 运行中...");
    m_launchBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_restoreBtn->setEnabled(false);

    m_hammerProcess->start();
    m_monitorTimer->start(1000);
}

void MainWindow::onHammerStarted() {
    m_isHammerRunning = true;
    m_notRunningCount = 0;
    m_hammerPid = m_hammerProcess->processId();
    if (m_hammerProcessHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_hammerProcessHandle));
        m_hammerProcessHandle = nullptr;
    }
    if (m_hammerPid > 0) {
        m_hammerProcessHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(m_hammerPid));
    }
    m_statusLabel->setText("状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)");
    appendLog(QString("[+] CS2 Hammer 进程已成功启动 (PID: %1)！正在监听运行生命周期...").arg(m_hammerPid), "#a6e22e");
}

void MainWindow::onHammerFinished(int exitCode, QProcess::ExitStatus exitStatus) {
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
    if (m_hammerProcess && m_hammerProcess->state() == QProcess::Running) {
        isRunning = true;
    } else if (m_hammerProcessHandle != nullptr) {
        DWORD waitRes = WaitForSingleObject(static_cast<HANDLE>(m_hammerProcessHandle), 0);
        if (waitRes == WAIT_TIMEOUT) {
            isRunning = true;
        }
    } else if (m_hammerPid > 0) {
        isRunning = Cs2Detector::IsProcessRunning(static_cast<DWORD>(m_hammerPid));
    }

    if (isRunning) {
        m_notRunningCount = 0;
        m_statusLabel->setText("状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)");
    } else {
        m_notRunningCount++;
        // 确认本程序启动的子进程已退出
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
        CloseHandle(static_cast<HANDLE>(m_hammerProcessHandle));
        m_hammerProcessHandle = nullptr;
    }
    m_hammerPid = 0;

    appendLog("[*] 检测到本程序启动的 CS2 与 Hammer 已退出，正在执行自动安全还原...", "#66d9ef");
    m_statusLabel->setText("状态: 正在恢复原版文件...");

    bool restoreOk = doRestore(true);
    if (restoreOk) {
        BackupManager::ClearSessionState(m_workingDir);
        m_statusLabel->setText("状态: 就绪 (原版环境已安全恢复)");
    } else {
        m_statusLabel->setText("状态: 恢复备份遇到占用，请手动点击【还原原版备份】");
        appendLog("[-] 部分原版文件还原失败（可能仍被其他程序占用），未清除会话状态以保护原版备份。", "#f92672");
        QMessageBox::warning(
            this,
            "恢复提示",
            "自动恢复原版文件时遇到部分文件被占用或写入受阻！\n\n"
            "请确认 CS2 与 Hammer 是否已完全退出，然后可手动点击【还原原版备份】。\n"
            "（启动器已安全保留会话状态与原版备份，下次启动也会自动再次尝试恢复）。"
        );
    }

    m_launchBtn->setText("🚀 启动 CS2 Workshop Tools (汉化版)");
    setUiBusy(false);
    updateRestoreButtonState();
}

void MainWindow::onHammerError(QProcess::ProcessError error) {
    if (!m_isHammerRunning) {
        appendLog(QString("[-] 启动 Hammer 进程出错 (错误码: %1)").arg(error), "#f92672");
        QMessageBox::critical(this, "启动错误", "无法启动 cs2.exe 进程，请检查 CS2 路径与游戏完整性。");
        doRestore(true);
        setUiBusy(false);
        m_statusLabel->setText("状态: 启动出错");
    }
}

bool MainWindow::doRestore(bool showLog) {
    fs::path workPath(m_workingDir);
    fs::path backupDir = workPath / L"backup";
    if (!BackupManager::HasBackup(backupDir.wstring())) {
        return true;
    }

    if (showLog) appendLog("[*] 正在还原原版 FGD 实体定义及核心二进制...", "#66d9ef");
    std::wstring err;
    if (!BackupManager::RestoreAll(m_cs2Root, backupDir.wstring(), err)) {
        if (showLog) appendLog(QString("[-] 还原备份失败: %1").arg(QString::fromStdWString(err)), "#f92672");
        return false;
    }

    if (showLog) appendLog("[SUCCESS] 还原操作完成！所有原版 FGD 实体定义及 Qt5Core.dll 已恢复原样。", "#a6e22e");
    return true;
}

void MainWindow::fetchUrlCandidates(const QStringList& urls, std::function<void(bool success, const QByteArray& data)> callback) {
    if (urls.isEmpty()) {
        callback(false, QByteArray());
        return;
    }

    auto fetchNext = std::make_shared<std::function<void(int)>>();
    *fetchNext = [this, urls, callback, fetchNext](int index) {
        if (index >= urls.size()) {
            callback(false, QByteArray());
            return;
        }

        QUrl url(urls[index]);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader, "CS2WorkshopToolsLocalizerCN");
        request.setTransferTimeout(10000);

        QNetworkReply* reply = m_networkManager->get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, index, callback, fetchNext]() {
            reply->deleteLater();
            int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && statusCode == 200) {
                QByteArray data = reply->readAll();
                if (!data.isEmpty()) {
                    callback(true, data);
                    return;
                }
            }
            (*fetchNext)(index + 1);
        });
    };

    (*fetchNext)(0);
}

void MainWindow::onUpdateTranslationsClicked() {
    if (m_isHammerRunning) {
        QMessageBox::warning(this, "警告", "Hammer 正在运行中，无法在运行时更新词典！");
        return;
    }
    int ret = QMessageBox::question(
        this,
        "确认更新在线翻译",
        "是否从 GitHub 仓库获取并更新最新的汉化词典文件？\n\n"
        "提示：此操作将使用在线最新词典覆盖本地的 qt_translations.json、fgd_translations.json 与 fgd_override.json。\n"
        "若您之前手动自定义过本地词典，请注意备份。",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (ret != QMessageBox::Yes) {
        return;
    }

    setUiBusy(true);
    m_statusLabel->setText("状态: 正在从 GitHub 获取最新翻译词典...");
    appendLog("[*] 正在从 GitHub 官方仓库下载最新词典文件...", "#66d9ef");

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

    auto stripJsonc = [](const QByteArray& input) -> QByteArray {
        QByteArray output;
        output.reserve(input.size());
        const char* p = input.constData();
        const char* end = p + input.size();

        while (p < end) {
            if (*p == '"') {
                output.append(*p++);
                while (p < end) {
                    char c = *p++;
                    output.append(c);
                    if (c == '"') break;
                    if (c == '\\' && p < end) output.append(*p++);
                }
            } else if (*p == '/' && (p + 1 < end)) {
                if (*(p + 1) == '/') {
                    p += 2;
                    while (p < end && *p != '\n' && *p != '\r') p++;
                } else if (*(p + 1) == '*') {
                    p += 2;
                    while (p + 1 < end && !(*p == '*' && *(p + 1) == '/')) p++;
                    if (p + 1 < end) p += 2;
                    else p = end;
                } else {
                    output.append(*p++);
                }
            } else {
                output.append(*p++);
            }
        }
        return output;
    };

    // 1. Fetch qt_translations.json
    appendLog("[1/3] 正在获取 qt_translations.json (界面词典)...", "#e6db74");
    fetchUrlCandidates(qtUrls, [this, fgdUrls, overrideUrls, stripJsonc](bool qtOk, const QByteArray& qtData) {
        if (!qtOk) {
            appendLog("[-] 获取 qt_translations.json 失败：所有节点连接超时或不可达，请检查网络或代理设置。", "#f92672");
            QMessageBox::critical(this, "更新失败", "获取 qt_translations.json 失败！\n无法连接到 GitHub 仓库，请检查您的网络连接或代理设置。");
            setUiBusy(false);
            m_statusLabel->setText("状态: 词典更新失败");
            return;
        }

        QJsonParseError qtParseErr;
        QJsonDocument qtDoc = QJsonDocument::fromJson(stripJsonc(qtData), &qtParseErr);
        if (qtParseErr.error != QJsonParseError::NoError || !qtDoc.isObject() || qtDoc.object().isEmpty()) {
            appendLog(QString("[-] 解析 qt_translations.json 失败: %1").arg(qtParseErr.errorString()), "#f92672");
            QMessageBox::critical(this, "更新失败", "下载的 qt_translations.json 格式异常或内容为空，已放弃更新。");
            setUiBusy(false);
            m_statusLabel->setText("状态: 词典校验失败");
            return;
        }

        qsizetype qtCount = qtDoc.object().keys().size();
        appendLog(QString("[+] qt_translations.json 获取成功，有效词条: %1 条").arg(qtCount), "#a6e22e");

        // 2. Fetch fgd_translations.json
        appendLog("[2/3] 正在获取 fgd_translations.json (实体定义词典)...", "#e6db74");
        fetchUrlCandidates(fgdUrls, [this, overrideUrls, qtData, qtCount, stripJsonc](bool fgdOk, const QByteArray& fgdData) {
            if (!fgdOk) {
                appendLog("[-] 获取 fgd_translations.json 失败：所有节点连接超时或不可达，请检查网络设置。", "#f92672");
                QMessageBox::critical(this, "更新失败", "获取 fgd_translations.json 失败！\n无法连接到 GitHub 仓库，请检查网络连接。");
                setUiBusy(false);
                m_statusLabel->setText("状态: 词典更新失败");
                return;
            }

            QJsonParseError fgdParseErr;
            QJsonDocument fgdDoc = QJsonDocument::fromJson(stripJsonc(fgdData), &fgdParseErr);
            if (fgdParseErr.error != QJsonParseError::NoError || !fgdDoc.isObject() || fgdDoc.object().isEmpty()) {
                appendLog(QString("[-] 解析 fgd_translations.json 失败: %1").arg(fgdParseErr.errorString()), "#f92672");
                QMessageBox::critical(this, "更新失败", "下载的 fgd_translations.json 格式异常或内容为空，已放弃更新。");
                setUiBusy(false);
                m_statusLabel->setText("状态: 词典校验失败");
                return;
            }

            qsizetype fgdCount = fgdDoc.object().keys().size();
            appendLog(QString("[+] fgd_translations.json 获取成功，有效词条: %1 条").arg(fgdCount), "#a6e22e");

            // 3. Fetch fgd_override.json
            appendLog("[3/3] 正在获取 fgd_override.json (实体覆盖词典)...", "#e6db74");
            fetchUrlCandidates(overrideUrls, [this, qtData, fgdData, qtCount, fgdCount, stripJsonc](bool overrideOk, const QByteArray& overrideData) {
                if (!overrideOk) {
                    appendLog("[-] 获取 fgd_override.json 失败：所有节点连接超时或不可达，请检查网络设置。", "#f92672");
                    QMessageBox::critical(this, "更新失败", "获取 fgd_override.json 失败！\n无法连接到 GitHub 仓库，请检查网络连接。");
                    setUiBusy(false);
                    m_statusLabel->setText("状态: 词典更新失败");
                    return;
                }

                QJsonParseError overrideParseErr;
                QJsonDocument overrideDoc = QJsonDocument::fromJson(stripJsonc(overrideData), &overrideParseErr);
                if (overrideParseErr.error != QJsonParseError::NoError || !overrideDoc.isObject() || overrideDoc.object().isEmpty()) {
                    appendLog(QString("[-] 解析 fgd_override.json 失败: %1").arg(overrideParseErr.errorString()), "#f92672");
                    QMessageBox::critical(this, "更新失败", "下载的 fgd_override.json 格式异常或内容为空，已放弃更新。");
                    setUiBusy(false);
                    m_statusLabel->setText("状态: 词典校验失败");
                    return;
                }

                qsizetype overrideCount = 0;
                QJsonObject overrideObj = overrideDoc.object();
                if (overrideObj.contains("properties") && overrideObj["properties"].isObject()) {
                    overrideCount += overrideObj["properties"].toObject().keys().size();
                }
                if (overrideObj.contains("io") && overrideObj["io"].isObject()) {
                    overrideCount += overrideObj["io"].toObject().keys().size();
                }
                if (overrideObj.contains("classes") && overrideObj["classes"].isObject()) {
                    overrideCount += overrideObj["classes"].toObject().keys().size();
                }
                for (auto it = overrideObj.begin(); it != overrideObj.end(); ++it) {
                    if (it.key() != "properties" && it.key() != "io" && it.key() != "classes" && !it.key().startsWith("_")) {
                        overrideCount++;
                    }
                }
                if (overrideCount == 0) {
                    overrideCount = overrideObj.keys().size();
                }

                appendLog(QString("[+] fgd_override.json 获取成功，有效规则: %1 条").arg(overrideCount), "#a6e22e");

                // Save all three files
                QString qtLocalPath = QString::fromStdWString((fs::path(m_workingDir) / L"qt_translations.json").wstring());
                QString fgdLocalPath = QString::fromStdWString((fs::path(m_workingDir) / L"fgd_translations.json").wstring());
                QString overrideLocalPath = QString::fromStdWString((fs::path(m_workingDir) / L"fgd_override.json").wstring());

                QFile qtFile(qtLocalPath);
                if (!qtFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    appendLog("[-] 写入本地 qt_translations.json 失败！", "#f92672");
                    QMessageBox::critical(this, "写入失败", "写入本地 qt_translations.json 失败，请检查文件权限。");
                    setUiBusy(false);
                    m_statusLabel->setText("状态: 写入失败");
                    return;
                }
                qtFile.write(qtData);
                qtFile.close();

                QFile fgdFile(fgdLocalPath);
                if (!fgdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    appendLog("[-] 写入本地 fgd_translations.json 失败！", "#f92672");
                    QMessageBox::critical(this, "写入失败", "写入本地 fgd_translations.json 失败，请检查文件权限。");
                    setUiBusy(false);
                    m_statusLabel->setText("状态: 写入失败");
                    return;
                }
                fgdFile.write(fgdData);
                fgdFile.close();

                QFile overrideFile(overrideLocalPath);
                if (!overrideFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    appendLog("[-] 写入本地 fgd_override.json 失败！", "#f92672");
                    QMessageBox::critical(this, "写入失败", "写入本地 fgd_override.json 失败，请检查文件权限。");
                    setUiBusy(false);
                    m_statusLabel->setText("状态: 写入失败");
                    return;
                }
                overrideFile.write(overrideData);
                overrideFile.close();

                appendLog(QString("[SUCCESS] 翻译词典更新成功！(界面: %1, 实体: %2, 覆盖: %3)").arg(qtCount).arg(fgdCount).arg(overrideCount), "#a6e22e");
                m_statusLabel->setText("状态: 在线词典更新成功");
                setUiBusy(false);

                QMessageBox::information(
                    this,
                    "更新成功",
                    QString("已成功从 GitHub 获取并更新最新汉化词典！\n\n"
                            "- 界面词典 (qt_translations.json): %1 条\n"
                            "- 实体词典 (fgd_translations.json): %2 条\n"
                            "- 覆盖词典 (fgd_override.json): %3 条\n\n"
                            "点击“启动 CS2 Workshop Tools (汉化版)”即可应用最新汉化。").arg(qtCount).arg(fgdCount).arg(overrideCount)
                );
            });
        });
    });
}

void MainWindow::onRestoreClicked() {
    if (m_isHammerRunning || Cs2Detector::IsCs2ProcessRunning()) {
        QMessageBox::warning(this, "警告", "CS2 / Hammer 正在运行中，无法在运行时执行手动还原！\n请先退出 CS2 或 Hammer。");
        return;
    }

    fs::path backupDir = fs::path(m_workingDir) / L"backup";
    if (!BackupManager::HasBackup(backupDir.wstring())) {
        QMessageBox::information(this, "提示", "未检测到原版备份文件，无需执行还原。");
        updateRestoreButtonState();
        return;
    }

    int ret = QMessageBox::question(this, "确认还原", "是否确认将 backup 中的所有原始文件覆盖恢复到 CS2 目录？", QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        if (doRestore(true)) {
            BackupManager::ClearSessionState(m_workingDir);
            QMessageBox::information(this, "成功", "所有原始文件已成功恢复！");
        } else {
            QMessageBox::critical(this, "失败", "还原过程中遇到错误，请查看日志。");
        }
        updateRestoreButtonState();
    }
}

void MainWindow::checkAndRecoverAbnormalExit() {
    fs::path workPath(m_workingDir);
    fs::path backupDir = workPath / L"backup";

    bool hasUnrestored = BackupManager::HasUnrestoredSession(m_workingDir) ||
                         (BackupManager::HasBackup(backupDir.wstring()) && BackupManager::IsPatchDeployed(m_cs2Root));

    if (!hasUnrestored) {
        return;
    }

    appendLog("[!] 检测到上一次程序未正常结束或存在未还原的补丁文件，正在进行安全恢复检查...", "#fd971f");

    // 检查 CS2 是否仍在运行
    while (Cs2Detector::IsCs2ProcessRunning()) {
        int ret = QMessageBox::warning(
            this,
            "检测到 CS2 正在运行",
            "检测到上一次程序未正常结束，且 CS2 (cs2.exe) 目前仍在运行中！\n\n"
            "为确保原版文件能够被安全恢复，请先退出 CS2 游戏或 Hammer 编辑器，然后点击【重试】。\n"
            "若点击【取消】，将推迟恢复（注意：未还原状态下直接启动可能导致原版备份异常）。",
            QMessageBox::Retry | QMessageBox::Cancel,
            QMessageBox::Retry
        );

        if (ret == QMessageBox::Cancel) {
            appendLog("[-] 用户推迟了异常退出的自动还原流程", "#f92672");
            return;
        }
    }

    // CS2 进程已退出，自动执行还原
    appendLog("[*] 正在自动从 backup 目录恢复纯净原版文件...", "#66d9ef");
    if (doRestore(true)) {
        BackupManager::ClearSessionState(m_workingDir);
        appendLog("[SUCCESS] 上次异常退出遗留的文件已成功还原为纯净原版备份！", "#a6e22e");
        updateRestoreButtonState();
        QMessageBox::information(
            this,
            "自动恢复成功",
            "检测到上一次程序未正常结束（如意外关闭、断电或崩溃）。\n\n"
            "启动器已自动将 CS2 原始文件完整恢复，以确保您的游戏文件纯净无损。"
        );
    } else {
        appendLog("[-] 自动恢复备份失败，请检查文件占用或稍后点击【还原原版备份】", "#f92672");
    }
}

void MainWindow::onHelpClicked() {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("翻译字典格式与填写指南");
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setText(
        "<h3>📚 CS2 Workshop Tools 汉化字典格式与编写指南</h3>"
        "<p>所有翻译字典均采用标准 <b>UTF-8 JSON / JSONC</b> 键值对格式：<code>\"英文原词\": \"中文翻译\"</code></p>"
        "<hr/>"
        "<h4>1. 实体定义翻译字典 (<code>fgd_translations.json</code>)</h4>"
        "<ul>"
        "<li><b>作用</b>：精准匹配替换 FGD 实体定义中已有的英文字符串。</li>"
        "<li><b>实体类说明</b>：例如 <code>\"Omnidirectional point light\": \"全向点光源\"</code></li>"
        "<li><b>属性显示名</b>：例如 <code>\"Light Source\": \"光源\"</code>、<code>\"Name\": \"名称\"</code></li>"
        "<li><b>属性悬停描述</b>：例如 <code>\"The name that other entities use...\": \"其他实体用于引用的名称。\"</code></li>"
        "<li><b>选项与标记</b>：例如 <code>\"Enabled\": \"已启用\"</code>、<code>\"Disabled\": \"已禁用\"</code></li>"
        "<li><b>输入/输出 (I/O) 说明</b>：例如 <code>\"Removes this entity from the world.\": \"从世界中移除此实体。\"</code></li>"
        "</ul>"
        "<hr/>"
        "<h4>2. 实体键值描述补充与覆盖字典 (<code>fgd_override.json</code>)</h4>"
        "<ul>"
        "<li><b>作用</b>：针对特定属性名 (Key)、实体类或 I/O <b>补充缺失描述</b>或<b>强制覆盖说明</b>。</li>"
        "<li><b>属性描述补充</b>：在 <code>properties</code> 分组中配置，如 <code>\"bodygroups\": \"设置模型的子部件与可选网格组合。\"</code></li>"
        "<li><b>I/O 说明补充</b>：在 <code>io</code> 分组中配置，如 <code>\"SetParent\": \"设置该实体的父级层级对象。\"</code></li>"
        "<li><b>实体类说明</b>：在 <code>classes</code> 分组中配置，如 <code>\"info_node\": \"AI 地面导航节点。\"</code></li>"
        "</ul>"
        "<hr/>"
        "<h4>3. 界面核心字典 (<code>qt_translations.json</code>)</h4>"
        "<ul>"
        "<li><b>主菜单与工具栏</b>：例如 <code>\"File\": \"文件\"</code>、<code>\"Clipping Tool\": \"剪切工具\"</code></li>"
        "<li><b>通用属性面板</b>：例如 <code>\"Transform Locked\": \"变换锁定\"</code>、<code>\"Pinned To\": \"固定至\"</code></li>"
        "<li><b>⚡ 快捷键自动适配</b>：对于带有快捷键后缀的文本（如 <code>[Shift+X]</code>、<code>(Ctrl+Z)</code>、<code>\\tCtrl+S</code>、<code>...</code>、<code>:</code>），<b>只需翻译基础英文</b>，快捷键后缀会被引擎自动保留和拼接，无需手动输入！</li>"
        "</ul>"
        "<hr/>"
        "<p>💡 <b>修改即生效</b>：直接用文本编辑器编辑上述 JSON 文件并保存，再次在启动器中点击 <b>启动</b> 即可立即生效！</p>"
    );
    msgBox.setIcon(QMessageBox::Information);
    msgBox.exec();
}

void MainWindow::closeEvent(QCloseEvent *event) {
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

    // 确保退出时尝试清理与恢复
    if (doRestore(false)) {
        BackupManager::ClearSessionState(m_workingDir);
    }
    event->accept();
}
