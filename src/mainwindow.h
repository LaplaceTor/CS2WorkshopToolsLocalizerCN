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

    // 执行注入，但不启动 HAMMER（UI 线程包装：读取 UI 状态后转后台执行核心流程）
    bool injectLocalization();

    // 注入核心流程（备份 → FGD 汉化 → PE 补丁），仅做 IO 与日志（appendLog 自行封送），运行于 worker 线程
    bool injectLocalizationCore(bool useMachineTrans);

    // 启动 HAMMER
    bool startHammerProcess();

    // 将耗时任务放入后台线程执行，UI 线程以事件循环等待（保持界面响应），返回任务结果；
    // 期间 m_workerBusy 置位，阻止并发注入/还原
    bool runHeavyInWorker(const std::function<bool()>& task);

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

    // 后台任务互斥标志
    bool m_workerBusy = false;

    // 备份一致性校验（含多次全文件 SHA256）异步缓存，避免按钮状态刷新阻塞 UI
    bool m_cachedValidationValid = false;
    bool m_validationPending = false;
    qint64 m_lastValidationMs = 0;
};