#pragma once

// 在线翻译词典的更新流程（原 MainWindow::onUpdateTranslationsClicked 及其周边辅助函数）。
//
// 职责边界：
//   - 只负责「下载 → 校验 → 落盘」这条业务流，不弹出任何对话框、不碰任何 UI 控件；
//   - 不自己实现 JSON 解析，统一复用 core 层的 DictionaryCompiler；
//   - 网络仍走 QNetworkAccessManager，不引入新的网络框架。
//
// 网络是异步的，因此结果通过 DoneCallback 回调外抛，由 UI 层决定如何提示用户。

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

    // 一次在线词典更新失败的原因
    enum class Failure {
        None,
        Fetch,  // 所有候选节点均不可达或超时
        Parse,  // 下载内容不是合法 JSONC，或内容为空
        Save    // 校验通过但原子落盘失败
    };

    struct UpdateResult {
        bool       ok       = false;
        Failure    failure  = Failure::None;
        QString    dictName;  // 失败的词典文件名（Fetch / Parse 时有效）
        QString    detail;    // 详细原因（如 JSON 解析错误描述）
        qsizetype  qtCount       = 0;
        qsizetype  fgdCount      = 0;
        qsizetype  overrideCount = 0;
    };

    // 日志回调：(消息, 颜色)
    using LogSink = std::function<void(const QString& message, const QString& color)>;

    // 结果回调：无论成功失败都会被调用一次
    using DoneCallback = std::function<void(const UpdateResult& result)>;

    // 串行拉取 qt / fgd / override 三个在线词典，全部校验通过后原子落盘。
    // 任一步失败都会中止后续拉取，并把失败原因带回 done 回调。
    void updateDictionaries(const std::wstring& workingDir,
                            const LogSink& log,
                            const DoneCallback& done);

private:
    // 一个在线词典的拉取结果
    struct OnlineDictionary {
        QByteArray    raw;    // 原始下载内容（落盘时使用）
        QJsonDocument doc;    // 剥离注释并解析后的文档（校验与计数使用）
        qsizetype     count;  // 有效条目数（日志与结果提示使用）
    };

    // 依次尝试多个候选 URL，取第一个返回 200 且内容非空的响应
    void fetchUrlCandidates(const QStringList& urls,
                            std::function<void(bool success, const QByteArray& data)> callback);

    // 单个词典的统一下载流程：尝试候选 URL → 剥离 JSONC 注释 → 解析 → 校验 → 计数
    void fetchOnlineDictionary(
        const QStringList& urls,
        const QString& stepLabel,                                // 如 "[1/3]"
        const QString& dictName,                                 // 如 "qt_translations.jsonc"
        const QString& desc,                                     // 如 "界面词典"
        std::function<qsizetype(const QJsonDocument&)> countOf,  // 有效条目数如何统计
        std::function<void(const OnlineDictionary&)> onSuccess
    );

    // 校验通过后用 QSaveFile 原子写入单个词典文件
    bool saveDictionaryAtomic(const QString& localPath, const QByteArray& data, const QString& name);

    void finishWith(const UpdateResult& result);

    QNetworkAccessManager* m_networkManager;

    LogSink      m_log;
    DoneCallback m_done;
};
