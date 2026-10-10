#pragma once

#include <string>
#include <vector>
#include <memory>
#include <QString>
#include <QObject>

#include "core/untranslated_capture.h"
#include "service/hammer_service.h"

enum class DictType {
    QtUI,
    FGD
};

enum class DictSource {
    Master,
    Fallback
};

struct DictEntryRecord {
    DictSource source;
    std::string module; // e.g. "hammer", "common", or ""
    std::string key;
    std::string value;
};

struct OperationResult {
    bool success{false};
    QString message;
    bool reloaded{false};
};

class DictionaryEditorService : public QObject {
    Q_OBJECT

public:
    explicit DictionaryEditorService(
        const std::wstring& cs2Root,
        const std::wstring& workingDir,
        HammerService* hammerService,
        QObject* parent = nullptr
    );
    ~DictionaryEditorService() override = default;

    // 验证搜索关键字（英文字母 >= 4 或 中文汉字 >= 2）
    static bool ValidateSearchQuery(const QString& query, QString& outError);

    // 搜索词典库
    std::vector<DictEntryRecord> searchEntries(
        DictType type,
        const QString& moduleFilter,
        const QString& keyword,
        QString& outError
    );

    // 获取特定词典可用的模块列表 (例如 "common", "hammer", "assetbrowser" 等)
    std::vector<std::string> getAvailableModules(DictType type);

    // 保存条目（支持 Fallback 自动精翻升级为主词典并剔除 Fallback 冗余）
    OperationResult saveEntry(
        DictType type,
        const DictEntryRecord& record,
        bool useMachineTrans
    );

    // 新增条目（始终保存至主词典，并从 Fallback 中剔除冲突）
    OperationResult addEntry(
        DictType type,
        const std::string& module,
        const std::string& key,
        const std::string& value,
        bool useMachineTrans
    );

    // 删除条目（物理删除）
    OperationResult deleteEntry(
        DictType type,
        const DictEntryRecord& record,
        bool useMachineTrans
    );

    // 拉取运行时未翻译捕获列表
    std::vector<capture::UntranslatedCaptureReader::Item> fetchCaptureEntries();

    // 清空运行时捕获池
    void clearCaptureEntries();

    // 触发热重载
    HammerService::ReloadResult triggerReload(bool useMachineTrans);

private:
    std::wstring resolveMasterPath(DictType type) const;
    std::wstring resolveFallbackPath(DictType type) const;

    std::wstring m_cs2Root;
    std::wstring m_workingDir;
    HammerService* m_hammerService{nullptr};
    std::unique_ptr<capture::UntranslatedCaptureReader> m_captureReader;
};
