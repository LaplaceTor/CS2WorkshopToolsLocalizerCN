#include "service/dictionary_editor_service.h"
#include "core/dictionary_paths.h"
#include "core/dictionary_compiler.h"
#include "core/dictionary_text_updater.h"
#include "core/encoding_util.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

DictionaryEditorService::DictionaryEditorService(
    const std::wstring& cs2Root,
    const std::wstring& workingDir,
    HammerService* hammerService,
    QObject* parent
) : QObject(parent),
    m_cs2Root(cs2Root),
    m_workingDir(workingDir),
    m_hammerService(hammerService),
    m_captureReader(std::make_unique<capture::UntranslatedCaptureReader>())
{
}

bool DictionaryEditorService::ValidateSearchQuery(const QString& query, QString& outError) {
    int alphaCount = 0;
    int cjkCount = 0;
    for (const QChar& ch : query) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            alphaCount++;
        } else if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF) {
            cjkCount++;
        }
    }
    if (cjkCount < 2 && alphaCount < 4) {
        outError = "搜索关键字至少需要包含 4 个英文字母或 2 个汉字！";
        return false;
    }
    return true;
}

std::wstring DictionaryEditorService::resolveMasterPath(DictType type) const {
    if (type == DictType::QtUI) {
        return ResolveDictionaryPath(m_workingDir, L"qt_translations.jsonc").wstring();
    } else {
        return ResolveDictionaryPath(m_workingDir, L"fgd_translations.jsonc").wstring();
    }
}

std::wstring DictionaryEditorService::resolveFallbackPath(DictType type) const {
    if (type == DictType::QtUI) {
        return ResolveDictionaryPath(m_workingDir, L"qt_fallback.jsonc").wstring();
    } else {
        return ResolveDictionaryPath(m_workingDir, L"fgd_fallback.jsonc").wstring();
    }
}

std::vector<std::string> DictionaryEditorService::getAvailableModules(DictType type) {
    std::vector<std::string> modules;
    if (type == DictType::FGD) {
        return modules; // FGD 全局无分组
    }

    std::wstring masterPath = resolveMasterPath(type);
    std::unordered_map<std::string, std::string> commonDict;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> scopedDicts;
    std::wstring err;
    DictionaryCompiler::ParseJsoncFileToMaps(masterPath, commonDict, scopedDicts, err);

    modules.push_back("common");
    for (const auto& kv : scopedDicts) {
        std::string sMod = enc::WideToUtf8(kv.first);
        if (sMod != "common") {
            modules.push_back(sMod);
        }
    }
    std::sort(modules.begin() + 1, modules.end());
    return modules;
}

std::vector<DictEntryRecord> DictionaryEditorService::searchEntries(
    DictType type,
    const QString& moduleFilter,
    const QString& keyword,
    QString& outError
) {
    std::vector<DictEntryRecord> results;
    if (!ValidateSearchQuery(keyword, outError)) {
        return results;
    }

    QString lowerQuery = keyword.toLower();
    bool filterByModule = (type == DictType::QtUI && !moduleFilter.isEmpty() && moduleFilter != "全部");
    std::string targetModule = moduleFilter.toStdString();

    // 1. 检索主词典
    std::wstring masterPath = resolveMasterPath(type);
    std::unordered_map<std::string, std::string> masterCommon;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> masterScoped;
    std::wstring err;
    DictionaryCompiler::ParseJsoncFileToMaps(masterPath, masterCommon, masterScoped, err);

    auto matchText = [&](const std::string& key, const std::string& val) -> bool {
        QString qKey = QString::fromUtf8(key.c_str()).toLower();
        QString qVal = QString::fromUtf8(val.c_str()).toLower();
        return qKey.contains(lowerQuery) || qVal.contains(lowerQuery);
    };

    if (type == DictType::QtUI) {
        // common 分组
        if (!filterByModule || targetModule == "common") {
            for (const auto& kv : masterCommon) {
                if (matchText(kv.first, kv.second)) {
                    results.push_back({DictSource::Master, "common", kv.first, kv.second});
                }
            }
        }
        // scoped 分组
        for (const auto& sec : masterScoped) {
            std::string modStr = enc::WideToUtf8(sec.first);
            if (!filterByModule || targetModule == modStr) {
                for (const auto& kv : sec.second) {
                    if (matchText(kv.first, kv.second)) {
                        results.push_back({DictSource::Master, modStr, kv.first, kv.second});
                    }
                }
            }
        }
    } else {
        // FGD 主词典
        for (const auto& kv : masterCommon) {
            if (matchText(kv.first, kv.second)) {
                results.push_back({DictSource::Master, "", kv.first, kv.second});
            }
        }
    }

    // 2. 检索 Fallback 兜底词典
    std::wstring fallbackPath = resolveFallbackPath(type);
    if (fs::exists(fallbackPath)) {
        std::unordered_map<std::string, std::string> fallbackCommon;
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> fallbackScoped;
        DictionaryCompiler::ParseJsoncFileToMaps(fallbackPath, fallbackCommon, fallbackScoped, err);

        // Fallback 全局扁平匹配
        if (!filterByModule || targetModule == "common") {
            for (const auto& kv : fallbackCommon) {
                if (matchText(kv.first, kv.second)) {
                    // 若主词典中已存在此 key，则忽略 Fallback 项（以主词典为准）
                    if (masterCommon.find(kv.first) == masterCommon.end()) {
                        results.push_back({DictSource::Fallback, "", kv.first, kv.second});
                    }
                }
            }
        }
    }

    return results;
}

OperationResult DictionaryEditorService::saveEntry(
    DictType type,
    const DictEntryRecord& record,
    bool useMachineTrans
) {
    std::wstring masterPath = resolveMasterPath(type);
    std::wstring fallbackPath = resolveFallbackPath(type);

    dict_updater::UpdateResult res;
    if (record.source == DictSource::Fallback) {
        // Fallback 精翻升级：落盘至主词典，并从 Fallback 中物理删除
        res = dict_updater::AddEntry(masterPath, record.module, record.key, record.value);
        if (!res.success) {
            return {false, QString::fromStdString("写入主词典失败: " + res.errorMessage), false};
        }
        // 从 Fallback 中物理删除该旧条目
        dict_updater::DeleteEntry(fallbackPath, "", record.key);
    } else {
        // 主词典就地修改
        res = dict_updater::UpdateEntry(masterPath, record.module, record.key, record.value);
        if (!res.success) {
            return {false, QString::fromStdString("更新词典失败: " + res.errorMessage), false};
        }
    }

    // 联动热重载
    auto reloadRes = triggerReload(useMachineTrans);
    QString msg = (record.source == DictSource::Fallback)
        ? "✓ 词条已成功升级收录至主词典！"
        : "✓ 词条已成功更新保存！";

    if (reloadRes.ipcOk) {
        msg += " (已完成运行时热重载)";
    } else if (m_hammerService && m_hammerService->isRunning()) {
        msg += " (提示: 热重载 IPC 发送未获响应)";
    }

    return {true, msg, reloadRes.ipcOk};
}

OperationResult DictionaryEditorService::addEntry(
    DictType type,
    const std::string& module,
    const std::string& key,
    const std::string& value,
    bool useMachineTrans
) {
    std::wstring masterPath = resolveMasterPath(type);
    std::wstring fallbackPath = resolveFallbackPath(type);

    auto res = dict_updater::AddEntry(masterPath, module, key, value);
    if (!res.success) {
        return {false, QString::fromStdString("新增词条失败: " + res.errorMessage), false};
    }

    // 若 Fallback 中曾存在该条目，自动清理
    dict_updater::DeleteEntry(fallbackPath, "", key);

    auto reloadRes = triggerReload(useMachineTrans);
    QString msg = "✓ 新词条已成功添加至主词典！";
    if (reloadRes.ipcOk) {
        msg += " (已完成运行时热重载)";
    } else if (m_hammerService && m_hammerService->isRunning()) {
        msg += " (提示: 热重载 IPC 发送未获响应)";
    }

    return {true, msg, reloadRes.ipcOk};
}

OperationResult DictionaryEditorService::deleteEntry(
    DictType type,
    const DictEntryRecord& record,
    bool useMachineTrans
) {
    dict_updater::UpdateResult res;
    if (record.source == DictSource::Fallback) {
        std::wstring fallbackPath = resolveFallbackPath(type);
        res = dict_updater::DeleteEntry(fallbackPath, "", record.key);
    } else {
        std::wstring masterPath = resolveMasterPath(type);
        res = dict_updater::DeleteEntry(masterPath, record.module, record.key);
    }

    if (!res.success) {
        return {false, QString::fromStdString("删除词条失败: " + res.errorMessage), false};
    }

    auto reloadRes = triggerReload(useMachineTrans);
    QString msg = "✓ 词条已成功物理删除！";
    if (reloadRes.ipcOk) {
        msg += " (已完成运行时热重载)";
    } else if (m_hammerService && m_hammerService->isRunning()) {
        msg += " (提示: 热重载 IPC 发送未获响应)";
    }

    return {true, msg, reloadRes.ipcOk};
}

std::vector<capture::UntranslatedCaptureReader::Item> DictionaryEditorService::fetchCaptureEntries() {
    if (m_captureReader) {
        return m_captureReader->fetchEntries();
    }
    return {};
}

void DictionaryEditorService::clearCaptureEntries() {
    if (m_captureReader) {
        m_captureReader->clearAll();
    }
}

HammerService::ReloadResult DictionaryEditorService::triggerReload(bool useMachineTrans) {
    if (m_hammerService && (m_hammerService->isRunning() || m_hammerService->isIpcAvailable())) {
        return m_hammerService->reloadDictionaries(m_cs2Root, m_workingDir, useMachineTrans);
    }
    return {};
}
