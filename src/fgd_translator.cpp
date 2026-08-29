#include "fgd_translator.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static bool ParseJsonTranslations(const char* jsonContent, size_t length, std::unordered_map<std::string, std::string>& outMap) {
    outMap.clear();
    outMap.reserve(15000);

    const char* p = jsonContent;
    const char* end = jsonContent + length;

    // 跳过 UTF-8 BOM
    if (length >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }

    auto skipWhitespaceAndComments = [&]() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') {
                p++;
            } else if (*p == '/' && (p + 1 < end)) {
                if (*(p + 1) == '/') {
                    // 单行注释 //
                    p += 2;
                    while (p < end && *p != '\n' && *p != '\r') {
                        p++;
                    }
                } else if (*(p + 1) == '*') {
                    // 块注释 /* ... */
                    p += 2;
                    while (p + 1 < end && !(*p == '*' && *(p + 1) == '/')) {
                        p++;
                    }
                    if (p + 1 < end) {
                        p += 2; // 跳过 '*/'
                    } else {
                        p = end;
                    }
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    };

    skipWhitespaceAndComments();
    if (p >= end || *p != '{') return false;
    p++;

    auto parseString = [&](std::string& str) -> bool {
        if (p >= end || *p != '"') return false;
        p++;
        str.clear();
        str.reserve(64);

        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char esc = *p++;
                switch (esc) {
                    case '"':  str.push_back('"'); break;
                    case '\\': str.push_back('\\'); break;
                    case '/':  str.push_back('/'); break;
                    case 'b':  str.push_back('\b'); break;
                    case 'f':  str.push_back('\f'); break;
                    case 'n':  str.push_back('\n'); break;
                    case 'r':  str.push_back('\r'); break;
                    case 't':  str.push_back('\t'); break;
                    case 'u': {
                        if (p + 4 > end) return false;
                        unsigned int codepoint = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            codepoint <<= 4;
                            if (h >= '0' && h <= '9') codepoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (h - 'A' + 10);
                            else return false;
                        }
                        if (codepoint <= 0x7F) {
                            str.push_back((char)codepoint);
                        } else if (codepoint <= 0x7FF) {
                            str.push_back((char)(0xC0 | ((codepoint >> 6) & 0x1F)));
                            str.push_back((char)(0x80 | (codepoint & 0x3F)));
                        } else if (codepoint <= 0xFFFF) {
                            str.push_back((char)(0xE0 | ((codepoint >> 12) & 0x0F)));
                            str.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
                            str.push_back((char)(0x80 | (codepoint & 0x3F)));
                        }
                        break;
                    }
                    default:
                        str.push_back(esc);
                        break;
                }
            } else {
                str.push_back(c);
            }
        }
        return false;
    };

    std::string key, val;
    while (p < end) {
        skipWhitespaceAndComments();
        if (p >= end || *p == '}') break;

        if (!parseString(key)) break;

        skipWhitespaceAndComments();
        if (p >= end || *p != ':') break;
        p++;

        skipWhitespaceAndComments();
        if (!parseString(val)) break;

        if (!key.empty() && !val.empty()) {
            outMap[key] = val;
        }
    }

    return !outMap.empty();
}

bool FgdTranslator::LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict) {
    FILE* fp = _wfopen(jsonPath.c_str(), L"rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(fp);
        return false;
    }

    std::vector<char> buffer(fsize + 1, 0);
    fread(buffer.data(), 1, fsize, fp);
    fclose(fp);

    return ParseJsonTranslations(buffer.data(), fsize, outDict);
}

static inline std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::string FgdTranslator::TranslateLine(const std::string& line, const std::unordered_map<std::string, std::string>& dict) {
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

    // 1. 实体类说明：= classname : "Description"
    std::regex classRegex(R"re((=)\s*([a-zA-Z0-9_]+)\s*:\s*"([^"]*)")re");
    std::smatch classMatch;
    if (std::regex_search(code, classMatch, classRegex)) {
        std::string prefix = code.substr(0, classMatch.position(0));
        std::string name = classMatch[2].str();
        std::string desc = classMatch[3].str();
        std::string suffix = code.substr(classMatch.position(0) + classMatch.length(0));
        std::string tr = getTrans(desc);
        return prefix + "= " + name + " : \"" + tr + "\"" + suffix + comment + lineEnding;
    }

    // 2. 输入 / 输出描述：input/output Name(type) : "Description"
    std::regex ioRegex(R"re(^(input|output)\s+([a-zA-Z0-9_]+\s*\([^)]*\))\s*:\s*"([^"]*)")re");
    std::smatch ioMatch;
    if (std::regex_search(code, ioMatch, ioRegex)) {
        std::string kind = ioMatch[1].str();
        std::string nameType = ioMatch[2].str();
        std::string desc = ioMatch[3].str();
        std::string suffix = code.substr(ioMatch.position(0) + ioMatch.length(0));
        std::string tr = getTrans(desc);
        return kind + " " + nameType + " : \"" + tr + "\"" + suffix + comment + lineEnding;
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

    // 4. 属性定义：prop(type) [attrs] {attrs} : "Display Name" [ : default [ : "Description" ]]
    std::regex propRegex(R"re(^(\s*[a-zA-Z0-9_\.]+\s*\([a-zA-Z0-9_:]+\)(?:\s*\[[^\]]*\]|\s*\{[^\}]*\})*\s*:)(.*)$)re");
    std::smatch propMatch;
    if (std::regex_match(code, propMatch, propRegex)) {
        std::string prefix = propMatch[1].str();
        std::string rest = propMatch[2].str();

        std::vector<std::string> parts;
        std::string curr = "";
        bool inQ = false;
        std::string choicesTail = "";

        for (size_t i = 0; i < rest.length(); ++i) {
            char c = rest[i];
            if (c == '"') {
                inQ = !inQ;
                curr.push_back(c);
            } else if (c == ':' && !inQ) {
                parts.push_back(curr);
                curr.clear();
            } else if (c == '=' && !inQ) {
                choicesTail = rest.substr(i);
                break;
            } else {
                curr.push_back(c);
            }
        }
        if (!curr.empty() || (!parts.empty() && choicesTail.empty())) {
            parts.push_back(curr);
        }

        std::string newRest = "";
        for (size_t idx = 0; idx < parts.size(); ++idx) {
            if (idx > 0) newRest += ":";
            std::string part = parts[idx];
            std::string pStrip = TrimString(part);

            if (idx == 0) {
                // 显示名称 (Display Name)
                if (pStrip.length() >= 2 && pStrip.front() == '"' && pStrip.back() == '"') {
                    std::string val = pStrip.substr(1, pStrip.length() - 2);
                    std::string tr = getTrans(val);
                    size_t pos = part.find("\"" + val + "\"");
                    if (pos != std::string::npos) {
                        part.replace(pos, val.length() + 2, "\"" + tr + "\"");
                    }
                }
            } else if (idx == 1) {
                // 默认值 (保持原样)
            } else if (idx == 2) {
                // 属性描述 (Description)
                if (pStrip.length() >= 2 && pStrip.front() == '"' && pStrip.back() == '"') {
                    std::string val = pStrip.substr(1, pStrip.length() - 2);
                    std::string tr = getTrans(val);
                    size_t pos = part.find("\"" + val + "\"");
                    if (pos != std::string::npos) {
                        part.replace(pos, val.length() + 2, "\"" + tr + "\"");
                    }
                }
            }
            newRest += part;
        }

        return prefix + newRest + choicesTail + comment + lineEnding;
    }

    // 5. 选项列表 (Choices / Flags)："0" : "Enabled" : "Option Desc" 或 1 : "Passable" : 0
    std::string trimmedCode = TrimString(code);
    if (!trimmedCode.empty() && trimmedCode.front() != '@' && code.find(':') != std::string::npos) {
        std::regex choiceRegex(R"re(^(\s*(?:"[^"]*"|[0-9a-zA-Z_\-]+)\s*:\s*)"([^"]*)"(.*)$)re");
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

bool FgdTranslator::TranslateFile(const std::wstring& srcPath, const std::wstring& dstPath, const std::unordered_map<std::string, std::string>& dict) {
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

    for (const auto& l : lines) {
        std::string transLine = TranslateLine(l, dict);
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
    std::unordered_map<std::string, std::string> dict;
    if (!LoadDictionary(jsonDictPath, dict)) {
        outError = L"无法加载 FGD 翻译字典: " + jsonDictPath;
        return false;
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
                if (!TranslateFile(entry.path().wstring(), transDst.wstring(), dict)) {
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

bool FgdTranslator::EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadDictionary(fallbackCachePath, loaded);
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

    outNotice = L"已自动生成 fgd_translations.json 模板字典（包含详细使用说明与格式示例）。";
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

    outNotice = L"已自动生成 qt_translations.json 模板字典（包含详细使用说明与格式示例）。";
    return true;
}


