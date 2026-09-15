#include "core/fgd_core.h"
#include <regex>
#include <sstream>
#include <algorithm>

namespace {

using FgdDict = FgdCore::FgdDict;

static inline std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// 准确提取 FGD 属性头部（支持任意深度的 KV3 嵌套 {} 与 [] 属性）
static bool ExtractPropertyHeader(
    const std::string& code,
    std::string& outPropHead,
    std::string& outPropKey,
    std::string& outPropType,
    std::string& outRest
) {
    size_t i = 0;
    size_t n = code.length();
    while (i < n && (code[i] == ' ' || code[i] == '\t')) i++;
    if (i >= n) return false;

    // 排除 @Class 或注释
    if (code[i] == '@' || code[i] == '/' || code[i] == ':') return false;

    size_t keyStart = i;
    while (i < n && (isalnum((unsigned char)code[i]) || code[i] == '_' || code[i] == '.' || code[i] == '-')) {
        i++;
    }
    if (i == keyStart) return false;
    std::string propKey = code.substr(keyStart, i - keyStart);

    // 跳过空格
    while (i < n && (code[i] == ' ' || code[i] == '\t')) i++;
    if (i >= n || code[i] != '(') return false;
    i++; // 跳过 '('

    size_t typeStart = i;
    while (i < n && code[i] != ')') {
        i++;
    }
    if (i >= n || code[i] != ')') return false;
    std::string propType = code.substr(typeStart, i - typeStart);
    i++; // 跳过 ')'

    // 解析后续属性 [ ... ] 与 { ... }，支持嵌套与内部双引号
    while (i < n) {
        while (i < n && (code[i] == ' ' || code[i] == '\t')) i++;
        if (i >= n) break;

        if (code[i] == '[') {
            size_t depth = 1;
            i++;
            while (i < n && depth > 0) {
                if (code[i] == '"') {
                    i++;
                    while (i < n && code[i] != '"') {
                        if (code[i] == '\\' && i + 1 < n) i += 2;
                        else i++;
                    }
                    if (i < n) i++;
                } else if (code[i] == '[') {
                    depth++;
                    i++;
                } else if (code[i] == ']') {
                    depth--;
                    i++;
                } else {
                    i++;
                }
            }
            if (depth != 0) return false;
        } else if (code[i] == '{') {
            size_t depth = 1;
            i++;
            while (i < n && depth > 0) {
                if (code[i] == '"') {
                    i++;
                    while (i < n && code[i] != '"') {
                        if (code[i] == '\\' && i + 1 < n) i += 2;
                        else i++;
                    }
                    if (i < n) i++;
                } else if (code[i] == '{') {
                    depth++;
                    i++;
                } else if (code[i] == '}') {
                    depth--;
                    i++;
                } else {
                    i++;
                }
            }
            if (depth != 0) return false;
        } else {
            break;
        }
    }

    outPropHead = code.substr(0, i);
    outPropKey = propKey;
    outPropType = propType;
    outRest = code.substr(i);
    return true;
}

// 查词典；未命中或译为空串时原样返回
static std::string GetTranslation(const FgdDict& dict, const std::string& text) {
    const std::string trimmed = TrimString(text);
    auto it = dict.find(trimmed);
    if (it != dict.end() && !it->second.empty()) {
        return it->second;
    }
    return text;
}

// 翻译形如 ` "显示名"` 的片段，保留原始缩进与空格
static std::string TranslateQuotedPart(const std::string& part, const FgdDict& dict) {
    const std::string stripped = TrimString(part);
    if (stripped.length() < 2 || stripped.front() != '"' || stripped.back() != '"') {
        return part;
    }
    const std::string value = stripped.substr(1, stripped.length() - 2);
    const std::string translated = GetTranslation(dict, value);

    std::string out = part;
    const size_t pos = out.find("\"" + value + "\"");
    if (pos != std::string::npos) {
        out.replace(pos, value.length() + 2, "\"" + translated + "\"");
    }
    return out;
}

// 按冒号切分属性体，忽略引号内的冒号
static bool SplitPropertyParts(const std::string& body, std::vector<std::string>& outParts) {
    outParts.clear();

    std::string curr;
    bool inQuote = false;
    bool hasFirstColon = false;

    for (size_t i = 0; i < body.length(); ++i) {
        const char c = body[i];
        if (c == '"') {
            inQuote = !inQuote;
            curr.push_back(c);
        } else if (c == ':' && !inQuote) {
            if (!hasFirstColon) {
                hasFirstColon = true;
                curr.clear();
            } else {
                outParts.push_back(curr);
                curr.clear();
            }
        } else {
            curr.push_back(c);
        }
    }
    if (hasFirstColon) {
        outParts.push_back(curr);
    }
    return hasFirstColon;
}

static std::string JoinPropertyParts(const std::vector<std::string>& parts, const std::string& colon) {
    std::string out = colon;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ":";
        out += parts[i];
    }
    return out;
}

// 0. 等待上一行的跨行类说明（@PointClass ... = classname :\n  "Desc"）
static std::optional<std::string> TryPendingClassDesc(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    std::string& inOutPendingClassDesc)
{
    if (inOutPendingClassDesc.empty()) {
        return std::nullopt;
    }

    const std::string trimmed = TrimString(code);
    if (trimmed.empty()) {
        return code;
    }

    static const std::regex standaloneStrRegex(R"re(^\s*"([^"]*)"\s*$)re");
    std::smatch strMatch;
    if (std::regex_match(code, strMatch, standaloneStrRegex)) {
        const std::string origDesc = strMatch[1].str();

        std::string finalDesc;
        auto itDesc = overrideData.classDescriptions.find(inOutPendingClassDesc);
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            finalDesc = itDesc->second;
        } else {
            finalDesc = GetTranslation(dict, origDesc);
        }
        inOutPendingClassDesc.clear();

        const size_t lead = code.find_first_not_of(" \t");
        const std::string indent = (lead != std::string::npos) ? code.substr(0, lead) : "\t";
        return indent + "\"" + finalDesc + "\"";
    }

    if (trimmed.front() == '[') {
        auto itDesc = overrideData.classDescriptions.find(inOutPendingClassDesc);
        inOutPendingClassDesc.clear();
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            const size_t lead = code.find_first_not_of(" \t");
            const std::string indent = (lead != std::string::npos) ? code.substr(0, lead) : "";
            return indent + "\t\"" + itDesc->second + "\"\n" + code;
        }
        return std::nullopt;
    }

    inOutPendingClassDesc.clear();
    return std::nullopt;
}

// 1. 实体类定义与说明跟踪
static std::optional<std::string> TryClassDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass, std::string& inOutPendingClassDesc)
{
    // 1.1 同行完整定义：= classname : "Description"
    static const std::regex classFullRegex(R"re((=\s*)([a-zA-Z0-9_]+)\s*:\s*"([^"]*)")re");
    std::smatch classFullMatch;
    if (std::regex_search(code, classFullMatch, classFullRegex)) {
        const std::string prefix = code.substr(0, classFullMatch.position(0));
        const std::string eq = classFullMatch[1].str();
        const std::string className = classFullMatch[2].str();
        const std::string origDesc = classFullMatch[3].str();
        const std::string suffix = code.substr(classFullMatch.position(0) + classFullMatch.length(0));

        inOutCurrentClass = className;
        inOutPendingClassDesc.clear();

        std::string finalDesc;
        auto itDesc = overrideData.classDescriptions.find(className);
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            finalDesc = itDesc->second;
        } else {
            finalDesc = GetTranslation(dict, origDesc);
        }
        return prefix + eq + className + " : \"" + finalDesc + "\"" + suffix;
    }

    // 1.2 跨行或末尾冒号/无描述类定义：= classname : 或 = classname (行尾)
    static const std::regex classHeaderRegex(R"re((=\s*)([a-zA-Z0-9_]+)(\s*:\s*|\s*)$)re");
    std::smatch classHeaderMatch;
    if (std::regex_search(code, classHeaderMatch, classHeaderRegex)) {
        const std::string className = classHeaderMatch[2].str();
        inOutCurrentClass = className;
        inOutPendingClassDesc = className;
        return code;
    }

    // 1.3 同行紧接中括号的无描述类定义：= classname [
    static const std::regex classBracketRegex(R"re((=\s*)([a-zA-Z0-9_]+)\s*(\[.*)$)re");
    std::smatch classBracketMatch;
    if (std::regex_search(code, classBracketMatch, classBracketRegex)) {
        const std::string prefix = code.substr(0, classBracketMatch.position(0));
        const std::string eq = classBracketMatch[1].str();
        const std::string className = classBracketMatch[2].str();
        const std::string bracketTail = classBracketMatch[3].str();

        inOutCurrentClass = className;
        inOutPendingClassDesc.clear();

        auto itDesc = overrideData.classDescriptions.find(className);
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            return prefix + eq + className + " : \"" + itDesc->second + "\" " + bracketTail;
        }
        return code;
    }

    return std::nullopt;
}

// 2. 输入 / 输出描述：input/output Name(type) [ : "Description" ]
static std::optional<std::string> TryIoDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    const std::string& currentClass)
{
    static const std::regex ioRegex(R"re(^(\s*(?:input|output)\s+)([a-zA-Z0-9_]+)(\s*\([^)]*\))(.*)$)re");
    std::smatch ioMatch;
    if (!std::regex_match(code, ioMatch, ioRegex)) {
        return std::nullopt;
    }

    const std::string ioPrefix = ioMatch[1].str();
    const std::string ioName = ioMatch[2].str();
    const std::string ioParam = ioMatch[3].str();
    const std::string ioRest = ioMatch[4].str();

    if (ioParam.find("api") != std::string::npos) {
        return code;
    }

    std::string overrideIoDesc;
    if (!currentClass.empty()) {
        auto itCls = overrideData.classProperties.find(currentClass);
        if (itCls != overrideData.classProperties.end()) {
            auto itP = itCls->second.find(ioName);
            if (itP != itCls->second.end() && !itP->second.description.empty()) {
                overrideIoDesc = itP->second.description;
            }
        }
    }
    if (overrideIoDesc.empty()) {
        auto itIo = overrideData.ioOverrides.find(ioName);
        if (itIo != overrideData.ioOverrides.end() && !itIo->second.empty()) {
            overrideIoDesc = itIo->second;
        }
    }
    if (overrideIoDesc.empty()) {
        auto itGlob = overrideData.globalProperties.find(ioName);
        if (itGlob != overrideData.globalProperties.end() && !itGlob->second.description.empty()) {
            overrideIoDesc = itGlob->second.description;
        }
    }

    static const std::regex descQuoteRegex(R"re(:\s*"([^"]*)")re");
    std::smatch descQuoteMatch;
    if (std::regex_search(ioRest, descQuoteMatch, descQuoteRegex)) {
        const std::string origDesc = descQuoteMatch[1].str();
        const std::string finalDesc = overrideIoDesc.empty() ? GetTranslation(dict, origDesc) : overrideIoDesc;
        const std::string replacedRest =
            ioRest.substr(0, descQuoteMatch.position(0)) + ": \"" + finalDesc + "\""
            + ioRest.substr(descQuoteMatch.position(0) + descQuoteMatch.length(0));
        return ioPrefix + ioName + ioParam + replacedRest;
    }

    if (!overrideIoDesc.empty()) {
        return ioPrefix + ioName + ioParam + " : \"" + overrideIoDesc + "\"";
    }
    return code;
}

// 3. 按钮/元数据说明：desc = "Description"
static std::optional<std::string> TryDescMetadata(const std::string& code, const FgdDict& dict) {
    static const std::regex descRegex(R"re(\bdesc\s*=\s*"([^"]*)")re");
    std::smatch descMatch;
    if (!std::regex_search(code, descMatch, descRegex)) {
        return std::nullopt;
    }

    const std::string prefix = code.substr(0, descMatch.position(0));
    const std::string desc = descMatch[1].str();
    const std::string suffix = code.substr(descMatch.position(0) + descMatch.length(0));
    return prefix + "desc = \"" + GetTranslation(dict, desc) + "\"" + suffix;
}

// 3.1 属性跨行定义块内部的独立组声明：group = "GroupName"
static std::optional<std::string> TryStandaloneGroup(const std::string& code, const FgdDict& dict) {
    static const std::regex standaloneGroupRegex(R"re(^\s*group(\s*=\s*)"([^"]*)")re");
    std::smatch standMatch;
    if (!std::regex_search(code, standMatch, standaloneGroupRegex)) {
        return std::nullopt;
    }

    const std::string prefix = code.substr(0, standMatch.position(0));
    const std::string eq = standMatch[1].str();
    const std::string gName = standMatch[2].str();
    const std::string suffix = code.substr(standMatch.position(0) + standMatch.length(0));
    return prefix + "group" + eq + "\"" + GetTranslation(dict, gName) + "\"" + suffix;
}

// 4. 属性定义：prop(type) [attrs] {attrs} : "Display Name" [ : default [ : "Description" ]] [ = [ choices ] ]
static std::optional<std::string> TryPropertyDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    const std::string& currentClass)
{
    std::string propHead, propKey, propType, rest;
    if (!ExtractPropertyHeader(code, propHead, propKey, propType, rest)) {
        return std::nullopt;
    }

    // 翻译属性行内部的属性组
    static const std::regex groupRegex(R"re(\bgroup(\s*=\s*)"([^"]*)")re");
    std::smatch groupMatch;
    std::string newPropHead;
    std::string searchHead = propHead;
    while (std::regex_search(searchHead, groupMatch, groupRegex)) {
        const std::string prefix = searchHead.substr(0, groupMatch.position(0));
        const std::string eq = groupMatch[1].str();
        const std::string groupName = groupMatch[2].str();
        newPropHead += prefix + "group" + eq + "\"" + GetTranslation(dict, groupName) + "\"";
        searchHead = searchHead.substr(groupMatch.position(0) + groupMatch.length(0));
    }
    if (!newPropHead.empty()) {
        newPropHead += searchHead;
        propHead = newPropHead;
    }

    const FgdPropertyOverride* propOverride = nullptr;
    if (!currentClass.empty()) {
        auto itCls = overrideData.classProperties.find(currentClass);
        if (itCls != overrideData.classProperties.end()) {
            auto itP = itCls->second.find(propKey);
            if (itP != itCls->second.end()) {
                propOverride = &itP->second;
            }
        }
    }
    if (!propOverride) {
        auto itGlob = overrideData.globalProperties.find(propKey);
        if (itGlob != overrideData.globalProperties.end()) {
            propOverride = &itGlob->second;
        }
    }

    std::string choicesTail;
    std::string propBody = rest;
    {
        bool inQuote = false;
        for (size_t i = 0; i < rest.length(); ++i) {
            const char c = rest[i];
            if (c == '"') {
                inQuote = !inQuote;
            } else if (c == '=' && !inQuote) {
                propBody = rest.substr(0, i);
                choicesTail = rest.substr(i);
                break;
            }
        }
    }

    if (TrimString(propBody).empty()) {
        return propHead + rest;
    }

    std::vector<std::string> parts;
    const bool hasFirstColon = SplitPropertyParts(propBody, parts);
    if (!hasFirstColon) {
        return code;
    }

    if (!parts.empty()) {
        if (propOverride && !propOverride->displayName.empty()) {
            parts[0] = " \"" + propOverride->displayName + "\"";
        } else {
            parts[0] = TranslateQuotedPart(parts[0], dict);
        }

        if (parts.size() == 1) {
            if (propOverride && !propOverride->description.empty()) {
                parts.push_back(" \"\"");
                parts.push_back(" \"" + propOverride->description + "\"");
            }
        } else if (parts.size() == 2) {
            if (propOverride && !propOverride->description.empty()) {
                parts.push_back(" \"" + propOverride->description + "\"");
            }
        } else if (propOverride && !propOverride->description.empty()) {
            parts[2] = " \"" + propOverride->description + "\"";
        } else {
            parts[2] = TranslateQuotedPart(parts[2], dict);
        }
    }

    return propHead + JoinPropertyParts(parts, " :") + choicesTail;
}

// 4.1 属性跨行定义的续行
static std::optional<std::string> TryPropertyContinuation(const std::string& code, const FgdDict& dict) {
    const std::string trimmedCode = TrimString(code);
    if (trimmedCode.empty() || trimmedCode.front() != ':') {
        return std::nullopt;
    }

    std::vector<std::string> parts;
    const bool hasFirstColon = SplitPropertyParts(code, parts);
    if (!hasFirstColon) {
        return std::nullopt;
    }

    if (parts.empty()) {
        return std::nullopt;
    }

    parts[0] = TranslateQuotedPart(parts[0], dict);
    if (parts.size() >= 3) {
        parts[2] = TranslateQuotedPart(parts[2], dict);
    }

    const size_t leadPos = code.find_first_not_of(" \t");
    const std::string leadingSpaces = (leadPos != std::string::npos) ? code.substr(0, leadPos) : "";
    return leadingSpaces + JoinPropertyParts(parts, ":");
}

// 5. 选项列表
static std::optional<std::string> TryChoiceEntry(const std::string& code, const FgdDict& dict) {
    const std::string trimmedCode = TrimString(code);
    if (trimmedCode.empty() || trimmedCode.front() == '@' || code.find(':') == std::string::npos) {
        return std::nullopt;
    }

    static const std::regex choiceRegex(R"re(^(\s*(?:"[^"]*"|[-0-9a-zA-Z_]+)\s*:\s*)"([^"]*)"(.*)$)re");
    std::smatch choiceMatch;
    if (!std::regex_match(code, choiceMatch, choiceRegex)) {
        return std::nullopt;
    }

    const std::string cPrefix = choiceMatch[1].str();
    const std::string display = choiceMatch[2].str();
    std::string tail = choiceMatch[3].str();

    if (tail.find(':') != std::string::npos) {
        static const std::regex tailDescRegex(R"re((:[^"]*)"([^"]*)")re");
        std::smatch tailMatch;
        if (std::regex_search(tail, tailMatch, tailDescRegex)) {
            const std::string tPre = tailMatch[1].str();
            const std::string tDesc = tailMatch[2].str();
            const std::string trD = GetTranslation(dict, tDesc);
            tail = tail.substr(0, tailMatch.position(0)) + tPre + "\"" + trD + "\""
                 + tail.substr(tailMatch.position(0) + tailMatch.length(0));
        }
    }

    return cPrefix + "\"" + GetTranslation(dict, display) + "\"" + tail;
}

} // namespace

std::string FgdCore::TranslateLine(const std::string& line, const FgdDict& dict) {
    std::string dummyClass;
    std::string dummyPending;
    FgdOverrideData dummyOverride;
    return TranslateLine(line, dict, dummyOverride, dummyClass, dummyPending);
}

std::string FgdCore::TranslateLine(
    const std::string& line,
    const FgdDict& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass
) {
    std::string dummyPending;
    return TranslateLine(line, dict, overrideData, inOutCurrentClass, dummyPending);
}

std::string FgdCore::TranslateLine(
    const std::string& line,
    const FgdDict& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass,
    std::string& inOutPendingClassDesc
) {
    std::string lineEnding;
    std::string raw = line;
    if (raw.length() >= 2 && raw.substr(raw.length() - 2) == "\r\n") {
        lineEnding = "\r\n";
        raw = raw.substr(0, raw.length() - 2);
    } else if (!raw.empty() && raw.back() == '\n') {
        lineEnding = "\n";
        raw = raw.substr(0, raw.length() - 1);
    }

    std::string comment;
    std::string code = raw;
    bool inQuote = false;
    for (size_t i = 0; i < raw.length(); ++i) {
        const char ch = raw[i];
        if (ch == '"') {
            inQuote = !inQuote;
        } else if (ch == '/' && !inQuote && i + 1 < raw.length() && raw[i + 1] == '/') {
            comment = raw.substr(i);
            code = raw.substr(0, i);
            break;
        }
    }

    const std::optional<std::string> translated = [&]() -> std::optional<std::string> {
        if (auto r = TryPendingClassDesc(code, dict, overrideData, inOutPendingClassDesc)) return r;
        if (auto r = TryClassDefinition(code, dict, overrideData, inOutCurrentClass, inOutPendingClassDesc)) return r;
        if (auto r = TryIoDefinition(code, dict, overrideData, inOutCurrentClass)) return r;
        if (auto r = TryDescMetadata(code, dict)) return r;
        if (auto r = TryStandaloneGroup(code, dict)) return r;
        if (auto r = TryPropertyDefinition(code, dict, overrideData, inOutCurrentClass)) return r;
        if (auto r = TryPropertyContinuation(code, dict)) return r;
        if (auto r = TryChoiceEntry(code, dict)) return r;
        return std::nullopt;
    }();

    return translated.value_or(code) + comment + lineEnding;
}

bool FgdCore::TranslateContent(
    const std::string& inputContent,
    std::string& outTranslated,
    const FgdDict& dict,
    const FgdOverrideData& overrideData
) {
    outTranslated.clear();
    outTranslated.reserve(inputContent.size() + inputContent.size() / 4);

    std::string currentClassName = "";
    std::string pendingClassDesc = "";

    size_t start = 0;
    size_t n = inputContent.size();
    while (start < n) {
        size_t end = inputContent.find('\n', start);
        std::string line;
        if (end == std::string::npos) {
            line = inputContent.substr(start);
            start = n;
        } else {
            line = inputContent.substr(start, end - start + 1);
            start = end + 1;
        }

        std::string transLine = TranslateLine(line, dict, overrideData, currentClassName, pendingClassDesc);
        outTranslated.append(transLine);
    }

    return true;
}
