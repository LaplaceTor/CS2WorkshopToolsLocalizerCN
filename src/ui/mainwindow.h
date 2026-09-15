#pragma once

#include <QMainWindow>
#include <string>

#include "service/dictionary_service.h"
#include "service/hammer_service.h"

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QTextEdit;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const std::wstring& cs2Root, QWidget *parent = nullptr);
    ~MainWindow() override;

    void openDebugWindow();
    bool performHotReload(bool silent = false);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onInjectClicked();
    void onLaunchClicked();
    void onRestoreClicked();
    void onUpdateTranslationsClicked();
    void onHelpClicked();
    void onToggleLangClicked();
    void onHotReloadClicked();
    void onDebugClicked();
    void onWatchedFileChanged(const QString& path);
    void onDebouncedHotReload();

    // HAMMER 生命周期回调：均由 HammerService 的信号驱动
    void onHammerStarted(qint64 pid);
    void onHammerTerminated();
    void onHammerStartFailed(int errorCode);
    void onHammerHeartbeat();

private:
    void setupUi();
    void setupFileWatcher();
    void populateAddons();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& msg, const QString& color = "#cccccc");

    void setUiBusy(bool busy);

    // 更新“仅注入 / 启动 HAMMER / 还原”三个核心按钮状态
    void updateActionButtonState();

    // 判断当前是否处于有效的“已注入”状态（对 core 的只读状态查询 + 本地异步校验缓存）
    bool isPatchDeployedAndValid();

    // 读取“使用机翻”复选框（控件缺失时沿用默认开启）
    bool machineTranslationEnabled() const;

    // 执行注入，但不启动 HAMMER（UI 线程包装：读取 UI 状态后转后台执行核心流程）
    bool injectLocalization();

    // 注入核心流程，运行于 worker 线程：读取 UI 状态后交由 LocalizationService 执行
    bool injectLocalizationCore(bool useMachineTrans);

    // 将耗时任务放入后台线程执行，UI 线程以事件循环等待（保持界面响应），返回任务结果；
    // 期间 m_workerBusy 置位，阻止并发注入/还原
    bool runHeavyInWorker(const std::function<bool()>& task);

    bool doRestore(bool showLog = true);
    void checkAndRecoverAbnormalExit();

    // 在线词典更新的统一收尾：按 Service 返回的失败原因记日志 + 弹窗 + 恢复 UI
    void onDictionariesUpdated(const DictionaryService::UpdateResult& result);

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
    QPushButton* m_toggleLangBtn;
    QPushButton* m_hotReloadBtn;
    QPushButton* m_debugBtn;

    class QFileSystemWatcher* m_fileWatcher;
    class QTimer* m_hotReloadDebounceTimer;

    QTextEdit* m_logEdit;
    QLabel* m_statusLabel;
    QLabel* m_cs2PathLabel;

    // Service 层：业务流程编排，MainWindow 只负责传参与展示结果
    DictionaryService* m_dictionaryService;
    HammerService*     m_hammerService;

    // 后台任务互斥标志
    bool m_workerBusy = false;

    // 备份一致性校验（含多次全文件 SHA256）异步缓存，避免按钮状态刷新阻塞 UI
    bool m_cachedValidationValid = false;
    bool m_validationPending = false;
    qint64 m_lastValidationMs = 0;
};
