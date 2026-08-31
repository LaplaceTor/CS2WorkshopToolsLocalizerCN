#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "lcld_format.h"

class DictionaryCompiler {
public:
    static constexpr size_t MAX_JSON_FILE_SIZE = 16 * 1024 * 1024;      // 16 MB 文本上限
    static constexpr size_t MAX_LCLD_FILE_SIZE = 32 * 1024 * 1024;      // 32 MB 二进制上限
    static constexpr size_t MAX_SINGLE_STRING_LENGTH = 65536;            // 64 KB 单字符串上限
    static constexpr size_t MAX_TOTAL_SECTIONS = 1000;                   // 1000 个作用域上限
    static constexpr size_t MAX_TOTAL_ENTRIES = 200000;                  // 20 万条翻译上限

    // 将包含 JSONC 注释的 JSON 文本编译为纯二进制 LCLD 格式
    static bool CompileJsonStringToBinary(const std::string& jsonContent, std::vector<uint8_t>& outBinary, std::wstring& outError);

    // 将 JSON 字典文件编译并保存为 .lcld 二进制字典文件
    static bool CompileJsonFileToLcld(const std::wstring& jsonPath, const std::wstring& lcldPath, std::wstring& outError);

    // 从二进制 LCLD 内存数据直接解析到字典 Map（0 Qt 依赖，极速纯 C ABI 内存解析）
    static bool ParseLcldBinaryToMaps(
        const uint8_t* data,
        size_t size,
        std::unordered_map<std::string, std::string>& outCommon,
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped
    );

    // 去除 JSONC 注释辅助函数
    static std::string StripJsonComments(const char* p, size_t length);
};

