#include "core/dictionary_text_updater.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace dict_updater {

std::string EscapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string UnescapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[++i];
            switch (next) {
                case '\"': out += '\"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += next; break;
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

namespace {

struct FileLines {
    bool hasBom{false};
    std::vector<std::string> lines;
};

FileLines ReadLines(const std::wstring& filePath, std::string& err) {
    FileLines res;
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        err = "无法打开词典文件进行读取";
        return res;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    size_t offset = 0;
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        res.hasBom = true;
        offset = 3;
    }

    std::string line;
    for (size_t i = offset; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                ++i;
            }
            res.lines.push_back(line);
            line.clear();
        } else if (c == '\n') {
            res.lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    res.lines.push_back(line);
    return res;
}

#include <windows.h>

// 检查索引 idx 处的双引号是否被转义（考虑前面连续反斜杠奇偶性）
bool IsQuoteEscaped(const std::string& s, size_t idx) {
    if (idx == 0 || s[idx] != '\"') return false;
    size_t slashCount = 0;
    size_t i = idx;
    while (i > 0 && s[i - 1] == '\\') {
        slashCount++;
        i--;
    }
    return (slashCount % 2) != 0;
}

// 在字符串中查找不在双引号内的 // 注释起始位置
size_t FindCommentOutsideStrings(const std::string& line) {
    bool inString = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\"' && !IsQuoteEscaped(line, i)) {
            inString = !inString;
        } else if (!inString && line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            return i;
        }
    }
    return std::string::npos;
}

bool WriteLines(const std::wstring& filePath, const FileLines& fl, std::string& err) {
    std::wstring tmpPath = filePath + L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            err = "无法打开临时词典文件进行写入";
            return false;
        }

        if (fl.hasBom) {
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            file.write(reinterpret_cast<const char*>(bom), 3);
        }

        for (size_t i = 0; i < fl.lines.size(); ++i) {
            file << fl.lines[i];
            if (i + 1 < fl.lines.size()) {
                file << "\r\n";
            }
        }
        file.flush();
        if (!file.good()) {
            file.close();
            DeleteFileW(tmpPath.c_str());
            err = "写入临时词典文件失败";
            return false;
        }
        file.close();
    }

    if (!MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        err = "原子替换词典文件失败 (错误码: " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

// 查找模块作用域的行范围 [startLine, endLine]
bool FindModuleBounds(const std::vector<std::string>& lines, const std::string& module, size_t& outStart, size_t& outEnd) {
    std::string targetMod = "\"" + module + "\"";
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        size_t pos = line.find(targetMod);
        if (pos != std::string::npos) {
            size_t colon = line.find(':', pos + targetMod.size());
            if (colon != std::string::npos) {
                size_t brace = line.find('{', colon);
                if (brace != std::string::npos) {
                    // 找到起始行，开始括号深度匹配
                    outStart = i;
                    int depth = 0;
                    for (size_t j = i; j < lines.size(); ++j) {
                        bool inString = false;
                        for (size_t k = 0; k < lines[j].size(); ++k) {
                            char c = lines[j][k];
                            if (c == '\"' && !IsQuoteEscaped(lines[j], k)) {
                                inString = !inString;
                            } else if (!inString) {
                                if (c == '/' && k + 1 < lines[j].size() && lines[j][k + 1] == '/') {
                                    break; // 行注释忽略
                                }
                                if (c == '{') depth++;
                                else if (c == '}') {
                                    depth--;
                                    if (depth == 0) {
                                        outEnd = j;
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// 检查行是否是纯注释或空行
bool IsEmptyOrCommentLine(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size()) return true;
    if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') return true;
    return false;
}

// 在指定行范围内查找包含 key 的键值对行
bool FindKeyLine(const std::vector<std::string>& lines, size_t startLine, size_t endLine, const std::string& escapedKey, size_t& outLine) {
    std::string targetKey = "\"" + escapedKey + "\"";
    for (size_t i = startLine; i <= endLine && i < lines.size(); ++i) {
        const auto& line = lines[i];
        size_t keyPos = line.find(targetKey);
        if (keyPos != std::string::npos) {
            // 确保不是注释内部
            size_t commentPos = FindCommentOutsideStrings(line);
            if (commentPos != std::string::npos && commentPos < keyPos) {
                continue;
            }
            // 确保后面紧跟冒号
            size_t colonPos = line.find(':', keyPos + targetKey.size());
            if (colonPos != std::string::npos) {
                outLine = i;
                return true;
            }
        }
    }
    return false;
}

// 移除行尾有效字符处的逗号（保留行末注释与空白）
void RemoveTrailingComma(std::string& line) {
    size_t commentPos = FindCommentOutsideStrings(line);
    std::string commentPart = (commentPos != std::string::npos) ? line.substr(commentPos) : "";
    std::string codePart = (commentPos != std::string::npos) ? line.substr(0, commentPos) : line;

    // 从后往前找 codePart 的最后一个非空白字符
    int lastIdx = static_cast<int>(codePart.size()) - 1;
    while (lastIdx >= 0 && (codePart[lastIdx] == ' ' || codePart[lastIdx] == '\t')) {
        lastIdx--;
    }
    if (lastIdx >= 0 && codePart[lastIdx] == ',') {
        codePart.erase(lastIdx, 1);
    }

    line = codePart + commentPart;
}

// 确保行尾有效字符处带有逗号（保留行末注释与空白）
void EnsureTrailingComma(std::string& line) {
    size_t commentPos = FindCommentOutsideStrings(line);
    std::string commentPart = (commentPos != std::string::npos) ? line.substr(commentPos) : "";
    std::string codePart = (commentPos != std::string::npos) ? line.substr(0, commentPos) : line;

    int lastIdx = static_cast<int>(codePart.size()) - 1;
    while (lastIdx >= 0 && (codePart[lastIdx] == ' ' || codePart[lastIdx] == '\t')) {
        lastIdx--;
    }
    if (lastIdx >= 0 && codePart[lastIdx] != ',') {
        codePart.insert(lastIdx + 1, ",");
    }
    line = codePart + commentPart;
}

} // namespace

UpdateResult UpdateEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& newValue
) {
    std::string err;
    FileLines fl = ReadLines(filePath, err);
    if (!err.empty()) return {false, err};

    std::string escapedKey = EscapeJsonString(key);
    std::string escapedVal = EscapeJsonString(newValue);

    size_t start = 0;
    size_t end = fl.lines.empty() ? 0 : fl.lines.size() - 1;

    if (!module.empty()) {
        if (!FindModuleBounds(fl.lines, module, start, end)) {
            return {false, "未在词典中找到模块作用域 [" + module + "]"};
        }
    }

    size_t keyLine = 0;
    if (!FindKeyLine(fl.lines, start, end, escapedKey, keyLine)) {
        return {false, "未在词典中找到键: \"" + key + "\""};
    }

    // 就地仅替换 value 双引号内的内容
    std::string& line = fl.lines[keyLine];
    std::string targetKey = "\"" + escapedKey + "\"";
    size_t keyPos = line.find(targetKey);
    size_t colonPos = line.find(':', keyPos + targetKey.size());
    if (colonPos == std::string::npos) {
        return {false, "解析键值对行冒号失败: " + line};
    }

    size_t valQuoteStart = line.find('\"', colonPos + 1);
    if (valQuoteStart == std::string::npos) {
        return {false, "解析键值对行值引号失败: " + line};
    }

    // 寻找匹配的闭合引号（跳过转义 \"，考虑双反斜杠）
    size_t valQuoteEnd = std::string::npos;
    for (size_t i = valQuoteStart + 1; i < line.size(); ++i) {
        if (line[i] == '\"' && !IsQuoteEscaped(line, i)) {
            valQuoteEnd = i;
            break;
        }
    }

    if (valQuoteEnd == std::string::npos) {
        return {false, "未找到闭合引号: " + line};
    }

    std::string newLine = line.substr(0, valQuoteStart + 1) + escapedVal + line.substr(valQuoteEnd);
    fl.lines[keyLine] = newLine;

    if (!WriteLines(filePath, fl, err)) {
        return {false, err};
    }

    return {true, ""};
}

UpdateResult AddEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& value
) {
    std::string err;
    FileLines fl = ReadLines(filePath, err);
    if (!err.empty()) return {false, err};

    std::string escapedKey = EscapeJsonString(key);
    std::string escapedVal = EscapeJsonString(value);

    // 1. 若指定了模块分组
    if (!module.empty()) {
        size_t modStart = 0;
        size_t modEnd = 0;
        if (FindModuleBounds(fl.lines, module, modStart, modEnd)) {
            // 模块已存在，检查是否已存在同名 key
            size_t existLine = 0;
            if (FindKeyLine(fl.lines, modStart, modEnd, escapedKey, existLine)) {
                // 已存在则转为更新
                return UpdateEntry(filePath, module, key, value);
            }

            // 在 modEnd 闭合括号前找最后一个有效非空行
            int lastValidLine = -1;
            for (int i = static_cast<int>(modEnd) - 1; i >= static_cast<int>(modStart); --i) {
                if (!IsEmptyOrCommentLine(fl.lines[i])) {
                    lastValidLine = i;
                    break;
                }
            }

            if (lastValidLine >= 0) {
                EnsureTrailingComma(fl.lines[lastValidLine]);
            }

            // 获取前一行的缩进
            std::string indent = "    ";
            if (lastValidLine >= 0) {
                const auto& prev = fl.lines[lastValidLine];
                size_t sp = 0;
                while (sp < prev.size() && (prev[sp] == ' ' || prev[sp] == '\t')) sp++;
                if (sp > 0) indent = prev.substr(0, sp);
            }

            std::string newEntryLine = indent + "\"" + escapedKey + "\": \"" + escapedVal + "\"";
            fl.lines.insert(fl.lines.begin() + modEnd, newEntryLine);
        } else {
            // 模块不存在：在根闭合 } 前追加新模块块
            int rootCloseLine = -1;
            for (int i = static_cast<int>(fl.lines.size()) - 1; i >= 0; --i) {
                if (fl.lines[i].find('}') != std::string::npos) {
                    rootCloseLine = i;
                    break;
                }
            }
            if (rootCloseLine < 0) {
                return {false, "未找到 JSON 根对象闭合括号 '}'"};
            }

            // 前一个有效非空行补逗号
            int lastValid = -1;
            for (int i = rootCloseLine - 1; i >= 0; --i) {
                if (!IsEmptyOrCommentLine(fl.lines[i])) {
                    lastValid = i;
                    break;
                }
            }
            if (lastValid >= 0) {
                EnsureTrailingComma(fl.lines[lastValid]);
            }

            std::vector<std::string> modBlock = {
                "  \"" + module + "\": {",
                "    \"" + escapedKey + "\": \"" + escapedVal + "\"",
                "  }"
            };
            fl.lines.insert(fl.lines.begin() + rootCloseLine, modBlock.begin(), modBlock.end());
        }
    } else {
        // 全局对象（如 FGD 或全局 Fallback）
        int rootCloseLine = -1;
        for (int i = static_cast<int>(fl.lines.size()) - 1; i >= 0; --i) {
            if (fl.lines[i].find('}') != std::string::npos) {
                rootCloseLine = i;
                break;
            }
        }
        if (rootCloseLine < 0) {
            return {false, "未找到 JSON 根对象闭合括号 '}'"};
        }

        // 检查是否已有该 key
        size_t existLine = 0;
        if (FindKeyLine(fl.lines, 0, rootCloseLine, escapedKey, existLine)) {
            return UpdateEntry(filePath, "", key, value);
        }

        int lastValid = -1;
        for (int i = rootCloseLine - 1; i >= 0; --i) {
            if (!IsEmptyOrCommentLine(fl.lines[i])) {
                lastValid = i;
                break;
            }
        }
        if (lastValid >= 0) {
            EnsureTrailingComma(fl.lines[lastValid]);
        }

        std::string newEntryLine = "  \"" + escapedKey + "\": \"" + escapedVal + "\"";
        fl.lines.insert(fl.lines.begin() + rootCloseLine, newEntryLine);
    }

    if (!WriteLines(filePath, fl, err)) {
        return {false, err};
    }

    return {true, ""};
}

UpdateResult DeleteEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key
) {
    std::string err;
    FileLines fl = ReadLines(filePath, err);
    if (!err.empty()) return {false, err};

    std::string escapedKey = EscapeJsonString(key);

    size_t start = 0;
    size_t end = fl.lines.empty() ? 0 : fl.lines.size() - 1;

    if (!module.empty()) {
        if (!FindModuleBounds(fl.lines, module, start, end)) {
            return {false, "未在词典中找到模块作用域 [" + module + "]"};
        }
    }

    size_t keyLine = 0;
    if (!FindKeyLine(fl.lines, start, end, escapedKey, keyLine)) {
        return {false, "未在词典中找到欲删除的键: \"" + key + "\""};
    }

    // 检查此行是否带有尾随逗号
    bool deletedLineHadComma = false;
    {
        size_t cPos = FindCommentOutsideStrings(fl.lines[keyLine]);
        std::string code = (cPos != std::string::npos) ? fl.lines[keyLine].substr(0, cPos) : fl.lines[keyLine];
        int li = static_cast<int>(code.size()) - 1;
        while (li >= 0 && (code[li] == ' ' || code[li] == '\t')) li--;
        if (li >= 0 && code[li] == ',') {
            deletedLineHadComma = true;
        }
    }

    // 删除键值对行
    fl.lines.erase(fl.lines.begin() + keyLine);

    // 如果被删除的行原本没有逗号（说明它是块内最后一个条目），
    // 那么现在处于它前面的最后一个有效条目不应该带有逗号！
    if (!deletedLineHadComma && keyLine > 0) {
        int prevValid = -1;
        for (int i = static_cast<int>(keyLine) - 1; i >= static_cast<int>(start); --i) {
            if (!IsEmptyOrCommentLine(fl.lines[i])) {
                prevValid = i;
                break;
            }
        }
        if (prevValid >= 0) {
            RemoveTrailingComma(fl.lines[prevValid]);
        }
    }

    if (!WriteLines(filePath, fl, err)) {
        return {false, err};
    }

    return {true, ""};
}

} // namespace dict_updater
