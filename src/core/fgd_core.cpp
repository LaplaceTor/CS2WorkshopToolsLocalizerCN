#include "core/fgd_core.h"
#include <regex>
#include <sstream>
#include <algorithm>

namespace {

using FgdDict = FgdCore::FgdDict;

struct FgdConcatState {
    bool awaitClassStart = false;
    bool classStarted = false;
    bool inChain = false;
    std::string className;
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
            if (i >= n) return false;
            outLiterals.push_back(code.substr(start, i - start));
            ++i;
            expectOperand = false;
        } else {
            if (code[i] != '+') return false;
            ++i;
            expectOperand = true;
        }
    }

    if (outLiterals.empty()) return false;
    outTailPlus = expectOperand;
    return true;
}

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

    if (code[i] == '@' || code[i] == '/' || code[i] == ':') return false;

    size_t keyStart = i;
    while (i < n && (isalnum((unsigned char)code[i]) || code[i] == '_' || code[i] == '.' || code[i] == '-')) {
        i++;
    }
    if (i == keyStart) return false;
    std::string propKey = code.substr(keyStart, i - keyStart);

    while (i < n && (code[i] == ' ' || code[i] == '\t')) i++;
    if (i >= n || code[i] != '(') return false;
    i++;

    size_t typeStart = i;
    while (i < n && code[i] != ')') {
        i++;
    }
    if (i >= n || code[i] != ')') return false;
    std::string propType = code.substr(typeStart, i - typeStart);
    i++;

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

static std::string GetTranslation(const FgdDict& dict, const std::string& text) {
    const std::string trimmed = TrimString(text);
    auto it = dict.find(trimmed);
    if (it != dict.end() && !it->second.empty()) {
        return it->second;
    }
    return text;
}

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

    return std::nullopt;
}

static std::optional<std::string> TryClassDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass, std::string& inOutPendingClassDesc)
{
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

    static const std::regex classHeaderRegex(R"re((=\s*)([a-zA-Z0-9_]+)(\s*:\s*|\s*)$)re");
    std::smatch classHeaderMatch;
    if (std::regex_search(code, classHeaderMatch, classHeaderRegex)) {
        const std::string className = classHeaderMatch[2].str();
        inOutCurrentClass = className;
        inOutPendingClassDesc = className;
        return code;
    }

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

static std::optional<std::string> TryPropertyDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    const std::string& currentClass)
{
    std::string propHead, propKey, propType, rest;
    if (!ExtractPropertyHeader(code, propHead, propKey, propType, rest)) {
        return std::nullopt;
    }

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

static bool IsClassDirectiveLine(const std::string& code) {
    static const std::regex classDirectiveRegex(
        R"re(^\s*@\s*[A-Za-z_][A-Za-z0-9_]*\b)re");
    if (!std::regex_search(code, classDirectiveRegex)) {
        return false;
    }
    return FindTopLevelEquals(code) != std::string::npos;
}

static bool ClassDeclTerminated(const std::string& code) {
    return code.find('[') != std::string::npos ||
           code.find(']') != std::string::npos ||
           code.find('{') != std::string::npos ||
           code.find('}') != std::string::npos;
}

static std::string ExtractClassNameFromDirective(const std::string& code) {
    const size_t eq = FindTopLevelEquals(code);
    if (eq == std::string::npos) return "";
    size_t i = eq + 1;
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
    const size_t start = i;
    while (i < code.size() && (isalnum((unsigned char)code[i]) || code[i] == '_')) ++i;
    return code.substr(start, i - start);
}

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

static std::optional<std::string> TryAppendConcatenatedClassDesc(
    const std::string& code, const FgdDict& dict,
    FgdConcatState& st)
{
    if (!st.awaitClassStart) {
        return std::nullopt;
    }

    if (IsClassDirectiveLine(code)) {
        return std::nullopt;
    }

    const std::string trimmed = TrimString(code);
    if (trimmed.empty()) {
        return code;
    }

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

    if (!st.classStarted && !tailPlus && literals.size() == 1) {
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
        st.inChain = false;
        st.decoded.clear();
        st.raw.clear();
        st.litCount = 0;
        st.className.clear();
        return std::nullopt;
    }

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

    st.decoded.clear();
    st.raw.clear();
    st.litCount = 0;
    st.className.clear();
    return translatedCode;
}

}

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

        if (IsClassDirectiveLine(body)) {
            size_t j = i + 1;
            while (j < rawLines.size() && TrimString(rawLines[j]).empty()) ++j;

            if (j < rawLines.size()) {
                std::vector<std::string> lits;
                bool tailPlus = false;
                if (ParseConcatLiterals(rawLines[j], lits, tailPlus)) {
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

                    if (chainLineIdx.size() > 1 || chainLits[0].size() > 1) {
                        std::string finalTranslation;
                        const std::string className = ExtractClassNameFromDirective(body);
                        if (!className.empty()) {
                            auto itDesc = overrideData.classDescriptions.find(className);
                            if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
                                finalTranslation = itDesc->second;
                            }
                        }

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
