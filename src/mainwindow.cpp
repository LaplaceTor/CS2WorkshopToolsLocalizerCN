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
#include <QFileInfo>
#include <QDir>
#include <filesystem>

namespace fs = std::filesystem;

MainWindow::MainWindow(const std::wstring& cs2Root, QWidget *parent)
    : QMainWindow(parent)
    , m_cs2Root(cs2Root)
    , m_hammerProcess(new QProcess(this))
    , m_isHammerRunning(false)
{
    // 获取当前工作目录
    try {
        m_workingDir = fs::current_path().wstring();
    } catch (...) {
        wchar_t curDir[MAX_PATH] = {0};
        GetCurrentDirectoryW(MAX_PATH, curDir);
        m_workingDir = curDir;
    }

    setupUi();
    populateAddons();

    connect(m_launchBtn, &QPushButton::clicked, this, &MainWindow::onLaunchClicked);
    connect(m_restoreBtn, &QPushButton::clicked, this, &MainWindow::onRestoreClicked);
    connect(m_helpBtn, &QPushButton::clicked, this, &MainWindow::onHelpClicked);
    connect(m_hammerProcess, &QProcess::started, this, &MainWindow::onHammerStarted);
    connect(m_hammerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::onHammerFinished);
    connect(m_hammerProcess, &QProcess::errorOccurred, this, &MainWindow::onHammerError);

    appendLog("==================================================", "#66d9ef");
    appendLog(" CS2HammerTranslateCN 深度汉化启动器已就绪", "#a6e22e");
    appendLog("==================================================", "#66d9ef");
    appendLog(QString("已锁定 CS2 安装目录: %1").arg(QString::fromStdWString(m_cs2Root)), "#f8f8f2");

    // 检查并生成翻译字典文件
    fs::path fgdPath = fs::path(m_workingDir) / L"fgd_translations.json";
    fs::path qtPath = fs::path(m_workingDir) / L"qt_translations.json";

    std::wstring notice;
    if (FgdTranslator::EnsureFgdDictionaryExists(fgdPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
    if (FgdTranslator::EnsureQtDictionaryExists(qtPath.wstring(), L"", notice)) {
        appendLog("[i] " + QString::fromStdWString(notice), "#66d9ef");
    }
}

MainWindow::~MainWindow() {
}

void MainWindow::setupUi() {
    setWindowTitle("CS2 Hammer 深度汉化启动器 - CS2HammerTranslateCN");
    resize(760, 560);
    setMinimumSize(680, 480);

    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // 1. 顶部 CS2 路径信息卡片
    QGroupBox* pathGroup = new QGroupBox("CS2 路径信息", centralWidget);
    QHBoxLayout* pathLayout = new QHBoxLayout(pathGroup);
    m_cs2PathLabel = new QLabel(QString::fromStdWString(m_cs2Root), pathGroup);
    m_cs2PathLabel->setStyleSheet("font-weight: bold; color: #4ec9b0;");
    m_cs2PathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLayout->addWidget(m_cs2PathLabel);
    mainLayout->addWidget(pathGroup);

    // 2. 参数与 Addon 选择卡片
    QGroupBox* configGroup = new QGroupBox("启动配置", centralWidget);
    QGridLayout* configLayout = new QGridLayout(configGroup);
    configLayout->setHorizontalSpacing(12);
    configLayout->setVerticalSpacing(10);

    QLabel* addonLabel = new QLabel("目标 Addon 模组:", configGroup);
    m_addonCombo = new QComboBox(configGroup);
    m_addonCombo->setMinimumHeight(32);
    configLayout->addWidget(addonLabel, 0, 0);
    configLayout->addWidget(m_addonCombo, 0, 1);

    QLabel* argsLabel = new QLabel("附加启动参数:", configGroup);
    m_argsEdit = new QLineEdit(configGroup);
    m_argsEdit->setMinimumHeight(32);
    m_argsEdit->setPlaceholderText("例如: -novid -language simplified_chinese (可选)");
    configLayout->addWidget(argsLabel, 1, 0);
    configLayout->addWidget(m_argsEdit, 1, 1);

    mainLayout->addWidget(configGroup);

    // 3. 操作按钮栏
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(12);

    m_launchBtn = new QPushButton("🚀 启动 CS2 Hammer (汉化版)", centralWidget);
    m_launchBtn->setMinimumHeight(42);
    m_launchBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #2ea043; color: white; font-weight: bold; font-size: 14px; border-radius: 6px;"
        "}"
        "QPushButton:hover { background-color: #3fb950; }"
        "QPushButton:pressed { background-color: #238636; }"
        "QPushButton:disabled { background-color: #555555; color: #888888; }"
    );

    m_restoreBtn = new QPushButton("🔄 还原原版备份", centralWidget);
    m_restoreBtn->setMinimumHeight(42);
    m_restoreBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #444c56; color: #adbac7; font-weight: bold; font-size: 13px; border-radius: 6px;"
        "}"
        "QPushButton:hover { background-color: #545d68; color: white; }"
        "QPushButton:pressed { background-color: #373e47; }"
        "QPushButton:disabled { background-color: #2d333b; color: #636e7b; }"
    );

    m_helpBtn = new QPushButton("📖 字典指南", centralWidget);
    m_helpBtn->setMinimumHeight(42);
    m_helpBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #1f6feb; color: white; font-weight: bold; font-size: 13px; border-radius: 6px;"
        "}"
        "QPushButton:hover { background-color: #388bfd; }"
        "QPushButton:pressed { background-color: #1158c7; }"
    );

    btnLayout->addWidget(m_launchBtn, 3);
    btnLayout->addWidget(m_restoreBtn, 1);
    btnLayout->addWidget(m_helpBtn, 1);
    mainLayout->addLayout(btnLayout);

    // 4. 实时日志视窗
    QGroupBox* logGroup = new QGroupBox("执行日志与状态", centralWidget);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(8, 8, 8, 8);

    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #1e1e1e; color: #d4d4d4; font-family: 'Consolas', 'Courier New', monospace;"
        "  font-size: 12px; border: 1px solid #333333; border-radius: 4px;"
        "}"
    );
    logLayout->addWidget(m_logEdit);
    mainLayout->addWidget(logGroup, 1);

    // 5. 底部状态栏
    m_statusLabel = new QLabel("状态: 就绪", this);
    m_statusLabel->setStyleSheet("color: #8b949e; padding-left: 4px;");
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

void MainWindow::appendLog(const QString& msg, const QString& color) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString formattedMsg = QString("<span style='color: #6a9955;'>[%1]</span> <span style='color: %2;'>%3</span>")
                               .arg(timeStr, color, msg.toHtmlEscaped());
    m_logEdit->append(formattedMsg);
}

void MainWindow::setUiBusy(bool busy) {
    m_launchBtn->setEnabled(!busy);
    m_restoreBtn->setEnabled(!busy);
    m_addonCombo->setEnabled(!busy);
    m_argsEdit->setEnabled(!busy);
}

void MainWindow::onLaunchClicked() {
    if (m_isHammerRunning) {
        QMessageBox::information(this, "提示", "Hammer 已经在运行中，请勿重复启动！");
        return;
    }

    setUiBusy(true);
    m_statusLabel->setText("状态: 正在部署补丁并准备启动...");

    fs::path workPath(m_workingDir);
    fs::path backupDir = workPath / L"backup";
    fs::path transDir = workPath / L"translations";
    fs::path cs2Bin = fs::path(m_cs2Root) / L"game" / L"bin" / L"win64";

    fs::path fgdDictPath = workPath / L"fgd_translations.json";
    fs::path qtDictPath = workPath / L"qt_translations.json";

    std::wstring notice;
    if (FgdTranslator::EnsureFgdDictionaryExists(fgdDictPath.wstring(), L"", notice)) {
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

    // 步骤 2: 使用 fgd_translations.json 汉化输出到 translations 文件夹相对路径，并覆盖到 CS2
    appendLog("[2/6] 正在解析字典并汉化 FGD 实体定义...", "#e6db74");
    std::vector<std::wstring> transFgd;
    if (!FgdTranslator::TranslateAndDeployAll(m_cs2Root, backupDir.wstring(), transDir.wstring(), fgdDictPath.wstring(), transFgd, err)) {
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

    appendLog(QString("[*] 执行命令: %1 %2").arg(cs2ExePath, processArgs.join(" ")), "#75715e");
    m_hammerProcess->start();
}

void MainWindow::onHammerStarted() {
    m_isHammerRunning = true;
    m_statusLabel->setText("状态: Hammer 编辑器正在运行中 (退出后将自动恢复备份)");
    m_launchBtn->setText("⏳ Hammer 运行中...");
    m_launchBtn->setEnabled(false);
    m_restoreBtn->setEnabled(false);
    appendLog("[+] CS2 Hammer 进程已成功启动！正在监听运行生命周期...", "#a6e22e");
}

void MainWindow::onHammerFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitStatus);
    m_isHammerRunning = false;
    appendLog(QString("[*] 检测到 CS2 Hammer 进程已退出 (代码: %1)，正在执行自动还原...").arg(exitCode), "#66d9ef");
    m_statusLabel->setText("状态: 正在恢复原版文件...");

    doRestore(true);

    m_launchBtn->setText("🚀 启动 CS2 Hammer (汉化版)");
    setUiBusy(false);
    m_statusLabel->setText("状态: 就绪 (原版环境已安全恢复)");
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
    std::wstring err;

    if (!fs::exists(backupDir)) {
        if (showLog) appendLog("[*] 未发现 backup 备份目录，无需还原。", "#75715e");
        return true;
    }

    if (showLog) appendLog("[*] 正在从 backup 目录恢复所有原始 FGD 与 Qt5Core.dll...", "#e6db74");

    if (!BackupManager::RestoreAll(m_cs2Root, backupDir.wstring(), err)) {
        if (showLog) appendLog(QString("[-] 还原备份失败: %1").arg(QString::fromStdWString(err)), "#f92672");
        return false;
    }

    if (showLog) appendLog("[SUCCESS] 还原操作完成！所有原版 FGD 实体定义及 Qt5Core.dll 已恢复原样。", "#a6e22e");
    return true;
}

void MainWindow::onRestoreClicked() {
    if (m_isHammerRunning) {
        QMessageBox::warning(this, "警告", "Hammer 正在运行中，无法在运行时执行手动还原！");
        return;
    }

    int ret = QMessageBox::question(this, "确认还原", "是否确认将 backup 中的所有原始文件覆盖恢复到 CS2 目录？", QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        if (doRestore(true)) {
            QMessageBox::information(this, "成功", "所有原始文件已成功恢复！");
        } else {
            QMessageBox::critical(this, "失败", "还原过程中遇到错误，请查看日志。");
        }
    }
}

void MainWindow::onHelpClicked() {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("翻译字典格式与填写指南");
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setText(
        "<h3>📚 CS2 Hammer 汉化字典格式与编写指南</h3>"
        "<p>所有翻译字典均采用标准 <b>UTF-8 JSON</b> 键值对格式：<code>\"英文原词\": \"中文翻译\"</code></p>"
        "<hr/>"
        "<h4>1. 实体定义字典 (<code>fgd_translations.json</code>)</h4>"
        "<ul>"
        "<li><b>实体类说明</b>：例如 <code>\"Omnidirectional point light\": \"全向点光源\"</code></li>"
        "<li><b>属性显示名</b>：例如 <code>\"Light Source\": \"光源\"</code>、<code>\"Name\": \"名称\"</code></li>"
        "<li><b>属性悬停描述</b>：例如 <code>\"The name that other entities use...\": \"其他实体用于引用的名称。\"</code></li>"
        "<li><b>选项与标记</b>：例如 <code>\"Enabled\": \"已启用\"</code>、<code>\"Disabled\": \"已禁用\"</code></li>"
        "<li><b>输入/输出 (I/O) 说明</b>：例如 <code>\"Removes this entity from the world.\": \"从世界中移除此实体。\"</code></li>"
        "<li><i>注：所有 RAW 代码标识符（如 <code>targetname</code>、<code>angles</code> 等）引擎会自动保护，请仅翻译双引号内的文本。</i></li>"
        "</ul>"
        "<hr/>"
        "<h4>2. 界面核心字典 (<code>qt_translations.json</code>)</h4>"
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
        int ret = QMessageBox::question(
            this,
            "退出确认",
            "CS2 Hammer 仍在运行中！\n\n关闭启动器将不会立即中断游戏，但可能会影响退出时的自动还原。\n是否强行关闭？",
            QMessageBox::Yes | QMessageBox::No
        );
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
    }

    // 确保退出时尝试清理与恢复
    doRestore(false);
    event->accept();
}
