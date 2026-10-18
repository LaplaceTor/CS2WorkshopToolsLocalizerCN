#include "core/fgd_core.h"
#include <regex>
#include <sstream>
#include <algorithm>

namespace {

using FgdDict = FgdCore::FgdDict;

// FGD 多行字符串拼接状态（见 fgd_core.h 的说明）。
struct FgdConcatState {
    // 已确认处于「@类定义 说明」中，但尚未读到第一个字面量
    bool awaitClassStart = false;
    // 当前类定义说明已读过至少一段字面量
    bool classStarted = false;
    // 处于拼接链中（上一行以 '+' 结尾），期待下一行继续
    bool inChain = false;
    // 当前说明所属实体类名（用于 fgd_override 的类说明覆盖）
    std::string className;
    // 拼接链累积内容：decoded 为按 FGD 转义规则解码后的文本，raw 为逐行原始文字（兜底）
    std::string decoded;
    std::string raw;
    std::size_t litCount = 0;
};

static inline std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// FGD 字符串字面量转义解码（\\ \" \n \r \t）。
// 仅用于「拼接后整体查表」的候选键——逐行翻译时仍然只按已解码的词典键替换引号内文本，
// 因此未收录的字符串一个字节都不会被改写，不会引入新的转义风险。
static std::string DecodeFgdEscapes(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            const char e = raw[i + 1];
            switch (e) {
                case 'n': out.push_back('\n'); ++i; continue;
                case 'r': out.push_back('\r'); ++i; continue;
                case 't': out.push_back('\t'); ++i; continue;
                case '"': out.push_back('"');  ++i; continue;
                case '\\': out.push_back('\\'); ++i; continue;
                default: break;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

// 解析一行「由若干字符串字面量用 + 连接」的 FGD 代码。
// 返回 true 表示该行完全匹配此形态（已剔除注释、只含字面量与 + 操作符）。
// 成功时：
//   outLiterals  —— 各字面量的原始内容（未解码，不含两侧引号）
//   outTailPlus  —— 该行是否以 '+' 结尾（即拼接链尚未结束）
static bool ParseConcatLiterals(const std::string& code,
                                std::vector<std::string>& outLiterals,
                                bool& outTailPlus) {
    outLiterals.clear();
    outTailPlus = false;

    size_t i = 0;
    const size_t n = code.size();
    bool expectOperand = true;

    while (true) {
        while (i < n && (code[i] == ' ' || code[i] == '\t')) ++i;
        if (i >= n) break;

        if (expectOperand) {
            if (code[i] != '"') return false;
            ++i;
            const size_t start = i;
            while (i < n) {
                if (code[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (code[i] == '"') break;
                ++i;
            }
            if (i >= n) return false;  // 字面量未闭合
            outLiterals.push_back(code.substr(start, i - start));
            ++i;                       // 跳过闭引号
            expectOperand = false;
        } else {
            if (code[i] != '+') return false;
            ++i;
            expectOperand = true;
        }
    }

    if (outLiterals.empty()) return false;
    outTailPlus = expectOperand;       // 以 '+' 收尾 → 期待续行
    return true;
}

// 在一行「类定义说明」中就地翻译已收录的字面量；未收录的字面量原样保留。
// 返回改写后的整行（保留原始缩进与 '+' 结构），未作任何改动时返回原行。
static std::string TranslateDescLiterals(const std::string& code,
                                         const std::vector<std::string>& literals,
                                         const FgdDict& dict) {
    std::string out;
    out.reserve(code.size());
    size_t cursor = 0;
    bool changed = false;

    for (const std::string& lit : literals) {
        const std::string needle = "\"" + lit + "\"";
        size_t pos = code.find(needle, cursor);
        if (pos == std::string::npos) continue;

        out.append(code, cursor, pos - cursor);
        cursor = pos + needle.size();

        const std::string origText = DecodeFgdEscapes(lit);
        std::string translated;
        // 依次尝试：解码后原样 → 解码后去首尾空白 → 未解码原样 → 未解码去首尾空白。
        // 注意必须先试「原样」，因为 FGD 中用于拼接长句的字面量常带有结尾空格，
        // 而这类空格往往被写进词典键；若先去掉空白就永远匹配不到。
        const std::string candidates[] = {
            origText,
            TrimString(origText),
            lit,
            TrimString(lit),
        };
        for (const std::string& key : candidates) {
            if (key.empty()) continue;
            auto it = dict.find(key);
            if (it != dict.end() && !it->second.empty()) {
                translated = it->second;
                break;
            }
        }

        if (!translated.empty()) {
            // 词典值中的换行以 \n 转义写回字面量，保证 FGD 语法合法
            std::string escaped;
            escaped.reserve(translated.size());
            for (char c : translated) {
                if (c == '\n') escaped += "\\n";
                else if (c == '\r') escaped += "\\r";
                else escaped += c;
            }
            out += "\"" + escaped + "\"";
            changed = true;
        } else {
            out += needle;
        }
    }

    if (!changed) return code;
    out.append(code, cursor, std::string::npos);
    return out;
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

    // 注意：此处不要清空 inOutPendingClassDesc——本行可能是跨行字符串拼接说明的
    // 起始行（形如 `= cls :` 然后是 `"..." +`），真正的说明尚未到来，
    // 提前清空会让后续续行被当作孤立文本处理，导致整条说明漏翻。
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

// 3.2 实体工具分类声明：@EntityGroup "GroupName" { ... }
static std::optional<std::string> TryEntityGroupDirective(const std::string& code, const FgdDict& dict) {
    static const std::regex egRegex(R"re(^(\s*@EntityGroup\s+)"([^"]+)"(.*)$)re", std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_match(code, match, egRegex)) {
        return std::nullopt;
    }
    const std::string prefix = match[1].str();
    const std::string groupName = match[2].str();
    const std::string suffix = match[3].str();
    return prefix + "\"" + GetTranslation(dict, groupName) + "\"" + suffix;
}

// 3.3 实体工具元数据：entity_tool_name / entity_tool_group / entity_tool_tip = "..."
static std::optional<std::string> TryEntityToolMetadata(const std::string& code, const FgdDict& dict) {
    static const std::regex toolRegex(R"re(^(\s*entity_tool_(?:name|group|tip)\s*=\s*)"([^"]*)(".*)$)re");
    std::smatch match;
    if (!std::regex_match(code, match, toolRegex)) {
        return std::nullopt;
    }
    const std::string prefix = match[1].str();
    const std::string text = match[2].str();
    const std::string suffix = match[3].str();
    return prefix + "\"" + GetTranslation(dict, text) + suffix;
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

// 6. FGD 多行字符串拼接（描述中的 C 风格 "..." + "..." 跨行写法）
// ------------------------------------------------------------------------------
// Valve 原版 FGD 大量使用这种写法，例如 csgo.fgd 的 func_bomb_target：
//     @SolidClass base(Targetname, Parentname ) = func_bomb_target:
//         "Bomb Target. The terrorists can place C4 ...\n\n" +
//         "When the bomb is planted and explodes inside this area, ... handles " +
//         "all of the bomb planting and scoring logic – the BombExplode outputs ..."
// 此前每一行都被当作独立文本处理，既无法匹配「拼接后整体」的词典键，
// 也无法匹配「单段字面量」的词典键，导致此类实体说明完全漏翻。
// 现在的策略：逐行按字面量查表（支持按整段或按拼接后整体收录），
// 逐行改写只会发生在词典命中时，未收录内容保持字节级原样，因此优先使用逐行翻译。
// 在 FGD 一行文本中查找「顶层」的 '='：跳过引号内、() 内、[] 内、{} 内的内容。
// 用于把「类定义分隔符 '='」与 helper 宏参数里的 '='（如 angles_key = "movedir"）区分开。
// 返回位置；找不到返回 std::string::npos。
static std::size_t FindTopLevelEquals(const std::string& code) {
    int depthParen = 0;
    int depthBracket = 0;
    int depthBrace = 0;
    bool inQuote = false;

    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < code.size()) { ++i; continue; }
            if (c == '"') inQuote = false;
            continue;
        }
        switch (c) {
            case '"': inQuote = true; break;
            case '(': ++depthParen; break;
            case ')': if (depthParen > 0) --depthParen; break;
            case '[': ++depthBracket; break;
            case ']': if (depthBracket > 0) --depthBracket; break;
            case '{': ++depthBrace; break;
            case '}': if (depthBrace > 0) --depthBrace; break;
            case '=':
                if (depthParen == 0 && depthBracket == 0 && depthBrace == 0) {
                    return i;
                }
                break;
            default: break;
        }
    }
    return std::string::npos;
}

// 判断一行是否为「类定义指令」。
//
// 【为什么不排除含双引号的行】
//   原版 FGD 里大量 @PointClass / @NPCClass 指令带有内容含引号的 helper 宏，例如
//     @PointClass base(Node) tags( AI ) editormodel("models/editor/air_node.vmdl", fixedbounds) color(...) = info_node
//     @BaseClass base(Targetname) iconsprite("editor/npc_maker.vmat") = BaseNPCMaker
//   实测 19 个原版 FGD 共 1229 条 @ 指令行，其中 541 条（约 44%）含双引号。
//   若在这里排除带引号的行，这些类的跨行拼接说明会全部漏翻。
//
// 【为什么不能简单地找任意 '='】
//   helper 宏的参数里也可能出现 '='，例如真实写法：
//     @PointClass ... drawangles_local{ angles_key = "movedir" } editormodel(...) = info_node_hint
//   若把第一个 '=' 当作「类名分隔符」，解析出来的类名会是 "movedir" 而不是 info_node_hint，
//   导致该类的 fgd_override 覆盖失效、整链折叠判断出错。
//   因此必须跳过括号 () / 方括号 [] / 花括号 {} 内部的内容，只在顶层寻找 '='。
//
// 注意：本函数只判断「这行是不是类定义行」。helper 里的路径类文本（models/editor/xxx.vmdl 等）
// 在这里不会被当作可翻译内容——后续翻译只作用于 '=' 之后的类说明字面量。
static bool IsClassDirectiveLine(const std::string& code) {
    static const std::regex classDirectiveRegex(
        R"re(^\s*@\s*[A-Za-z_][A-Za-z0-9_]*\b)re");
    if (!std::regex_search(code, classDirectiveRegex)) {
        return false;
    }
    return FindTopLevelEquals(code) != std::string::npos;
}

// 判断类定义语句是否已经结束（遇到 '[' 进入属性块，或其它非说明语句）
static bool ClassDeclTerminated(const std::string& code) {
    return code.find('[') != std::string::npos ||
           code.find(']') != std::string::npos ||
           code.find('{') != std::string::npos ||
           code.find('}') != std::string::npos;
}

// 从类定义指令中取出类名：`@PointClass base(...) = info_node :` → info_node
//
// 必须用「顶层 '='」定位类名分隔符：helper 参数里可能出现 '='，
// 例如 `drawangles_local{ angles_key = "movedir" } = info_node_hint`，
// 用第一个 '=' 会错误地取出 "movedir"。
static std::string ExtractClassNameFromDirective(const std::string& code) {
    const size_t eq = FindTopLevelEquals(code);
    if (eq == std::string::npos) return "";
    size_t i = eq + 1;
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
    const size_t start = i;
    while (i < code.size() && (isalnum((unsigned char)code[i]) || code[i] == '_')) ++i;
    return code.substr(start, i - start);
}

// FGD 字符串字面量的转义还原（词典值为多行文本时写回字面量）
static std::string EscapeForFgdLiteral(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else escaped += c;
    }
    return escaped;
}

// 把整行改写为「单个字面量」（保留原缩进），用于整条说明替换为一条译文
static std::string CollapseToSingleLiteral(const std::string& code, const std::string& text) {
    const std::string trimmed = TrimString(code);
    std::string indent;
    if (!trimmed.empty() && code.size() >= trimmed.size()) {
        const size_t lead = code.find(trimmed);
        if (lead != std::string::npos && lead < code.size()) {
            indent = code.substr(0, lead);
        }
    } else {
        indent = "\t";
    }
    return indent + "\"" + EscapeForFgdLiteral(text) + "\"";
}

// 整条拼接链读完后：若「拼接后整体」被词典收录，把整条说明折叠为一条完整译文。
//
// 键的推荐写法是「按 FGD 转义解码后的文本」（即 \n 为真实换行，与 README 一致）。
// 这里额外接受「未解码的原始拼接文本」与「去首尾空白」的变体，纯粹是为了兼容
// 手工抄写或旧版脚本生成的键，属于匹配容错，不是并列的收录方式——请勿依赖它。
//
// 命中返回折叠后的行；未命中返回 std::nullopt，由调用方使用逐行译文。
static std::optional<std::string> TryCollapseJoinedChain(
    const std::string& code, const FgdDict& dict, const FgdConcatState& st)
{
    if (st.litCount < 2) {
        return std::nullopt;
    }

    const std::string candidates[] = {
        st.decoded,
        TrimString(st.decoded),
        st.raw,
        TrimString(st.raw),
    };
    for (const std::string& key : candidates) {
        if (key.empty()) continue;
        auto it = dict.find(key);
        if (it != dict.end() && !it->second.empty()) {
            return CollapseToSingleLiteral(code, it->second);
        }
    }
    return std::nullopt;
}

// 逐行阶段的多行拼接跟踪：只按「单段字面量」查译文。
// 整条链的折叠（含 fgd_override 整类覆盖）统一由 TranslateContent 负责，
// 因此这里不需要 overrideData。
static std::optional<std::string> TryAppendConcatenatedClassDesc(
    const std::string& code, const FgdDict& dict,
    FgdConcatState& st)
{
    if (!st.awaitClassStart) {
        return std::nullopt;
    }

    // 当前行本身就是类定义指令（刚登记完状态的那一行）：不处理，保留状态给下一行
    if (IsClassDirectiveLine(code)) {
        return std::nullopt;
    }

    const std::string trimmed = TrimString(code);
    if (trimmed.empty()) {
        return code;
    }

    // 属性块边界：说明已结束
    if (ClassDeclTerminated(code) ||
        trimmed[0] == ']' || trimmed[0] == '}') {
        st.awaitClassStart = false;
        st.classStarted = false;
        return std::nullopt;
    }

    std::vector<std::string> literals;
    bool tailPlus = false;
    if (!ParseConcatLiterals(code, literals, tailPlus)) {
        st.awaitClassStart = false;
        st.classStarted = false;
        return std::nullopt;
    }

    // 【不在此处理 fgd_override 的整类说明覆盖】
    // 覆盖是「整段替换一条多行说明」，必须在能同时改写整条链所有行的层面完成，
    // 即 TranslateContent。若在逐行阶段处理，链中每一行都会各自命中一次覆盖文本，
    // 输出 N 份重复的说明（且彼此缺少 '+' 连接，语法非法）。
    // 这里只做「按单段字面量查译文」。
    if (!st.classStarted && !tailPlus && literals.size() == 1) {
        // 单段说明（原逻辑已覆盖），交回既有流程
        st.awaitClassStart = false;
        return std::nullopt;
    }

    const std::string translatedCode = TranslateDescLiterals(code, literals, dict);
    st.classStarted = true;

    for (const std::string& lit : literals) {
        st.decoded += DecodeFgdEscapes(lit);
        st.raw += lit;
        st.litCount++;
    }

    if (!tailPlus) {
        // 链在本行收尾。只有「整条链都在本行内」（同类定义行首，且本行含多个字面量）
        // 时才允许就地折叠——此时本行是整条链唯一的载体，折叠是安全的。
        // 跨行链的收尾行不能折叠：前面的行已经作为返回值交给了调用方，无法撤回。
        if (st.litCount == literals.size() && literals.size() > 1) {
            if (auto collapsed = TryCollapseJoinedChain(code, dict, st)) {
                st.awaitClassStart = false;
                st.classStarted = false;
                st.inChain = false;
                st.decoded.clear();
                st.raw.clear();
                st.litCount = 0;
                st.className.clear();
                return collapsed;
            }
        }
        st.awaitClassStart = false;
        st.classStarted = false;
        st.inChain = false;
        st.decoded.clear();
        st.raw.clear();
        st.litCount = 0;
        st.className.clear();
        return translatedCode;
    }

    st.inChain = true;
    return translatedCode;
}

// 拼接链的续行（上一行以 '+' 结尾）：按单段字面量查译文。
// 注意：此处不做整链折叠——流式调用时前面的行已经交出，无法回头修改；
// 在最后一行替换成整段译文会造成中英混杂。整链折叠归口 TranslateContent。
static std::optional<std::string> TryConcatContinuation(
    const std::string& code, const FgdDict& dict,
    FgdConcatState& st)
{
    if (!st.inChain) {
        return std::nullopt;
    }

    std::vector<std::string> literals;
    bool tailPlus = false;
    const std::string trimmed = TrimString(code);

    if (trimmed.empty() || !ParseConcatLiterals(code, literals, tailPlus)) {
        // 语法无法识别的续行：放弃整条链
        st.inChain = false;
        st.decoded.clear();
        st.raw.clear();
        st.litCount = 0;
        st.className.clear();
        return std::nullopt;
    }

    // 【不在此处理 fgd_override 的整类说明覆盖】——理由见 TryAppendConcatenatedClassDesc。
    // 逐行阶段只按单段字面量查译文，整链折叠统一由 TranslateContent 完成。
    const std::string translatedCode = TranslateDescLiterals(code, literals, dict);

    for (const std::string& lit : literals) {
        st.decoded += DecodeFgdEscapes(lit);
        st.raw += lit;
        st.litCount++;
    }

    if (tailPlus) {
        return translatedCode;
    }

    st.inChain = false;
    st.classStarted = false;
    st.awaitClassStart = false;

    // 【重要】这里不做「整链折叠」。
    // 逐行流式处理时，链中前面的行已经作为返回值交给了调用方，无法再回头修改；
    // 若在此把最后一行替换成整段译文，输出会变成
    //     "English line 2" + "English line 3 " + "完整中文翻译"
    // 即中英混杂的坏结果。
    // 整链折叠统一由 TranslateContent 在「内容层面」完成（它能同时改写整条链的所有行）。
    // 单行流模式下只按字面量逐段翻译，保证每一行的语义独立且正确。
    st.decoded.clear();
    st.raw.clear();
    st.litCount = 0;
    st.className.clear();
    return translatedCode;
}

} // namespace

// ==============================================================================
// LineState：把类作用域状态与「多行字符串拼接」跟踪状态打包给调用方持有
// ==============================================================================
struct FgdCore::LineState::Impl {
    std::string currentClass;
    std::string pendingClassDesc;
    FgdConcatState concat;
};

FgdCore::LineState::LineState() : m_impl(new Impl()) {}
FgdCore::LineState::~LineState() = default;
FgdCore::LineState::LineState(LineState&&) noexcept = default;
FgdCore::LineState& FgdCore::LineState::operator=(LineState&&) noexcept = default;

std::string FgdCore::TranslateLine(
    const std::string& line,
    const FgdDict& dict,
    const FgdOverrideData& overrideData,
    LineState& state
) {
    std::string& inOutCurrentClass = state.m_impl->currentClass;
    std::string& inOutPendingClassDesc = state.m_impl->pendingClassDesc;
    FgdConcatState& concat = state.m_impl->concat;


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

    // 1. @类定义指令：必须先登记「等待说明字面量」状态。
    //    注意必须在任何 return 之前完成，否则 TryClassDefinition 命中后直接返回，
    //    状态永远不会被设置（这正是多行拼接说明长期漏翻的根因之一）。
    const bool bIsDirective = IsClassDirectiveLine(code);
    if (bIsDirective) {
        const std::string trimmed = TrimString(code);
        concat.awaitClassStart = !trimmed.empty() && trimmed.back() != '"';
        concat.classStarted = false;
        concat.inChain = false;
        concat.className = ExtractClassNameFromDirective(code);
        concat.decoded.clear();
        concat.raw.clear();
        concat.litCount = 0;
    }

    const std::optional<std::string> translated = [&]() -> std::optional<std::string> {
        // 0. 多行字符串拼接：类定义说明的续行 / 拼接链续行
        if (auto r = TryAppendConcatenatedClassDesc(code, dict, concat)) return r;
        if (auto r = TryConcatContinuation(code, dict, concat)) return r;

        if (auto r = TryPendingClassDesc(code, dict, overrideData, inOutPendingClassDesc)) return r;
        if (auto r = TryClassDefinition(code, dict, overrideData, inOutCurrentClass, inOutPendingClassDesc)) return r;
        if (auto r = TryIoDefinition(code, dict, overrideData, inOutCurrentClass)) return r;
        if (auto r = TryEntityGroupDirective(code, dict)) return r;
        if (auto r = TryEntityToolMetadata(code, dict)) return r;
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

    // 先按行切分（保留行尾），便于把「跨行字符串拼接」当作一个整体处理
    std::vector<std::string> rawLines;
    std::vector<std::string> lineEndings;
    {
        size_t start = 0;
        const size_t n = inputContent.size();
        while (start < n) {
            const size_t end = inputContent.find('\n', start);
            std::string ln = (end == std::string::npos)
                ? inputContent.substr(start)
                : inputContent.substr(start, end - start + 1);
            start = (end == std::string::npos) ? n : end + 1;

            std::string ending;
            if (ln.length() >= 2 && ln.substr(ln.length() - 2) == "\r\n") {
                ending = "\r\n";
                ln.resize(ln.length() - 2);
            } else if (!ln.empty() && ln.back() == '\n') {
                ending = "\n";
                ln.pop_back();
            }
            rawLines.push_back(std::move(ln));
            lineEndings.push_back(std::move(ending));
        }
    }

    LineState state;

    for (size_t i = 0; i < rawLines.size(); ++i) {
        std::string& body = rawLines[i];

        // ------------------------------------------------------------------
        // 跨行字符串拼接的类说明：在「内容层面」整链折叠。
        //   1) 当前行是类定义指令，且下一非空行是字面量链的首段
        //   2) 向后收集完整链，按 FGD 语义（相邻字面量直接首尾相接）还原整段文本
        //   3) 若整段被词典收录：首行写整段译文，其余续行替换为 // 注释
        //
        // 【重要】续行不能写成 ""：FGD 里字符串之间必须由 '+' 连接，
        //   `"译文"` 后面直接跟 `""` 会让 Hammer 的 FGD 解析器报语法错误。
        //   用注释占位既不改变行数，也能被词法分析器安全跳过。
        // ------------------------------------------------------------------
        if (IsClassDirectiveLine(body)) {
            size_t j = i + 1;
            while (j < rawLines.size() && TrimString(rawLines[j]).empty()) ++j;

            if (j < rawLines.size()) {
                std::vector<std::string> lits;
                bool tailPlus = false;
                if (ParseConcatLiterals(rawLines[j], lits, tailPlus)) {
                    // 收集整条链（仅收集能完整解析的行）
                    std::vector<size_t> chainLineIdx{j};
                    std::vector<std::vector<std::string>> chainLits{lits};
                    size_t k = j;
                    bool ended = !tailPlus;
                    while (!ended && k + 1 < rawLines.size()) {
                        ++k;
                        std::vector<std::string> more;
                        bool moreTail = false;
                        if (!ParseConcatLiterals(rawLines[k], more, moreTail)) break;
                        chainLineIdx.push_back(k);
                        chainLits.push_back(more);
                        ended = !moreTail;
                    }

                    // 只有真正的「多行拼接」才需要折叠；单段说明交给原有逻辑
                    if (chainLineIdx.size() > 1 || chainLits[0].size() > 1) {
                        // 1. 优先采用 fgd_override.jsonc 的整类说明覆盖。
                        //    覆盖是按类名生效、与原文内容无关的「整段替换」，
                        //    因此必须先于词典匹配判断，且必须在内容层面一次性折叠，
                        //    否则逐行阶段每行都会各自命中一次覆盖文本，输出 N 份重复。
                        std::string finalTranslation;
                        const std::string className = ExtractClassNameFromDirective(body);
                        if (!className.empty()) {
                            auto itDesc = overrideData.classDescriptions.find(className);
                            if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
                                finalTranslation = itDesc->second;
                            }
                        }

                        // 2. 其次按「拼接后整段文本」查翻译词典
                        if (finalTranslation.empty()) {
                            std::string decoded;
                            for (const auto& group : chainLits) {
                                for (const auto& lit : group) decoded += DecodeFgdEscapes(lit);
                            }
                            auto it = dict.find(TrimString(decoded));
                            if (it != dict.end() && !it->second.empty()) {
                                finalTranslation = it->second;
                            }
                        }

                        if (!finalTranslation.empty()) {
                            rawLines[j] = CollapseToSingleLiteral(rawLines[j], finalTranslation);
                            for (size_t x = 1; x < chainLineIdx.size(); ++x) {
                                const std::string& ln = rawLines[chainLineIdx[x]];
                                const size_t lead = ln.find_first_not_of(" \t");
                                const std::string indent = (lead != std::string::npos) ? ln.substr(0, lead) : "\t";
                                // 注释占位：语法安全、行数不变
                                rawLines[chainLineIdx[x]] = indent + "// (已合并至上一行的完整译文)";
                            }
                        }
                    }
                }
            }
        }

        const std::string out = TranslateLine(body + lineEndings[i], dict, overrideData, state);
        outTranslated.append(out);
    }

    return true;
}
