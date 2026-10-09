#include "ui/debug_window.h"
#include "ui/mainwindow.h"
#include "core/cs2_detector.h"
#include "core/hammer_ipc.h"
#include "core/path_constants.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollBar>
#include <QApplication>

#ifndef WM_LOCALIZER_QUERY_STATUS
#define WM_LOCALIZER_QUERY_STATUS  (WM_USER + 103)
#endif



// 跨进程共享内存日志环形缓冲区（100% 纯内存传输，0 磁盘 I/O）
static constexpr const wchar_t* SHARED_LOG_MEM_NAME = L"Local\\CS2_Hammer_Localizer_SharedLog";
static constexpr size_t SHARED_LOG_CAPACITY = 128 * 1024; // 128 KB 环形缓冲区

#pragma pack(push, 4)
struct SharedLogHeader {
    uint32_t magic;         // 0x5A4C434C ('LCLZ')
    uint32_t writeSeq;      // 单调递增写入总字节数
    uint32_t writeOffset;   // 当前写入环形偏移
    char buffer[SHARED_LOG_CAPACITY];
};
#pragma pack(pop)

DebugWindow::DebugWindow(const std::wstring& cs2Root, QWidget* parent)
    : QDialog(parent)
    , m_cs2Root(cs2Root)
    , m_pollTimer(new QTimer(this))
    , m_cachedHammerPid(0)
    , m_lastKnownLang(0)
    , m_hSharedLogMap(nullptr)
    , m_pSharedLog(nullptr)
    , m_lastReadSeq(0)
{
    setWindowTitle("CS2 Workshop Tools 汉化调试与监控面板");
    resize(660, 480);
    setMinimumSize(540, 360);

    setupUi();

    connect(m_pollTimer, &QTimer::timeout, this, &DebugWindow::onRefreshTimer);
    m_pollTimer->start(300); // 300ms 刷新周期

    // 立即执行一次初始状态检测
    onRefreshTimer();
    queryLoadedModules();
}

DebugWindow::~DebugWindow() {
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
    if (m_pSharedLog) {
        UnmapViewOfFile(m_pSharedLog);
        m_pSharedLog = nullptr;
    }
    if (m_hSharedLogMap) {
        CloseHandle(reinterpret_cast<HANDLE>(m_hSharedLogMap));
        m_hSharedLogMap = nullptr;
    }
}

// 顶部状态看板里大量使用的同款小号状态标签
namespace {

QLabel* MakeStatusLabel(const QString& text, const QString& color = "#c9d1d9") {
    QLabel* lbl = new QLabel(text);
    lbl->setStyleSheet(QString("font-size: 12px; color: %1;").arg(color));
    return lbl;
}

}  // namespace

QWidget* DebugWindow::createStatusDashboard() {
    QGroupBox* grpStatus = new QGroupBox("📊 目标进程与汉化运行状态", this);
    grpStatus->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #30363d; border-radius: 6px; margin-top: 6px; padding-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #58a6ff; }"
    );

    QGridLayout* statusGrid = new QGridLayout(grpStatus);
    statusGrid->setContentsMargins(12, 8, 12, 8);
    statusGrid->setHorizontalSpacing(16);
    statusGrid->setVerticalSpacing(4);

    m_lblHammerStatus = MakeStatusLabel("未检测到 cs2.exe 运行", "#f85149");
    m_lblHammerPid    = MakeStatusLabel("-");
    m_lblHammerMem    = MakeStatusLabel("-");
    m_lblIpcStatus    = MakeStatusLabel("未连接", "#f85149");
    m_lblCurrentLang  = MakeStatusLabel("未知", "#8b949e");

    statusGrid->addWidget(new QLabel("Hammer 进程:"), 0, 0);
    statusGrid->addWidget(m_lblHammerStatus, 0, 1);
    statusGrid->addWidget(new QLabel("PID:"), 0, 2);
    statusGrid->addWidget(m_lblHammerPid, 0, 3);

    statusGrid->addWidget(new QLabel("内存占用:"), 1, 0);
    statusGrid->addWidget(m_lblHammerMem, 1, 1);
    statusGrid->addWidget(new QLabel("IPC 状态:"), 1, 2);
    statusGrid->addWidget(m_lblIpcStatus, 1, 3);

    statusGrid->addWidget(new QLabel("生效语言:"), 2, 0);
    statusGrid->addWidget(m_lblCurrentLang, 2, 1, 1, 3);

    return grpStatus;
}

QWidget* DebugWindow::createHookLogTab() {
    QWidget* tabLog = new QWidget();
    QVBoxLayout* logLayout = new QVBoxLayout(tabLog);
    logLayout->setContentsMargins(8, 8, 8, 8);
    logLayout->setSpacing(6);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel("🔍 文本过滤:", tabLog));

    m_txtFilter = new QLineEdit(tabLog);
    m_txtFilter->setPlaceholderText("输入关键词实时过滤，如 [LANG], [IPC], [TR], [GUARD]...");
    m_txtFilter->setStyleSheet("background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d; border-radius: 4px; padding: 4px 8px;");
    connect(m_txtFilter, &QLineEdit::textChanged, this, &DebugWindow::onFilterTextChanged);
    filterLayout->addWidget(m_txtFilter);

    m_chkAutoScroll = new QCheckBox("自动滚屏", tabLog);
    m_chkAutoScroll->setChecked(true);
    filterLayout->addWidget(m_chkAutoScroll);

    m_btnClearLog = new QPushButton("🗑 清空日志", tabLog);
    m_btnClearLog->setStyleSheet("QPushButton { background-color: #21262d; color: #8b949e; padding: 4px 10px; border-radius: 4px; border: 1px solid #30363d; } QPushButton:hover { background-color: #30363d; color: #c9d1d9; }");
    connect(m_btnClearLog, &QPushButton::clicked, this, &DebugWindow::onClearLogClicked);
    filterLayout->addWidget(m_btnClearLog);

    logLayout->addLayout(filterLayout);

    m_txtHookLog = new QPlainTextEdit(tabLog);
    m_txtHookLog->setReadOnly(true);
    m_txtHookLog->setStyleSheet("background-color: #0d1117; color: #c9d1d9; font-family: 'Consolas', 'Courier New', monospace; font-size: 11px;");
    logLayout->addWidget(m_txtHookLog);

    return tabLog;
}

QWidget* DebugWindow::createModulesTab() {
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

    return tabMod;
}

void DebugWindow::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // 1. 顶部状态监控仪表盘
    mainLayout->addWidget(createStatusDashboard());

    // 2. Tab 视窗
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #30363d; border-radius: 4px; background-color: #0d1117; }"
        "QTabBar::tab { background: #161b22; color: #8b949e; padding: 6px 14px; font-weight: bold; border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:selected { background: #0d1117; color: #58a6ff; border-bottom: 2px solid #58a6ff; }"
    );
    m_tabWidget->addTab(createHookLogTab(), "📜 实时 Hook 日志");
    m_tabWidget->addTab(createModulesTab(), "🧩 已加载工具模块");

    mainLayout->addWidget(m_tabWidget);
}

void DebugWindow::onRefreshTimer() {
    updateProcessDashboard();
    pollHookRuntimeLog();
    queryHammerLanguageStatus();
    queryLoadedModules();
}

void DebugWindow::updateProcessDashboard() {
    DWORD pid = Cs2Detector::FindCs2ProcessId();
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

        HWND hIpc = HammerIpc::FindIpcWindow();
        if (hIpc) {
            m_lblIpcStatus->setText(QString("✅ 已连接 (HWND: 0x%1)").arg((uintptr_t)hIpc, 0, 16).toUpper());
            m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #3fb950;");
        } else {
            m_lblIpcStatus->setText("⚠️ 模块未初始化 IPC 窗口 (可能正在启动)");
            m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #d29922;");
        }
    } else {
        m_lblHammerStatus->setText("❌ 未检测到 cs2.exe");
        m_lblHammerStatus->setStyleSheet("font-size: 12px; color: #f85149; font-weight: bold;");
        m_lblHammerPid->setText("-");
        m_lblHammerMem->setText("-");
        m_lblIpcStatus->setText("未运行");
        m_lblIpcStatus->setStyleSheet("font-size: 12px; color: #8b949e;");
        m_lblCurrentLang->setText("-");
    }
}

void DebugWindow::queryHammerLanguageStatus() {
    HWND hIpc = HammerIpc::FindIpcWindow();
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
    if (!m_pSharedLog) {
        if (!m_hSharedLogMap) {
            m_hSharedLogMap = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_LOG_MEM_NAME);
        }
        if (m_hSharedLogMap) {
            m_pSharedLog = MapViewOfFile(reinterpret_cast<HANDLE>(m_hSharedLogMap), FILE_MAP_READ, 0, 0, sizeof(SharedLogHeader));
            if (m_pSharedLog) {
                const auto* pHeader = reinterpret_cast<const SharedLogHeader*>(m_pSharedLog);
                if (pHeader->magic != 0x5A4C434C) {
                    UnmapViewOfFile(m_pSharedLog);
                    m_pSharedLog = nullptr;
                }
            }
        }
    }

    if (!m_pSharedLog) return;

    const auto* pHeader = reinterpret_cast<const SharedLogHeader*>(m_pSharedLog);
    uint32_t currentSeq = pHeader->writeSeq;
    if (currentSeq == m_lastReadSeq) return;

    // 若写入跨度超出缓冲区大小，对齐至最新有效窗口
    if (currentSeq < m_lastReadSeq || (currentSeq - m_lastReadSeq) > SHARED_LOG_CAPACITY) {
        m_lastReadSeq = (currentSeq > SHARED_LOG_CAPACITY) ? (currentSeq - SHARED_LOG_CAPACITY) : 0;
    }

    uint32_t bytesToRead = currentSeq - m_lastReadSeq;
    if (bytesToRead == 0) return;

    QByteArray rawBytes;
    rawBytes.reserve(static_cast<int>(bytesToRead));

    uint32_t readOffset = m_lastReadSeq % SHARED_LOG_CAPACITY;
    for (uint32_t i = 0; i < bytesToRead; ++i) {
        rawBytes.append(pHeader->buffer[(readOffset + i) % SHARED_LOG_CAPACITY]);
    }
    m_lastReadSeq = currentSeq;

    QString newText = QString::fromUtf8(rawBytes);
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
    if (m_pSharedLog) {
        const auto* pHeader = reinterpret_cast<const SharedLogHeader*>(m_pSharedLog);
        m_lastReadSeq = pHeader->writeSeq;
    }
}

void DebugWindow::queryLoadedModules() {
    m_tblModules->setRowCount(0);
    DWORD pid = m_cachedHammerPid;
    if (pid == 0) pid = Cs2Detector::FindCs2ProcessId();
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
