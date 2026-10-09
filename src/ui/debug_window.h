#ifndef DEBUG_WINDOW_H
#define DEBUG_WINDOW_H

#include <QDialog>
#include <QString>
#include <QTimer>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QTabWidget>
#include <QTableWidget>
#include <cstdint>

// 分区构建函数的返回类型只需前向声明即可（布局与分组盒仅在 .cpp 中使用）
class QHBoxLayout;
class QVBoxLayout;
class QGridLayout;
class QGroupBox;

class DebugWindow : public QDialog {
    Q_OBJECT

public:
    explicit DebugWindow(const std::wstring& cs2Root, QWidget* parent = nullptr);
    ~DebugWindow() override;

private slots:
    void onRefreshTimer();
    void onClearLogClicked();
    void onFilterTextChanged(const QString& text);

private:
    void setupUi();

    // setupUi 的分区构建
    QWidget* createStatusDashboard();  // 顶部：目标进程与汉化状态看板
    QWidget* createHookLogTab();       // Tab 1：实时 Hook 日志流
    QWidget* createModulesTab();       // Tab 2：已加载模块诊断

    void updateProcessDashboard();
    void pollHookRuntimeLog();
    void queryHammerLanguageStatus();
    void queryLoadedModules();

    std::wstring m_cs2Root;
    QTimer* m_pollTimer;

    // 顶部状态看板
    QLabel* m_lblHammerStatus;
    QLabel* m_lblHammerPid;
    QLabel* m_lblHammerMem;
    QLabel* m_lblIpcStatus;
    QLabel* m_lblCurrentLang;

    // Tab 视窗
    QTabWidget* m_tabWidget;

    // Tab 1: 实时 Hook 日志流
    QPlainTextEdit* m_txtHookLog;
    QLineEdit* m_txtFilter;
    QCheckBox* m_chkAutoScroll;
    QPushButton* m_btnClearLog;
    QStringList m_allLogLines;

    // 共享内存日志读取状态（100% 纯内存传输，0 磁盘文件）
    void* m_hSharedLogMap = nullptr;
    const void* m_pSharedLog = nullptr;
    uint32_t m_lastReadSeq = 0;

    // Tab 2: 已加载模块诊断
    QTableWidget* m_tblModules;

    // 运行缓存
    unsigned long m_cachedHammerPid;
    int m_lastKnownLang; // 0: unknown, 1: zh, 2: en
};

#endif // DEBUG_WINDOW_H
