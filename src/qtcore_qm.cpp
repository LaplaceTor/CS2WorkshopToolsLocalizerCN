#include <windows.h>
#include <winternl.h>
#include <intrin.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>
#include <stdio.h>

#pragma intrinsic(_ReturnAddress)

// ==============================================================================
// 1. NT 内部 DLL 加载通知结构体定义
// ==============================================================================
typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef NTSTATUS (NTAPI *fnLdrRegisterDllNotification)(
    ULONG Flags,
    VOID (CALLBACK *NotificationFunction)(ULONG NotificationReason, const LDR_DLL_NOTIFICATION_DATA* NotificationData, PVOID Context),
    PVOID Context,
    PVOID* Cookie
);

// ==============================================================================
// 2. Qt 函数指针定义
// ==============================================================================
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

typedef void (__fastcall *fnQPainter_drawText_Rect)(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_RectF)(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_PointF)(void* pPainter, const void* pPointF, const void* pQString);

// 原函数指针存储
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

static fnQPainter_drawText_Rect g_o_QPainter_drawText_Rect = nullptr;
static fnQPainter_drawText_RectF g_o_QPainter_drawText_RectF = nullptr;
static fnQPainter_drawText_PointF g_o_QPainter_drawText_PointF = nullptr;

static fnQString_fromUtf8 g_pfn_fromUtf8 = nullptr;
static fnQString_utf16 g_pfn_utf16 = nullptr;
static fnQString_dtor g_pfn_QString_dtor = nullptr;

// ==============================================================================
// 3. 单文件多子块字典存储与管理（Single-File Multi-Section Dict Manager）
// ==============================================================================
struct ModuleRange {
    HMODULE hModule = NULL;
    uintptr_t baseAddress = 0;
    uintptr_t endAddress = 0;
    std::wstring stemName;
};

// 全局主字典：公共通用区 + 各 DLL 独立子作用域
static std::unordered_map<std::string, std::string> g_CommonDict;
static std::unordered_map<std::string, std::string> g_CommonCache;
static std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedDicts;
static std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedCaches;
static std::mutex g_DictMutex;
static bool g_bDictLoaded = false;

// 动态注册的模块内存地址区间
static std::vector<ModuleRange> g_ModuleRanges;
static std::mutex g_RangesMutex;

static void* InstallHookX64(void* targetFunc, void* detourFunc, int copyBytes);

// ==============================================================================
// 4. VEH 全局异常安全守卫（针对 Hammer / Tool 内部野指针与析构崩溃的跨版本防护）
// ==============================================================================
static int GetInstructionLengthX64(const BYTE* code) {
    int offset = 0;
    // REX 与传统指令前缀处理
    while (true) {
        BYTE b = code[offset];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
            (b >= 0x40 && b <= 0x4F)) {
            offset++;
        } else {
            break;
        }
    }

    BYTE op = code[offset++];
    
    // 2-byte opcode (0x0F)
    if (op == 0x0F) {
        BYTE op2 = code[offset++];
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || (op2 >= 0x80 && op2 <= 0x8F)) {
            if (op2 >= 0x80 && op2 <= 0x8F) return offset + 4;
            BYTE modrm = code[offset++];
            int mod = (modrm >> 6) & 3;
            int rm = modrm & 7;
            if (mod != 3 && rm == 4) offset++;
            if (mod == 1) offset += 1;
            if (mod == 2 || (mod == 0 && rm == 5)) offset += 4;
            return offset;
        }
        return offset + 2;
    }

    // 常见单字节带 ModR/M 操作码
    if ((op >= 0x00 && op <= 0x3B) || (op >= 0x88 && op <= 0x8B) || op == 0x8D || op == 0x8F ||
        op == 0xC6 || op == 0xC7 || (op >= 0xD0 && op <= 0xD3) || op == 0xF6 || op == 0xF7 ||
        op == 0xFE || op == 0xFF || (op >= 0x84 && op <= 0x87)) {
        BYTE modrm = code[offset++];
        int mod = (modrm >> 6) & 3;
        int rm = modrm & 7;
        if (mod != 3 && rm == 4) offset++;
        if (mod == 1) offset += 1;
        if (mod == 2 || (mod == 0 && rm == 5)) offset += 4;
        if (op == 0xC7 || op == 0xF7) offset += 4;
        if (op == 0xC6 || op == 0xF6) offset += 1;
        return offset;
    }

    // 单字节独立操作码
    if (op >= 0x50 && op <= 0x5F) return offset;
    if (op == 0xC3 || op == 0xCB || op == 0xC2 || op == 0xCA) return offset;
    if (op == 0x90) return offset;

    return offset + 2;
}

static LONG WINAPI ToolCrashGuardVectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    if (pExceptionInfo && pExceptionInfo->ExceptionRecord && pExceptionInfo->ContextRecord) {
        if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            uintptr_t rip = (uintptr_t)pExceptionInfo->ContextRecord->Rip;
            ULONG_PTR faultAddr = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

            // 检查崩溃点是否位于 Hammer 或 Workshop Tools 的 DLL 地址范围内
            bool isInsideToolModule = false;
            {
                std::lock_guard<std::mutex> lock(g_RangesMutex);
                for (const auto& r : g_ModuleRanges) {
                    if (rip >= r.baseAddress && rip < r.endAddress) {
                        isInsideToolModule = true;
                        break;
                    }
                }
            }

            // 若在工具模块内发生野指针 / 空指针异常（地址 < 0x10000），安全跨越故障指令，防止整个游戏/编辑器闪退
            if (isInsideToolModule && faultAddr < 0x10000) {
                int insLen = GetInstructionLengthX64((const BYTE*)rip);
                if (insLen > 0 && insLen <= 15) {
                    pExceptionInfo->ContextRecord->Rip += insLen;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 获取 CS2 根二进制目录 (game/bin/win64/)
static std::wstring GetBinDirectory() {
    wchar_t szPath[MAX_PATH];
    HMODULE hMod = GetModuleHandleW(L"Qt5Core.dll");
    if (!hMod) hMod = GetModuleHandleW(NULL);
    GetModuleFileNameW(hMod, szPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(szPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(szPath);
}

// 提取 DLL 路径中的文件名 stem（小写，不带扩展名，如 "tools\hammer.dll" -> "hammer"）
static std::wstring ExtractStem(const std::wstring& path) {
    if (path.empty()) return L"";
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring filename = (lastSlash == std::wstring::npos) ? path : path.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(L'.');
    std::wstring stem = (lastDot == std::wstring::npos) ? filename : filename.substr(0, lastDot);
    std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
    return stem;
}

static std::wstring NormalizeSectionName(const std::string& name) {
    std::wstring wname;
    wname.reserve(name.size());
    for (char c : name) wname.push_back((wchar_t)c);
    std::transform(wname.begin(), wname.end(), wname.begin(), ::towlower);
    if (wname.length() > 4 && wname.substr(wname.length() - 4) == L".dll") {
        wname = wname.substr(0, wname.length() - 4);
    }
    return wname;
}

// 快速、零依赖的高性能 JSON/JSONC 解析器（支持 UTF-8、注释 // 与 /* */、子块对象以及扁平键值对）
static bool ParseSectionedJson(const char* jsonContent, size_t length) {
    g_CommonDict.clear();
    g_CommonCache.clear();
    g_ScopedDicts.clear();
    g_ScopedCaches.clear();

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
                    p += 2;
                    while (p < end && *p != '\n' && *p != '\r') p++;
                } else if (*(p + 1) == '*') {
                    p += 2;
                    while (p + 1 < end && !(*p == '*' && *(p + 1) == '/')) p++;
                    if (p + 1 < end) p += 2;
                    else p = end;
                } else {
                    break;
                }
            } else {
                break;
            }
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

    skipWhitespaceAndComments();
    if (p >= end || *p != '{') return false;
    p++;

    std::string key, val;
    while (p < end) {
        skipWhitespaceAndComments();
        if (p >= end || *p == '}') break;

        if (!parseString(key)) break;

        skipWhitespaceAndComments();
        if (p >= end || *p != ':') break;
        p++;

        skipWhitespaceAndComments();
        if (p >= end) break;

        if (*p == '{') {
            // 模块独立子块：例如 "hammer": { ... } 或 "modeldoc_editor": { ... }
            p++;
            std::wstring sectionName = NormalizeSectionName(key);
            auto& targetMap = (sectionName == L"common" || sectionName == L"general") ? g_CommonDict : g_ScopedDicts[sectionName];

            std::string subKey, subVal;
            while (p < end) {
                skipWhitespaceAndComments();
                if (p >= end || *p == '}') break;

                if (!parseString(subKey)) break;

                skipWhitespaceAndComments();
                if (p >= end || *p != ':') break;
                p++;

                skipWhitespaceAndComments();
                if (!parseString(subVal)) break;

                if (!subKey.empty() && !subVal.empty()) {
                    targetMap[subKey] = subVal;
                }
            }
            if (p < end && *p == '}') p++;
        } else if (*p == '"') {
            // 顶层扁平词条：自动归入通用公共字典
            if (!parseString(val)) break;
            if (!key.empty() && !val.empty()) {
                g_CommonDict[key] = val;
            }
        } else {
            // 跳过其他非字符串/非对象值
            while (p < end && *p != ',' && *p != '}') p++;
        }
    }

    return (!g_CommonDict.empty() || !g_ScopedDicts.empty());
}

static void LoadMasterTranslations() {
    std::lock_guard<std::mutex> lock(g_DictMutex);
    if (g_bDictLoaded) return;
    g_bDictLoaded = true;

    std::wstring binDir = GetBinDirectory();
    std::wstring mainPath = binDir + L"qt_translations.json";
    std::wstring backupPath = binDir + L"translations_cache.json";

    auto tryLoadFile = [](const std::wstring& path) -> bool {
        FILE* fp = _wfopen(path.c_str(), L"rb");
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

        return ParseSectionedJson(buffer.data(), fsize);
    };

    if (!tryLoadFile(mainPath)) {
        tryLoadFile(backupPath);
    }
}

// 注册模块内存地址范围
static void RegisterModuleRange(HMODULE hMod, const std::wstring& stemName) {
    if (!hMod || stemName.empty()) return;

    BYTE* pBase = (BYTE*)hMod;
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)pBase;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(pBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE) return;

    std::lock_guard<std::mutex> lock(g_RangesMutex);
    for (const auto& r : g_ModuleRanges) {
        if (r.hModule == hMod) return;
    }

    ModuleRange mr;
    mr.hModule = hMod;
    mr.baseAddress = (uintptr_t)pBase;
    mr.endAddress = mr.baseAddress + pNt->OptionalHeader.SizeOfImage;
    mr.stemName = stemName;
    g_ModuleRanges.push_back(mr);
}

// ==============================================================================
// 5. 递归智能拆分与快捷键/后缀匹配算法
// ==============================================================================
static bool MatchAndTranslateInternal(const std::unordered_map<std::string, std::string>& dict, 
                                      const std::string& text, std::string& outResult, int depth = 0);

static inline bool TryDirectMatch(const std::unordered_map<std::string, std::string>& dict, 
                                  const std::string& text, std::string& outResult) {
    if (text.empty()) return false;
    auto it = dict.find(text);
    if (it != dict.end()) {
        outResult = it->second;
        return true;
    }
    return false;
}

static bool MatchAndTranslateInternal(const std::unordered_map<std::string, std::string>& dict, 
                                      const std::string& text, std::string& outResult, int depth) {
    if (text.empty() || depth > 4) return false;

    // 1. 直接全匹配
    if (TryDirectMatch(dict, text, outResult)) return true;

    // 2. 去除快捷键加速符 '&'
    if (text.find('&') != std::string::npos) {
        std::string stripped;
        stripped.reserve(text.length());
        for (char c : text) {
            if (c != '&') stripped.push_back(c);
        }
        if (MatchAndTranslateInternal(dict, stripped, outResult, depth + 1)) return true;
    }

    // 3. 去除前后空白字符（保留原始前后空格填充）
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        size_t last = text.find_last_not_of(" \t\r\n");
        if (first > 0 || last < text.length() - 1) {
            std::string sub = text.substr(first, last - first + 1);
            std::string subTrans;
            if (MatchAndTranslateInternal(dict, sub, subTrans, depth + 1)) {
                outResult = text.substr(0, first) + subTrans + text.substr(last + 1);
                return true;
            }
        }
    }

    // 4. 智能匹配动态数字前缀（支持如 "5/5 Mods", "13/13 Tags + Untagged", "5/5 Asset Types", "128 Assets Visible" 等）
    if (isdigit((unsigned char)text[0]) || (text[0] == '+' && text.length() > 1 && isdigit((unsigned char)text[1]))) {
        size_t i = 0;
        while (i < text.length() && (isdigit((unsigned char)text[i]) || text[i] == '/' || text[i] == '.' || 
                                     text[i] == '+' || text[i] == '-' || text[i] == '%' || text[i] == ':')) {
            i++;
        }
        if (i > 0 && i < text.length() && (text[i] == ' ' || text[i] == '\t')) {
            std::string numPrefix = text.substr(0, i);
            size_t remainderStart = text.find_first_not_of(" \t", i);
            if (remainderStart != std::string::npos) {
                std::string remainder = text.substr(remainderStart);
                std::string remainderTrans;

                if (MatchAndTranslateInternal(dict, remainder, remainderTrans, depth + 1)) {
                    outResult = numPrefix + " " + remainderTrans;
                    return true;
                }

                std::string genericPattern = "%1 " + remainder;
                if (TryDirectMatch(dict, genericPattern, remainderTrans)) {
                    size_t pos = remainderTrans.find("%1");
                    if (pos != std::string::npos) {
                        outResult = remainderTrans.substr(0, pos) + numPrefix + remainderTrans.substr(pos + 2);
                        return true;
                    }
                }
            }
        }
    }

    // 5. 智能匹配由 " + " 或 " - " 连接的复合词（如 "Tags + Untagged" -> "标签 + 无标签"）
    size_t plusPos = text.find(" + ");
    if (plusPos != std::string::npos && plusPos > 0) {
        std::string partA = text.substr(0, plusPos);
        std::string partB = text.substr(plusPos + 3);
        std::string transA, transB;
        bool okA = MatchAndTranslateInternal(dict, partA, transA, depth + 1);
        bool okB = MatchAndTranslateInternal(dict, partB, transB, depth + 1);
        if (okA || okB) {
            outResult = (okA ? transA : partA) + " + " + (okB ? transB : partB);
            return true;
        }
    }

    // 6. 智能匹配中括号快捷键后缀：例如 "Clipping Tool [Shift+X]" -> "剪切工具 [Shift+X]"
    if (text.back() == ']') {
        size_t openBracket = text.rfind('[');
        if (openBracket != std::string::npos && openBracket > 0) {
            std::string base = text.substr(0, openBracket);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openBracket);
                    return true;
                }
            }
        }
    }

    // 7. 智能匹配圆括号快捷键后缀：例如 "Undo (Ctrl+Z)" -> "撤销 (Ctrl+Z)"
    if (text.back() == ')') {
        size_t openParen = text.rfind('(');
        if (openParen != std::string::npos && openParen > 0) {
            std::string base = text.substr(0, openParen);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openParen);
                    return true;
                }
            }
        }
    }

    // 8. 智能匹配制表符快捷键后缀：例如 "Save\tCtrl+S" -> "保存\tCtrl+S"
    size_t tabPos = text.find('\t');
    if (tabPos != std::string::npos && tabPos > 0) {
        std::string base = text.substr(0, tabPos);
        std::string baseTrans;
        if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
            outResult = baseTrans + text.substr(tabPos);
            return true;
        }
    }

    // 9. 智能匹配省略号后缀：例如 "Save As..." -> "另存为..."
    if (text.length() > 3 && text.substr(text.length() - 3) == "...") {
        std::string base = text.substr(0, text.length() - 3);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
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
            if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                outResult = baseTrans + "\xe2\x80\xa6";
                return true;
            }
        }
    }

    // 10. 智能匹配冒号后缀：例如 "Name:" -> "名称:"
    if (text.back() == ':') {
        std::string base = text.substr(0, text.length() - 1);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                outResult = baseTrans + ":";
                return true;
            }
        }
    }

    return false;
}

// 核心多级分层翻译查找函数（基于 Caller Address 精准隔离）
static bool FindTranslationScoped(void* callerAddr, const char* text, std::string& outResult) {
    if (!text || text[0] == '\0') return false;
    if (!g_bDictLoaded) LoadMasterTranslations();

    std::string textStr(text);

    // 1. 优先根据 callerAddr 判定发起调用的模块
    if (callerAddr != nullptr) {
        uintptr_t addr = (uintptr_t)callerAddr;
        std::wstring callerStem;

        {
            std::lock_guard<std::mutex> lock(g_RangesMutex);
            for (const auto& r : g_ModuleRanges) {
                if (addr >= r.baseAddress && addr < r.endAddress) {
                    callerStem = r.stemName;
                    break;
                }
            }
        }

        // 如果命中了特定模块的专属子块，优先在其独立字典中查找
        if (!callerStem.empty()) {
            std::lock_guard<std::mutex> lock(g_DictMutex);
            auto itSec = g_ScopedDicts.find(callerStem);
            if (itSec != g_ScopedDicts.end()) {
                const auto& secDict = itSec->second;
                auto& secCache = g_ScopedCaches[callerStem];

                // 静态查找
                auto itDirect = secDict.find(textStr);
                if (itDirect != secDict.end()) {
                    outResult = itDirect->second;
                    return true;
                }

                // 缓存查找
                auto itCache = secCache.find(textStr);
                if (itCache != secCache.end()) {
                    outResult = itCache->second;
                    return true;
                }

                // 递归拆分匹配
                if (MatchAndTranslateInternal(secDict, textStr, outResult)) {
                    secCache[textStr] = outResult;
                    return true;
                }
            }
        }
    }

    // 2. 专属子块未找到，或来自 Qt 公共底层绘制，回退到全局公共字典 (common)
    {
        std::lock_guard<std::mutex> lock(g_DictMutex);
        if (!g_CommonDict.empty()) {
            // 静态查找
            auto itDirect = g_CommonDict.find(textStr);
            if (itDirect != g_CommonDict.end()) {
                outResult = itDirect->second;
                return true;
            }

            // 缓存查找
            auto itCache = g_CommonCache.find(textStr);
            if (itCache != g_CommonCache.end()) {
                outResult = itCache->second;
                return true;
            }

            // 递归拆分匹配
            if (MatchAndTranslateInternal(g_CommonDict, textStr, outResult)) {
                g_CommonCache[textStr] = outResult;
                return true;
            }
        }
    }

    return false;
}

static bool FindTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult) {
    if (!wstr || wstr[0] == L'\0') return false;
    char utf8[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, sizeof(utf8), NULL, NULL);
    return FindTranslationScoped(callerAddr, utf8, outResult);
}

// ==============================================================================
// 6. NT 原生 DLL 加载同步通知回调（仅在内存中记录地址范围，零死锁）
// ==============================================================================
static inline bool IsKnownToolDll(const std::wstring& stem) {
    static const wchar_t* kToolStems[] = {
        L"hammer", L"modeldoc_editor", L"pet", L"met",
        L"cs2_item_editor", L"cs2_workshop_manager", L"sfm",
        L"postprocessingeditor", L"subrecteditor",
        L"dashboard_subtool", L"convarhelper_subtool",
        L"netgraph_subtool", L"soundviewer_subtool", L"vprof_subtool"
    };

    for (const wchar_t* name : kToolStems) {
        if (stem == name) return true;
    }
    return false;
}

static VOID CALLBACK DllNotificationCallback(
    ULONG NotificationReason,
    const LDR_DLL_NOTIFICATION_DATA* NotificationData,
    PVOID Context
) {
    if (NotificationReason == 1 && NotificationData && NotificationData->Loaded.BaseDllName) { // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED
        const UNICODE_STRING* pBase = NotificationData->Loaded.BaseDllName;
        if (pBase->Buffer && pBase->Length > 0) {
            std::wstring baseName(pBase->Buffer, pBase->Length / sizeof(wchar_t));
            std::wstring stem = ExtractStem(baseName);
            if (IsKnownToolDll(stem)) {
                RegisterModuleRange((HMODULE)NotificationData->Loaded.DllBase, stem);
            }
        }
    }
}

// ==============================================================================
// 7. Qt 核心与界面钩子（全部传递 _ReturnAddress()）
// ==============================================================================

// 1. QMetaObject::tr
static void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    void* caller = _ReturnAddress();
    if (sourceText && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScoped(caller, sourceText, trans)) {
            return g_pfn_fromUtf8(pOutQString, trans.c_str(), -1);
        }
    }
    return g_o_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
}

// 2. QPainter::drawText (全面覆盖 PropertyEditor 属性面板与树形表格渲染)
static void __fastcall hk_QPainter_drawText_Rect(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect) {
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationScopedW(caller, wstr, trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationScopedW(caller, wstr, trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        const wchar_t* wstr = g_pfn_utf16(pQString);
        if (wstr && wstr[0] != L'\0') {
            std::string trans;
            if (FindTranslationScopedW(caller, wstr, trans)) {
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

// 3. QAction
static void __fastcall hk_QAction_setText(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QAction_setWhatsThis(pAction, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QAction_setWhatsThis(pAction, pQString);
}

// 4. 按钮/标签/窗口标题
static void __fastcall hk_QAbstractButton_setText(void* pButton, const void* pQString) {
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QWidget_setWindowTitle(pWidget, qstr);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QWidget_setWindowTitle(pWidget, pQString);
}

// 5. Item 控件
static void __fastcall hk_QTreeWidgetItem_setText(void* pItem, int column, const void* pQString) {
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
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
    void* caller = _ReturnAddress();
    if (pQString && g_pfn_utf16 && g_pfn_fromUtf8) {
        std::string trans;
        if (FindTranslationScopedW(caller, g_pfn_utf16(pQString), trans)) {
            void* qstr[1] = {0};
            g_pfn_fromUtf8(qstr, trans.c_str(), -1);
            g_o_QComboBox_addItem(pBox, qstr, pUserData);
            if (g_pfn_QString_dtor) g_pfn_QString_dtor(qstr);
            return;
        }
    }
    g_o_QComboBox_addItem(pBox, pQString, pUserData);
}

// ==============================================================================
// 8. 机器码 Hook 工具
// ==============================================================================
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

// 扫描已经存在的模块（兜底已在注入前加载的模块）
static void ScanExistingModules() {
    static const wchar_t* kToolDlls[] = {
        L"hammer.dll", L"modeldoc_editor.dll", L"pet.dll", L"met.dll",
        L"cs2_item_editor.dll", L"cs2_workshop_manager.dll", L"sfm.dll",
        L"postprocessingeditor.dll", L"subrecteditor.dll",
        L"dashboard_subtool.dll", L"convarhelper_subtool.dll",
        L"netgraph_subtool.dll", L"soundviewer_subtool.dll", L"vprof_subtool.dll"
    };

    for (const wchar_t* dllName : kToolDlls) {
        HMODULE hMod = GetModuleHandleW(dllName);
        if (hMod) {
            std::wstring stem = ExtractStem(dllName);
            RegisterModuleRange(hMod, stem);
        }
    }
}

static DWORD WINAPI ToolsHookThread(LPVOID lpParam) {
    // 1. 注册 VEH 全局异常安全守卫（拦截并安全恢复 Hammer/Tools 模块内的空指针与悬空析构异常）
    AddVectoredExceptionHandler(1, ToolCrashGuardVectoredExceptionHandler);

    // 2. 注册 NT 原生 DLL 加载通知 (零崩溃、原生支持、同步拦截所有 tools 与 subtools 加载)
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        fnLdrRegisterDllNotification pLdrRegister = (fnLdrRegisterDllNotification)GetProcAddress(hNtdll, "LdrRegisterDllNotification");
        if (pLdrRegister) {
            PVOID cookie = NULL;
            pLdrRegister(0, DllNotificationCallback, NULL, &cookie);
        }
    }

    // 3. Hook Qt5Widgets.dll
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

    // 4. Hook Qt5Gui.dll QPainter::drawText (全面覆盖 PropertyEditor 属性面板与树形表格渲染)
    HMODULE hQtGui = NULL;
    while (!hQtGui) {
        hQtGui = GetModuleHandleW(L"Qt5Gui.dll");
        if (!hQtGui) Sleep(50);
    }

    void* pDrawRect   = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z");
    void* pDrawRectF  = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z");
    void* pDrawPointF = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z");

    if (pDrawRect && !g_o_QPainter_drawText_Rect) g_o_QPainter_drawText_Rect = (fnQPainter_drawText_Rect)InstallHookX64(pDrawRect, (void*)hk_QPainter_drawText_Rect, 15);
    if (pDrawRectF && !g_o_QPainter_drawText_RectF) g_o_QPainter_drawText_RectF = (fnQPainter_drawText_RectF)InstallHookX64(pDrawRectF, (void*)hk_QPainter_drawText_RectF, 15);
    if (pDrawPointF && !g_o_QPainter_drawText_PointF) g_o_QPainter_drawText_PointF = (fnQPainter_drawText_PointF)InstallHookX64(pDrawPointF, (void*)hk_QPainter_drawText_PointF, 15);

    // 5. 扫描可能已载入的模块兜底
    ScanExistingModules();

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
