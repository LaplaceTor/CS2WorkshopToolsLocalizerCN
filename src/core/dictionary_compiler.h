#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "core/fgd_core.h"

class DictionaryCompiler {
public:
    static constexpr size_t MAX_JSON_FILE_SIZE = 16 * 1024 * 1024;
    static constexpr size_t MAX_SINGLE_STRING_LENGTH = 65536;
    static constexpr size_t MAX_TOTAL_SECTIONS = 1000;
    static constexpr size_t MAX_TOTAL_ENTRIES = 200000;

    static bool ParseJsoncStringToMaps(
        const std::string& jsonContent,
        std::unordered_map<std::string, std::string>& outCommon,
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
        std::wstring& outError
    );

    static bool ParseJsoncFileToMaps(
        const std::wstring& jsonPath,
        std::unordered_map<std::string, std::string>& outCommon,
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
        std::wstring& outError,
        const std::wstring& fallbackJsonPath = L""
    );

    static bool ParseFgdOverrideFile(
        const std::wstring& jsonPath,
        struct FgdOverrideData& outOverride,
        std::wstring& outError
    );

    static std::string StripJsonComments(const char* p, size_t length);
};

