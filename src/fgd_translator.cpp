#include "fgd_translator.h"
#include "dictionary_compiler.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
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

std::string FgdTranslator::TranslateLine(
    const std::string& line,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass,
    std::string& inOutPendingClassDesc
) {
    std::string lineEnding = "";
    std::string raw = line;
    if (raw.length() >= 2 && raw.substr(raw.length() - 2) == "\r\n") {
        lineEnding = "\r\n";
        raw = raw.substr(0, raw.length() - 2);
    } else if (!raw.empty() && raw.back() == '\n') {
        lineEnding = "\n";
        raw = raw.substr(0, raw.length() - 1);
    }

    // 分离代码与单行注释 //（避免匹配引号内的 //）
    std::string comment = "";
    std::string code = raw;
    bool inQuote = false;
    for (size_t i = 0; i < raw.length(); ++i) {
        char ch = raw[i];
        if (ch == '"') {
            inQuote = !inQuote;
        } else if (ch == '/' && !inQuote && i + 1 < raw.length() && raw[i + 1] == '/') {
            comment = raw.substr(i);
            code = raw.substr(0, i);
            break;
        }
    }

    auto getTrans = [&](const std::string& text) -> std::string {
        std::string trimmed = TrimString(text);
        auto it = dict.find(trimmed);
        if (it != dict.end() && !it->second.empty()) {
            return it->second;
        }
        return text;
    };

    // 0. 检查是否正在等待上一行的跨行类说明 (如 @PointClass ... = classname :\n  "Desc")
    if (!inOutPendingClassDesc.empty()) {
        std::string trimmed = TrimString(code);
        if (trimmed.empty()) {
            // 空行保持状态
            return code + comment + lineEnding;
        }

        std::regex standaloneStrRegex(R"re(^\s*"([^"]*)"\s*$)re");
        std::smatch strMatch;
        if (std::regex_match(code, strMatch, standaloneStrRegex)) {
            std::string origDesc = strMatch[1].str();
            std::string finalDesc = "";
            auto itDesc = overrideData.classDescriptions.find(inOutPendingClassDesc);
            if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
                finalDesc = itDesc->second;
            } else {
                finalDesc = getTrans(origDesc);
            }
            inOutPendingClassDesc = ""; // 已完成匹配替换

            size_t lead = code.find_first_not_of(" \t");
            std::string indent = (lead != std::string::npos) ? code.substr(0, lead) : "\t";
            return indent + "\"" + finalDesc + "\"" + comment + lineEnding;
        } else if (trimmed.front() == '[') {
            // 原类定义没有独立描述行，直接遇到了 [
            auto itDesc = overrideData.classDescriptions.find(inOutPendingClassDesc);
            std::string cls = inOutPendingClassDesc;
            inOutPendingClassDesc = "";
            if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
                size_t lead = code.find_first_not_of(" \t");
                std::string indent = (lead != std::string::npos) ? code.substr(0, lead) : "";
                return indent + "\t\"" + itDesc->second + "\"\n" + code + comment + lineEnding;
            }
        } else {
            inOutPendingClassDesc = "";
        }
    }

    // 1. 实体类定义与说明跟踪：
    // 1.1 同行完整定义：= classname : "Description"
    std::regex classFullRegex(R"re((=\s*)([a-zA-Z0-9_]+)\s*:\s*"([^"]*)")re");
    std::smatch classFullMatch;
    if (std::regex_search(code, classFullMatch, classFullRegex)) {
        std::string prefix = code.substr(0, classFullMatch.position(0));
        std::string eq = classFullMatch[1].str();
        std::string className = classFullMatch[2].str();
        std::string origDesc = classFullMatch[3].str();
        std::string suffix = code.substr(classFullMatch.position(0) + classFullMatch.length(0));

        inOutCurrentClass = className;
        inOutPendingClassDesc = "";

        std::string finalDesc = "";
        auto itDesc = overrideData.classDescriptions.find(className);
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            finalDesc = itDesc->second;
        } else {
            finalDesc = getTrans(origDesc);
        }

        return prefix + eq + className + " : \"" + finalDesc + "\"" + suffix + comment + lineEnding;
    }

    // 1.2 跨行或末尾冒号/无描述类定义：= classname : 或 = classname (行尾)
    std::regex classHeaderRegex(R"re((=\s*)([a-zA-Z0-9_]+)(\s*:\s*|\s*)$)re");
    std::smatch classHeaderMatch;
    if (std::regex_search(code, classHeaderMatch, classHeaderRegex)) {
        std::string className = classHeaderMatch[2].str();
        inOutCurrentClass = className;
        inOutPendingClassDesc = className;
        return code + comment + lineEnding;
    }

    // 1.3 同行紧接中括号的无描述类定义：= classname [
    std::regex classBracketRegex(R"re((=\s*)([a-zA-Z0-9_]+)\s*(\[.*)$)re");
    std::smatch classBracketMatch;
    if (std::regex_search(code, classBracketMatch, classBracketRegex)) {
        std::string prefix = code.substr(0, classBracketMatch.position(0));
        std::string eq = classBracketMatch[1].str();
        std::string className = classBracketMatch[2].str();
        std::string bracketTail = classBracketMatch[3].str();

        inOutCurrentClass = className;
        inOutPendingClassDesc = "";

        auto itDesc = overrideData.classDescriptions.find(className);
        if (itDesc != overrideData.classDescriptions.end() && !itDesc->second.empty()) {
            return prefix + eq + className + " : \"" + itDesc->second + "\" " + bracketTail + comment + lineEnding;
        }
        return code + comment + lineEnding;
    }

    // 2. 输入 / 输出描述：input/output Name(type) [ : "Description" ]
    std::regex ioRegex(R"re(^(\s*(?:input|output)\s+)([a-zA-Z0-9_]+)(\s*\([^)]*\))(.*)$)re");
    std::smatch ioMatch;
    if (std::regex_match(code, ioMatch, ioRegex)) {
        std::string ioPrefix = ioMatch[1].str();
        std::string ioName = ioMatch[2].str();
        std::string ioParam = ioMatch[3].str();
        std::string ioRest = ioMatch[4].str();

        // 如果参数类型包含 api (如 (api))，Valve FGD 语法不支持冒号和描述，直接原样保留
        if (ioParam.find("api") != std::string::npos) {
            return code + comment + lineEnding;
        }

        std::string overrideIoDesc = "";
        // 优先在当前类中查找，其次在全局 ioOverrides，最后在 globalProperties
        if (!inOutCurrentClass.empty()) {
            auto itCls = overrideData.classProperties.find(inOutCurrentClass);
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

        std::regex descQuoteRegex(R"re(:\s*"([^"]*)")re");
        std::smatch descQuoteMatch;
        if (std::regex_search(ioRest, descQuoteMatch, descQuoteRegex)) {
            std::string origDesc = descQuoteMatch[1].str();
            std::string finalDesc = overrideIoDesc.empty() ? getTrans(origDesc) : overrideIoDesc;
            std::string replacedRest = ioRest.substr(0, descQuoteMatch.position(0)) + ": \"" + finalDesc + "\"" + ioRest.substr(descQuoteMatch.position(0) + descQuoteMatch.length(0));
            return ioPrefix + ioName + ioParam + replacedRest + comment + lineEnding;
        } else if (!overrideIoDesc.empty()) {
            // 原行无描述，追加描述
            return ioPrefix + ioName + ioParam + " : \"" + overrideIoDesc + "\"" + comment + lineEnding;
        }
        return code + comment + lineEnding;
    }

    // 3. 按钮/元数据说明：desc = "Description"
    std::regex descRegex(R"re(\bdesc\s*=\s*"([^"]*)")re");
    std::smatch descMatch;
    if (std::regex_search(code, descMatch, descRegex)) {
        std::string prefix = code.substr(0, descMatch.position(0));
        std::string desc = descMatch[1].str();
        std::string suffix = code.substr(descMatch.position(0) + descMatch.length(0));
        std::string tr = getTrans(desc);
        return prefix + "desc = \"" + tr + "\"" + suffix + comment + lineEnding;
    }

    // 3.1 属性跨行定义块内部独立组声明：group = "GroupName" (如 base.fgd useLocalOffset 属性内部)
    std::regex standaloneGroupRegex(R"re(^\s*group(\s*=\s*)"([^"]*)")re");
    std::smatch standMatch;
    if (std::regex_search(code, standMatch, standaloneGroupRegex)) {
        std::string prefix = code.substr(0, standMatch.position(0));
        std::string eq = standMatch[1].str();
        std::string gName = standMatch[2].str();
        std::string suffix = code.substr(standMatch.position(0) + standMatch.length(0));
        std::string trGroup = getTrans(gName);
        return prefix + "group" + eq + "\"" + trGroup + "\"" + suffix + comment + lineEnding;
    }

    // 4. 属性定义：prop(type) [attrs] {attrs} : "Display Name" [ : default [ : "Description" ]] [ = [ choices ] ]
    std::string propHead, propKey, propType, rest;
    if (ExtractPropertyHeader(code, propHead, propKey, propType, rest)) {
        // 翻译属性行内部的属性组：[ group="Render Properties" ] 或 { group="Style" } 等
        std::regex groupRegex(R"re(\bgroup(\s*=\s*)"([^"]*)")re");
        std::smatch groupMatch;
        std::string newPropHead = "";
        std::string searchHead = propHead;
        while (std::regex_search(searchHead, groupMatch, groupRegex)) {
            std::string prefix = searchHead.substr(0, groupMatch.position(0));
            std::string eq = groupMatch[1].str();
            std::string groupName = groupMatch[2].str();
            std::string trGroup = getTrans(groupName);
            newPropHead += prefix + "group" + eq + "\"" + trGroup + "\"";
            searchHead = searchHead.substr(groupMatch.position(0) + groupMatch.length(0));
        }
        if (!newPropHead.empty()) {
            newPropHead += searchHead;
            propHead = newPropHead;
        }

        // 查找该属性是否有 override
        const FgdPropertyOverride* propOverride = nullptr;
        if (!inOutCurrentClass.empty()) {
            auto itCls = overrideData.classProperties.find(inOutCurrentClass);
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

        // 分离 choices 尾部 (如果有 = )
        std::string choicesTail = "";
        std::string propBody = rest;
        bool inQ = false;
        for (size_t i = 0; i < rest.length(); ++i) {
            char c = rest[i];
            if (c == '"') {
                inQ = !inQ;
            } else if (c == '=' && !inQ) {
                propBody = rest.substr(0, i);
                choicesTail = rest.substr(i);
                break;
            }
        }

        // 如果属性没有冒号定义（例如跨行定义的属性头部 useLocalOffset(boolean) 或 spawnflags(flags) [ group="Physics Properties" ] =）
        if (TrimString(propBody).empty()) {
            return propHead + rest + comment + lineEnding;
        }

        // 解析 propBody 中的冒号分隔项
        std::vector<std::string> parts;
        std::string curr = "";
        inQ = false;
        bool hasFirstColon = false;

        for (size_t i = 0; i < propBody.length(); ++i) {
            char c = propBody[i];
            if (c == '"') {
                inQ = !inQ;
                curr.push_back(c);
            } else if (c == ':' && !inQ) {
                if (!hasFirstColon) {
                    hasFirstColon = true;
                    curr.clear();
                } else {
                    parts.push_back(curr);
                    curr.clear();
                }
            } else {
                curr.push_back(c);
            }
        }
        if (hasFirstColon) {
            parts.push_back(curr);
        }

        if (!hasFirstColon) {
            return code + comment + lineEnding;
        }

        // 处理显示名称 (parts[0])
        if (!parts.empty()) {
            std::string dispPart = parts[0];
            std::string dispStrip = TrimString(dispPart);
            if (propOverride && !propOverride->displayName.empty()) {
                parts[0] = " \"" + propOverride->displayName + "\"";
            } else if (dispStrip.length() >= 2 && dispStrip.front() == '"' && dispStrip.back() == '"') {
                std::string val = dispStrip.substr(1, dispStrip.length() - 2);
                std::string tr = getTrans(val);
                size_t pos = dispPart.find("\"" + val + "\"");
                if (pos != std::string::npos) {
                    dispPart.replace(pos, val.length() + 2, "\"" + tr + "\"");
                }
                parts[0] = dispPart;
            }

            // 处理描述与默认值
            if (parts.size() == 1) {
                // 仅有显示名
                if (propOverride && !propOverride->description.empty()) {
                    parts.push_back(" \"\"");
                    parts.push_back(" \"" + propOverride->description + "\"");
                }
            } else if (parts.size() == 2) {
                // 有显示名 + 默认值，无描述
                if (propOverride && !propOverride->description.empty()) {
                    parts.push_back(" \"" + propOverride->description + "\"");
                }
            } else {
                // 有显示名 + 默认值 + 描述 (parts.size() >= 3)
                std::string descPart = parts[2];
                std::string descStrip = TrimString(descPart);

                if (propOverride && !propOverride->description.empty()) {
                    parts[2] = " \"" + propOverride->description + "\"";
                } else if (descStrip.length() >= 2 && descStrip.front() == '"' && descStrip.back() == '"') {
                    std::string val = descStrip.substr(1, descStrip.length() - 2);
                    std::string tr = getTrans(val);
                    size_t pos = descPart.find("\"" + val + "\"");
                    if (pos != std::string::npos) {
                        descPart.replace(pos, val.length() + 2, "\"" + tr + "\"");
                    }
                    parts[2] = descPart;
                }
            }
        }

        std::string newPropBody = " :";
        for (size_t idx = 0; idx < parts.size(); ++idx) {
            if (idx > 0) newPropBody += ":";
            newPropBody += parts[idx];
        }

        return propHead + newPropBody + choicesTail + comment + lineEnding;
    }

    // 4.1 属性跨行定义的续行（以冒号开头，例如 : "Use Local Transform" : 0 : "..."）
    std::string trimmedCode = TrimString(code);
    if (!trimmedCode.empty() && trimmedCode.front() == ':') {
        std::vector<std::string> parts;
        std::string curr = "";
        bool inQ = false;
        bool hasFirstColon = false;
        for (size_t k = 0; k < code.length(); ++k) {
            char c = code[k];
            if (c == '"') {
                inQ = !inQ;
                curr.push_back(c);
            } else if (c == ':' && !inQ) {
                if (!hasFirstColon) {
                    hasFirstColon = true;
                    curr.clear();
                } else {
                    parts.push_back(curr);
                    curr.clear();
                }
            } else {
                curr.push_back(c);
            }
        }
        if (hasFirstColon) {
            parts.push_back(curr);
        }

        if (!parts.empty()) {
            // parts[0] display name
            std::string dStrip = TrimString(parts[0]);
            if (dStrip.length() >= 2 && dStrip.front() == '"' && dStrip.back() == '"') {
                std::string val = dStrip.substr(1, dStrip.length() - 2);
                std::string tr = getTrans(val);
                size_t pos = parts[0].find("\"" + val + "\"");
                if (pos != std::string::npos) {
                    parts[0].replace(pos, val.length() + 2, "\"" + tr + "\"");
                }
            }
            // parts[2] description
            if (parts.size() >= 3) {
                std::string descStrip = TrimString(parts[2]);
                if (descStrip.length() >= 2 && descStrip.front() == '"' && descStrip.back() == '"') {
                    std::string val = descStrip.substr(1, descStrip.length() - 2);
                    std::string tr = getTrans(val);
                    size_t pos = parts[2].find("\"" + val + "\"");
                    if (pos != std::string::npos) {
                        parts[2].replace(pos, val.length() + 2, "\"" + tr + "\"");
                    }
                }
            }

            size_t leadPos = code.find_first_not_of(" \t");
            std::string leadingSpaces = (leadPos != std::string::npos) ? code.substr(0, leadPos) : "";
            std::string reassembled = leadingSpaces + ":";
            for (size_t k = 0; k < parts.size(); ++k) {
                if (k > 0) reassembled += ":";
                reassembled += parts[k];
            }
            return reassembled + comment + lineEnding;
        }
    }

    // 5. 选项列表 (Choices / Flags)："0" : "Enabled" : "Option Desc" 或 1 : "Passable" : 0
    if (!trimmedCode.empty() && trimmedCode.front() != '@' && code.find(':') != std::string::npos) {
        std::regex choiceRegex(R"re(^(\s*(?:"[^"]*"|[-0-9a-zA-Z_]+)\s*:\s*)"([^"]*)"(.*)$)re");
        std::smatch choiceMatch;
        if (std::regex_match(code, choiceMatch, choiceRegex)) {
            std::string cPrefix = choiceMatch[1].str();
            std::string display = choiceMatch[2].str();
            std::string tail = choiceMatch[3].str();
            std::string trDisplay = getTrans(display);

            // 替换 tail 中的附加选项描述 (如果存在)
            if (tail.find(':') != std::string::npos) {
                std::regex tailDescRegex(R"re((:[^"]*)"([^"]*)")re");
                std::smatch tailMatch;
                if (std::regex_search(tail, tailMatch, tailDescRegex)) {
                    std::string tPre = tailMatch[1].str();
                    std::string tDesc = tailMatch[2].str();
                    std::string trD = getTrans(tDesc);
                    std::string replacedTail = tail.substr(0, tailMatch.position(0)) + tPre + "\"" + trD + "\"" + tail.substr(tailMatch.position(0) + tailMatch.length(0));
                    tail = replacedTail;
                }
            }

            return cPrefix + "\"" + trDisplay + "\"" + tail + comment + lineEnding;
        }
    }

    return code + comment + lineEnding;
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

    std::ofstream outFile(dstPath, std::ios::binary);
    if (!outFile.is_open()) return false;

    std::string currentClassName = "";
    std::string pendingClassDesc = "";
    for (const auto& l : lines) {
        std::string transLine = TranslateLine(l, dict, overrideData, currentClassName, pendingClassDesc);
        outFile.write(transLine.data(), transLine.length());
    }
    outFile.close();
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

    try {
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file() && entry.path().extension() == L".fgd") {
                fs::path relPath = fs::relative(entry.path(), backupRoot);
                fs::path transDst = fs::path(translationsDir) / relPath;
                fs::path cs2Dst = fs::path(cs2Root) / relPath;

                // 翻译至 translationsDir
                if (!TranslateFile(entry.path().wstring(), transDst.wstring(), dict, overrideData)) {
                    outError = L"翻译 FGD 失败: " + entry.path().wstring();
                    return false;
                }

                // 覆盖复制到 CS2 对应目录
                fs::create_directories(cs2Dst.parent_path());
                fs::copy_file(transDst, cs2Dst, fs::copy_options::overwrite_existing);

                outProcessedFiles.push_back(relPath.wstring());
            }
        }
        return !outProcessedFiles.empty();
    } catch (const std::exception& e) {
        outError = L"处理 FGD 异常: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

static std::string EscapeJsonString(const std::string& str) {
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

    std::ofstream out(jsonPath, std::ios::binary);
    if (!out.is_open()) {
        outNotice = L"无法创建 FGD 翻译字典文件: " + jsonPath;
        return false;
    }

    out << "// ==============================================================================\n";
    out << "// CS2 Hammer FGD 实体定义翻译字典 (JSONC 格式)\n";
    out << "// ==============================================================================\n";
    out << "// \n";
    out << "// 【使用指南】\n";
    out << "// - 格式为标准的键值对：\"英文原词\": \"中文翻译\"\n";
    out << "// - 支持 // 单行注释 与 /* 块注释 */\n";
    out << "// \n";
    out << "// 【可翻译内容】\n";
    out << "// 1. 实体类说明 (@PointClass ... = name : \"Description\")\n";
    out << "// 2. 属性显示名称 (targetname : \"Name\" : : \"...\")\n";
    out << "// 3. 属性悬停描述 (... : \"Name\" : default : \"Description\")\n";
    out << "// 4. 选项与标记 (\"0\" : \"Enabled\" : \"Option Desc\")\n";
    out << "// 5. 输入输出 (input Kill : \"Description\")\n";
    out << "// 6. 绑定按钮说明 (desc = \"Description\")\n";
    out << "// \n";
    out << "// 【格式与安全】\n";
    out << "// - 所有底层 RAW 标识符（如 targetname、angles、thinkalways、io 类型与默认值）引擎会自动保护，请仅翻译双引号内的文本。\n";
    out << "// - 修改保存后重新在启动器点击启动即可自动重新编译部署。\n";
    out << "// ==============================================================================\n";
    out << "{\n";

    if (loaded.empty()) {
        out << "  \"Omnidirectional point light\": \"全向点光源\",\n";
        out << "  \"Light Source\": \"光源\",\n";
        out << "  \"Name\": \"名称\",\n";
        out << "  \"The name that other entities use to refer to this entity.\": \"其他实体用于引用此实体的名称。\",\n";
        out << "  \"Removes this entity from the world.\": \"从世界中移除此实体。\",\n";
        out << "  \"Enabled\": \"已启用\",\n";
        out << "  \"Disabled\": \"已禁用\"\n";
    } else {
        std::vector<std::pair<std::string, std::string>> validEntries;
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            validEntries.push_back(kv);
        }
        for (size_t i = 0; i < validEntries.size(); ++i) {
            out << "  \"" << EscapeJsonString(validEntries[i].first) << "\": \"" << EscapeJsonString(validEntries[i].second) << "\"";
            if (i + 1 < validEntries.size()) {
                out << ",\n";
            } else {
                out << "\n";
            }
        }
    }
    out << "}\n";
    out.close();

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

    std::ofstream out(jsonPath, std::ios::binary);
    if (!out.is_open()) {
        outNotice = L"无法创建 FGD 覆盖字典文件: " + jsonPath;
        return false;
    }

    out << "{\n";
    out << "  // ==============================================================================\n";
    out << "  // CS2 FGD 实体键值描述补充与覆盖字典 (JSONC 格式)\n";
    out << "  // ==============================================================================\n";
    out << "  //\n";
    out << "  // 【作用说明】\n";
    out << "  // - 本文件用于针对 FGD 中特定的【属性名 (Key)】、【实体类名 (Class)】或【输入输出 (I/O)】\n";
    out << "  //   补充缺失的说明描述，或覆盖原版已有描述。\n";
    out << "  // - 与 fgd_translations.jsonc 互为补充：\n";
    out << "  //   * fgd_translations.jsonc: 负责已有英文字符串 -> 中文翻译。\n";
    out << "  //   * fgd_override.jsonc: 负责针对特定键名无描述时【自动新增描述】或【强制覆盖描述】。\n";
    out << "  //\n";
    out << "  // 【支持格式】\n";
    out << "  // 1. 全局属性描述补充 (properties): \"属性键名\": \"描述文本\" 或 \"属性键名\": { \"description\": \"...\", \"displayName\": \"...\" }\n";
    out << "  // 2. 输入输出说明补充 (io): \"IOName\": \"说明文本\"\n";
    out << "  // 3. 实体类说明补充 (classes): \"classname\": \"说明文本\" 或 \"classname\": { \"description\": \"...\", \"properties\": { ... } }\n";
    out << "  // 4. 顶层快速简写: \"键名\": \"描述文本\"\n";
    out << "  // ==============================================================================\n\n";

    out << "  \"properties\": {\n";
    out << "    \"bodygroups\": \"设置模型的子部件与可选身体部件网格组合。\",\n";
    out << "    \"vscripts\": \"实体生成后自动加载并执行的 VScript 脚本文件列表。\",\n";
    out << "    \"clientSideEntity\": \"是否仅在客户端创建并运行此实体（不向服务器同步）。\",\n";
    out << "    \"TeamNum\": \"所属队伍编号（0: 任意/无队伍, 2: T 阵营, 3: CT 阵营）。\",\n";
    out << "    \"box_mins\": \"包围盒/光照探针体积的最小边界坐标 (X Y Z)。\",\n";
    out << "    \"box_maxs\": \"包围盒/光照探针体积的最大边界坐标 (X Y Z)。\",\n";
    out << "    \"flood_fill\": \"忽略玩家不可达的空间，加快光照烘焙速度并节省显存。\",\n";
    out << "    \"voxelize\": \"忽略已体素化的实体空间，优化光照探针计算。\",\n";
    out << "    \"light_probe_volume_from_cubemap\": \"是否使用立方体贴图 (Cubemap) 计算漫反射光照探针。\",\n";
    out << "    \"moveable\": \"是否允许在游戏运行时移动、绑定父级、启用或禁用此对象。\",\n";
    out << "    \"edge_fade_dist\": \"反射或光照边界平滑淡出过渡距离。\",\n";
    out << "    \"max_lightmap_resolution\": \"限制此对象在烘焙时的最大光照贴图分辨率（0 为默认）。\"\n";
    out << "  },\n\n";

    out << "  \"io\": {\n";
    out << "    \"SetParent\": \"设置该实体的父级对象。\",\n";
    out << "    \"ClearParent\": \"解除与父级实体的挂载绑定关系，使其独立运动。\",\n";
    out << "    \"FollowEntity\": \"骨骼合并 (Bone Merge) 附加到目标实体。\",\n";
    out << "    \"Kill\": \"从世界中移除此实体并释放资源。\",\n";
    out << "    \"SetHealth\": \"设置该实体的当前生命值。\"\n";
    out << "  },\n\n";

    out << "  \"classes\": {\n";
    out << "    \"info_node\": \"AI 地面导航节点，供 NPC 寻路与路径规划计算使用。\",\n";
    out << "    \"csm_fov_override\": \"级联阴影贴图 (CSM) 视场角覆盖控制器。\",\n";
    out << "    \"env_cubemap\": {\n";
    out << "      \"description\": \"用于采样环境间接镜面反射的高动态范围立方体贴图实体。\",\n";
    out << "      \"properties\": {\n";
    out << "        \"influenceradius\": \"当前立方体贴图的生效影响半径（单位：英寸）。\"\n";
    out << "      }\n";
    out << "    }\n";
    out << "  }\n";
    out << "}\n";
    out.close();

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

    std::ofstream out(jsonPath, std::ios::binary);
    if (!out.is_open()) {
        outNotice = L"无法创建 Qt 界面翻译字典文件: " + jsonPath;
        return false;
    }

    out << "// ==============================================================================\n";
    out << "// CS2 Hammer 界面与菜单核心翻译字典 (JSONC 格式)\n";
    out << "// ==============================================================================\n";
    out << "// \n";
    out << "// 【使用指南】\n";
    out << "// - 格式为标准的键值对：\"英文原词\": \"中文翻译\"\n";
    out << "// - 支持 // 单行注释 与 /* 块注释 */\n";
    out << "// \n";
    out << "// 【可翻译内容】\n";
    out << "// 1. 主菜单与二级菜单项\n";
    out << "// 2. 工具栏按钮与悬停提示\n";
    out << "// 3. 属性面板属性名\n";
    out << "// 4. 树形视图、列表与下拉框文本\n";
    out << "// 5. 弹窗对话框与按钮文本\n";
    out << "// \n";
    out << "// 【快捷键自动适配】\n";
    out << "// - 核心注入模块已内置动态快捷键识别与拆分引擎。\n";
    out << "// - 遇到如 'Clipping Tool [Shift+X]'、'Undo (Ctrl+Z)'、'Save\\tCtrl+S'、'Save As...'、'Name:' 等文本，\n";
    out << "//   只需翻译基础英文（如 \"Clipping Tool\": \"剪切工具\"），快捷键后缀会被自动保留与拼接，无需手动输入快捷键！\n";
    out << "// ==============================================================================\n";
    out << "{\n";

    if (loaded.empty()) {
        out << "  \"File\": \"文件\",\n";
        out << "  \"Edit\": \"编辑\",\n";
        out << "  \"View\": \"视图\",\n";
        out << "  \"Tools\": \"工具\",\n";
        out << "  \"New\": \"新建\",\n";
        out << "  \"Open\": \"打开\",\n";
        out << "  \"Save\": \"保存\",\n";
        out << "  \"Save As...\": \"另存为...\",\n";
        out << "  \"Close\": \"关闭\",\n";
        out << "  \"Exit\": \"退出\",\n";
        out << "  \"Undo\": \"撤销\",\n";
        out << "  \"Redo\": \"重做\",\n";
        out << "  \"Clipping Tool\": \"剪切工具\",\n";
        out << "  \"Transform Locked\": \"变换锁定\",\n";
        out << "  \"Pinned To\": \"固定至\",\n";
        out << "  \"Force Hidden\": \"强制隐藏\"\n";
    } else {
        std::vector<std::pair<std::string, std::string>> validEntries;
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            validEntries.push_back(kv);
        }
        for (size_t i = 0; i < validEntries.size(); ++i) {
            out << "  \"" << EscapeJsonString(validEntries[i].first) << "\": \"" << EscapeJsonString(validEntries[i].second) << "\"";
            if (i + 1 < validEntries.size()) {
                out << ",\n";
            } else {
                out << "\n";
            }
        }
    }
    out << "}\n";
    out.close();

    outNotice = L"已自动生成 qt_translations.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}
