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

    QWidget* createStatusDashboard();
    QWidget* createHookLogTab();
    QWidget* createModulesTab();

    void updateProcessDashboard();
    void pollHookRuntimeLog();
    void queryHammerLanguageStatus();
    void queryLoadedModules();

    std::wstring m_cs2Root;
    QTimer* m_pollTimer;

    QLabel* m_lblHammerStatus;
    QLabel* m_lblHammerPid;
    QLabel* m_lblHammerMem;
    QLabel* m_lblIpcStatus;
    QLabel* m_lblCurrentLang;

    QTabWidget* m_tabWidget;

    QPlainTextEdit* m_txtHookLog;
    QLineEdit* m_txtFilter;
    QCheckBox* m_chkAutoScroll;
    QPushButton* m_btnClearLog;
    QStringList m_allLogLines;

    void* m_hSharedLogMap = nullptr;
    const void* m_pSharedLog = nullptr;
    uint32_t m_lastReadSeq = 0;

    QTableWidget* m_tblModules;

    unsigned long m_cachedHammerPid;
    int m_lastKnownLang;
};

#endif
