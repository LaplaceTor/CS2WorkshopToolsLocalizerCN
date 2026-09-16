#pragma once

#include "core/fgd_core.h"
#include <string>
#include <vector>
#include <unordered_map>

class FgdTranslator {
public:
    // 纯内存批量翻译整个 FGD 文本内容
    static bool TranslateContentToBuffer(
        const std::string& inputContent,
        std::string& outTranslated,
        const std::unordered_map<std::string, std::string>& dict,
        const FgdOverrideData& overrideData = FgdOverrideData()
    ) {
        return FgdCore::TranslateContent(inputContent, outTranslated, dict, overrideData);
    }
    // 加载 JSONC 字典文件 (fgd_translations.jsonc，支持可选的 fallback 兜底字典)
    static bool LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict, const std::wstring& fallbackJsonPath = L"");

    // 加载 FGD 覆盖字典文件 (fgd_override.jsonc)
    static bool LoadOverrideDictionary(const std::wstring& jsonPath, FgdOverrideData& outOverride);

    // 翻译单行 FGD 内容（带类状态机跟踪与跨行说明覆盖支持）
    static std::string TranslateLine(
        const std::string& line,
        const std::unordered_map<std::string, std::string>& dict,
        const FgdOverrideData& overrideData,
        std::string& inOutCurrentClass,
        std::string& inOutPendingClassDesc
    );

    static std::string TranslateLine(
        const std::string& line,
        const std::unordered_map<std::string, std::string>& dict,
        const FgdOverrideData& overrideData,
        std::string& inOutCurrentClass
    );

    // 兼容重载（无需状态与覆盖时）
    static std::string TranslateLine(const std::string& line, const std::unordered_map<std::string, std::string>& dict);

    // 确保 FGD 翻译字典存在，若不存在则自动生成带详细说明与范例的模板字典
    static bool EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);

    // 确保 FGD 覆盖字典存在，若不存在则自动生成带详细说明与范例的模板字典
    static bool EnsureFgdOverrideDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);

    // 确保 Qt 界面翻译字典存在，若不存在则自动生成带详细说明与范例的模板字典
    static bool EnsureQtDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);
};
