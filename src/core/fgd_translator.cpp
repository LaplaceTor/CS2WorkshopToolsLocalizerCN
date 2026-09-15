#include "core/fgd_translator.h"
#include "core/dictionary_compiler.h"
#include "core/backup_manager.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <optional>
#include <iostream>
#include <memory>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QFile>
#include <QSaveFile>
#include <QString>

namespace fs = std::filesystem;

// ==============================================================================
// 字典加载与解析（使用 Qt 原生 QJsonDocument 解析）
// ==============================================================================

bool FgdTranslator::LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict, const std::wstring& fallbackJsonPath) {
    outDict.clear();

    auto loadSingleJson = [&](const std::wstring& path) -> bool {
        if (path.empty()) return false;
        QFile file(QString::fromStdWString(path));
        if (!file.open(QIODevice::ReadOnly)) return false;
        QByteArray rawData = file.readAll();
        file.close();

        std::string clean = DictionaryCompiler::StripJsonComments(rawData.constData(), rawData.size());
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(clean.c_str(), clean.size()), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }

        QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isString()) {
                QString val = it.value().toString().trimmed();
                if (!val.isEmpty()) {
                    outDict[it.key().toStdString()] = val.toStdString();
                }
            }
        }
        return true;
    };

    // 1. 若提供了 fallback 字典，先载入作为兜底
    if (!fallbackJsonPath.empty() && fs::exists(fallbackJsonPath)) {
        loadSingleJson(fallbackJsonPath);
    }

    // 2. 载入主字典，覆盖/补充 fallback
    loadSingleJson(jsonPath);

    return !outDict.empty();
}

bool FgdTranslator::LoadOverrideDictionary(const std::wstring& jsonPath, FgdOverrideData& outOverride) {
    outOverride = FgdOverrideData();
    QFile file(QString::fromStdWString(jsonPath));
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray rawData = file.readAll();
    file.close();

    std::string clean = DictionaryCompiler::StripJsonComments(rawData.constData(), rawData.size());
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(clean.c_str(), clean.size()), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();

    auto parsePropOverride = [](const QJsonValue& val, FgdPropertyOverride& propOut) {
        if (val.isString()) {
            propOut.description = val.toString().toStdString();
        } else if (val.isObject()) {
            QJsonObject o = val.toObject();
            if (o.contains("description") && o["description"].isString()) {
                propOut.description = o["description"].toString().toStdString();
            }
            if (o.contains("displayName") && o["displayName"].isString()) {
                propOut.displayName = o["displayName"].toString().toStdString();
            }
        }
    };

    for (auto it = root.begin(); it != root.end(); ++it) {
        QString key = it.key();
        const QJsonValue& val = it.value();

        if (key == "properties" && val.isObject()) {
            QJsonObject pObj = val.toObject();
            for (auto pit = pObj.begin(); pit != pObj.end(); ++pit) {
                FgdPropertyOverride prop;
                parsePropOverride(pit.value(), prop);
                if (!prop.description.empty() || !prop.displayName.empty()) {
                    outOverride.globalProperties[pit.key().toStdString()] = std::move(prop);
                }
            }
        } else if (key == "io" && val.isObject()) {
            QJsonObject ioObj = val.toObject();
            for (auto ioit = ioObj.begin(); ioit != ioObj.end(); ++ioit) {
                if (ioit.value().isString()) {
                    outOverride.ioOverrides[ioit.key().toStdString()] = ioit.value().toString().toStdString();
                } else if (ioit.value().isObject()) {
                    QJsonObject o = ioit.value().toObject();
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.ioOverrides[ioit.key().toStdString()] = o["description"].toString().toStdString();
                    }
                }
            }
        } else if (key == "classes" && val.isObject()) {
            QJsonObject cObj = val.toObject();
            for (auto cit = cObj.begin(); cit != cObj.end(); ++cit) {
                std::string clsName = cit.key().toStdString();
                if (cit.value().isString()) {
                    outOverride.classDescriptions[clsName] = cit.value().toString().toStdString();
                } else if (cit.value().isObject()) {
                    QJsonObject o = cit.value().toObject();
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.classDescriptions[clsName] = o["description"].toString().toStdString();
                    }
                    if (o.contains("properties") && o["properties"].isObject()) {
                        QJsonObject cpObj = o["properties"].toObject();
                        for (auto cpit = cpObj.begin(); cpit != cpObj.end(); ++cpit) {
                            FgdPropertyOverride prop;
                            parsePropOverride(cpit.value(), prop);
                            if (!prop.description.empty() || !prop.displayName.empty()) {
                                outOverride.classProperties[clsName][cpit.key().toStdString()] = std::move(prop);
                            }
                        }
                    }
                }
            }
        } else if (!key.startsWith("_")) {
            if (val.isString()) {
                FgdPropertyOverride prop;
                prop.description = val.toString().toStdString();
                outOverride.globalProperties[key.toStdString()] = std::move(prop);
            } else if (val.isObject()) {
                QJsonObject o = val.toObject();
                if (o.contains("properties") && o["properties"].isObject()) {
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.classDescriptions[key.toStdString()] = o["description"].toString().toStdString();
                    }
                    QJsonObject cpObj = o["properties"].toObject();
                    for (auto cpit = cpObj.begin(); cpit != cpObj.end(); ++cpit) {
                        FgdPropertyOverride prop;
                        parsePropOverride(cpit.value(), prop);
                        if (!prop.description.empty() || !prop.displayName.empty()) {
                            outOverride.classProperties[key.toStdString()][cpit.key().toStdString()] = std::move(prop);
                        }
                    }
                } else {
                    FgdPropertyOverride prop;
                    parsePropOverride(val, prop);
                    if (!prop.description.empty() || !prop.displayName.empty()) {
                        outOverride.globalProperties[key.toStdString()] = std::move(prop);
                    }
                }
            }
        }
    }

    return !outOverride.empty();
}

static inline std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// ==============================================================================
// 准确提取 FGD 属性头部（支持任意深度的 KV3 嵌套 {} 与 [] 属性）
// ==============================================================================
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

// ==============================================================================
// 单行 FGD 翻译与覆盖处理
// ==============================================================================

std::string FgdTranslator::TranslateLine(const std::string& line, const std::unordered_map<std::string, std::string>& dict) {
    std::string dummyClass;
    std::string dummyPending;
    FgdOverrideData dummyOverride;
    return TranslateLine(line, dict, dummyOverride, dummyClass, dummyPending);
}

std::string FgdTranslator::TranslateLine(
    const std::string& line,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass
) {
    std::string dummyPending;
    return TranslateLine(line, dict, overrideData, inOutCurrentClass, dummyPending);
}

namespace {

using FgdDict = std::unordered_map<std::string, std::string>;

// 查词典；未命中或译为空串时原样返回
std::string GetTranslation(const FgdDict& dict, const std::string& text) {
    const std::string trimmed = TrimString(text);
    auto it = dict.find(trimmed);
    if (it != dict.end() && !it->second.empty()) {
        return it->second;
    }
    return text;
}

// 翻译形如 ` "显示名"` 的片段，保留原始缩进与空格
// （FGD 属性体按冒号切分后，每个片段都带前导空格，不能直接整体替换）
std::string TranslateQuotedPart(const std::string& part, const FgdDict& dict) {
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

// 按冒号切分属性体，忽略引号内的冒号。
// 第一个冒号之前属于属性定义头，会被丢弃；返回 false 表示整行没有冒号分隔。
// 该逻辑在「属性定义」与「属性跨行续行」两处完全相同，此前是两份复制。
bool SplitPropertyParts(const std::string& body, std::vector<std::string>& outParts) {
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

// 把切分后的片段重新拼回冒号分隔串。
// 首冒号前的空格由 colon 决定，两种调用场景并不相同，不能统一：
//   * 属性定义行   " :"（FGD 里 prop(type) : "Name" 冒号前有空格）
//   * 跨行续行     ":" （续行以冒号顶格开始，冒号前无空格，否则会多出一个前导空格）
std::string JoinPropertyParts(const std::vector<std::string>& parts, const std::string& colon) {
    std::string out = colon;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ":";
        out += parts[i];
    }
    return out;
}

// ===========================================================================
// 以下各 Try* 函数对应 FGD 语法中的一种结构。
// 约定：返回 nullopt = 本行不属于该结构，交由后续分支继续尝试；
//       返回字符串   = 该行的最终内容（不含行尾注释与换行）。
// 这些分支原先以 `// 1.1 / 1.2 / 3.1` 手工编号的形式挤在一个 447 行的函数里。
// ===========================================================================

// 0. 等待上一行的跨行类说明（@PointClass ... = classname :\n  "Desc"）
std::optional<std::string> TryPendingClassDesc(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    std::string& inOutPendingClassDesc)
{
    if (inOutPendingClassDesc.empty()) {
        return std::nullopt;
    }

    const std::string trimmed = TrimString(code);
    if (trimmed.empty()) {
        // 空行保持状态，原样输出
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
        inOutPendingClassDesc.clear(); // 已完成匹配替换

        const size_t lead = code.find_first_not_of(" \t");
        const std::string indent = (lead != std::string::npos) ? code.substr(0, lead) : "\t";
        return indent + "\"" + finalDesc + "\"";
    }

    if (trimmed.front() == '[') {
        // 原类定义没有独立描述行，直接遇到了 [
        auto itDesc = overrideData.classDescriptions.find(inOutPendingClassDesc);
        const std::string cls = inOutPendingClassDesc;
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
std::optional<std::string> TryClassDefinition(
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
std::optional<std::string> TryIoDefinition(
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

    // 参数类型包含 api 时，Valve FGD 语法不支持冒号和描述，直接原样保留
    if (ioParam.find("api") != std::string::npos) {
        return code;
    }

    // 覆盖优先级：当前类 > 全局 ioOverrides > globalProperties
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
        // 原行无描述，追加描述
        return ioPrefix + ioName + ioParam + " : \"" + overrideIoDesc + "\"";
    }
    return code;
}

// 3. 按钮/元数据说明：desc = "Description"
std::optional<std::string> TryDescMetadata(const std::string& code, const FgdDict& dict) {
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
std::optional<std::string> TryStandaloneGroup(const std::string& code, const FgdDict& dict) {
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
std::optional<std::string> TryPropertyDefinition(
    const std::string& code, const FgdDict& dict, const FgdOverrideData& overrideData,
    const std::string& currentClass)
{
    std::string propHead, propKey, propType, rest;
    if (!ExtractPropertyHeader(code, propHead, propKey, propType, rest)) {
        return std::nullopt;
    }

    // 翻译属性行内部的属性组：[ group="Render Properties" ] 或 { group="Style" }
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

    // 查找该属性是否有 override（当前类优先，其次全局）
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

    // 分离 choices 尾部（以等号开始，忽略引号内的等号）
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

    // 属性没有冒号定义（例如跨行定义的属性头 useLocalOffset(boolean) 或 spawnflags(flags) [ ... ] =）
    if (TrimString(propBody).empty()) {
        return propHead + rest;
    }

    std::vector<std::string> parts;
    const bool hasFirstColon = SplitPropertyParts(propBody, parts);
    if (!hasFirstColon) {
        return code;
    }

    if (!parts.empty()) {
        // parts[0] 显示名
        if (propOverride && !propOverride->displayName.empty()) {
            parts[0] = " \"" + propOverride->displayName + "\"";
        } else {
            parts[0] = TranslateQuotedPart(parts[0], dict);
        }

        if (parts.size() == 1) {
            // 仅有显示名
            if (propOverride && !propOverride->description.empty()) {
                parts.push_back(" \"\"");
                parts.push_back(" \"" + propOverride->description + "\"");
            }
        } else if (parts.size() == 2) {
            // 显示名 + 默认值，无描述
            if (propOverride && !propOverride->description.empty()) {
                parts.push_back(" \"" + propOverride->description + "\"");
            }
        } else if (propOverride && !propOverride->description.empty()) {
            // 显示名 + 默认值 + 描述
            parts[2] = " \"" + propOverride->description + "\"";
        } else {
            parts[2] = TranslateQuotedPart(parts[2], dict);
        }
    }

    return propHead + JoinPropertyParts(parts, " :") + choicesTail;
}

// 4.1 属性跨行定义的续行（以冒号开头，如 : "Use Local Transform" : 0 : "..."）
std::optional<std::string> TryPropertyContinuation(const std::string& code, const FgdDict& dict) {
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

    parts[0] = TranslateQuotedPart(parts[0], dict);          // 显示名
    if (parts.size() >= 3) {
        parts[2] = TranslateQuotedPart(parts[2], dict);      // 描述
    }

    const size_t leadPos = code.find_first_not_of(" \t");
    const std::string leadingSpaces = (leadPos != std::string::npos) ? code.substr(0, leadPos) : "";
    return leadingSpaces + JoinPropertyParts(parts, ":");
}

// 5. 选项列表 (Choices / Flags)："0" : "Enabled" : "Option Desc" 或 1 : "Passable" : 0
std::optional<std::string> TryChoiceEntry(const std::string& code, const FgdDict& dict) {
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

    // 替换 tail 中的附加选项描述（如果存在）
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

// ==============================================================================
// 单行翻译主流程：
//   分离行尾与注释 → 依次尝试各类 FGD 语法结构 → 未命中则原样返回
// 各语法结构的判定与翻译见上方匿名命名空间中的 Try* 函数。
// ==============================================================================
std::string FgdTranslator::TranslateLine(
    const std::string& line,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass,
    std::string& inOutPendingClassDesc
) {
    // 1. 分离行尾换行符
    std::string lineEnding;
    std::string raw = line;
    if (raw.length() >= 2 && raw.substr(raw.length() - 2) == "\r\n") {
        lineEnding = "\r\n";
        raw = raw.substr(0, raw.length() - 2);
    } else if (!raw.empty() && raw.back() == '\n') {
        lineEnding = "\n";
        raw = raw.substr(0, raw.length() - 1);
    }

    // 2. 分离代码与单行注释 //（避免匹配引号内的 //）
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

    // 3. 按 FGD 语法优先级依次尝试，第一个命中的分支决定该行结果
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

// ==============================================================================
// FGD 文件批量与单文件翻译
// ==============================================================================

bool FgdTranslator::TranslateFile(
    const std::wstring& srcPath,
    const std::wstring& dstPath,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData
) {
    std::ifstream inFile(srcPath, std::ios::binary);
    if (!inFile.is_open()) return false;

    std::string line;
    std::vector<std::string> lines;
    while (std::getline(inFile, line)) {
        if (!inFile.eof() || !line.empty()) {
            line += "\n";
        }
        lines.push_back(line);
    }
    inFile.close();

    fs::path outDir = fs::path(dstPath).parent_path();
    if (!outDir.empty()) {
        fs::create_directories(outDir);
    }

    // QSaveFile 原子写入：先写临时文件，commit 时整体替换，避免中断产生半写文件
    QSaveFile outFile(QString::fromStdWString(dstPath));
    if (!outFile.open(QIODevice::WriteOnly)) {
        return false;
    }

    std::string currentClassName = "";
    std::string pendingClassDesc = "";
    for (const auto& l : lines) {
        std::string transLine = TranslateLine(l, dict, overrideData, currentClassName, pendingClassDesc);
        if (outFile.write(transLine.data(), static_cast<qint64>(transLine.size())) < 0) {
            outFile.cancelWriting();
            return false;
        }
    }

    if (!outFile.commit()) {
        return false;
    }
    return true;
}

bool FgdTranslator::TranslateAndDeployAll(
    const std::wstring& cs2Root,
    const std::wstring& backupDir,
    const std::wstring& translationsDir,
    const std::wstring& jsonDictPath,
    std::vector<std::wstring>& outProcessedFiles,
    std::wstring& outError
) {
    return TranslateAndDeployAll(cs2Root, backupDir, translationsDir, jsonDictPath, L"", outProcessedFiles, outError);
}

bool FgdTranslator::TranslateAndDeployAll(
    const std::wstring& cs2Root,
    const std::wstring& backupDir,
    const std::wstring& translationsDir,
    const std::wstring& jsonDictPath,
    const std::wstring& jsonOverridePath,
    std::vector<std::wstring>& outProcessedFiles,
    std::wstring& outError,
    const std::wstring& jsonFallbackPath
) {
    std::unordered_map<std::string, std::string> dict;
    if (!LoadDictionary(jsonDictPath, dict, jsonFallbackPath)) {
        outError = L"无法加载 FGD 翻译字典: " + jsonDictPath;
        return false;
    }

    FgdOverrideData overrideData;
    if (!jsonOverridePath.empty() && fs::exists(jsonOverridePath)) {
        LoadOverrideDictionary(jsonOverridePath, overrideData);
    }

    fs::path backupRoot(backupDir);
    if (!fs::exists(backupRoot)) {
        outError = L"找不到 backup 目录: " + backupDir;
        return false;
    }

    std::vector<std::wstring> failedFiles;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file() && entry.path().extension() == L".fgd") {
                fs::path relPath = fs::relative(entry.path(), backupRoot);
                fs::path transDst = fs::path(translationsDir) / relPath;
                fs::path cs2Dst = fs::path(cs2Root) / relPath;

                // 翻译至 translationsDir；单个文件失败记录后继续，最后统一汇总
                if (!TranslateFile(entry.path().wstring(), transDst.wstring(), dict, overrideData)) {
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }

                // 覆盖复制到 CS2 对应目录（先拷贝到临时名再整体替换，避免半写；被占用时自动重试）
                fs::create_directories(cs2Dst.parent_path());
                fs::path cs2Tmp = cs2Dst;
                cs2Tmp += L".tmp";
                if (!BackupManager::SafeCopyFileWithRetry(transDst, cs2Tmp)) {
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }
                std::error_code replaceEc;
                fs::rename(cs2Tmp, cs2Dst, replaceEc);
                if (replaceEc) {
                    fs::remove(cs2Tmp, replaceEc);
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }

                outProcessedFiles.push_back(relPath.wstring());
            }
        }

        if (!failedFiles.empty()) {
            std::wstring failedList;
            for (const auto& f : failedFiles) {
                failedList += L"\n  - " + f;
            }
            outError = L"有 " + std::to_wstring(failedFiles.size()) + L" 个 FGD 文件处理失败:" + failedList;
            return false;
        }
    } catch (const std::exception& e) {
        outError = L"处理 FGD 异常: " + QString::fromUtf8(e.what()).toStdWString();
        return false;
    }

    if (outProcessedFiles.empty()) {
        outError = L"backup 目录中未找到任何 FGD 文件: " + backupDir;
        return false;
    }
    return true;
}

// ==============================================================================
// 确保字典文件存在 (模板自动生成)
// ==============================================================================

bool FgdTranslator::EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 数据体由 QJsonDocument 组装输出，转义交给 Qt 处理
    QJsonObject entries;
    if (loaded.empty()) {
        entries["Omnidirectional point light"] = "全向点光源";
        entries["Light Source"] = "光源";
        entries["Name"] = "名称";
        entries["The name that other entities use to refer to this entity."] = "其他实体用于引用此实体的名称。";
        entries["Removes this entity from the world."] = "从世界中移除此实体。";
        entries["Enabled"] = "已启用";
        entries["Disabled"] = "已禁用";
    } else {
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            entries[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
        }
    }

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 FGD 翻译字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 Hammer FGD 实体定义翻译字典 (JSONC 格式)
// ==============================================================================
//
// 【使用指南】
// - 格式为标准的键值对："英文原词": "中文翻译"
// - 支持 // 单行注释 与 /* 块注释 */
//
// 【可翻译内容】
// 1. 实体类说明 (@PointClass ... = name : "Description")
// 2. 属性显示名称 (targetname : "Name" : : "...")
// 3. 属性悬停描述 (... : "Name" : default : "Description")
// 4. 选项与标记 ("0" : "Enabled" : "Option Desc")
// 5. 输入输出 (input Kill : "Description")
// 6. 绑定按钮说明 (desc = "Description")
//
// 【格式与安全】
// - 所有底层 RAW 标识符（如 targetname、angles、thinkalways、io 类型与默认值）引擎会自动保护，请仅翻译双引号内的文本。
// - 修改保存后重新在启动器点击启动即可自动重新编译部署。
// ==============================================================================
)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 FGD 翻译字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 fgd_translations.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}

bool FgdTranslator::EnsureFgdOverrideDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    FgdOverrideData loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadOverrideDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 示例数据由 QJsonDocument 组装输出
    QJsonObject properties;
    properties["bodygroups"] = "设置模型的子部件与可选身体部件网格组合。";
    properties["vscripts"] = "实体生成后自动加载并执行的 VScript 脚本文件列表。";
    properties["clientSideEntity"] = "是否仅在客户端创建并运行此实体（不向服务器同步）。";
    properties["TeamNum"] = "所属队伍编号（0: 任意/无队伍, 2: T 阵营, 3: CT 阵营）。";
    properties["box_mins"] = "包围盒/光照探针体积的最小边界坐标 (X Y Z)。";
    properties["box_maxs"] = "包围盒/光照探针体积的最大边界坐标 (X Y Z)。";
    properties["flood_fill"] = "忽略玩家不可达的空间，加快光照烘焙速度并节省显存。";
    properties["voxelize"] = "忽略已体素化的实体空间，优化光照探针计算。";
    properties["light_probe_volume_from_cubemap"] = "是否使用立方体贴图 (Cubemap) 计算漫反射光照探针。";
    properties["moveable"] = "是否允许在游戏运行时移动、绑定父级、启用或禁用此对象。";
    properties["edge_fade_dist"] = "反射或光照边界平滑淡出过渡距离。";
    properties["max_lightmap_resolution"] = "限制此对象在烘焙时的最大光照贴图分辨率（0 为默认）。";

    QJsonObject io;
    io["SetParent"] = "设置该实体的父级对象。";
    io["ClearParent"] = "解除与父级实体的挂载绑定关系，使其独立运动。";
    io["FollowEntity"] = "骨骼合并 (Bone Merge) 附加到目标实体。";
    io["Kill"] = "从世界中移除此实体并释放资源。";
    io["SetHealth"] = "设置该实体的当前生命值。";

    QJsonObject envCubemapProps;
    envCubemapProps["influenceradius"] = "当前立方体贴图的生效影响半径（单位：英寸）。";

    QJsonObject envCubemap;
    envCubemap["description"] = "用于采样环境间接镜面反射的高动态范围立方体贴图实体。";
    envCubemap["properties"] = envCubemapProps;

    QJsonObject classes;
    classes["info_node"] = "AI 地面导航节点，供 NPC 寻路与路径规划计算使用。";
    classes["csm_fov_override"] = "级联阴影贴图 (CSM) 视场角覆盖控制器。";
    classes["env_cubemap"] = envCubemap;

    QJsonObject root;
    root["properties"] = properties;
    root["io"] = io;
    root["classes"] = classes;

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 FGD 覆盖字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 FGD 实体键值描述补充与覆盖字典 (JSONC 格式)
// ==============================================================================
//
// 【作用说明】
// - 本文件用于针对 FGD 中特定的【属性名 (Key)】、【实体类名 (Class)】或【输入输出 (I/O)】
//   补充缺失的说明描述，或覆盖原版已有描述。
// - 与 fgd_translations.jsonc 互为补充：
//   * fgd_translations.jsonc: 负责已有英文字符串 -> 中文翻译。
//   * fgd_override.jsonc: 负责针对特定键名无描述时【自动新增描述】或【强制覆盖描述】。
//
// 【支持格式】
// 1. 全局属性描述补充 (properties): "属性键名": "描述文本" 或 "属性键名": { "description": "...", "displayName": "..." }
// 2. 输入输出说明补充 (io): "IOName": "说明文本"
// 3. 实体类说明补充 (classes): "classname": "说明文本" 或 "classname": { "description": "...", "properties": { ... } }
// 4. 顶层快速简写: "键名": "描述文本"
// ==============================================================================

)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 FGD 覆盖字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 fgd_override.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}

bool FgdTranslator::EnsureQtDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 数据体由 QJsonDocument 组装输出，转义交给 Qt 处理
    QJsonObject entries;
    if (loaded.empty()) {
        entries["File"] = "文件";
        entries["Edit"] = "编辑";
        entries["View"] = "视图";
        entries["Tools"] = "工具";
        entries["New"] = "新建";
        entries["Open"] = "打开";
        entries["Save"] = "保存";
        entries["Save As..."] = "另存为...";
        entries["Close"] = "关闭";
        entries["Exit"] = "退出";
        entries["Undo"] = "撤销";
        entries["Redo"] = "重做";
        entries["Clipping Tool"] = "剪切工具";
        entries["Transform Locked"] = "变换锁定";
        entries["Pinned To"] = "固定至";
        entries["Force Hidden"] = "强制隐藏";
    } else {
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            entries[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
        }
    }

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 Qt 界面翻译字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 Hammer 界面与菜单核心翻译字典 (JSONC 格式)
// ==============================================================================
//
// 【使用指南】
// - 格式为标准的键值对："英文原词": "中文翻译"
// - 支持 // 单行注释 与 /* 块注释 */
//
// 【可翻译内容】
// 1. 主菜单与二级菜单项
// 2. 工具栏按钮与悬停提示
// 3. 属性面板属性名
// 4. 树形视图、列表与下拉框文本
// 5. 弹窗对话框与按钮文本
//
// 【快捷键自动适配】
// - 核心注入模块已内置动态快捷键识别与拆分引擎。
// - 遇到如 'Clipping Tool [Shift+X]'、'Undo (Ctrl+Z)'、'Save\tCtrl+S'、'Save As...'、'Name:' 等文本，
//   只需翻译基础英文（如 "Clipping Tool": "剪切工具"），快捷键后缀会被自动保留与拼接，无需手动输入快捷键！
// ==============================================================================
)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 Qt 界面翻译字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 qt_translations.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}
