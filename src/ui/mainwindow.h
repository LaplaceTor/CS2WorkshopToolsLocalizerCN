#pragma once

#include <QMainWindow>
#include <string>

#include "service/dictionary_service.h"
#include "service/hammer_service.h"
#include "service/update_service.h"

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QTextEdit;
class QLabel;
class DictionaryEditorService;
class DictionaryEditorWindow;

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
    void onChangeCs2PathClicked();
    void onInjectClicked();
    void onLaunchClicked();
    void onRestoreClicked();
    void onUpdateTranslationsClicked();
    void onHelpClicked();
    void onToggleLangClicked();
    void onHotReloadClicked();
    void onDebugClicked();
    void onDictEditorClicked();
    void onNewAddonClicked();
    void onRemoveAddonClicked();
    void onWatchedFileChanged(const QString& path);
    void onDebouncedHotReload();

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

    void updateActionButtonState();

    bool hasValidSelectedAddon() const;

    bool isPatchDeployedAndValid();

    bool machineTranslationEnabled() const;

    bool injectLocalization();

    bool injectLocalizationCore(bool useMachineTrans);

    bool runHeavyInWorker(const std::function<bool()>& task);

    bool doRestore(bool showLog = true);
    void checkAndRecoverAbnormalExit();

    void onDictionariesUpdated(const DictionaryService::UpdateResult& result);

    void startStartupUpdateCheck();
    void onStartupUpdateChecked(const UpdateService::StartupCheckResult& result);
    void startDictionaryUpdate();

    std::wstring m_cs2Root;
    std::wstring m_workingDir;

    QComboBox*   m_addonCombo{nullptr};
    QPushButton* m_newAddonBtn{nullptr};
    QPushButton* m_removeAddonBtn{nullptr};
    QLineEdit*   m_argsEdit{nullptr};
    QCheckBox*   m_useMachineTransCheck{nullptr};

    QPushButton* m_injectBtn{nullptr};
    QPushButton* m_launchBtn{nullptr};
    QPushButton* m_restoreBtn{nullptr};

    QPushButton* m_updateBtn{nullptr};
    QPushButton* m_helpBtn{nullptr};
    QPushButton* m_toggleLangBtn{nullptr};
    QPushButton* m_hotReloadBtn{nullptr};
    QPushButton* m_debugBtn{nullptr};
    QPushButton* m_dictEditorBtn{nullptr};

    DictionaryEditorService* m_dictEditorService{nullptr};
    DictionaryEditorWindow*  m_dictEditorWindow{nullptr};
    qint64 m_lastEditorReloadTick = 0;

    class QFileSystemWatcher* m_fileWatcher{nullptr};
    class QTimer* m_hotReloadDebounceTimer{nullptr};

    QTextEdit* m_logEdit{nullptr};
    QLabel*    m_statusLabel{nullptr};
    QLabel*      m_cs2PathLabel{nullptr};
    QPushButton* m_changePathBtn{nullptr};

    DictionaryService* m_dictionaryService;
    HammerService*     m_hammerService;
    UpdateService*     m_updateService;

    bool m_workerBusy = false;

    bool m_cachedValidationValid = false;
    bool m_validationPending = false;
    qint64 m_lastValidationMs = 0;
};
