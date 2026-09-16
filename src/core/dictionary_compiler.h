#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

class DictionaryCompiler {
public:
    static constexpr size_t MAX_JSON_FILE_SIZE = 16 * 1024 * 1024;      // 16 MB 文本上限
    static constexpr size_t MAX_SINGLE_STRING_LENGTH = 65536;            // 64 KB 单字符串上限
    static constexpr size_t MAX_TOTAL_SECTIONS = 1000;                   // 1000 个作用域上限
    static constexpr size_t MAX_TOTAL_ENTRIES = 200000;                  // 20 万条翻译上限

    // 解析包含 JSONC 注释的 JSON 文本到内存字典 Map（支持顶层与嵌套子模块）
    static bool ParseJsoncStringToMaps(
        const std::string& jsonContent,
        std::unordered_map<std::string, std::string>& outCommon,
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
        std::wstring& outError
    );

    // 从 JSONC 文件解析到内存字典 Map（支持可选的 fallback 兜底字典合并）
    static bool ParseJsoncFileToMaps(
        const std::wstring& jsonPath,
        std::unordered_map<std::string, std::string>& outCommon,
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
        std::wstring& outError,
        const std::wstring& fallbackJsonPath = L""
    );

    // 去除 JSONC 注释与尾随逗号辅助函数
    static std::string StripJsonComments(const char* p, size_t length);
};

