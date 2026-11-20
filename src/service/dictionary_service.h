#pragma once

#include <functional>
#include <string>

#include <QByteArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

class DictionaryService : public QObject {
    Q_OBJECT

public:
    explicit DictionaryService(QObject* parent = nullptr);

    enum class Failure {
        None,
        Fetch,
        Parse,
        Save
    };

    struct UpdateResult {
        bool       ok       = false;
        Failure    failure  = Failure::None;
        QString    dictName;
        QString    detail;
        qsizetype  qtCount       = 0;
        qsizetype  fgdCount      = 0;
        qsizetype  overrideCount = 0;
    };

    using LogSink = std::function<void(const QString& message, const QString& color)>;

    using DoneCallback = std::function<void(const UpdateResult& result)>;

    void updateDictionaries(const std::wstring& workingDir,
                            const LogSink& log,
                            const DoneCallback& done);

    static QStringList ensureTemplates(const std::wstring& workingDir);

private:
    struct OnlineDictionary {
        QByteArray    raw;
        QJsonDocument doc;
        qsizetype     count;
    };

    void fetchUrlCandidates(const QStringList& urls,
                            std::function<void(bool success, const QByteArray& data)> callback);

    void fetchOnlineDictionary(
        const QStringList& urls,
        const QString& stepLabel,
        const QString& dictName,
        const QString& desc,
        std::function<qsizetype(const QJsonDocument&)> countOf,
        std::function<void(const OnlineDictionary&)> onSuccess
    );

    bool saveDictionaryAtomic(const QString& localPath, const QByteArray& data, const QString& name);

    void finishWith(const UpdateResult& result);

    QNetworkAccessManager* m_networkManager;

    LogSink      m_log;
    DoneCallback m_done;
};
