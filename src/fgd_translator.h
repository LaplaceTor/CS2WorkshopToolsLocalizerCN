#pragma once

#include <string>
#include <vector>
#include <unordered_map>

class FgdTranslator {
public:
    // 加载 JSON 字典文件
    static bool LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict);

    // 翻译单行 FGD 内容
    static std::string TranslateLine(const std::string& line, const std::unordered_map<std::string, std::string>& dict);

    // 翻译单个 FGD 文件并写入目标路径
    static bool TranslateFile(const std::wstring& srcPath, const std::wstring& dstPath, const std::unordered_map<std::string, std::string>& dict);

    // 批量翻译所有 FGD 文件（读取 backup 相对路径，写入 translations 相对路径，并覆盖到 CS2 对应目录）
    static bool TranslateAndDeployAll(
        const std::wstring& cs2Root,
        const std::wstring& backupDir,
        const std::wstring& translationsDir,
        const std::wstring& jsonDictPath,
        std::vector<std::wstring>& outProcessedFiles,
        std::wstring& outError
    );
    // 确保 FGD 翻译字典存在，若不存在则自动生成带详细说明与范例的模板字典
    static bool EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);

    // 确保 Qt 界面翻译字典存在，若不存在则自动生成带详细说明与范例的模板字典
    static bool EnsureQtDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice);
};

