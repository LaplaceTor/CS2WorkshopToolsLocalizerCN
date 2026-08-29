#include <windows.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <stdio.h>
#include <vector>

typedef void* (__fastcall *fnQMetaObject_tr)(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);
typedef void* (__fastcall *fnQString_fromUtf8)(void* pOutQString, const char* utf8, int size);
typedef const wchar_t* (__fastcall *fnQString_utf16)(const void* pQString);
typedef void (__fastcall *fnQString_dtor)(void* pQString);

typedef void (__fastcall *fnQAction_setText)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setToolTip)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setStatusTip)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setWhatsThis)(void* pAction, const void* pQString);

typedef void (__fastcall *fnQAbstractButton_setText)(void* pButton, const void* pQString);
typedef void (__fastcall *fnQLabel_setText)(void* pLabel, const void* pQString);
typedef void (__fastcall *fnQWidget_setWindowTitle)(void* pWidget, const void* pQString);

typedef void (__fastcall *fnQTreeWidgetItem_setText)(void* pItem, int column, const void* pQString);
typedef void (__fastcall *fnQTableWidgetItem_setText)(void* pItem, const void* pQString);
typedef void (__fastcall *fnQListWidgetItem_setText)(void* pItem, const void* pQString);
typedef void (__fastcall *fnQComboBox_addItem)(void* pBox, const void* pQString, const void* pUserData);

typedef void* (__fastcall *fnHammer_RegisterAction)(void* rcx, void* rdx, const char* action_name, const char* shortcut, const char* description, void* p6, void* p7, void* p8);
typedef void* (__fastcall *fnHammer_VectorPurge)(void* rcx, void* rdx, void* r8, void* r9, void* p5, void* p6, void* p7);

typedef void (__fastcall *fnQPainter_drawText_Rect)(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_RectF)(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_PointF)(void* pPainter, const void* pPointF, const void* pQString);

static fnQMetaObject_tr g_o_QMetaObject_tr = nullptr;
static fnQAction_setText g_o_QAction_setText = nullptr;
static fnQAction_setToolTip g_o_QAction_setToolTip = nullptr;
static fnQAction_setStatusTip g_o_QAction_setStatusTip = nullptr;
static fnQAction_setWhatsThis g_o_QAction_setWhatsThis = nullptr;
static fnQAbstractButton_setText g_o_QAbstractButton_setText = nullptr;
static fnQLabel_setText g_o_QLabel_setText = nullptr;
static fnQWidget_setWindowTitle g_o_QWidget_setWindowTitle = nullptr;

static fnQTreeWidgetItem_setText g_o_QTreeWidgetItem_setText = nullptr;
static fnQTableWidgetItem_setText g_o_QTableWidgetItem_setText = nullptr;
static fnQListWidgetItem_setText g_o_QListWidgetItem_setText = nullptr;
static fnQComboBox_addItem g_o_QComboBox_addItem = nullptr;

static fnHammer_RegisterAction g_o_Hammer_RegisterAction = nullptr;
static fnHammer_VectorPurge g_o_Hammer_VectorPurge = nullptr;

static fnQPainter_drawText_Rect g_o_QPainter_drawText_Rect = nullptr;
static fnQPainter_drawText_RectF g_o_QPainter_drawText_RectF = nullptr;
static fnQPainter_drawText_PointF g_o_QPainter_drawText_PointF = nullptr;

static fnQString_fromUtf8 g_pfn_fromUtf8 = nullptr;
static fnQString_utf16 g_pfn_utf16 = nullptr;
static fnQString_dtor g_pfn_QString_dtor = nullptr;

static std::unordered_map<std::string, std::string> g_Translations;
static std::unordered_map<std::string, std::string> g_DynamicCache;
static bool g_bLoaded = false;
static std::mutex g_Mutex;
static std::mutex g_CacheMutex;

// 快速、零依赖的高性能 JSON 解析器（支持 UTF-8、转义符 \", \\, \n, \t, \r, \uXXXX）
static bool ParseJsonTranslations(const char* jsonContent, size_t length, std::unordered_map<std::string, std::string>& outMap) {
    outMap.clear();
    outMap.reserve(15000);

    const char* p = jsonContent;
    const char* end = jsonContent + length;

    // 跳过 UTF-8 BOM
    if (length >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }

    // 寻找起始大括号 '{'
    while (p < end && *p != '{') p++;
    if (p >= end) return false;
    p++;

    auto skipWhitespace = [&]() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) {
            p++;
        }
    };

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
        skipWhitespace();
        if (p >= end || *p == '}') break;

        if (!parseString(key)) break;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end || *p != ':') break;
        p++;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (!parseString(val)) break;

        if (!key.empty() && !val.empty()) {
            outMap[key] = val;
        }
    }

    return !outMap.empty();
}

static void LoadTranslationsFromJson() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_bLoaded) return;
    g_bLoaded = true;

    wchar_t szPath[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(L"Qt5Core.dll"), szPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(szPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    // 读取与 Qt5Core.dll 同目录下的 qt_translations.json (备选 translations_cache.json)
    wchar_t szJsonPath[MAX_PATH];
    wcscpy_s(szJsonPath, szPath);
    wcscat_s(szJsonPath, L"qt_translations.json");

    FILE* fp = _wfopen(szJsonPath, L"rb");
    if (!fp) {
        wcscpy_s(szJsonPath, szPath);
        wcscat_s(szJsonPath, L"translations_cache.json");
        fp = _wfopen(szJsonPath, L"rb");
    }

    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize > 0) {
        std::vector<char> buffer(fsize + 1, 0);
        fread(buffer.data(), 1, fsize, fp);
        ParseJsonTranslations(buffer.data(), fsize, g_Translations);
    }
    fclose(fp);
}

static bool MatchAndTranslateInternal(const std::string& text, std::string& outResult, int depth = 0);

static inline bool TryDirectMatch(const std::string& text, std::string& outResult) {
    if (text.empty()) return false;
    auto it = g_Translations.find(text);
    if (it != g_Translations.end()) {
        outResult = it->second;
        return true;
    }
    return false;
}

static bool MatchAndTranslateInternal(const std::string& text, std::string& outResult, int depth) {
    if (text.empty() || depth > 3) return false;

    // 1. 直接全匹配
    if (TryDirectMatch(text, outResult)) return true;

    // 2. 去除快捷键加速符 '&'
    if (text.find('&') != std::string::npos) {
        std::string stripped;
        stripped.reserve(text.length());
        for (char c : text) {
            if (c != '&') stripped.push_back(c);
        }
        if (MatchAndTranslateInternal(stripped, outResult, depth + 1)) return true;
    }

    // 3. 去除前后空白字符（保留原始前后空格填充）
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        size_t last = text.find_last_not_of(" \t\r\n");
        if (first > 0 || last < text.length() - 1) {
            std::string sub = text.substr(first, last - first + 1);
            std::string subTrans;
            if (MatchAndTranslateInternal(sub, subTrans, depth + 1)) {
                outResult = text.substr(0, first) + subTrans + text.substr(last + 1);
                return true;
            }
        }
    }

    // 4. 智能匹配中括号快捷键后缀：例如 "Clipping Tool [Shift+X]" -> "剪切工具 [Shift+X]"
    if (text.back() == ']') {
        size_t openBracket = text.rfind('[');
        if (openBracket != std::string::npos && openBracket > 0) {
            std::string base = text.substr(0, openBracket);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openBracket);
                    return true;
                }
            }
        }
    }

    // 5. 智能匹配圆括号快捷键后缀：例如 "Undo (Ctrl+Z)" -> "撤销 (Ctrl+Z)"
    if (text.back() == ')') {
        size_t openParen = text.rfind('(');
        if (openParen != std::string::npos && openParen > 0) {
            std::string base = text.substr(0, openParen);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openParen);
                    return true;
                }
            }
        }
    }

    // 6. 智能匹配制表符快捷键后缀：例如 "Save\tCtrl+S" -> "保存\tCtrl+S"
    size_t tabPos = text.find('\t');
    if (tabPos != std::string::npos && tabPos > 0) {
        std::string base = text.substr(0, tabPos);
        std::string baseTrans;
        if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
            outResult = baseTrans + text.substr(tabPos);
            return true;
        }
    }

    // 7. 智能匹配省略号后缀：例如 "Save As..." -> "另存为..."
    if (text.length() > 3 && text.substr(text.length() - 3) == "...") {
        std::string base = text.substr(0, text.length() - 3);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
                outResult = baseTrans + "...";
                return true;
            }
        }
    } else if (text.length() >= 3 && text.substr(text.length() - 3) == "\xe2\x80\xa6") { // UTF-8 "…"
        std::string base = text.substr(0, text.length() - 3);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
                outResult = baseTrans + "\xe2\x80\xa6";
                return true;
            }
        }
    }

    // 8. 智能匹配冒号后缀：例如 "Name:" -> "名称:"
    if (text.back() == ':') {
        std::string base = text.substr(0, text.length() - 1);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(base, baseTrans, depth + 1)) {
                outResult = baseTrans + ":";
                return true;
            }
        }
    }

    return false;
}

static bool FindTranslation(const char* text, std::string& outResult) {
    if (!text || text[0] == '\0') return false;

    // 1. 静态主字典查找
    auto it = g_Translations.find(text);
    if (it != g_Translations.end()) {
        outResult = it->second;
        return true;
    }

    // 2. 动态快捷键组合缓存查找
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        auto itCache = g_DynamicCache.find(text);
        if (itCache != g_DynamicCache.end()) {
            outResult = itCache->second;
            return true;
        }
    }

    // 3. 递归智能拆分匹配
    std::string textStr(text);
    if (MatchAndTranslateInternal(textStr, outResult)) {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_DynamicCache[textStr] = outResult;
        return true;
    }

    return false;
}

static bool FindTranslationW(const wchar_t* wstr, std::string& outResult) {
    if (!wstr || wstr[0] == L'\0') return false;
    char utf8[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, sizeof(utf8), NULL, NULL);
    return FindTranslation(utf8, outResult);
}

// 1. QMetaObject::tr
static void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (sourceText && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslation(sourceText, trans)) {
            return g_pfn_fromUtf8(pOutQString, trans.c_str(), -1);
        }
    }
    return g_o_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
}

// 2. QPainter::drawText (全面覆盖所有树形视图、属性面板、自定义委托文本绘制，如 "Clipping Tool [Shift+X]"、"Transform Locked" 等)
static void __fastcall hk_QPainter_drawText_Rect(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationW(wstr, trans)) {
                void* qstr[1] = {0};
                g_pfn_fromUtf8(qstr, trans.c_str(), -1);
                g_o_QPainter_drawText_Rect(pPainter, pRect, flags, qstr, pBoundingRect);
                if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
                return;
            }
        }
    }
    g_o_QPainter_drawText_Rect(pPainter, pRect, flags, pQString, pBoundingRect);
}

static void __fastcall hk_QPainter_drawText_RectF(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationW(wstr, trans)) {
                void* qstr[1] = {0};
                g_pfn_fromUtf8(qstr, trans.c_str(), -1);
                g_o_QPainter_drawText_RectF(pPainter, pRectF, flags, qstr, pBoundingRect);
                if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
                return;
            }
        }
    }
    g_o_QPainter_drawText_RectF(pPainter, pRectF, flags, pQString, pBoundingRect);
}

static void __fastcall hk_QPainter_drawText_PointF(void* pPainter, const void* pPointF, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationW(wstr, trans)) {
                void* qstr[1] = {0};
                g_pfn_fromUtf8(qstr, trans.c_str(), -1);
                g_o_QPainter_drawText_PointF(pPainter, pPointF, qstr);
                if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
                return;
            }
        }
    }
    g_o_QPainter_drawText_PointF(pPainter, pPointF, pQString);
}

// 3. hammer.dll RegisterAction
static void* __fastcall hk_Hammer_RegisterAction(void* rcx, void* rdx, const char* action_name, const char* shortcut, const char* description, void* p6, void* p7, void* p8) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    std::string transDesc;
    if (description && description[0] != '\0') {
        if (FindTranslation(description, transDesc)) {
            description = transDesc.c_str();
        }
    }
    return g_o_Hammer_RegisterAction(rcx, rdx, action_name, shortcut, description, p6, p7, p8);
}

// 4. hammer.dll 按钮防御钩子
static void* __fastcall hk_Hammer_VectorPurge(void* rcx, void* rdx, void* r8, void* r9, void* p5, void* p6, void* p7) {
    if ((uintptr_t)p7 < 0x10000) {
        p7 = nullptr;
    }
    return g_o_Hammer_VectorPurge(rcx, rdx, r8, r9, p5, p6, p7);
}

// 5. QAction
static void __fastcall hk_QAction_setText(void* pAction, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAction_setText(pAction, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAction_setText(pAction, pQString);
}

static void __fastcall hk_QAction_setToolTip(void* pAction, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAction_setToolTip(pAction, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAction_setToolTip(pAction, pQString);
}

static void __fastcall hk_QAction_setStatusTip(void* pAction, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAction_setStatusTip(pAction, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAction_setStatusTip(pAction, pQString);
}

static void __fastcall hk_QAction_setWhatsThis(void* pAction, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAction_setWhatsThis(pAction, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAction_setWhatsThis(pAction, pQString);
}

// 6. 按钮/标签/窗口标题
static void __fastcall hk_QAbstractButton_setText(void* pButton, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAbstractButton_setText(pButton, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAbstractButton_setText(pButton, pQString);
}

static void __fastcall hk_QLabel_setText(void* pLabel, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QLabel_setText(pLabel, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QLabel_setText(pLabel, pQString);
}

static void __fastcall hk_QWidget_setWindowTitle(void* pWidget, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QWidget_setWindowTitle(pWidget, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QWidget_setWindowTitle(pWidget, pQString);
}

// 7. Item 控件
static void __fastcall hk_QTreeWidgetItem_setText(void* pItem, int column, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QTreeWidgetItem_setText(pItem, column, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QTreeWidgetItem_setText(pItem, column, pQString);
}

static void __fastcall hk_QTableWidgetItem_setText(void* pItem, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QTableWidgetItem_setText(pItem, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QTableWidgetItem_setText(pItem, pQString);
}

static void __fastcall hk_QListWidgetItem_setText(void* pItem, const void* pQString) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QListWidgetItem_setText(pItem, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QListWidgetItem_setText(pItem, pQString);
}

static void __fastcall hk_QComboBox_addItem(void* pBox, const void* pQString, const void* pUserData) {
    if (!g_bLoaded) LoadTranslationsFromJson();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationW(g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QComboBox_addItem(pBox, qstr, pUserData);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QComboBox_addItem(pBox, pQString, pUserData);
}

static void* InstallHookX64(void* targetFunc, void* detourFunc, int copyBytes) {
    BYTE* target = (BYTE*)targetFunc;
    BYTE* detour = (BYTE*)detourFunc;

    BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return nullptr;

    DWORD oldProtect;
    VirtualProtect(target, copyBytes, PAGE_EXECUTE_READWRITE, &oldProtect);

    memcpy(trampoline, target, copyBytes);

    trampoline[copyBytes] = 0xFF;
    trampoline[copyBytes + 1] = 0x25;
    *(DWORD*)(trampoline + copyBytes + 2) = 0x00000000;
    *(UINT64*)(trampoline + copyBytes + 6) = (UINT64)(target + copyBytes);

    target[0] = 0xFF;
    target[1] = 0x25;
    *(DWORD*)(target + 2) = 0x00000000;
    *(UINT64*)(target + 6) = (UINT64)detour;

    for (int i = 14; i < copyBytes; i++) {
        target[i] = 0x90;
    }

    VirtualProtect(target, copyBytes, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, copyBytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, copyBytes + 14);

    return trampoline;
}

static DWORD WINAPI ToolsHookThread(LPVOID lpParam) {
    // 1. Hook Qt5Widgets.dll
    HMODULE hQtWidgets = NULL;
    while (!hQtWidgets) {
        hQtWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
        if (!hQtWidgets) Sleep(50);
    }

    void* pActionSetText   = (void*)GetProcAddress(hQtWidgets, "?setText@QAction@@QEAAXAEBVQString@@@Z");
    void* pActionSetTip    = (void*)GetProcAddress(hQtWidgets, "?setToolTip@QAction@@QEAAXAEBVQString@@@Z");
    void* pActionSetStatus = (void*)GetProcAddress(hQtWidgets, "?setStatusTip@QAction@@QEAAXAEBVQString@@@Z");
    void* pActionSetWhats  = (void*)GetProcAddress(hQtWidgets, "?setWhatsThis@QAction@@QEAAXAEBVQString@@@Z");
    void* pButtonSetText   = (void*)GetProcAddress(hQtWidgets, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z");
    void* pLabelSetText    = (void*)GetProcAddress(hQtWidgets, "?setText@QLabel@@QEAAXAEBVQString@@@Z");
    void* pSetTitle        = (void*)GetProcAddress(hQtWidgets, "?setWindowTitle@QWidget@@QEAAXAEBVQString@@@Z");

    void* pTreeSetText     = (void*)GetProcAddress(hQtWidgets, "?setText@QTreeWidgetItem@@QEAAXHAEBVQString@@@Z");
    void* pTableSetText    = (void*)GetProcAddress(hQtWidgets, "?setText@QTableWidgetItem@@QEAAXAEBVQString@@@Z");
    void* pListSetText     = (void*)GetProcAddress(hQtWidgets, "?setText@QListWidgetItem@@QEAAXAEBVQString@@@Z");
    void* pComboAddItem    = (void*)GetProcAddress(hQtWidgets, "?addItem@QComboBox@@QEAAXAEBVQString@@AEBVQVariant@@@Z");

    if (pActionSetText && !g_o_QAction_setText) g_o_QAction_setText = (fnQAction_setText)InstallHookX64(pActionSetText, (void*)hk_QAction_setText, 15);
    if (pActionSetTip && !g_o_QAction_setToolTip) g_o_QAction_setToolTip = (fnQAction_setToolTip)InstallHookX64(pActionSetTip, (void*)hk_QAction_setToolTip, 15);
    if (pActionSetStatus && !g_o_QAction_setStatusTip) g_o_QAction_setStatusTip = (fnQAction_setStatusTip)InstallHookX64(pActionSetStatus, (void*)hk_QAction_setStatusTip, 15);
    if (pActionSetWhats && !g_o_QAction_setWhatsThis) g_o_QAction_setWhatsThis = (fnQAction_setWhatsThis)InstallHookX64(pActionSetWhats, (void*)hk_QAction_setWhatsThis, 15);
    if (pButtonSetText && !g_o_QAbstractButton_setText) g_o_QAbstractButton_setText = (fnQAbstractButton_setText)InstallHookX64(pButtonSetText, (void*)hk_QAbstractButton_setText, 16);
    if (pLabelSetText && !g_o_QLabel_setText) g_o_QLabel_setText = (fnQLabel_setText)InstallHookX64(pLabelSetText, (void*)hk_QLabel_setText, 16);
    if (pSetTitle && !g_o_QWidget_setWindowTitle) g_o_QWidget_setWindowTitle = (fnQWidget_setWindowTitle)InstallHookX64(pSetTitle, (void*)hk_QWidget_setWindowTitle, 16);

    if (pTreeSetText && !g_o_QTreeWidgetItem_setText) g_o_QTreeWidgetItem_setText = (fnQTreeWidgetItem_setText)InstallHookX64(pTreeSetText, (void*)hk_QTreeWidgetItem_setText, 15);
    if (pTableSetText && !g_o_QTableWidgetItem_setText) g_o_QTableWidgetItem_setText = (fnQTableWidgetItem_setText)InstallHookX64(pTableSetText, (void*)hk_QTableWidgetItem_setText, 16);
    if (pListSetText && !g_o_QListWidgetItem_setText) g_o_QListWidgetItem_setText = (fnQListWidgetItem_setText)InstallHookX64(pListSetText, (void*)hk_QListWidgetItem_setText, 16);
    if (pComboAddItem && !g_o_QComboBox_addItem) g_o_QComboBox_addItem = (fnQComboBox_addItem)InstallHookX64(pComboAddItem, (void*)hk_QComboBox_addItem, 15);

    // 2. Hook Qt5Gui.dll QPainter::drawText (全面覆盖 PropertyEditor 属性面板与树形表格渲染)
    HMODULE hQtGui = NULL;
    while (!hQtGui) {
        hQtGui = GetModuleHandleW(L"Qt5Gui.dll");
        if (!hQtGui) Sleep(50);
    }

    void* pDrawRect  = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z");
    void* pDrawRectF = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z");
    void* pDrawPointF = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z");

    if (pDrawRect && !g_o_QPainter_drawText_Rect) g_o_QPainter_drawText_Rect = (fnQPainter_drawText_Rect)InstallHookX64(pDrawRect, (void*)hk_QPainter_drawText_Rect, 15);
    if (pDrawRectF && !g_o_QPainter_drawText_RectF) g_o_QPainter_drawText_RectF = (fnQPainter_drawText_RectF)InstallHookX64(pDrawRectF, (void*)hk_QPainter_drawText_RectF, 15);
    if (pDrawPointF && !g_o_QPainter_drawText_PointF) g_o_QPainter_drawText_PointF = (fnQPainter_drawText_PointF)InstallHookX64(pDrawPointF, (void*)hk_QPainter_drawText_PointF, 15);

    // 3. Hook hammer.dll RegisterAction & VectorPurge
    HMODULE hHammer = NULL;
    while (!hHammer) {
        hHammer = GetModuleHandleW(L"hammer.dll");
        if (!hHammer) Sleep(100);
    }

    static const BYTE patRegisterAction[] = {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08,
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
        0x57, 0x48, 0x8d, 0x6c, 0x24, 0xe1
    };

    static const BYTE patVectorPurge[] = {
        0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
        0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xe0
    };

    BYTE* pBase = (BYTE*)hHammer;
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)pBase;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(pBase + pDos->e_lfanew);
    IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);

    for (int i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
        if (memcmp(pSec[i].Name, ".text", 5) == 0) {
            BYTE* pText = pBase + pSec[i].VirtualAddress;
            DWORD textSize = pSec[i].Misc.VirtualSize;

            for (DWORD j = 0; j < textSize - sizeof(patRegisterAction); j++) {
                if (memcmp(pText + j, patRegisterAction, sizeof(patRegisterAction)) == 0) {
                    void* pFunc = (void*)(pText + j);
                    if (!g_o_Hammer_RegisterAction) {
                        g_o_Hammer_RegisterAction = (fnHammer_RegisterAction)InstallHookX64(pFunc, (void*)hk_Hammer_RegisterAction, 15);
                    }
                    break;
                }
            }

            for (DWORD j = 0; j < textSize - sizeof(patVectorPurge); j++) {
                if (memcmp(pText + j, patVectorPurge, sizeof(patVectorPurge)) == 0) {
                    void* pFunc = (void*)(pText + j);
                    if (!g_o_Hammer_VectorPurge) {
                        g_o_Hammer_VectorPurge = (fnHammer_VectorPurge)InstallHookX64(pFunc, (void*)hk_Hammer_VectorPurge, 19);
                    }
                    break;
                }
            }

            break;
        }
    }

    return 0;
}

extern "C" __declspec(dllexport) void InitQtCoreQmTranslator() {
    HMODULE hQtCore = GetModuleHandleW(L"Qt5Core.dll");
    if (!hQtCore) return;

    g_pfn_fromUtf8 = (fnQString_fromUtf8)GetProcAddress(hQtCore, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
    g_pfn_utf16 = (fnQString_utf16)GetProcAddress(hQtCore, "?utf16@QString@@QEBAPEBGXZ");
    g_pfn_QString_dtor = (fnQString_dtor)GetProcAddress(hQtCore, "??1QString@@QEAA@XZ");
    void* pTr = (void*)GetProcAddress(hQtCore, "?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z");

    if (pTr && g_pfn_fromUtf8 && !g_o_QMetaObject_tr) {
        g_o_QMetaObject_tr = (fnQMetaObject_tr)InstallHookX64(pTr, (void*)hk_QMetaObject_tr, 16);
    }

    CreateThread(NULL, 0, ToolsHookThread, NULL, 0, NULL);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitQtCoreQmTranslator();
    }
    return TRUE;
}
