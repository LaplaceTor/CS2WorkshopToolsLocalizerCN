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
    std::string module;
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

    static bool ValidateSearchQuery(const QString& query, QString& outError);

    std::vector<DictEntryRecord> searchEntries(
        DictType type,
        const QString& moduleFilter,
        const QString& keyword,
        QString& outError
    );

    std::vector<std::string> getAvailableModules(DictType type);

    OperationResult saveEntry(
        DictType type,
        const DictEntryRecord& record,
        bool useMachineTrans
    );

    OperationResult addEntry(
        DictType type,
        const std::string& module,
        const std::string& key,
        const std::string& value,
        bool useMachineTrans
    );

    OperationResult deleteEntry(
        DictType type,
        const DictEntryRecord& record,
        bool useMachineTrans
    );

    std::vector<capture::UntranslatedCaptureReader::Item> fetchCaptureEntries();

    void clearCaptureEntries();

    HammerService::ReloadResult triggerReload(bool useMachineTrans);

private:
    std::wstring resolveMasterPath(DictType type) const;
    std::wstring resolveFallbackPath(DictType type) const;

    std::wstring m_cs2Root;
    std::wstring m_workingDir;
    HammerService* m_hammerService{nullptr};
    std::unique_ptr<capture::UntranslatedCaptureReader> m_captureReader;
};
