#include "service/dictionary_service.h"

#include <filesystem>

#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>

#include "core/dictionary_compiler.h"
#include "core/dictionary_paths.h"
#include "core/fgd_translator.h"
#include "core/path_constants.h"

namespace fs = std::filesystem;

namespace {

// 在线词典的候选节点：GitHub 原始地址作为首选，jsDelivr CDN 作为国内可达性兜底
const char* const kQtUrls[] = {
    "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/qt_translations.jsonc",
    "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/qt_translations.jsonc",
};

const char* const kFgdUrls[] = {
    "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_translations.jsonc",
    "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_translations.jsonc",
};

const char* const kOverrideUrls[] = {
    "https://raw.githubusercontent.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/main/translations/fgd_override.jsonc",
    "https://cdn.jsdelivr.net/gh/LaplaceTor/CS2WorkshopToolsLocalizerCN@main/translations/fgd_override.jsonc",
};

QStringList toList(const char* const (&arr)[2]) {
    return QStringList{arr[0], arr[1]};
}

// 剥离 JSONC 注释，使其可被 QJsonDocument 解析（含尾随逗号移除）
QByteArray StripJsonc(const QByteArray& input) {
    const std::string stripped =
        DictionaryCompiler::StripJsonComments(input.constData(), static_cast<size_t>(input.size()));
    return QByteArray(stripped.data(), static_cast<qsizetype>(stripped.size()));
}

// 统计 fgd_override 词典的有效规则数：
// properties / io / classes 三个子对象的键数之和，再加上其余非下划线开头的顶层键；
// 三者合计为 0 时退化为顶层键总数。
qsizetype CountOverrideRules(const QJsonObject& obj) {
    qsizetype count = 0;

    for (const char* section : {"properties", "io", "classes"}) {
        if (obj.contains(section) && obj[section].isObject()) {
            count += obj[section].toObject().keys().size();
        }
    }

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() != "properties" && it.key() != "io" && it.key() != "classes" &&
            !it.key().startsWith("_")) {
            ++count;
        }
    }

    return (count == 0) ? obj.keys().size() : count;
}

}  // namespace

DictionaryService::DictionaryService(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this)) {}

QStringList DictionaryService::ensureTemplates(const std::wstring& workingDir) {
    // 三个词典的模板生成函数签名一致，用表驱动避免三份复制粘贴
    struct TemplateSpec {
        const wchar_t* fileName;
        bool (*ensure)(const std::wstring&, const std::wstring&, std::wstring&);
    };

    const TemplateSpec specs[] = {
        { L"fgd_translations.jsonc", &FgdTranslator::EnsureFgdDictionaryExists },
        { L"fgd_override.jsonc",     &FgdTranslator::EnsureFgdOverrideDictionaryExists },
        { paths::kQtDictFile,        &FgdTranslator::EnsureQtDictionaryExists },
    };

    QStringList notices;
    std::wstring notice;
    for (const TemplateSpec& spec : specs) {
        if (spec.ensure(ResolveDictionaryPath(workingDir, spec.fileName).wstring(), L"", notice)) {
            notices << QString::fromStdWString(notice);
        }
    }
    return notices;
}

void DictionaryService::updateDictionaries(const std::wstring& workingDir,
                                           const LogSink& log,
                                           const DoneCallback& done) {
    m_log  = log;
    m_done = done;

    // 依次拉取三个在线词典：任一步失败都会中止（失败收尾由 fetchOnlineDictionary 统一处理）
    fetchOnlineDictionary(
        toList(kQtUrls),
        "[1/3]",
        "qt_translations.jsonc",
        "界面词典",
        [](const QJsonDocument& doc) { return doc.object().keys().size(); },
        [this, workingDir](const OnlineDictionary& qt) {
            m_log(
                QString("[+] qt_translations.jsonc 获取成功，有效词条: %1 条").arg(qt.count),
                "#a6e22e"
            );

            fetchOnlineDictionary(
                toList(kFgdUrls),
                "[2/3]",
                "fgd_translations.jsonc",
                "实体定义词典",
                [](const QJsonDocument& doc) { return doc.object().keys().size(); },
                [this, workingDir, qt](const OnlineDictionary& fgd) {
                    m_log(
                        QString("[+] fgd_translations.jsonc 获取成功，有效词条: %1 条").arg(fgd.count),
                        "#a6e22e"
                    );

                    fetchOnlineDictionary(
                        toList(kOverrideUrls),
                        "[3/3]",
                        "fgd_override.jsonc",
                        "实体覆盖词典",
                        [](const QJsonDocument& doc) { return CountOverrideRules(doc.object()); },
                        [this, workingDir, qt, fgd](const OnlineDictionary& ovr) {
                            m_log(
                                QString("[+] fgd_override.jsonc 获取成功，有效规则: %1 条").arg(ovr.count),
                                "#a6e22e"
                            );

                            fs::path transDir = fs::path(workingDir) / paths::kTranslationsDir;
                            std::error_code ec;
                            fs::create_directories(transDir, ec);

                            const QString qtLocalPath =
                                QString::fromStdWString((transDir / L"qt_translations.jsonc").wstring());
                            const QString fgdLocalPath =
                                QString::fromStdWString((transDir / L"fgd_translations.jsonc").wstring());
                            const QString overrideLocalPath =
                                QString::fromStdWString((transDir / L"fgd_override.jsonc").wstring());

                            if (!saveDictionaryAtomic(qtLocalPath, qt.raw, "qt_translations.jsonc") ||
                                !saveDictionaryAtomic(fgdLocalPath, fgd.raw, "fgd_translations.jsonc") ||
                                !saveDictionaryAtomic(overrideLocalPath, ovr.raw, "fgd_override.jsonc")) {
                                finishWith(UpdateResult{false, Failure::Save, QString(), QString(), 0, 0, 0});
                                return;
                            }

                            UpdateResult result;
                            result.ok            = true;
                            result.qtCount       = qt.count;
                            result.fgdCount      = fgd.count;
                            result.overrideCount = ovr.count;
                            finishWith(result);
                        }
                    );
                }
            );
        }
    );
}

void DictionaryService::fetchUrlCandidates(
    const QStringList& urls,
    std::function<void(bool success, const QByteArray& data)> callback
) {
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

        QUrl url(urls[index]);

        QNetworkRequest request(url);

        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy
        );

        request.setHeader(
            QNetworkRequest::UserAgentHeader,
            "CS2WorkshopToolsLocalizerCN"
        );

        request.setTransferTimeout(10000);

        QNetworkReply* reply = m_networkManager->get(request);

        connect(reply, &QNetworkReply::finished, this,
                [reply, index, callback, fetchNext]() {
            reply->deleteLater();

            const int statusCode =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (reply->error() == QNetworkReply::NoError && statusCode == 200) {
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

void DictionaryService::fetchOnlineDictionary(
    const QStringList& urls,
    const QString& stepLabel,
    const QString& dictName,
    const QString& desc,
    std::function<qsizetype(const QJsonDocument&)> countOf,
    std::function<void(const OnlineDictionary&)> onSuccess
) {
    m_log(
        QString("%1 正在获取 %2 (%3)...").arg(stepLabel, dictName, desc),
        "#e6db74"
    );

    fetchUrlCandidates(
        urls,
        [this, dictName, countOf, onSuccess](bool ok, const QByteArray& data) {
            if (!ok) {
                finishWith(UpdateResult{false, Failure::Fetch, dictName, QString(), 0, 0, 0});
                return;
            }

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(StripJsonc(data), &parseErr);

            if (parseErr.error != QJsonParseError::NoError ||
                !doc.isObject() || doc.object().isEmpty()) {
                finishWith(UpdateResult{false, Failure::Parse, dictName, parseErr.errorString(), 0, 0, 0});
                return;
            }

            OnlineDictionary result;
            result.raw   = data;
            result.doc   = doc;
            result.count = countOf(doc);
            onSuccess(result);
        }
    );
}

bool DictionaryService::saveDictionaryAtomic(const QString& localPath,
                                             const QByteArray& data,
                                             const QString& name) {
    const std::string clean =
        DictionaryCompiler::StripJsonComments(data.constData(), static_cast<size_t>(data.size()));

    QJsonParseError parseErr;
    const QJsonDocument doc =
        QJsonDocument::fromJson(
            QByteArray(clean.data(), static_cast<qsizetype>(clean.size())),
            &parseErr
        );

    if (parseErr.error != QJsonParseError::NoError || doc.isNull() || !doc.isObject()) {
        m_log(
            QString("[-] 词典数据校验未通过，已放弃写入 %1: %2").arg(name, parseErr.errorString()),
            "#f92672"
        );
        return false;
    }

    QSaveFile saveFile(localPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_log(
            QString("[-] 无法以安全原子模式打开文件 %1: %2").arg(localPath, saveFile.errorString()),
            "#f92672"
        );
        return false;
    }

    const qint64 written = saveFile.write(data);
    if (written != data.size()) {
        saveFile.cancelWriting();
        m_log(
            QString("[-] 写入数据不完整 (%1): 预期 %2 字节，实际写入 %3 字节")
                .arg(localPath).arg(data.size()).arg(written),
            "#f92672"
        );
        return false;
    }

    if (!saveFile.commit()) {
        m_log(
            QString("[-] 提交安全写入失败 (%1): %2").arg(localPath, saveFile.errorString()),
            "#f92672"
        );
        return false;
    }

    return true;
}

void DictionaryService::finishWith(const UpdateResult& result) {
    if (m_done) {
        m_done(result);
    }
}
