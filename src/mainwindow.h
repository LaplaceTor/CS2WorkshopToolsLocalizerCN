#pragma once

#include <QMainWindow>
#include <QProcess>
#include <string>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QTextEdit;
class QLabel;
class QNetworkAccessManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const std::wstring& cs2Root, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onInjectClicked();
    void onLaunchClicked();
    void onRestoreClicked();
    void onUpdateTranslationsClicked();
    void onHelpClicked();
    void onHammerStarted();
    void onHammerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onHammerError(QProcess::ProcessError error);
    void onCheckProcessState();

private:
    void setupUi();
    void populateAddons();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& msg, const QString& color = "#cccccc");

    void setUiBusy(bool busy);

    // 更新“仅注入 / 启动 HAMMER / 还原”三个核心按钮状态
    void updateActionButtonState();

    // 保留旧接口，内部转发到统一状态更新
    void updateRestoreButtonState();

    // 判断当前是否处于有效的“已注入”状态
    bool isPatchDeployedAndValid();

    // 执行注入，但不启动 HAMMER
    bool injectLocalization();

    // 启动 HAMMER
    bool startHammerProcess();

    bool doRestore(bool showLog = true);
    void checkAndRecoverAbnormalExit();
    void handleHammerProcessTerminated();

    void fetchUrlCandidates(
        const QStringList& urls,
        std::function<void(bool success, const QByteArray& data)> callback
    );

    std::wstring m_cs2Root;
    std::wstring m_workingDir;

    QComboBox* m_addonCombo;
    QLineEdit* m_argsEdit;
    QCheckBox* m_useMachineTransCheck;

    // 核心操作按钮
    QPushButton* m_injectBtn;
    QPushButton* m_launchBtn;
    QPushButton* m_restoreBtn;

    // 其他功能按钮
    QPushButton* m_updateBtn;
    QPushButton* m_helpBtn;

    QTextEdit* m_logEdit;
    QLabel* m_statusLabel;
    QLabel* m_cs2PathLabel;

    QNetworkAccessManager* m_networkManager;
    QProcess* m_hammerProcess;
    class QTimer* m_monitorTimer;

    int m_notRunningCount;
    bool m_isHammerRunning;
    qint64 m_hammerPid;
    void* m_hammerProcessHandle;
};