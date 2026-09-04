#include "dictionary_compiler.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

std::string DictionaryCompiler::StripJsonComments(const char* p, size_t length) {
    std::string out;
    out.reserve(length);
    const char* end = p + length;
    bool inQuote = false;
    bool escape = false;

    while (p < end) {
        if (inQuote) {
            char c = *p++;
            out.push_back(c);
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inQuote = false;
            }
        } else {
            if (*p == '"') {
                inQuote = true;
                out.push_back(*p++);
            } else if (*p == '/' && (p + 1 < end)) {
                if (*(p + 1) == '/') {
                    p += 2;
                    while (p < end && *p != '\n' && *p != '\r') p++;
                } else if (*(p + 1) == '*') {
                    p += 2;
                    while (p + 1 < end && !(*p == '*' && *(p + 1) == '/')) p++;
                    if (p + 1 < end) p += 2;
                    else p = end;
                } else {
                    out.push_back(*p++);
                }
            } else {
                out.push_back(*p++);
            }
        }
    }

    // 移除尾随逗号 (trailing commas before } or ])，使 JSONC 兼容 QJsonDocument
    std::string result;
    result.reserve(out.size());
    inQuote = false;
    escape = false;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (inQuote) {
            result.push_back(c);
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inQuote = false;
        } else {
            if (c == '"') {
                inQuote = true;
                result.push_back(c);
            } else if (c == ',') {
                size_t j = i + 1;
                while (j < out.size() && (out[j] == ' ' || out[j] == '\t' || out[j] == '\r' || out[j] == '\n')) {
                    j++;
                }
                if (j < out.size() && (out[j] == '}' || out[j] == ']')) {
                    continue; // 跳过尾随逗号
                }
                result.push_back(c);
            } else {
                result.push_back(c);
            }
        }
    }
    return result;
}

namespace {

class SimpleJsonParser {
public:
    explicit SimpleJsonParser(const std::string& str) : m_str(str), m_idx(0), m_len(str.size()) {}

    bool Parse(std::unordered_map<std::string, std::string>& outCommon,
               std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
               std::wstring& outError) {
        SkipWhitespace();
        if (m_idx >= m_len || m_str[m_idx] != '{') {
            outError = L"JSON 根节点必须为对象 '{'";
            return false;
        }
        m_idx++; // skip '{'

        while (true) {
            SkipWhitespace();
            if (m_idx >= m_len) {
                outError = L"JSON 解析异常结束，缺少闭合 '}'";
                return false;
            }
            if (m_str[m_idx] == '}') {
                m_idx++;
                break;
            }

            std::string key;
            if (!ParseString(key, outError)) {
                return false;
            }

            SkipWhitespace();
            if (m_idx >= m_len || m_str[m_idx] != ':') {
                outError = L"JSON 缺少冒号 ':' 分隔符";
                return false;
            }
            m_idx++; // skip ':'
            SkipWhitespace();

            if (m_idx < m_len && m_str[m_idx] == '{') {
                // 嵌套作用域对象 (如 "hammer": { ... })
                m_idx++; // skip '{'
                std::wstring secName = NormalizeSectionName(key);
                auto& secDict = (secName == L"common" || secName == L"general") ? outCommon : outScoped[secName];

                while (true) {
                    SkipWhitespace();
                    if (m_idx >= m_len) {
                        outError = L"子作用域对象解析异常，缺少 '}'";
                        return false;
                    }
                    if (m_str[m_idx] == '}') {
                        m_idx++;
                        break;
                    }

                    std::string subKey;
                    if (!ParseString(subKey, outError)) return false;

                    SkipWhitespace();
                    if (m_idx >= m_len || m_str[m_idx] != ':') {
                        outError = L"子条目缺少冒号 ':'";
                        return false;
                    }
                    m_idx++;
                    SkipWhitespace();

                    std::string subVal;
                    if (!ParseString(subVal, outError)) return false;

                    if (!subKey.empty() && !subVal.empty()) {
                        secDict[subKey] = subVal;
                    }

                    SkipWhitespace();
                    if (m_idx < m_len && m_str[m_idx] == ',') {
                        m_idx++;
                    }
                }
            } else if (m_idx < m_len && m_str[m_idx] == '"') {
                // 扁平顶级字符串键值对 (归入 common)
                std::string val;
                if (!ParseString(val, outError)) return false;
                if (!key.empty() && !val.empty()) {
                    outCommon[key] = val;
                }
            } else {
                outError = L"不支持的 JSON 值类型";
                return false;
            }

            SkipWhitespace();
            if (m_idx < m_len && m_str[m_idx] == ',') {
                m_idx++;
            }
        }
        return true;
    }

private:
    void SkipWhitespace() {
        while (m_idx < m_len && (m_str[m_idx] == ' ' || m_str[m_idx] == '\t' ||
                                 m_str[m_idx] == '\r' || m_str[m_idx] == '\n')) {
            m_idx++;
        }
    }

    bool ParseString(std::string& outStr, std::wstring& outError) {
        SkipWhitespace();
        if (m_idx >= m_len || m_str[m_idx] != '"') {
            outError = L"期望字符串起始双引号 '\"'";
            return false;
        }
        m_idx++; // skip '"'

        outStr.clear();
        while (m_idx < m_len) {
            char c = m_str[m_idx++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (m_idx >= m_len) {
                    outError = L"转义字符 '\\' 未闭合";
                    return false;
                }
                char esc = m_str[m_idx++];
                switch (esc) {
                    case '"':  outStr.push_back('"'); break;
                    case '\\': outStr.push_back('\\'); break;
                    case '/':  outStr.push_back('/'); break;
                    case 'b':  outStr.push_back('\b'); break;
                    case 'f':  outStr.push_back('\f'); break;
                    case 'n':  outStr.push_back('\n'); break;
                    case 'r':  outStr.push_back('\r'); break;
                    case 't':  outStr.push_back('\t'); break;
                    case 'u': {
                        if (m_idx + 4 > m_len) {
                            outError = L"无效的 \\u unicode 转义";
                            return false;
                        }
                        unsigned int codePoint = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = m_str[m_idx++];
                            codePoint <<= 4;
                            if (h >= '0' && h <= '9') codePoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codePoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codePoint |= (h - 'A' + 10);
                            else {
                                outError = L"非十六进制 unicode 字符";
                                return false;
                            }
                        }
                        // Encode codePoint to UTF-8
                        if (codePoint <= 0x7F) {
                            outStr.push_back(static_cast<char>(codePoint));
                        } else if (codePoint <= 0x7FF) {
                            outStr.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
                            outStr.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                        } else {
                            outStr.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
                            outStr.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                            outStr.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                        }
                        break;
                    }
                    default:
                        outStr.push_back(esc);
                        break;
                }
            } else {
                outStr.push_back(c);
            }
            if (outStr.size() > DictionaryCompiler::MAX_SINGLE_STRING_LENGTH) {
                outError = L"单字符串长度超出 64KB 安全上限";
                return false;
            }
        }
        outError = L"字符串未正常闭合";
        return false;
    }

    static std::wstring NormalizeSectionName(const std::string& name) {
        std::wstring wname;
        wname.reserve(name.size());
        for (char c : name) wname.push_back(static_cast<wchar_t>(c));
        std::transform(wname.begin(), wname.end(), wname.begin(), ::towlower);
        if (wname.length() > 4 && wname.substr(wname.length() - 4) == L".dll") {
            wname = wname.substr(0, wname.length() - 4);
        }
        return wname;
    }

    const std::string& m_str;
    size_t m_idx;
    size_t m_len;
};

} // namespace

bool DictionaryCompiler::ParseJsoncStringToMaps(
    const std::string& jsonContent,
    std::unordered_map<std::string, std::string>& outCommon,
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
    std::wstring& outError
) {
    if (jsonContent.empty()) {
        outError = L"JSON 文本为空";
        return false;
    }

    if (jsonContent.size() > MAX_JSON_FILE_SIZE) {
        outError = L"JSON 文本大小超出 16MB 安全上限";
        return false;
    }

    // 1. 去除注释与尾随逗号
    std::string cleanJson = StripJsonComments(jsonContent.data(), jsonContent.size());

    // 2. 解析为分块字典结构
    SimpleJsonParser parser(cleanJson);
    if (!parser.Parse(outCommon, outScoped, outError)) {
        return false;
    }

    return (!outCommon.empty() || !outScoped.empty());
}

bool DictionaryCompiler::ParseJsoncFileToMaps(
    const std::wstring& jsonPath,
    std::unordered_map<std::string, std::string>& outCommon,
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped,
    std::wstring& outError,
    const std::wstring& fallbackJsonPath
) {
    outCommon.clear();
    outScoped.clear();

    // 1. 若提供了 fallback 字典，先加载 fallback 作为兜底层
    if (!fallbackJsonPath.empty() && fs::exists(fallbackJsonPath)) {
        FILE* fbFp = _wfopen(fallbackJsonPath.c_str(), L"rb");
        if (fbFp) {
            fseek(fbFp, 0, SEEK_END);
            long fbSize = ftell(fbFp);
            fseek(fbFp, 0, SEEK_SET);
            if (fbSize > 0 && fbSize <= static_cast<long>(MAX_JSON_FILE_SIZE)) {
                std::string fbStr(static_cast<size_t>(fbSize), '\0');
                fread(fbStr.data(), 1, fbSize, fbFp);
                fclose(fbFp);
                fbFp = nullptr;

                std::string cleanFb = StripJsonComments(fbStr.data(), fbStr.size());
                std::wstring fbErr;
                SimpleJsonParser fbParser(cleanFb);
                fbParser.Parse(outCommon, outScoped, fbErr);
            } else {
                fclose(fbFp);
            }
        }
    }

    // 2. 加载主字典覆盖/补充
    FILE* fp = _wfopen(jsonPath.c_str(), L"rb");
    if (!fp) {
        if (!outCommon.empty() || !outScoped.empty()) {
            return true; // fallback 字典有效
        }
        outError = L"无法打开源 JSONC 文件: " + jsonPath;
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > static_cast<long>(MAX_JSON_FILE_SIZE)) {
        fclose(fp);
        if (!outCommon.empty() || !outScoped.empty()) {
            return true;
        }
        outError = L"源 JSONC 文件为空或大小超出 16MB 安全上限";
        return false;
    }

    std::string jsonStr(static_cast<size_t>(fsize), '\0');
    fread(jsonStr.data(), 1, fsize, fp);
    fclose(fp);

    std::string cleanPrimary = StripJsonComments(jsonStr.data(), jsonStr.size());
    SimpleJsonParser primaryParser(cleanPrimary);
    if (!primaryParser.Parse(outCommon, outScoped, outError)) {
        if (!outCommon.empty() || !outScoped.empty()) {
            return true;
        }
        return false;
    }

    return (!outCommon.empty() || !outScoped.empty());
}

static std::string EscapeJsonStr(const std::string& str) {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

bool DictionaryCompiler::MergeJsonFiles(
    const std::wstring& primaryJsonPath,
    const std::wstring& fallbackJsonPath,
    const std::wstring& outJsonPath,
    std::wstring& outError
) {
    std::unordered_map<std::string, std::string> commonDict;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> scopedDicts;

    if (!ParseJsoncFileToMaps(primaryJsonPath, commonDict, scopedDicts, outError, fallbackJsonPath)) {
        outError = L"合并字典失败：未找到有效词条";
        return false;
    }

    FILE* outFp = _wfopen(outJsonPath.c_str(), L"wb");
    if (!outFp) {
        outError = L"无法创建目标 JSONC 文件: " + outJsonPath;
        return false;
    }

    fputs("{\n", outFp);
    bool first = true;
    for (const auto& kv : commonDict) {
        if (!first) fputs(",\n", outFp);
        first = false;
        std::string line = "  \"" + EscapeJsonStr(kv.first) + "\": \"" + EscapeJsonStr(kv.second) + "\"";
        fputs(line.c_str(), outFp);
    }

    for (const auto& sc : scopedDicts) {
        if (sc.second.empty()) continue;
        if (!first) fputs(",\n", outFp);
        first = false;
        std::string secUtf8;
        for (wchar_t wc : sc.first) secUtf8.push_back(static_cast<char>(wc));
        std::string secHead = "  \"" + EscapeJsonStr(secUtf8) + "\": {\n";
        fputs(secHead.c_str(), outFp);
        bool secFirst = true;
        for (const auto& kv : sc.second) {
            if (!secFirst) fputs(",\n", outFp);
            secFirst = false;
            std::string subLine = "    \"" + EscapeJsonStr(kv.first) + "\": \"" + EscapeJsonStr(kv.second) + "\"";
            fputs(subLine.c_str(), outFp);
        }
        fputs("\n  }", outFp);
    }
    fputs("\n}\n", outFp);
    fclose(outFp);
    return true;
}

