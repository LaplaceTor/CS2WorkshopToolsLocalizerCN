#include "dictionary_compiler.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

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
    return out;
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

bool DictionaryCompiler::CompileJsonStringToBinary(const std::string& jsonContent, std::vector<uint8_t>& outBinary, std::wstring& outError) {
    if (jsonContent.empty()) {
        outError = L"JSON 文本为空";
        return false;
    }

    if (jsonContent.size() > MAX_JSON_FILE_SIZE) {
        outError = L"JSON 文本大小超出 16MB 安全上限";
        return false;
    }

    // 1. 去除注释
    std::string cleanJson = StripJsonComments(jsonContent.data(), jsonContent.size());

    // 2. 解析为分块字典结构
    std::unordered_map<std::string, std::string> commonDict;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> scopedDicts;

    SimpleJsonParser parser(cleanJson);
    if (!parser.Parse(commonDict, scopedDicts, outError)) {
        return false;
    }

    // 3. 构建 LCLD 结构：Sections, Entries, String Table
    struct SectionWorkItem {
        std::string sectionNameUtf8;
        std::vector<std::pair<std::string, std::string>> entries;
    };

    std::vector<SectionWorkItem> workItems;
    if (!commonDict.empty()) {
        SectionWorkItem item;
        item.sectionNameUtf8 = "common";
        for (const auto& kv : commonDict) {
            item.entries.push_back(kv);
        }
        workItems.push_back(std::move(item));
    }

    for (const auto& sc : scopedDicts) {
        if (!sc.second.empty()) {
            SectionWorkItem item;
            std::string utf8Name;
            for (wchar_t wc : sc.first) utf8Name.push_back(static_cast<char>(wc));
            item.sectionNameUtf8 = utf8Name;
            for (const auto& kv : sc.second) {
                item.entries.push_back(kv);
            }
            workItems.push_back(std::move(item));
        }
    }

    if (workItems.empty()) {
        outError = L"未解析到有效的字典条目";
        return false;
    }

    // 4. 构建字符串表并记录偏移
    std::unordered_map<std::string, uint32_t> stringMap;
    std::vector<char> stringTable;

    auto addString = [&](const std::string& str) -> uint32_t {
        auto it = stringMap.find(str);
        if (it != stringMap.end()) {
            return it->second;
        }
        uint32_t off = static_cast<uint32_t>(stringTable.size());
        stringTable.insert(stringTable.end(), str.begin(), str.end());
        stringTable.push_back('\0');
        stringMap[str] = off;
        return off;
    };

    // 检查数量上限保护
    if (workItems.size() > 10000) {
        outError = L"作用域数量超过安全上限";
        return false;
    }

    uint64_t totalSections64 = workItems.size();
    uint64_t totalEntries64 = 0;
    for (const auto& w : workItems) {
        totalEntries64 += w.entries.size();
    }
    if (totalEntries64 > 1000000) {
        outError = L"字典总条目数超过安全上限";
        return false;
    }

    uint32_t totalSections = static_cast<uint32_t>(totalSections64);
    uint32_t totalEntries = static_cast<uint32_t>(totalEntries64);

    uint64_t headerSize = sizeof(LcldHeader);
    uint64_t sectionsArraySize = static_cast<uint64_t>(totalSections) * sizeof(LcldSection);
    uint64_t entriesArraySize = static_cast<uint64_t>(totalEntries) * sizeof(LcldEntry);

    uint64_t sectionsOffset = headerSize;
    uint64_t entriesStartOffset = headerSize + sectionsArraySize;
    uint64_t currentEntryOffset = entriesStartOffset;

    std::vector<LcldSection> compiledSections;
    std::vector<LcldEntry> compiledEntries;

    for (const auto& w : workItems) {
        LcldSection sec;
        sec.nameOffset = addString(w.sectionNameUtf8);
        sec.entryCount = static_cast<uint32_t>(w.entries.size());
        sec.entriesOffset = static_cast<uint32_t>(currentEntryOffset);
        compiledSections.push_back(sec);

        for (const auto& e : w.entries) {
            LcldEntry entry;
            entry.keyOffset = addString(e.first);
            entry.valOffset = addString(e.second);
            compiledEntries.push_back(entry);
        }
        currentEntryOffset += static_cast<uint64_t>(w.entries.size()) * sizeof(LcldEntry);
    }

    uint64_t stringTableOffset = headerSize + sectionsArraySize + entriesArraySize;
    uint64_t stringTableSize = stringTable.size();
    uint64_t totalBinarySize = stringTableOffset + stringTableSize;

    if (totalBinarySize > 0xFFFFFFFFULL) {
        outError = L"编译后的字典数据超出 4GB 限制";
        return false;
    }

    LcldHeader header;
    std::memcpy(header.magic, "LCLD", 4);
    header.version = 1;
    header.totalSections = totalSections;
    header.totalEntries = totalEntries;
    header.sectionsOffset = static_cast<uint32_t>(sectionsOffset);
    header.stringTableOffset = static_cast<uint32_t>(stringTableOffset);
    header.stringTableSize = static_cast<uint32_t>(stringTableSize);

    outBinary.clear();
    outBinary.resize(static_cast<size_t>(totalBinarySize), 0);

    std::memcpy(outBinary.data(), &header, sizeof(LcldHeader));
    std::memcpy(outBinary.data() + sectionsOffset, compiledSections.data(), static_cast<size_t>(sectionsArraySize));
    std::memcpy(outBinary.data() + entriesStartOffset, compiledEntries.data(), static_cast<size_t>(entriesArraySize));
    std::memcpy(outBinary.data() + stringTableOffset, stringTable.data(), static_cast<size_t>(stringTableSize));

    return true;
}

bool DictionaryCompiler::CompileJsonFileToLcld(const std::wstring& jsonPath, const std::wstring& lcldPath, std::wstring& outError) {
    FILE* fp = _wfopen(jsonPath.c_str(), L"rb");
    if (!fp) {
        outError = L"无法打开源 JSON 文件: " + jsonPath;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > static_cast<long>(MAX_JSON_FILE_SIZE)) {
        fclose(fp);
        outError = L"源 JSON 文件为空或大小超出 16MB 安全上限";
        return false;
    }
    std::string jsonStr(static_cast<size_t>(fsize), '\0');
    fread(jsonStr.data(), 1, fsize, fp);
    fclose(fp);

    std::vector<uint8_t> binary;
    if (!CompileJsonStringToBinary(jsonStr, binary, outError)) {
        return false;
    }

    FILE* outFp = _wfopen(lcldPath.c_str(), L"wb");
    if (!outFp) {
        outError = L"无法创建目标 LCLD 二进制文件: " + lcldPath;
        return false;
    }
    fwrite(binary.data(), 1, binary.size(), outFp);
    fclose(outFp);
    return true;
}

bool DictionaryCompiler::ParseLcldBinaryToMaps(
    const uint8_t* data,
    size_t size,
    std::unordered_map<std::string, std::string>& outCommon,
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>>& outScoped) {

    if (!data || size < sizeof(LcldHeader) || size > MAX_LCLD_FILE_SIZE) return false;

    const LcldHeader* hdr = reinterpret_cast<const LcldHeader*>(data);
    if (std::memcmp(hdr->magic, "LCLD", 4) != 0 || hdr->version != 1) {
        return false;
    }

    // 检查合理的 section 和 entry 数量上限，防止畸形数据造成拒绝服务
    if (hdr->totalSections > MAX_TOTAL_SECTIONS || hdr->totalEntries > MAX_TOTAL_ENTRIES) {
        return false;
    }

    // 1. 严格 64-bit 无溢出计算 String Table 边界
    uint64_t stringTableStart = static_cast<uint64_t>(hdr->stringTableOffset);
    uint64_t stringTableSize = static_cast<uint64_t>(hdr->stringTableSize);
    uint64_t stringTableEnd = stringTableStart + stringTableSize;
    if (stringTableEnd > size || stringTableEnd < stringTableStart || stringTableSize > MAX_LCLD_FILE_SIZE) return false;

    const char* strTable = reinterpret_cast<const char*>(data + stringTableStart);

    // 2. 严格 64-bit 无溢出计算 Sections 数组边界
    uint64_t sectionsStart = static_cast<uint64_t>(hdr->sectionsOffset);
    uint64_t sectionsSize = static_cast<uint64_t>(hdr->totalSections) * sizeof(LcldSection);
    uint64_t sectionsEnd = sectionsStart + sectionsSize;
    if (sectionsEnd > size || sectionsEnd < sectionsStart) return false;

    const LcldSection* sections = reinterpret_cast<const LcldSection*>(data + sectionsStart);

    outCommon.clear();
    outScoped.clear();

    // 3. 安全提取以 '\0' 结尾的字符串，严格杜绝越界读取与超长字符串
    auto getSafeString = [&](uint32_t offset) -> const char* {
        if (offset >= stringTableSize) return nullptr;
        size_t maxLen = static_cast<size_t>(stringTableSize - offset);
        if (maxLen > MAX_SINGLE_STRING_LENGTH + 1) {
            maxLen = MAX_SINGLE_STRING_LENGTH + 1;
        }
        const char* p = strTable + offset;
        const char* nullPos = static_cast<const char*>(std::memchr(p, '\0', maxLen));
        if (!nullPos) return nullptr; // 字符串在限制长度内未以 '\0' 终结
        return p;
    };

    auto NormalizeSectionName = [](const char* name) -> std::wstring {
        std::wstring wname;
        for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
        std::transform(wname.begin(), wname.end(), wname.begin(), ::towlower);
        if (wname.length() > 4 && wname.substr(wname.length() - 4) == L".dll") {
            wname = wname.substr(0, wname.length() - 4);
        }
        return wname;
    };

    for (uint32_t s = 0; s < hdr->totalSections; ++s) {
        const LcldSection& sec = sections[s];
        const char* secNameUtf8 = getSafeString(sec.nameOffset);
        if (!secNameUtf8) continue;
        std::wstring secName = NormalizeSectionName(secNameUtf8);

        // 严格 64-bit 无溢出计算 Entries 数组边界
        uint64_t entriesStart = static_cast<uint64_t>(sec.entriesOffset);
        uint64_t entriesSize = static_cast<uint64_t>(sec.entryCount) * sizeof(LcldEntry);
        uint64_t entriesEnd = entriesStart + entriesSize;
        if (entriesEnd > size || entriesEnd < entriesStart) continue;

        const LcldEntry* entries = reinterpret_cast<const LcldEntry*>(data + entriesStart);
        auto& targetMap = (secName == L"common" || secName == L"general") ? outCommon : outScoped[secName];

        for (uint32_t e = 0; e < sec.entryCount; ++e) {
            const LcldEntry& entry = entries[e];
            const char* key = getSafeString(entry.keyOffset);
            const char* val = getSafeString(entry.valOffset);
            if (key && val && key[0] != '\0' && val[0] != '\0') {
                targetMap[key] = val;
            }
        }
    }

    return (!outCommon.empty() || !outScoped.empty());
}

