#include "debug_window.h"
#include "mainwindow.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollBar>
#include <QClipboard>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QProcess>
#include <QDir>
#include <chrono>

#ifndef WM_LOCALIZER_TOGGLE_LANG
#define WM_LOCALIZER_TOGGLE_LANG   (WM_USER + 101)
#endif
#ifndef WM_LOCALIZER_RELOAD_DICT
#define WM_LOCALIZER_RELOAD_DICT   (WM_USER + 102)
#endif
#ifndef WM_LOCALIZER_QUERY_STATUS
#define WM_LOCALIZER_QUERY_STATUS  (WM_USER + 103)
#endif

static HWND FindHammerIpcWindow() {
    HWND hWnd = FindWindowExW(HWND_MESSAGE, NULL, L"CS2_HAMMER_LOCALIZER_IPC", L"CS2_Hammer_Localizer_MsgWnd");
    if (!hWnd) {
        hWnd = FindWindowW(L"CS2_HAMMER_LOCALIZER_IPC", L"CS2_Hammer_Localizer_MsgWnd");
    }
    return hWnd;
}

static DWORD FindCs2ProcessId() {
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    return pid;
}

DebugWindow::DebugWindow(const std::wstring& cs2Root, QWidget* parent)
    : QDialog(parent)
    , m_cs2Root(cs2Root)
    , m_pollTimer(new QTimer(this))
    , m_lastLogFilePos(0)
    , m_cachedHammerPid(0)
    , m_lastKnownLang(0)
{
    setWindowTitle("CS2 Workshop Tools 汉化调试与监控面板");
    resize(920, 680);
    setMinimumSize(780, 520);

    setupUi();

    connect(m_pollTimer, &QTimer::timeout, this, &DebugWindow::onRefreshTimer);
    m_pollTimer->start(500); // 500ms 刷新周期

    // 立即执行一次初始状态检测
    onRefreshTimer();
    onCheckCrashEventsClicked();
}

DebugWindow::~DebugWindow() {
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
}

void DebugWindow::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    // 1. 顶部状态监控仪表盘
    QGroupBox* grpStatus = new QGroupBox("📊 目标进程与汉化运行状态 (Real-time Dashboard)", this);
    grpStatus->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #30363d; border-radius: 6px; margin-top: 6px; padding-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #58a6ff; }"
    );
    QGridLayout* statusGrid = new QGridLayout(grpStatus);
    statusGrid->setContentsMargins(12, 8, 12, 8);
    statusGrid->setHorizontalSpacing(20);
    statusGrid->setVerticalSpacing(6);

    auto makeStatusLabel = [](const QString& text, const QString& color = "#c9d1d9") -> QLabel* {
        QLabel* lbl = new QLabel(text);
        lbl->setStyleSheet(QString("font-size: 12px; color: %1;").arg(color));
        return lbl;
    };

    m_lblHammerStatus = makeStatusLabel("未检测到 cs2.exe 运行", "#f85149");
    m_lblHammerPid    = makeStatusLabel("-");
    m_lblHammerMem    = makeStatusLabel("-");
    m_lblIpcStatus    = makeStatusLabel("未连接", "#f85149");
    m_lblCurrentLang  = makeStatusLabel("未知", "#8b949e");

    statusGrid->addWidget(new QLabel("Hammer 进程状态:"), 0, 0);
    statusGrid->addWidget(m_lblHammerStatus, 0, 1);
    statusGrid->addWidget(new QLabel("进程 PID:"), 0, 2);
    statusGrid->addWidget(m_lblHammerPid, 0, 3);

    statusGrid->addWidget(new QLabel("内存占用 (WorkingSet):"), 1, 0);
    statusGrid->addWidget(m_lblHammerMem, 1, 1);
    statusGrid->addWidget(new QLabel("IPC 消息窗口:"), 1, 2);
    statusGrid->addWidget(m_lblIpcStatus, 1, 3);

    statusGrid->addWidget(new QLabel("当前生效语言:"), 2, 0);
    statusGrid->addWidget(m_lblCurrentLang, 2, 1, 1, 3);

    mainLayout->addWidget(grpStatus);

    // 2. 交互控制按钮工具条
    QHBoxLayout* actLayout = new QHBoxLayout();
    actLayout->setSpacing(8);

    m_btnToggleLang = new QPushButton("🔀 切换原文 / 翻译", this);
    m_btnToggleLang->setToolTip("向注入模块发送 WM_LOCALIZER_TOGGLE_LANG 消息");
    m_btnToggleLang->setStyleSheet("QPushButton { background-color: #d29922; color: white; font-weight: bold; padding: 6px 12px; border-radius: 4px; } QPushButton:hover { background-color: #e3b341; }");

    m_btnHotReload = new QPushButton("⚡ 免重启热重载词典", this);
    m_btnHotReload->setToolTip("向注入模块发送 WM_LOCALIZER_RELOAD_DICT 消息");
    m_btnHotReload->setStyleSheet("QPushButton { background-color: #8957e5; color: white; font-weight: bold; padding: 6px 12px; border-radius: 4px; } QPushButton:hover { background-color: #a371f7; }");

    m_btnLaunchDebug = new QPushButton("🚀 启动调试版 Hammer", this);
    m_btnLaunchDebug->setToolTip("启动带 -debug -verbosehook 参数的 cs2.exe -tools -addons test");
    m_btnLaunchDebug->setStyleSheet("QPushButton { background-color: #238636; color: white; font-weight: bold; padding: 6px 12px; border-radius: 4px; } QPushButton:hover { background-color: #2ea043; }");

    m_btnCopyReport = new QPushButton("📋 复制完整诊断报告", this);
    m_btnCopyReport->setStyleSheet("QPushButton { background-color: #30363d; color: #c9d1d9; font-weight: bold; padding: 6px 12px; border-radius: 4px; } QPushButton:hover { background-color: #484f58; }");

    m_btnClearLog = new QPushButton("🗑 清空日志", this);
    m_btnClearLog->setStyleSheet("QPushButton { background-color: #21262d; color: #8b949e; padding: 6px 12px; border-radius: 4px; } QPushButton:hover { background-color: #30363d; color: #c9d1d9; }");

    actLayout->addWidget(m_btnToggleLang);
    actLayout->addWidget(m_btnHotReload);
    actLayout->addWidget(m_btnLaunchDebug);
    actLayout->addWidget(m_btnCopyReport);
    actLayout->addWidget(m_btnClearLog);

    connect(m_btnToggleLang, &QPushButton::clicked, this, &DebugWindow::onToggleLangClicked);
    connect(m_btnHotReload, &QPushButton::clicked, this, &DebugWindow::onHotReloadClicked);
    connect(m_btnLaunchDebug, &QPushButton::clicked, this, &DebugWindow::onLaunchDebugHammerClicked);
    connect(m_btnCopyReport, &QPushButton::clicked, this, &DebugWindow::onCopyReportClicked);
    connect(m_btnClearLog, &QPushButton::clicked, this, &DebugWindow::onClearLogClicked);

    mainLayout->addLayout(actLayout);

    // 3. Tab 视窗
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #30363d; border-radius: 4px; background-color: #0d1117; }"
        "QTabBar::tab { background: #161b22; color: #8b949e; padding: 8px 16px; font-weight: bold; border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:selected { background: #0d1117; color: #58a6ff; border-bottom: 2px solid #58a6ff; }"
    );

    // Tab 1: 实时 Hook 日志流
    QWidget* tabLog = new QWidget();
    QVBoxLayout* logLayout = new QVBoxLayout(tabLog);
    logLayout->setContentsMargins(8, 8, 8, 8);
    logLayout->setSpacing(6);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel("🔍 文本过滤:", tabLog));
    m_txtFilter = new QLineEdit(tabLog);
    m_txtFilter->setPlaceholderText("输入关键词实时过滤，如 [LANG], [IPC], [TR], [GUARD], [DUMP]...");
    m_txtFilter->setStyleSheet("background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d; border-radius: 4px; padding: 4px 8px;");
    connect(m_txtFilter, &QLineEdit::textChanged, this, &DebugWindow::onFilterTextChanged);
    filterLayout->addWidget(m_txtFilter);

    m_chkAutoScroll = new QCheckBox("自动滚屏", tabLog);
    m_chkAutoScroll->setChecked(true);
    filterLayout->addWidget(m_chkAutoScroll);
    logLayout->addLayout(filterLayout);

    m_txtHookLog = new QPlainTextEdit(tabLog);
    m_txtHookLog->setReadOnly(true);
    m_txtHookLog->setStyleSheet("background-color: #0d1117; color: #c9d1d9; font-family: 'Consolas', 'Courier New', monospace; font-size: 11px;");
    logLayout->addWidget(m_txtHookLog);

    m_tabWidget->addTab(tabLog, "📜 实时 Hook 日志 (hook_runtime.log)");

    // Tab 2: 崩溃捕获与事件诊断
    QWidget* tabCrash = new QWidget();
    QVBoxLayout* crashLayout = new QVBoxLayout(tabCrash);
    crashLayout->setContentsMargins(8, 8, 8, 8);
    crashLayout->setSpacing(6);

    QHBoxLayout* crashTopLayout = new QHBoxLayout();
    crashTopLayout->addWidget(new QLabel("🚨 MiniDump 转储与 Windows 事件日志 (Event 1000 崩溃检测):", tabCrash));
    crashTopLayout->addStretch();
    m_btnCheckCrash = new QPushButton("🔄 重新检测崩溃日志", tabCrash);
    m_btnCheckCrash->setStyleSheet("QPushButton { background-color: #21262d; color: #c9d1d9; padding: 4px 10px; border-radius: 4px; } QPushButton:hover { background-color: #30363d; }");
    connect(m_btnCheckCrash, &QPushButton::clicked, this, &DebugWindow::onCheckCrashEventsClicked);
    crashTopLayout->addWidget(m_btnCheckCrash);
    crashLayout->addLayout(crashTopLayout);

    m_txtCrashReport = new QPlainTextEdit(tabCrash);
    m_txtCrashReport->setReadOnly(true);
    m_txtCrashReport->setStyleSheet("background-color: #0d1117; color: #c9d1d9; font-family: 'Consolas', 'Courier New', monospace; font-size: 11px;");
    crashLayout->addWidget(m_txtCrashReport);

    m_tabWidget->addTab(tabCrash, "🚨 崩溃捕获与事件诊断 (Crash Monitor)");

    // Tab 3: 已加载模块诊断
    QWidget* tabMod = new QWidget();
    QVBoxLayout* modLayout = new QVBoxLayout(tabMod);
    modLayout->setContentsMargins(8, 8, 8, 8);
    modLayout->setSpacing(6);

    m_tblModules = new QTableWidget(tabMod);
    m_tblModules->setColumnCount(4);
    m_tblModules->setHorizontalHeaderLabels(QStringList() << "模块名称" << "基地址 (Base)" << "大小 (Size)" << "完整路径");
    m_tblModules->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tblModules->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tblModules->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tblModules->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tblModules->setStyleSheet("background-color: #0d1117; color: #c9d1d9; gridline-color: #30363d; font-family: 'Consolas', monospace;");
    modLayout->addWidget(m_tblModules);

    m_tabWidget->addTab(tabMod, "🧩 已加载工具模块 (Loaded Modules)");

    mainLayout->addWidget(m_tabWidget);
}

void DebugWindow::onRefreshTimer() {
    updateProcessDashboard();
    pollHookRuntimeLog();
    queryHammerLanguageStatus();
}

void DebugWindow::updateProcessDashboard() {
    DWORD pid = FindCs2ProcessId();
    m_cachedHammerPid = pid;

    if (pid != 0) {
        m_lblHammerStatus->setText("✅ cs2.exe 正在运行");
        m_lblHammerStatus->setStyleSheet("font-size: 12px; color: #3fb950; font-weight: bold;");
        m_lblHammerPid->setText(QString("%1 (0x%2)").arg(pid).arg(pid, 0, 16).toUpper());

        // 查询进程内存
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (hProcess) {
            PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
            if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                double mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
                if (mb >= 1024.0) {
                    m_lblHammerMem->setText(QString("%1 GB").arg(mb / 1024.0, 0, 'f', 2));
                } else {
                    m_lblHammerMem->setText(QString("%1 MB").arg(mb, 0, 'f', 1));
                }
            }
            CloseHandle(hProcess);
        }

        HWND hIpc = FindHammerIpcWindow();
        if (hIpc) {
            m_lblIpcStatus->setText(QString("✅ 已连接 (HWND: 0x%1)").arg((uintptr_t)hIpc, 0, 16).toUpper());
            m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #3fb950;");
            m_btnToggleLang->setEnabled(true);
            m_btnHotReload->setEnabled(true);
        } else {
            m_lblIpcStatus->setText("⚠️ 模块未初始化 IPC 窗口 (可能正在启动)");
            m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #d29922;");
            m_btnToggleLang->setEnabled(false);
            m_btnHotReload->setEnabled(false);
        }
    } else {
        m_lblHammerStatus->setText("❌ 未检测到 cs2.exe");
        m_lblHammerStatus->setStyleSheet("font-size: 12px; color: #f85149; font-weight: bold;");
        m_lblHammerPid->setText("-");
        m_lblHammerMem->setText("-");
        m_lblIpcStatus->setText("未运行");
        m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #8b949e;");
        m_lblCurrentLang->setText("-");
        m_btnToggleLang->setEnabled(false);
        m_btnHotReload->setEnabled(false);
    }
}

void DebugWindow::queryHammerLanguageStatus() {
    HWND hIpc = FindHammerIpcWindow();
    if (!hIpc) {
        return;
    }

    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(hIpc, WM_LOCALIZER_QUERY_STATUS, 0, 0, SMTO_ABORTIFHUNG, 300, &res);
    if (ok) {
        m_lastKnownLang = static_cast<int>(res);
        if (res == 1) {
            m_lblCurrentLang->setText("🇨🇳 中文翻译生效中 (Chinese Enabled)");
            m_lblCurrentLang->setStyleSheet("font-size: 12px; color: #58a6ff; font-weight: bold;");
        } else if (res == 2) {
            m_lblCurrentLang->setText("🇺🇸 英文原文模式 (English Native)");
            m_lblCurrentLang->setStyleSheet("font-size: 12px; color: #e3b341; font-weight: bold;");
        }
    }
}

void DebugWindow::pollHookRuntimeLog() {
    QString binDir = QString::fromStdWString(m_cs2Root) + "/game/bin/win64/";
    QString logPath = binDir + "hook_runtime.log";

    QFile file(logPath);
    if (!file.exists()) {
        return;
    }

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qint64 size = file.size();
        if (size < m_lastLogFilePos) {
            // 文件可能被轮转或截断重置
            m_lastLogFilePos = 0;
            m_allLogLines.clear();
            m_txtHookLog->clear();
        }

        if (size > m_lastLogFilePos) {
            file.seek(m_lastLogFilePos);
            QByteArray newBytes = file.readAll();
            m_lastLogFilePos = file.pos();

            QString newText = QString::fromUtf8(newBytes);
            QStringList lines = newText.split('\n', Qt::SkipEmptyParts);

            QString filter = m_txtFilter->text().trimmed();
            for (const QString& line : lines) {
                QString trimmed = line.trimmed();
                if (trimmed.isEmpty()) continue;
                m_allLogLines.append(trimmed);

                if (filter.isEmpty() || trimmed.contains(filter, Qt::CaseInsensitive)) {
                    m_txtHookLog->appendPlainText(trimmed);
                }
            }

            if (m_chkAutoScroll->isChecked()) {
                m_txtHookLog->verticalScrollBar()->setValue(m_txtHookLog->verticalScrollBar()->maximum());
            }
        }
        file.close();
    }
}

void DebugWindow::onFilterTextChanged(const QString& text) {
    m_txtHookLog->clear();
    QString filter = text.trimmed();
    for (const QString& line : m_allLogLines) {
        if (filter.isEmpty() || line.contains(filter, Qt::CaseInsensitive)) {
            m_txtHookLog->appendPlainText(line);
        }
    }
    if (m_chkAutoScroll->isChecked()) {
        m_txtHookLog->verticalScrollBar()->setValue(m_txtHookLog->verticalScrollBar()->maximum());
    }
}

void DebugWindow::onClearLogClicked() {
    m_txtHookLog->clear();
    m_allLogLines.clear();
}

void DebugWindow::onToggleLangClicked() {
    HWND hIpc = FindHammerIpcWindow();
    if (!hIpc) {
        m_txtHookLog->appendPlainText("[CLIENT] 错误: 未检测到 Hammer IPC 窗口，无法切换");
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(hIpc, WM_LOCALIZER_TOGGLE_LANG, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();

    if (ok) {
        m_lastKnownLang = static_cast<int>(res);
        QString langStr = (res == 1) ? "🇨🇳 中文翻译" : ((res == 2) ? "🇺🇸 英文原文" : "已切换");
        m_txtHookLog->appendPlainText(QString("[CLIENT] 🔀 语言切换成功 -> 当前模式: %1 (耗时: %2 μs)").arg(langStr).arg(elapsed));
        queryHammerLanguageStatus();
    } else {
        m_txtHookLog->appendPlainText(QString("[CLIENT] ⚠️ 发送切换指令超时或失败 (错误代码: %1)").arg(GetLastError()));
    }
}

void DebugWindow::onHotReloadClicked() {
    MainWindow* mainWin = qobject_cast<MainWindow*>(parentWidget());
    if (mainWin) {
        bool ok = mainWin->performHotReload(false);
        if (ok) {
            m_txtHookLog->appendPlainText("[CLIENT] ⚡ FGD + Qt 词典全量热重载成功！无需重启 Hammer 即可渲染新翻译");
        } else {
            m_txtHookLog->appendPlainText("[CLIENT] ⚠️ 全量热重载执行完成（请检查启动器日志输出）");
        }
        return;
    }

    HWND hIpc = FindHammerIpcWindow();
    if (!hIpc) {
        m_txtHookLog->appendPlainText("[CLIENT] 错误: 未检测到 Hammer IPC 窗口，无法热重载");
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(hIpc, WM_LOCALIZER_RELOAD_DICT, 0, 0, SMTO_ABORTIFHUNG, 2000, &res);
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();

    if (ok) {
        m_txtHookLog->appendPlainText(QString("[CLIENT] ⚡ 词典热重载成功！无需重启 Hammer 即可渲染新翻译 (耗时: %1 μs)").arg(elapsed));
    } else {
        m_txtHookLog->appendPlainText(QString("[CLIENT] ⚠️ 发送热重载指令超时或失败 (错误代码: %1)").arg(GetLastError()));
    }
}

void DebugWindow::onLaunchDebugHammerClicked() {
    QString cs2Exe = QString::fromStdWString(m_cs2Root) + "/game/bin/win64/cs2.exe";
    if (!QFile::exists(cs2Exe)) {
        m_txtHookLog->appendPlainText(QString("[CLIENT] 未找到 cs2.exe: %1").arg(cs2Exe));
        return;
    }

    QStringList args;
    args << "-tools" << "-addons" << "test" << "-debug" << "-verbosehook";

    bool ok = QProcess::startDetached(cs2Exe, args, QString::fromStdWString(m_cs2Root) + "/game/bin/win64/");
    if (ok) {
        m_txtHookLog->appendPlainText("[CLIENT] 🚀 已拉起调试版 Hammer: cs2.exe -tools -addons test -debug -verbosehook");
    } else {
        m_txtHookLog->appendPlainText("[CLIENT] ❌ 启动调试版 Hammer 失败");
    }
}

void DebugWindow::onCheckCrashEventsClicked() {
    QString binDir = QString::fromStdWString(m_cs2Root) + "/game/bin/win64/";
    m_txtCrashReport->clear();

    QString report;
    report += "================================================================================\n";
    report += "                    CS2 Hammer 崩溃捕获与事件诊断报告\n";
    report += "================================================================================\n";
    report += QString("诊断时间: %1\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report += QString("CS2 根目录: %1\n\n").arg(QString::fromStdWString(m_cs2Root));

    // 1. 扫描转储文件
    report += "--- [1. 本地 MiniDump 文件扫描] ---\n";
    QDir dir(binDir);
    QStringList filters;
    filters << "*.dmp" << "*.mdmp";
    QFileInfoList dumps = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    if (dumps.isEmpty()) {
        report += "[OK] 未在 game/bin/win64/ 目录下检测到任何崩溃转储文件 (.dmp / .mdmp)\n";
    } else {
        report += QString("检测到 %1 个转储文件:\n").arg(dumps.size());
        for (const QFileInfo& fi : dumps) {
            double sizeKb = static_cast<double>(fi.size()) / 1024.0;
            report += QString("  - %1  (%2 KB, %3)\n")
                .arg(fi.fileName())
                .arg(sizeKb, 0, 'f', 1)
                .arg(fi.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
        }
    }
    report += "\n";

    // 2. 检查 Windows 应用程序事件日志（Event ID 1000 - Application Error）
    report += "--- [2. Windows 应用程序错误事件日志提取 (Event ID 1000)] ---\n";
    QProcess ps;
    QString psScript =
        "Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'; Id=1000} -MaxEvents 5 -ErrorAction SilentlyContinue | "
        "Where-Object { $_.Message -match 'cs2.exe|hammer.dll|propertyeditor.dll|qtcore_qm.dll' } | "
        "ForEach-Object { "
        "  [PSCustomObject]@{ Time=$_.TimeCreated; Message=$_.Message } "
        "} | Format-List";

    ps.start("powershell", QStringList() << "-NoProfile" << "-NonInteractive" << "-Command" << psScript);
    if (ps.waitForFinished(4000)) {
        QString psOut = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
        if (psOut.isEmpty()) {
            report += "[OK] 最近 5 条 Application Error 事件中未发现属于 cs2.exe / hammer.dll 的崩溃记录。\n";
        } else {
            report += psOut + "\n";
        }
    } else {
        report += "[!] 查询 Windows 事件日志超时。\n";
    }

    m_txtCrashReport->setPlainText(report);

    // 同步更新 Tab 3 模块列表
    queryLoadedModules();
}

void DebugWindow::queryLoadedModules() {
    m_tblModules->setRowCount(0);
    DWORD pid = m_cachedHammerPid;
    if (pid == 0) pid = FindCs2ProcessId();
    if (pid == 0) return;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(hSnap, &me)) {
        int row = 0;
        do {
            QString name = QString::fromWCharArray(me.szModule);
            QString path = QString::fromWCharArray(me.szExePath);

            // 仅突出显示 Workshop Tools 与 Qt 相关重要核心模块
            QString lower = name.toLower();
            bool isCore = (lower.contains("cs2") || lower.contains("hammer") || lower.contains("property") ||
                           lower.contains("qtcore_qm") || lower.contains("qt5") || lower.contains("tier0") ||
                           lower.contains("assetbrowser") || lower.contains("subtool"));

            if (isCore) {
                m_tblModules->insertRow(row);
                QTableWidgetItem* item0 = new QTableWidgetItem(name);
                if (lower.contains("qtcore_qm")) {
                    item0->setForeground(QBrush(QColor("#58a6ff")));
                }
                m_tblModules->setItem(row, 0, item0);
                m_tblModules->setItem(row, 1, new QTableWidgetItem(QString("0x%1").arg((uintptr_t)me.modBaseAddr, 0, 16).toUpper()));
                m_tblModules->setItem(row, 2, new QTableWidgetItem(QString("%1 KB").arg(me.modBaseSize / 1024)));
                m_tblModules->setItem(row, 3, new QTableWidgetItem(path));
                row++;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
}

void DebugWindow::onCopyReportClicked() {
    QString report;
    report += "==================== CS2 Hammer Localizer 完整诊断报告 ====================\n";
    report += QString("生成时间: %1\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report += QString("操作系统: Windows %1\n").arg(QSysInfo::prettyProductName());
    report += QString("CS2 路径: %1\n").arg(QString::fromStdWString(m_cs2Root));
    report += QString("Hammer 进程: %1 (PID: %2)\n").arg(m_lblHammerStatus->text()).arg(m_lblHammerPid->text());
    report += QString("内存占用: %1\n").arg(m_lblHammerMem->text());
    report += QString("IPC 状态: %1\n").arg(m_lblIpcStatus->text());
    report += QString("当前语言: %1\n\n").arg(m_lblCurrentLang->text());

    report += "--- [最近 40 行 Hook 日志] ---\n";
    int total = m_allLogLines.size();
    int start = (std::max)(0, total - 40);
    for (int i = start; i < total; ++i) {
        report += m_allLogLines[i] + "\n";
    }
    report += "\n";

    report += m_txtCrashReport->toPlainText();
    report += "\n==============================================================================\n";

    QClipboard* cb = QApplication::clipboard();
    if (cb) {
        cb->setText(report);
        m_txtHookLog->appendPlainText("[CLIENT] 📋 完整诊断报告已复制到剪贴板！");
    }
}
