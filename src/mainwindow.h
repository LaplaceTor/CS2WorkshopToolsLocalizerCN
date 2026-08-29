#pragma once

#include <QMainWindow>
#include <QProcess>
#include <string>

class QComboBox;
class QLineEdit;
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
    void onLaunchClicked();
    void onRestoreClicked();
    void onUpdateTranslationsClicked();
    void onHelpClicked();
    void onHammerStarted();
    void onHammerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onHammerError(QProcess::ProcessError error);

private:
    void setupUi();
    void populateAddons();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& msg, const QString& color = "#cccccc");
    void setUiBusy(bool busy);
    void updateRestoreButtonState();
    bool doRestore(bool showLog = true);
    void checkAndRecoverAbnormalExit();
    void fetchUrlCandidates(const QStringList& urls, std::function<void(bool success, const QByteArray& data)> callback);

    std::wstring m_cs2Root;
    std::wstring m_workingDir;

    QComboBox* m_addonCombo;
    QLineEdit* m_argsEdit;
    QPushButton* m_launchBtn;
    QPushButton* m_updateBtn;
    QPushButton* m_restoreBtn;
    QPushButton* m_helpBtn;
    QTextEdit* m_logEdit;
    QLabel* m_statusLabel;
    QLabel* m_cs2PathLabel;

    QNetworkAccessManager* m_networkManager;
    QProcess* m_hammerProcess;
    bool m_isHammerRunning;
};

