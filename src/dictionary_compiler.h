#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "lcld_format.h"

class DictionaryCompiler {
public:
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

