#pragma once

// 启动时的自动更新检查服务（程序版本检查 + 在线词典更新检查）。
//
// 职责边界：
//   - 仅负责异步网络探测、版本/哈希比对与结果聚合；
//   - 不弹出任何对话框、不操作任何 Qt 控件；
//   - 不落盘覆盖本地文件（词典实际下载与落盘仍由 DictionaryService 负责）。

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
        QStringList outdatedDicts;  // 与远端存在差异的词典文件名列表
    };

    struct StartupCheckResult {
        AppUpdateResult app;
        DictCheckResult dict;
    };

    using DoneCallback = std::function<void(const StartupCheckResult& result)>;

    // 并发执行一次程序版本检查与在线词典差异检查，全部完成后触发 done 回调。
    void checkStartupUpdates(const std::wstring& workingDir, const DoneCallback& done);

private:
    void checkAppUpdate(std::function<void(const AppUpdateResult&)> callback);
    void checkDictionaries(const std::wstring& workingDir,
                           std::function<void(const DictCheckResult&)> callback);

    void fetchUrlCandidates(const QStringList& urls,
                            std::function<void(bool success, const QByteArray& data)> callback);

    QNetworkAccessManager* m_networkManager;
};
