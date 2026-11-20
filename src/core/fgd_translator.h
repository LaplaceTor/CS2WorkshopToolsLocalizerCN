#pragma once

#include "core/fgd_core.h"
#include <string>
#include <vector>
#include <unordered_map>

class FgdTranslator {
public:
    static bool TranslateContentToBuffer(
        const std::string& inputContent,
        std::string& outTranslated,
        const std::unordered_map<std::string, std::string>& dict,
        const FgdOverrideData& overrideData = FgdOverrideData()
    ) {
        return FgdCore::TranslateContent(inputContent, outTranslated, dict, overrideData);
    }
    static bool LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict, const std::wstring& fallbackJsonPath = L"");

    static bool LoadOverrideDictionary(const std::wstring& jsonPath, FgdOverrideData& outOverride);

    static std::string TranslateLine(
        const std::string& line,
        const std::unordered_map<std::string, std::string>& dict,
        const FgdOverrideData& overrideData,
        FgdCore::LineState& state
    );

    static bool EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);

    static bool EnsureFgdOverrideDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);

    static bool EnsureQtDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);
};
