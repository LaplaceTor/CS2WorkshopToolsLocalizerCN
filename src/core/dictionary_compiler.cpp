#include "core/dictionary_compiler.h"
// 引入 UTF-8/UTF-16 转换工具（内部已带 NOMINMAX 与 windows.h，供 MoveFileExW 等使用）
#include "core/encoding_util.h"
#include <fstream>
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

                        // 高代理：尝试与紧随的 \uXXXX 低代理组合为完整码点（UTF-16 代理对）
                        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                            if (m_idx + 6 <= m_len && m_str[m_idx] == '\\' && m_str[m_idx + 1] == 'u') {
                                size_t lowIdx = m_idx + 2;
                                unsigned int low = 0;
                                bool lowOk = true;
                                for (int k = 0; k < 4; ++k) {
                                    char h = m_str[lowIdx + k];
                                    low <<= 4;
                                    if (h >= '0' && h <= '9') low |= (h - '0');
                                    else if (h >= 'a' && h <= 'f') low |= (h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') low |= (h - 'A' + 10);
                                    else {
                                        lowOk = false;
                                        break;
                                    }
                                }
                                if (lowOk && low >= 0xDC00 && low <= 0xDFFF) {
                                    m_idx = lowIdx + 4;
                                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                                }
                            }
                        }

                        // Encode codePoint to UTF-8；非法孤立代理替换为 U+FFFD，避免产出非法 UTF-8
                        if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
                            outStr.push_back(static_cast<char>(0xEF));
                            outStr.push_back(static_cast<char>(0xBF));
                            outStr.push_back(static_cast<char>(0xBD));
                        } else if (codePoint <= 0x7F) {
                            outStr.push_back(static_cast<char>(codePoint));
                        } else if (codePoint <= 0x7FF) {
                            outStr.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
                            outStr.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                        } else if (codePoint <= 0xFFFF) {
                            outStr.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
                            outStr.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                            outStr.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                        } else {
                            outStr.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
                            outStr.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
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

public:
    bool ParseFgdOverride(FgdOverrideData& outOverride, std::wstring& outError) {
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
            if (!ParseString(key, outError)) return false;

            SkipWhitespace();
            if (m_idx >= m_len || m_str[m_idx] != ':') {
                outError = L"JSON 缺少冒号 ':' 分隔符";
                return false;
            }
            m_idx++;
            SkipWhitespace();

            if (key == "properties" && m_idx < m_len && m_str[m_idx] == '{') {
                m_idx++; // skip '{'
                while (true) {
                    SkipWhitespace();
                    if (m_idx >= m_len) { outError = L"properties 缺少 '}'"; return false; }
                    if (m_str[m_idx] == '}') { m_idx++; break; }

                    std::string pKey;
                    if (!ParseString(pKey, outError)) return false;
                    SkipWhitespace();
                    if (m_idx >= m_len || m_str[m_idx] != ':') { outError = L"缺少冒号"; return false; }
                    m_idx++;
                    SkipWhitespace();

                    FgdPropertyOverride prop;
                    if (m_idx < m_len && m_str[m_idx] == '"') {
                        std::string desc;
                        if (!ParseString(desc, outError)) return false;
                        prop.description = desc;
                    } else if (m_idx < m_len && m_str[m_idx] == '{') {
                        m_idx++; // skip '{'
                        while (true) {
                            SkipWhitespace();
                            if (m_idx >= m_len) { outError = L"缺少 '}'"; return false; }
                            if (m_str[m_idx] == '}') { m_idx++; break; }
                            std::string subField;
                            if (!ParseString(subField, outError)) return false;
                            SkipWhitespace();
                            if (m_idx >= m_len || m_str[m_idx] != ':') { outError = L"缺少冒号"; return false; }
                            m_idx++;
                            SkipWhitespace();
                            std::string subVal;
                            if (!ParseString(subVal, outError)) return false;
                            if (subField == "description") prop.description = subVal;
                            else if (subField == "displayName") prop.displayName = subVal;
                            SkipWhitespace();
                            if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                        }
                    } else {
                        outError = L"不支持的属性覆盖值类型";
                        return false;
                    }

                    if (!prop.description.empty() || !prop.displayName.empty()) {
                        outOverride.globalProperties[pKey] = std::move(prop);
                    }
                    SkipWhitespace();
                    if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                }
            } else if (key == "io" && m_idx < m_len && m_str[m_idx] == '{') {
                m_idx++; // skip '{'
                while (true) {
                    SkipWhitespace();
                    if (m_idx >= m_len) { outError = L"io 缺少 '}'"; return false; }
                    if (m_str[m_idx] == '}') { m_idx++; break; }

                    std::string ioKey;
                    if (!ParseString(ioKey, outError)) return false;
                    SkipWhitespace();
                    if (m_idx >= m_len || m_str[m_idx] != ':') { outError = L"缺少冒号"; return false; }
                    m_idx++;
                    SkipWhitespace();

                    std::string ioDesc;
                    if (m_idx < m_len && m_str[m_idx] == '"') {
                        if (!ParseString(ioDesc, outError)) return false;
                    } else if (m_idx < m_len && m_str[m_idx] == '{') {
                        m_idx++;
                        while (true) {
                            SkipWhitespace();
                            if (m_idx >= m_len) { outError = L"缺少 '}'"; return false; }
                            if (m_str[m_idx] == '}') { m_idx++; break; }
                            std::string subF;
                            if (!ParseString(subF, outError)) return false;
                            SkipWhitespace();
                            if (m_idx >= m_len || m_str[m_idx] != ':') return false;
                            m_idx++;
                            SkipWhitespace();
                            std::string subV;
                            if (!ParseString(subV, outError)) return false;
                            if (subF == "description") ioDesc = subV;
                            SkipWhitespace();
                            if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                        }
                    }
                    if (!ioDesc.empty()) {
                        outOverride.ioOverrides[ioKey] = ioDesc;
                    }
                    SkipWhitespace();
                    if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                }
            } else if (key == "classes" && m_idx < m_len && m_str[m_idx] == '{') {
                m_idx++; // skip '{'
                while (true) {
                    SkipWhitespace();
                    if (m_idx >= m_len) { outError = L"classes 缺少 '}'"; return false; }
                    if (m_str[m_idx] == '}') { m_idx++; break; }

                    std::string clsName;
                    if (!ParseString(clsName, outError)) return false;
                    SkipWhitespace();
                    if (m_idx >= m_len || m_str[m_idx] != ':') { outError = L"缺少冒号"; return false; }
                    m_idx++;
                    SkipWhitespace();

                    if (m_idx < m_len && m_str[m_idx] == '"') {
                        std::string cDesc;
                        if (!ParseString(cDesc, outError)) return false;
                        if (!cDesc.empty()) outOverride.classDescriptions[clsName] = cDesc;
                    } else if (m_idx < m_len && m_str[m_idx] == '{') {
                        m_idx++; // skip '{'
                        while (true) {
                            SkipWhitespace();
                            if (m_idx >= m_len) { outError = L"缺少 '}'"; return false; }
                            if (m_str[m_idx] == '}') { m_idx++; break; }

                            std::string fName;
                            if (!ParseString(fName, outError)) return false;
                            SkipWhitespace();
                            if (m_idx >= m_len || m_str[m_idx] != ':') return false;
                            m_idx++;
                            SkipWhitespace();

                            if (fName == "description" && m_idx < m_len && m_str[m_idx] == '"') {
                                std::string cDesc;
                                if (!ParseString(cDesc, outError)) return false;
                                if (!cDesc.empty()) outOverride.classDescriptions[clsName] = cDesc;
                            } else if (fName == "properties" && m_idx < m_len && m_str[m_idx] == '{') {
                                m_idx++; // skip '{'
                                while (true) {
                                    SkipWhitespace();
                                    if (m_idx >= m_len) return false;
                                    if (m_str[m_idx] == '}') { m_idx++; break; }

                                    std::string cpKey;
                                    if (!ParseString(cpKey, outError)) return false;
                                    SkipWhitespace();
                                    if (m_idx >= m_len || m_str[m_idx] != ':') return false;
                                    m_idx++;
                                    SkipWhitespace();

                                    FgdPropertyOverride cpProp;
                                    if (m_idx < m_len && m_str[m_idx] == '"') {
                                        std::string cpDesc;
                                        if (!ParseString(cpDesc, outError)) return false;
                                        cpProp.description = cpDesc;
                                    } else if (m_idx < m_len && m_str[m_idx] == '{') {
                                        m_idx++;
                                        while (true) {
                                            SkipWhitespace();
                                            if (m_idx >= m_len) return false;
                                            if (m_str[m_idx] == '}') { m_idx++; break; }
                                            std::string sf;
                                            if (!ParseString(sf, outError)) return false;
                                            SkipWhitespace();
                                            if (m_idx >= m_len || m_str[m_idx] != ':') return false;
                                            m_idx++;
                                            SkipWhitespace();
                                            std::string sv;
                                            if (!ParseString(sv, outError)) return false;
                                            if (sf == "description") cpProp.description = sv;
                                            else if (sf == "displayName") cpProp.displayName = sv;
                                            SkipWhitespace();
                                            if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                                        }
                                    }
                                    if (!cpProp.description.empty() || !cpProp.displayName.empty()) {
                                        outOverride.classProperties[clsName][cpKey] = std::move(cpProp);
                                    }
                                    SkipWhitespace();
                                    if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                                }
                            }
                            SkipWhitespace();
                            if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                        }
                    }
                    SkipWhitespace();
                    if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
                }
            } else if (!key.empty() && key[0] != '_') {
                // 顶层简写
                if (m_idx < m_len && m_str[m_idx] == '"') {
                    std::string desc;
                    if (!ParseString(desc, outError)) return false;
                    FgdPropertyOverride prop;
                    prop.description = desc;
                    outOverride.globalProperties[key] = std::move(prop);
                }
            }

            SkipWhitespace();
            if (m_idx < m_len && m_str[m_idx] == ',') m_idx++;
        }
        return true;
    }

    static std::wstring NormalizeSectionName(const std::string& name) {
        std::wstring wname = enc::Utf8ToWide(name);
        if (wname.empty()) {
            return {};
        }
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

static bool ReadFileToString(const std::wstring& path, std::string& outContent, std::wstring& outError) {
    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp) {
        outError = L"无法打开源文件: " + path;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > static_cast<long>(DictionaryCompiler::MAX_JSON_FILE_SIZE)) {
        fclose(fp);
        outError = L"文件为空或大小超出 16MB 安全上限: " + path;
        return false;
    }
    outContent.resize(static_cast<size_t>(fsize));
    size_t fRead = fread(outContent.data(), 1, static_cast<size_t>(fsize), fp);
    fclose(fp);
    if (fRead != static_cast<size_t>(fsize)) {
        outError = L"读取文件失败 (读取不完整): " + path;
        return false;
    }
    return true;
}

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

    std::string cleanJson = StripJsonComments(jsonContent.data(), jsonContent.size());
    SimpleJsonParser parser(cleanJson);
    if (!parser.Parse(outCommon, outScoped, outError)) {
        return false;
    }

    if (outScoped.size() > MAX_TOTAL_SECTIONS) {
        outError = L"作用域数量超出上限 (" + std::to_wstring(MAX_TOTAL_SECTIONS) + L")";
        return false;
    }

    size_t totalEntries = outCommon.size();
    for (const auto& scoped : outScoped) {
        totalEntries += scoped.second.size();
    }
    if (totalEntries > MAX_TOTAL_ENTRIES) {
        outError = L"词条总数超出上限 (" + std::to_wstring(MAX_TOTAL_ENTRIES) + L")";
        return false;
    }

    return (!outCommon.empty() || !outScoped.empty());
}

bool DictionaryCompiler::ParseFgdOverrideFile(
    const std::wstring& jsonPath,
    FgdOverrideData& outOverride,
    std::wstring& outError
) {
    outOverride = FgdOverrideData();
    std::string rawData;
    if (!ReadFileToString(jsonPath, rawData, outError)) {
        return false;
    }
    std::string clean = StripJsonComments(rawData.data(), rawData.size());
    SimpleJsonParser parser(clean);
    return parser.ParseFgdOverride(outOverride, outError);
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
        std::string fbStr;
        std::wstring fbErr;
        if (ReadFileToString(fallbackJsonPath, fbStr, fbErr)) {
            std::string cleanFb = StripJsonComments(fbStr.data(), fbStr.size());
            SimpleJsonParser fbParser(cleanFb);
            fbParser.Parse(outCommon, outScoped, fbErr);
        }
    }

    // 2. 加载主字典覆盖/补充
    std::string jsonStr;
    if (!ReadFileToString(jsonPath, jsonStr, outError)) {
        return (!outCommon.empty() || !outScoped.empty());
    }

    std::string cleanPrimary = StripJsonComments(jsonStr.data(), jsonStr.size());
    SimpleJsonParser primaryParser(cleanPrimary);
    if (!primaryParser.Parse(outCommon, outScoped, outError)) {
        if (!outCommon.empty() || !outScoped.empty()) {
            return true;
        }
        return false;
    }

    if (outScoped.size() > MAX_TOTAL_SECTIONS) {
        outError = L"作用域数量超出上限 (" + std::to_wstring(MAX_TOTAL_SECTIONS) + L")";
        return false;
    }
    size_t totalEntries = outCommon.size();
    for (const auto& scoped : outScoped) {
        totalEntries += scoped.second.size();
    }
    if (totalEntries > MAX_TOTAL_ENTRIES) {
        outError = L"词条总数超出上限 (" + std::to_wstring(MAX_TOTAL_ENTRIES) + L")";
        return false;
    }

    return (!outCommon.empty() || !outScoped.empty());
}

