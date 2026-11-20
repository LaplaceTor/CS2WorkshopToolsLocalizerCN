#include "service/update_service.h"

#include <memory>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QVersionNumber>

#include "core/dictionary_compiler.h"
#include "core/dictionary_paths.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

constexpr int kRequestTimeoutMs = 8000;

constexpr const char* kGitHubApiLatestUrl =
    "https://api.github.com/repos/LaplaceTor/CS2WorkshopToolsLocalizerCN/releases/latest";
constexpr const char* kJsDelivrPackageUrl =
    "https://data.jsdelivr.com/v1/packages/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN";
constexpr const char* kGitHubReleasesLatestUrl =
    "https://github.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/releases/latest";
constexpr const char* kGitHubTagUrlPrefix =
    "https://github.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/releases/tag/";

struct DictTarget {
    const wchar_t* fileName;
    const char*    displayName;
    const char*    rawUrl;
    const char*    cdnUrl;
};

const DictTarget kDictTargets[] = {
    {
        L"qt_translations.jsonc",
        "qt_translations.jsonc",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/qt_translations.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/qt_translations.jsonc",
    },
    {
        L"fgd_translations.jsonc",
        "fgd_translations.jsonc",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_translations.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_translations.jsonc",
    },
    {
        L"fgd_override.jsonc",
        "fgd_override.jsonc",
        "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_override.jsonc",
        "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_override.jsonc",
    },
};

bool ParseSemver(const QString& raw, QVersionNumber& outVer) {
    static const QRegularExpression re(QStringLiteral("^v?(\\d+\\.\\d+\\.\\d+)$"));
    const QRegularExpressionMatch m = re.match(raw.trimmed());
    if (!m.hasMatch()) {
        return false;
    }
    outVer = QVersionNumber::fromString(m.captured(1));
    return !outVer.isNull();
}

QByteArray NormalizedSha256(QByteArray data) {
    data.replace("\r\n", "\n");
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool IsValidJsoncObject(const QByteArray& rawData) {
    const std::string clean =
        DictionaryCompiler::StripJsonComments(rawData.constData(), static_cast<size_t>(rawData.size()));
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(clean.data(), static_cast<qsizetype>(clean.size())),
        &err
    );
    return err.error == QJsonParseError::NoError && doc.isObject() && !doc.object().isEmpty();
}

QNetworkRequest MakeRequest(const QString& urlStr) {
    QNetworkRequest req{QUrl(urlStr)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "CS2WorkshopToolsLocalizerCN");
    req.setTransferTimeout(kRequestTimeoutMs);
    return req;
}

}

UpdateService::UpdateService(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this)) {}

void UpdateService::checkStartupUpdates(const std::wstring& workingDir, const DoneCallback& done) {
    struct SharedState {
        bool               appDone  = false;
        bool               dictDone = false;
        StartupCheckResult result;
        DoneCallback       done;
    };

    auto state = std::make_shared<SharedState>();
    state->done = done;

    auto tryFinish = [state]() {
        if (state->appDone && state->dictDone && state->done) {
            state->done(state->result);
        }
    };

    checkAppUpdate([state, tryFinish](const AppUpdateResult& appRes) {
        state->result.app = appRes;
        state->appDone    = true;
        tryFinish();
    });

    checkDictionaries(workingDir, [state, tryFinish](const DictCheckResult& dictRes) {
        state->result.dict = dictRes;
        state->dictDone    = true;
        tryFinish();
    });
}

void UpdateService::checkAppUpdate(std::function<void(const AppUpdateResult&)> callback) {
    const QString currentVerStr = QString::fromUtf8(APP_VERSION).trimmed();
    QVersionNumber currentVer;
    const bool isSemver = ParseSemver(currentVerStr, currentVer);

    auto finalize = [callback, currentVerStr, currentVer, isSemver](
                        bool ok, const QString& latestTag, const QString& releaseUrl) {
        AppUpdateResult res;
        res.checkOk        = ok;
        res.currentVersion = currentVerStr;
        res.latestVersion  = latestTag;
        res.releaseUrl     = releaseUrl.isEmpty() ? QString::fromLatin1(kGitHubReleasesLatestUrl) : releaseUrl;
        res.isDevBuild     = !isSemver;

        if (ok && isSemver) {
            QVersionNumber latestVer;
            if (ParseSemver(latestTag, latestVer) && latestVer > currentVer) {
                res.hasUpdate = true;
            }
        }
        callback(res);
    };

    auto tryGitHubRedirect = [this, finalize]() {
        QNetworkReply* reply = m_networkManager->get(MakeRequest(QString::fromLatin1(kGitHubReleasesLatestUrl)));
        connect(reply, &QNetworkReply::finished, this, [reply, finalize]() {
            reply->deleteLater();
            const QString finalUrl = reply->url().toString();
            const int tagIdx = finalUrl.indexOf(QStringLiteral("/releases/tag/"));
            if (reply->error() == QNetworkReply::NoError && tagIdx >= 0) {
                const QString tag = finalUrl.mid(tagIdx + 14).trimmed();
                QVersionNumber parsed;
                if (ParseSemver(tag, parsed)) {
                    finalize(true, tag, finalUrl);
                    return;
                }
            }
            finalize(false, QString(), QString());
        });
    };

    auto tryJsDelivr = [this, finalize, tryGitHubRedirect]() {
        QNetworkReply* reply = m_networkManager->get(MakeRequest(QString::fromLatin1(kJsDelivrPackageUrl)));
        connect(reply, &QNetworkReply::finished, this, [reply, finalize, tryGitHubRedirect]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status == 200) {
                const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                if (doc.isObject()) {
                    const QJsonArray versions = doc.object().value(QStringLiteral("versions")).toArray();
                    QVersionNumber bestVer;
                    for (const QJsonValue& val : versions) {
                        const QString vStr = val.toObject().value(QStringLiteral("version")).toString();
                        QVersionNumber vNum;
                        if (ParseSemver(vStr, vNum) && (bestVer.isNull() || vNum > bestVer)) {
                            bestVer = vNum;
                        }
                    }
                    if (!bestVer.isNull()) {
                        const QString tag = QStringLiteral("v") + bestVer.toString();
                        finalize(true, tag, QString::fromLatin1(kGitHubTagUrlPrefix) + tag);
                        return;
                    }
                }
            }
            tryGitHubRedirect();
        });
    };

    QNetworkRequest apiReq = MakeRequest(QString::fromLatin1(kGitHubApiLatestUrl));
    apiReq.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply* reply = m_networkManager->get(apiReq);
    connect(reply, &QNetworkReply::finished, this, [reply, finalize, tryJsDelivr]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isObject()) {
                const QJsonObject obj  = doc.object();
                const QString     tag  = obj.value(QStringLiteral("tag_name")).toString().trimmed();
                const QString     html = obj.value(QStringLiteral("html_url")).toString().trimmed();
                QVersionNumber parsed;
                if (ParseSemver(tag, parsed)) {
                    finalize(true, tag, html);
                    return;
                }
            }
        }
        tryJsDelivr();
    });
}

void UpdateService::checkDictionaries(const std::wstring& workingDir,
                                      std::function<void(const DictCheckResult&)> callback) {
    struct DictShared {
        size_t      remaining = std::size(kDictTargets);
        bool        allOk     = true;
        QStringList outdated;
    };

    auto shared = std::make_shared<DictShared>();

    for (const DictTarget& target : kDictTargets) {
        const QStringList urls{QString::fromLatin1(target.rawUrl), QString::fromLatin1(target.cdnUrl)};
        const QString dictName = QString::fromLatin1(target.displayName);
        const std::wstring localPath = ResolveDictionaryPath(workingDir, target.fileName).wstring();

        fetchUrlCandidates(urls, [shared, dictName, localPath, callback](bool ok, const QByteArray& data) {
            if (!ok || !IsValidJsoncObject(data)) {
                shared->allOk = false;
            } else {
                const QByteArray remoteHash = NormalizedSha256(data);
                QFile localFile(QString::fromStdWString(localPath));
                if (!localFile.open(QIODevice::ReadOnly)) {
                    shared->outdated.append(dictName);
                } else {
                    const QByteArray localHash = NormalizedSha256(localFile.readAll());
                    if (localHash != remoteHash) {
                        shared->outdated.append(dictName);
                    }
                }
            }

            if (--shared->remaining == 0) {
                DictCheckResult res;
                res.checkOk       = shared->allOk;
                res.outdatedDicts = shared->outdated;
                callback(res);
            }
        });
    }
}

void UpdateService::fetchUrlCandidates(
    const QStringList& urls,
    std::function<void(bool success, const QByteArray& data)> callback) {
    if (urls.isEmpty()) {
        callback(false, QByteArray());
        return;
    }

    auto fetchNext = std::make_shared<std::function<void(int)>>();
    *fetchNext = [this, urls, callback, fetchNext](int index) {
        if (index >= urls.size()) {
            callback(false, QByteArray());
            return;
        }

        QNetworkReply* reply = m_networkManager->get(MakeRequest(urls[index]));
        connect(reply, &QNetworkReply::finished, this, [reply, index, callback, fetchNext]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status == 200) {
                const QByteArray data = reply->readAll();
                if (!data.isEmpty()) {
                    callback(true, data);
                    return;
                }
            }
            (*fetchNext)(index + 1);
        });
    };

    (*fetchNext)(0);
}
