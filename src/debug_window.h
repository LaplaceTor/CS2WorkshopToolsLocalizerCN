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

class DebugWindow : public QDialog {
    Q_OBJECT

public:
    explicit DebugWindow(const std::wstring& cs2Root, QWidget* parent = nullptr);
    ~DebugWindow() override;

private slots:
    void onRefreshTimer();
    void onToggleLangClicked();
    void onHotReloadClicked();
    void onLaunchDebugHammerClicked();
    void onCopyReportClicked();
    void onClearLogClicked();
    void onFilterTextChanged(const QString& text);
    void onCheckCrashEventsClicked();

private:
    void setupUi();
    void updateProcessDashboard();
    void pollHookRuntimeLog();
    void queryHammerLanguageStatus();
    void checkCrashDumps();
    void queryLoadedModules();

    std::wstring m_cs2Root;
    QTimer* m_pollTimer;

    // 顶部状态看板
    QLabel* m_lblHammerStatus;
    QLabel* m_lblHammerPid;
    QLabel* m_lblHammerMem;
    QLabel* m_lblIpcStatus;
    QLabel* m_lblCurrentLang;

    // 交互操作按钮
    QPushButton* m_btnToggleLang;
    QPushButton* m_btnHotReload;
    QPushButton* m_btnLaunchDebug;
    QPushButton* m_btnCopyReport;
    QPushButton* m_btnClearLog;

    // Tab 视窗
    QTabWidget* m_tabWidget;

    // Tab 1: 实时 Hook 日志流
    QPlainTextEdit* m_txtHookLog;
    QLineEdit* m_txtFilter;
    QCheckBox* m_chkAutoScroll;
    qint64 m_lastLogFilePos;
    QStringList m_allLogLines;

    // Tab 2: 崩溃捕获与事件诊断
    QPlainTextEdit* m_txtCrashReport;
    QPushButton* m_btnCheckCrash;

    // Tab 3: 已加载模块诊断
    QTableWidget* m_tblModules;

    // 运行缓存
    unsigned long m_cachedHammerPid;
    int m_lastKnownLang; // 0: unknown, 1: zh, 2: en
};

#endif // DEBUG_WINDOW_H
