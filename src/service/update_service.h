#pragma once

#include <functional>
#include <string>

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

class UpdateService : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr);

    struct AppUpdateResult {
        bool    checkOk        = false;
        bool    hasUpdate      = false;
        bool    isDevBuild     = false;
        QString currentVersion;
        QString latestVersion;
        QString releaseUrl;
    };

    struct DictCheckResult {
        bool        checkOk = false;
        QStringList outdatedDicts;
    };

    struct StartupCheckResult {
        AppUpdateResult app;
        DictCheckResult dict;
    };

    using DoneCallback = std::function<void(const StartupCheckResult& result)>;

    void checkStartupUpdates(const std::wstring& workingDir, const DoneCallback& done);

private:
    void checkAppUpdate(std::function<void(const AppUpdateResult&)> callback);
    void checkDictionaries(const std::wstring& workingDir,
                           std::function<void(const DictCheckResult&)> callback);

    void fetchUrlCandidates(const QStringList& urls,
                            std::function<void(bool success, const QByteArray& data)> callback);

    QNetworkAccessManager* m_networkManager;
};
