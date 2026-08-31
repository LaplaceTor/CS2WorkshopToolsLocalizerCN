#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <intrin.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include "hook_manager.h"
#include "pe_patcher.h"
#include "dictionary_compiler.h"
#include "hde/hde64.h"

#pragma intrinsic(_ReturnAddress)

// ==============================================================================
// 1. Qt 核心函数指针定义 (MSVC x64 C++ ABI)
// ==============================================================================
// 在 MSVC x64 C++ ABI 下，非 POD / 含有析构函数的类 (如 QString) 按值返回时，
// 调用方会在栈上分配缓冲区，并将其指针作为隐式第 1 个参数传递（对于成员函数，this 为第 1 个参数，返回指针为第 2 个参数），
// 并且函数在 RAX 中返回该缓冲区指针。
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
// 2. 单文件多子块字典存储与管理（Single-File Multi-Section Dict Manager）
// ==============================================================================
static std::unordered_map<std::string, std::string> g_CommonDict;
static std::unordered_map<std::string, std::string> g_CommonCache;
static std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedDicts;
static std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedCaches;
static std::mutex g_DictMutex;
static std::once_flag g_dictInitFlag;
static std::atomic<bool> g_bDictLoaded{false};
static std::atomic<bool> g_bTranslatorInitialized{false};
static std::mutex g_TranslatorInitMutex;

// ==============================================================================
// 预扫描调用者模块地址区间表（0 锁、0 系统 API、无锁极速判断）
// ==============================================================================
struct CachedModuleRange {
    uintptr_t base;
    uintptr_t end;
    std::wstring stem;
};

static constexpr size_t MAX_CACHED_MODULE_RANGES = 128;
static CachedModuleRange g_FastRanges[MAX_CACHED_MODULE_RANGES];
static std::atomic<size_t> g_FastRangeCount{0};
static std::mutex g_ScanModulesMutex;

// 获取 CS2 根二进制目录 (game/bin/win64/)
static std::wstring GetBinDirectory() {
    wchar_t szPath[MAX_PATH] = {0};
    HMODULE hMod = GetModuleHandleW(L"Qt5Core.dll");
    if (!hMod) hMod = GetModuleHandleW(NULL);
    GetModuleFileNameW(hMod, szPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(szPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(szPath);
}

// 详细翻译日志开关：默认在 Release 模式下关闭以彻底消除巨量 I/O。
// 仅当 CS2 启动命令行中包含 -debug 或 -verbosehook 时才动态开启逐条翻译日志记录。
static std::atomic<int> g_nVerboseHookLog{-1}; // -1: 未检测, 0: 关闭, 1: 开启

static bool IsVerboseLogEnabled() {
    int v = g_nVerboseHookLog.load(std::memory_order_relaxed);
    if (v != -1) return v == 1;

    bool enabled = false;
    LPCWSTR pCmdLine = GetCommandLineW();
    if (pCmdLine) {
        std::wstring cmd = pCmdLine;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);
        if (cmd.find(L"-debug") != std::wstring::npos ||
            cmd.find(L"--debug") != std::wstring::npos ||
            cmd.find(L"-verbosehook") != std::wstring::npos) {
            enabled = true;
        }
    }
    g_nVerboseHookLog.store(enabled ? 1 : 0, std::memory_order_relaxed);
    return enabled;
}

static void LogHook(const char* fmt, ...) {
    static std::mutex s_logMtx;
    std::lock_guard<std::mutex> lock(s_logMtx);
    std::wstring binDir = GetBinDirectory();
    std::wstring logPath = binDir + L"hook_runtime.log";
    FILE* fp = _wfopen(logPath.c_str(), L"a");
    if (!fp) return;
    va_list va;
    va_start(va, fmt);
    vfprintf(fp, fmt, va);
    va_end(va);
    fprintf(fp, "\n");
    fflush(fp);
    fclose(fp);
}

static inline void LogVerboseTr(const char* fmt, ...) {
    if (!IsVerboseLogEnabled()) return;
    static std::mutex s_logMtx;
    std::lock_guard<std::mutex> lock(s_logMtx);
    std::wstring binDir = GetBinDirectory();
    std::wstring logPath = binDir + L"hook_runtime.log";
    FILE* fp = _wfopen(logPath.c_str(), L"a");
    if (!fp) return;
    va_list va;
    va_start(va, fmt);
    vfprintf(fp, fmt, va);
    va_end(va);
    fprintf(fp, "\n");
    fflush(fp);
    fclose(fp);
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

static bool AddModuleRange(HMODULE hMod, const std::wstring& overrideStem = L"") {
    if (!hMod) return false;
    uint32_t sizeOfImage = PePatcher::GetModuleSizeOfImage(hMod);
    if (sizeOfImage == 0) return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
    uintptr_t end = base + sizeOfImage;

    std::wstring stem = overrideStem;
    if (stem.empty()) {
        wchar_t szPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(hMod, szPath, MAX_PATH)) {
            stem = ExtractStem(szPath);
        }
    }
    if (stem.empty()) return false;

    std::lock_guard<std::mutex> lock(g_ScanModulesMutex);
    size_t count = g_FastRangeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (g_FastRanges[i].base == base) {
            return true; // 已注册
        }
    }
    if (count < MAX_CACHED_MODULE_RANGES) {
        g_FastRanges[count].base = base;
        g_FastRanges[count].end = end;
        g_FastRanges[count].stem = stem;
        g_FastRangeCount.store(count + 1, std::memory_order_release);
        return true;
    }
    return false;
}

static bool RemoveModuleRangeByBase(uintptr_t base) {
    std::lock_guard<std::mutex> lock(g_ScanModulesMutex);
    size_t count = g_FastRangeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (g_FastRanges[i].base == base) {
            for (size_t j = i; j + 1 < count; ++j) {
                g_FastRanges[j] = g_FastRanges[j + 1];
            }
            g_FastRanges[count - 1] = CachedModuleRange{};
            g_FastRangeCount.store(count - 1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

static void InvalidateUnloadedModules() {
    std::lock_guard<std::mutex> lock(g_ScanModulesMutex);
    size_t count = g_FastRangeCount.load(std::memory_order_relaxed);
    size_t writeIdx = 0;
    for (size_t i = 0; i < count; ++i) {
        HMODULE hCheck = reinterpret_cast<HMODULE>(g_FastRanges[i].base);
        HMODULE hVerified = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)hCheck, &hVerified) && hVerified == hCheck) {
            if (writeIdx != i) {
                g_FastRanges[writeIdx] = g_FastRanges[i];
            }
            writeIdx++;
        }
    }
    for (size_t i = writeIdx; i < count; ++i) {
        g_FastRanges[i] = CachedModuleRange{};
    }
    g_FastRangeCount.store(writeIdx, std::memory_order_release);
}

static void ScanKnownToolModules() {
    InvalidateUnloadedModules();

    static const wchar_t* const s_KnownModules[] = {
        L"hammer.dll",
        L"modeldoc_editor.dll",
        L"pet.dll",
        L"met.dll",
        L"sfm.dll",
        L"postprocessing.dll",
        L"smartprops_editor.dll",
        L"pulse_editor.dll",
        L"subtool_modeldoc.dll",
        L"subtool_worldeditor.dll",
        L"subtool_particle.dll",
        L"worldeditor.dll",
        L"assetbrowser.dll",
        L"vconsole2.dll",
        L"particles.dll",
        L"worldrenderer.dll",
        L"soundsystem.dll",
        L"schemasystem.dll",
        L"vscript.dll",
        L"engine2.dll",
        L"client.dll",
        L"server.dll",
        L"qt5widgets.dll",
        L"qt5gui.dll",
        L"qt5core.dll"
    };

    for (const wchar_t* modName : s_KnownModules) {
        HMODULE hMod = GetModuleHandleW(modName);
        if (hMod) {
            AddModuleRange(hMod);
        }
    }
}

// 安全复制调用者模块名称至调用者提供的缓冲区（加锁保护，杜绝内部引用失效与数据竞争）
static bool GetCallerModuleName(void* callerAddr, wchar_t* outBuf, size_t maxLen) {
    if (!callerAddr || !outBuf || maxLen == 0) return false;
    outBuf[0] = L'\0';

    uintptr_t addr = reinterpret_cast<uintptr_t>(callerAddr);

    // 1. 热路径：加锁快速检索并直接复制到 caller-provided buffer
    {
        std::lock_guard<std::mutex> lock(g_ScanModulesMutex);
        size_t count = g_FastRangeCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            if (addr >= g_FastRanges[i].base && addr < g_FastRanges[i].end) {
                wcsncpy_s(outBuf, maxLen, g_FastRanges[i].stem.c_str(), _TRUNCATE);
                return (outBuf[0] != L'\0');
            }
        }
    }

    // 2. 冷路径：遇到动态新加载且未预注册的未知模块，加锁解析并写入区间快表
    HMODULE hMod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)callerAddr, &hMod) && hMod) {
        if (AddModuleRange(hMod)) {
            std::lock_guard<std::mutex> lock(g_ScanModulesMutex);
            size_t newCount = g_FastRangeCount.load(std::memory_order_relaxed);
            for (size_t i = 0; i < newCount; ++i) {
                if (addr >= g_FastRanges[i].base && addr < g_FastRanges[i].end) {
                    wcsncpy_s(outBuf, maxLen, g_FastRanges[i].stem.c_str(), _TRUNCATE);
                    return (outBuf[0] != L'\0');
                }
            }
        }
    }

    return false;
}

// ==============================================================================
// 3. 零 ABI 依赖纯 C 二进制字典 (LCLD) 与标准 C++ 解析引擎
// ==============================================================================
static void LoadMasterTranslations() {
    std::wstring binDir = GetBinDirectory();
    std::wstring binaryPath = binDir + L"qt_translations.lcld";
    std::wstring jsonPath = binDir + L"qt_translations.json";
    std::wstring backupJsonPath = binDir + L"translations_cache.json";

    // 1. 优先加载由 Launcher 预编译的纯 C 二进制字典 .lcld（0 ABI 依赖，< 50μs 极速纯内存解析）
    auto tryLoadLcld = [](const std::wstring& path) -> bool {
        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsize <= 0) {
            fclose(fp);
            return false;
        }
        std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
        fread(buffer.data(), 1, fsize, fp);
        fclose(fp);
        return DictionaryCompiler::ParseLcldBinaryToMaps(buffer.data(), fsize, g_CommonDict, g_ScopedDicts);
    };

    if (tryLoadLcld(binaryPath)) {
        LogHook("[DICT] Loaded compiled binary dictionary (LCLD): %zu common, %zu scoped modules",
            g_CommonDict.size(), g_ScopedDicts.size());
        return;
    }

    // 2. 回退：直接使用纯 C++ 零依赖标准 JSON 编译器与解析器加载文本字典
    auto tryLoadJson = [](const std::wstring& path) -> bool {
        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsize <= 0) {
            fclose(fp);
            return false;
        }
        std::string jsonStr(static_cast<size_t>(fsize), '\0');
        fread(jsonStr.data(), 1, fsize, fp);
        fclose(fp);

        std::vector<uint8_t> binary;
        std::wstring err;
        if (!DictionaryCompiler::CompileJsonStringToBinary(jsonStr, binary, err)) {
            LogHook("[DICT] CompileJsonStringToBinary failed: %ls", err.c_str());
            return false;
        }
        return DictionaryCompiler::ParseLcldBinaryToMaps(binary.data(), binary.size(), g_CommonDict, g_ScopedDicts);
    };

    if (tryLoadJson(jsonPath)) {
        LogHook("[DICT] Loaded JSON dictionary via pure C++ parser fallback: %zu common, %zu scoped modules",
            g_CommonDict.size(), g_ScopedDicts.size());
        return;
    }

    if (tryLoadJson(backupJsonPath)) {
        LogHook("[DICT] Loaded backup JSON dictionary via pure C++ parser fallback: %zu common, %zu scoped modules",
            g_CommonDict.size(), g_ScopedDicts.size());
        return;
    }
}

static void EnsureDictionaryLoaded() {
    if (g_bDictLoaded.load(std::memory_order_acquire)) {
        return;
    }
    std::call_once(g_dictInitFlag, [] {
        ScanKnownToolModules();
        LoadMasterTranslations();
        g_bDictLoaded.store(true, std::memory_order_release);
    });
}

// ==============================================================================
// 3. 递归智能拆分与快捷键/后缀匹配算法
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

    // 若字典尚未由后台 Worker 线程加载完毕，绝不在此同步阻塞业务线程，直接返回 false
    if (!g_bDictLoaded.load(std::memory_order_acquire)) {
        return false;
    }

    std::string textStr(text);

    // 1. 优先根据 callerAddr 判定发起调用的模块
    if (callerAddr != nullptr) {
        wchar_t callerStemBuf[64] = {0};
        if (GetCallerModuleName(callerAddr, callerStemBuf, 64)) {
            std::wstring callerStem(callerStemBuf);

            // 如果命中了特定模块的专属子块，优先在其独立字典中查找
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

static std::string WStringToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 1) return "";
    std::string str(static_cast<size_t>(sizeNeeded), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str.data(), sizeNeeded, NULL, NULL);
    if (written > 0) {
        str.resize(static_cast<size_t>(written - 1));
    } else {
        str.clear();
    }
    return str;
}

static bool FindTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult) {
    if (!wstr || wstr[0] == L'\0') return false;
    char utf8Stack[1024] = {0};
    int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8Stack, sizeof(utf8Stack), NULL, NULL);
    if (written > 0) {
        return FindTranslationScoped(callerAddr, utf8Stack, outResult);
    }
    std::string dynamicUtf8 = WStringToUtf8(wstr);
    if (!dynamicUtf8.empty()) {
        return FindTranslationScoped(callerAddr, dynamicUtf8.c_str(), outResult);
    }
    return false;
}

// ==============================================================================
// 4. Qt 核心与界面钩子（全部传递 _ReturnAddress()，使用 SEH 安全守卫防护异常）
// ==============================================================================

static inline bool SafeGetUtf16(fnQString_utf16 pfn, const void* pQString, const wchar_t*& outWstr) {
    if (!pfn || !pQString || (uintptr_t)pQString < 0x10000) return false;
    __try {
        const void* d = *(const void* const*)pQString;
        if (!d || (uintptr_t)d < 0x10000) return false;
        outWstr = pfn(pQString);
        return (outWstr != nullptr && outWstr[0] != L'\0');
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outWstr = nullptr;
        return false;
    }
}

static inline bool SafeCreateQString(fnQString_fromUtf8 pfn, void* pOutQString, const char* utf8, int size) {
    if (!pfn || !pOutQString || !utf8 || size <= 0) return false;
    __try {
        pfn(pOutQString, utf8, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static inline void SafeDestroyQString(fnQString_dtor pfn, void* pQString) {
    if (!pfn || !pQString) return;
    __try {
        pfn(pQString);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

extern "C" __declspec(dllexport) bool InitializeTranslator();
static inline void EnsureInitialized() {
    if (!g_bTranslatorInitialized.load(std::memory_order_acquire)) {
        InitializeTranslator();
    }
}

// 1. QMetaObject::tr (导出作为 EAT 延迟引导入口)
extern "C" __declspec(dllexport) void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    void* caller = _ReturnAddress();
    EnsureInitialized();
    if (sourceText && g_pfn_fromUtf8 && pOutQString && g_bDictLoaded.load(std::memory_order_acquire)) {
        std::string trans;
        if (FindTranslationScoped(caller, sourceText, trans)) {
            LogVerboseTr("[TR] '%s' -> '%s'", sourceText, trans.c_str());
            if (SafeCreateQString(g_pfn_fromUtf8, pOutQString, trans.c_str(), (int)trans.length())) {
                return pOutQString;
            }
        }
    }
    if (g_o_QMetaObject_tr) {
        return g_o_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
    }
    return nullptr;
}

extern "C" __declspec(dllexport) void* __fastcall tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    return hk_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
}

// 2. QPainter::drawText (全面覆盖 PropertyEditor 属性面板与树形表格渲染)
static void __fastcall hk_QPainter_drawText_Rect(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QPainter_drawText_Rect) g_o_QPainter_drawText_Rect(pPainter, pRect, flags, qstr, pBoundingRect);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QPainter_drawText_Rect) g_o_QPainter_drawText_Rect(pPainter, pRect, flags, pQString, pBoundingRect);
}

static void __fastcall hk_QPainter_drawText_RectF(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QPainter_drawText_RectF) g_o_QPainter_drawText_RectF(pPainter, pRectF, flags, qstr, pBoundingRect);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QPainter_drawText_RectF) g_o_QPainter_drawText_RectF(pPainter, pRectF, flags, pQString, pBoundingRect);
}

static void __fastcall hk_QPainter_drawText_PointF(void* pPainter, const void* pPointF, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QPainter_drawText_PointF) g_o_QPainter_drawText_PointF(pPainter, pPointF, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QPainter_drawText_PointF) g_o_QPainter_drawText_PointF(pPainter, pPointF, pQString);
}

// 3. QAction
static void __fastcall hk_QAction_setText(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QAction_setText) g_o_QAction_setText(pAction, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QAction_setText) g_o_QAction_setText(pAction, pQString);
}

static void __fastcall hk_QAction_setToolTip(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QAction_setToolTip) g_o_QAction_setToolTip(pAction, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QAction_setToolTip) g_o_QAction_setToolTip(pAction, pQString);
}

static void __fastcall hk_QAction_setStatusTip(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QAction_setStatusTip) g_o_QAction_setStatusTip(pAction, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QAction_setStatusTip) g_o_QAction_setStatusTip(pAction, pQString);
}

static void __fastcall hk_QAction_setWhatsThis(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QAction_setWhatsThis) g_o_QAction_setWhatsThis(pAction, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QAction_setWhatsThis) g_o_QAction_setWhatsThis(pAction, pQString);
}

// 4. 按钮/标签/窗口标题
static void __fastcall hk_QAbstractButton_setText(void* pButton, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QAbstractButton_setText) g_o_QAbstractButton_setText(pButton, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QAbstractButton_setText) g_o_QAbstractButton_setText(pButton, pQString);
}

static void __fastcall hk_QLabel_setText(void* pLabel, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QLabel_setText) g_o_QLabel_setText(pLabel, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QLabel_setText) g_o_QLabel_setText(pLabel, pQString);
}

static void __fastcall hk_QWidget_setWindowTitle(void* pWidget, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QWidget_setWindowTitle) g_o_QWidget_setWindowTitle(pWidget, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QWidget_setWindowTitle) g_o_QWidget_setWindowTitle(pWidget, pQString);
}

// 5. Item 控件
static void __fastcall hk_QTreeWidgetItem_setText(void* pItem, int column, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QTreeWidgetItem_setText) g_o_QTreeWidgetItem_setText(pItem, column, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QTreeWidgetItem_setText) g_o_QTreeWidgetItem_setText(pItem, column, pQString);
}

static void __fastcall hk_QTableWidgetItem_setText(void* pItem, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QTableWidgetItem_setText) g_o_QTableWidgetItem_setText(pItem, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QTableWidgetItem_setText) g_o_QTableWidgetItem_setText(pItem, pQString);
}

static void __fastcall hk_QListWidgetItem_setText(void* pItem, const void* pQString) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QListWidgetItem_setText) g_o_QListWidgetItem_setText(pItem, qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QListWidgetItem_setText) g_o_QListWidgetItem_setText(pItem, pQString);
}

static void __fastcall hk_QComboBox_addItem(void* pBox, const void* pQString, const void* pUserData) {
    void* caller = _ReturnAddress();
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        if (FindTranslationScopedW(caller, wstr, trans)) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), (int)trans.length())) {
                if (g_o_QComboBox_addItem) g_o_QComboBox_addItem(pBox, qstr, pUserData);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    if (g_o_QComboBox_addItem) g_o_QComboBox_addItem(pBox, pQString, pUserData);
}

// ==============================================================================
// 5. 后台监听、Ldr DLL 通知与 Hook 安装
// ==============================================================================
static std::atomic<bool> g_bStopHookThread{false};
static std::atomic<bool> g_bWidgetsHooked{false};
static std::atomic<bool> g_bGuiHooked{false};
static HANDLE g_hToolsHookThread = NULL;
static HANDLE g_hWakeHookEvent = NULL;

struct HookRequest {
    void* pTarget;
    void* pDetour;
    void** ppOriginal;
    const char* name;
};

static bool InstallHookBatch(const std::vector<HookRequest>& requests, const char* batchName) {
    std::vector<size_t> installedIndices;
    bool allOk = true;

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        if (!req.pTarget) {
            LogHook("[HOOK] [%s] %s symbol not found!", batchName, req.name);
            allOk = false;
            break;
        }
        if (!*req.ppOriginal) {
            LogHook("[HOOK] [%s] Calling InstallHook for %s (target=%p, detour=%p)...", batchName, req.name, req.pTarget, req.pDetour);
            bool ok = HookManager::Instance().InstallHook(req.pTarget, req.pDetour, req.ppOriginal, req.name);
            if (!ok) {
                LogHook("[HOOK] [%s] %s InstallHook failed!", batchName, req.name);
                allOk = false;
                break;
            }
            installedIndices.push_back(i);
            LogHook("[HOOK] [%s] %s hooked successfully (orig=%p)", batchName, req.name, *req.ppOriginal);
        }
    }

    if (!allOk) {
        LogHook("[HOOK] [%s] Batch hook installation failed! Rolling back %zu installed hooks...", batchName, installedIndices.size());
        for (auto idx : installedIndices) {
            const auto& req = requests[idx];
            HookManager::Instance().UninstallHook(req.pTarget);
            if (req.ppOriginal) {
                *req.ppOriginal = nullptr;
            }
        }
        return false;
    }

    return true;
}

static bool TryHookQtToolsModules() {
    if (!g_bWidgetsHooked.load()) {
        HMODULE hQtWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
        if (hQtWidgets) {
            LogHook("[HOOK] Found Qt5Widgets.dll (%p), installing widget hooks...", hQtWidgets);
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

            std::vector<HookRequest> widgetRequests = {
                { pActionSetText,   (void*)hk_QAction_setText,          (void**)&g_o_QAction_setText,          "QAction::setText" },
                { pActionSetTip,    (void*)hk_QAction_setToolTip,       (void**)&g_o_QAction_setToolTip,       "QAction::setToolTip" },
                { pActionSetStatus, (void*)hk_QAction_setStatusTip,     (void**)&g_o_QAction_setStatusTip,     "QAction::setStatusTip" },
                { pActionSetWhats,  (void*)hk_QAction_setWhatsThis,     (void**)&g_o_QAction_setWhatsThis,     "QAction::setWhatsThis" },
                { pButtonSetText,   (void*)hk_QAbstractButton_setText,  (void**)&g_o_QAbstractButton_setText,  "QAbstractButton::setText" },
                { pLabelSetText,    (void*)hk_QLabel_setText,           (void**)&g_o_QLabel_setText,           "QLabel::setText" },
                { pSetTitle,        (void*)hk_QWidget_setWindowTitle,   (void**)&g_o_QWidget_setWindowTitle,   "QWidget::setWindowTitle" },
                { pTreeSetText,     (void*)hk_QTreeWidgetItem_setText,  (void**)&g_o_QTreeWidgetItem_setText,  "QTreeWidgetItem::setText" },
                { pTableSetText,    (void*)hk_QTableWidgetItem_setText, (void**)&g_o_QTableWidgetItem_setText, "QTableWidgetItem::setText" },
                { pListSetText,     (void*)hk_QListWidgetItem_setText,  (void**)&g_o_QListWidgetItem_setText,  "QListWidgetItem::setText" },
                { pComboAddItem,    (void*)hk_QComboBox_addItem,        (void**)&g_o_QComboBox_addItem,        "QComboBox::addItem" }
            };

            if (InstallHookBatch(widgetRequests, "Qt5Widgets")) {
                g_bWidgetsHooked.store(true);
                LogHook("[HOOK] Qt5Widgets hooks installed successfully (all 11/11 OK)");
            }
        }
    }

    if (!g_bGuiHooked.load()) {
        HMODULE hQtGui = GetModuleHandleW(L"Qt5Gui.dll");
        if (hQtGui) {
            LogHook("[HOOK] Found Qt5Gui.dll (%p), installing GUI hooks...", hQtGui);
            void* pDrawRect   = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z");
            void* pDrawRectF  = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z");
            void* pDrawPointF = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z");

            std::vector<HookRequest> guiRequests = {
                { pDrawRect,   (void*)hk_QPainter_drawText_Rect,   (void**)&g_o_QPainter_drawText_Rect,   "QPainter::drawText(Rect)" },
                { pDrawRectF,  (void*)hk_QPainter_drawText_RectF,  (void**)&g_o_QPainter_drawText_RectF,  "QPainter::drawText(RectF)" },
                { pDrawPointF, (void*)hk_QPainter_drawText_PointF, (void**)&g_o_QPainter_drawText_PointF, "QPainter::drawText(PointF)" }
            };

            if (InstallHookBatch(guiRequests, "Qt5Gui")) {
                g_bGuiHooked.store(true);
                LogHook("[HOOK] Qt5Gui hooks installed successfully (all 3/3 OK)");
            }
        }
    }

    return (g_bWidgetsHooked.load() && g_bGuiHooked.load());
}

static VOID CALLBACK OnDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context) {
    if ((NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED || NotificationReason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) && g_hWakeHookEvent) {
        SetEvent(g_hWakeHookEvent);
    }
}

static bool SafeReadPointer(void* ptr, void*& outVal) {
    __try {
        outVal = *(void**)ptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outVal = nullptr;
        return false;
    }
}

// ==============================================================================
// 6. 异常诊断捕获器（VEH 保持极简纯内存快照，耗时格式化/文件 I/O/模块查询/MiniDump 全部交给 Worker 线程）
// ==============================================================================
static HMODULE g_hModule = NULL;

struct CrashContextSnapshot {
    std::atomic<bool> captured{false};
    DWORD threadId{0};
    DWORD processId{0};
    DWORD exceptionCode{0};
    void* rip{nullptr};
    ULONG_PTR faultAddr{0};
    int accessType{0};
    CONTEXT contextRecord{};
    void* stackSnapshot[32]{};
    size_t stackDepth{0};
};

static CrashContextSnapshot g_CrashSnapshot;
static HANDLE g_hCrashReportEvent = NULL;
static std::atomic<bool> g_bCrashSnapshotConsumed{false};

static void ProcessCrashReportAsync() {
    if (!g_CrashSnapshot.captured.load(std::memory_order_acquire)) {
        return;
    }

    // 严格保证单消费者执行语义（Single-Consumer Semantics）
    bool expected = false;
    if (!g_bCrashSnapshotConsumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // 延迟在 Worker 线程上下文中安全停用所有 Hook（避免在 VEH 异常拦截上下文中调用 VirtualProtect 或争用内部锁）
    HookManager::Instance().EmergencyDisableAllHooks();

    DWORD code = g_CrashSnapshot.exceptionCode;
    void* rip = g_CrashSnapshot.rip;
    wchar_t modNameBuf[MAX_PATH] = {0};
    GetCallerModuleName(rip, modNameBuf, MAX_PATH);
    int accessType = g_CrashSnapshot.accessType;
    ULONG_PTR faultAddr = g_CrashSnapshot.faultAddr;

    LogHook("================ [CRASH EXCEPTION CAPTURED (WORKER)] ================");
    LogHook("Thread ID      : %u (0x%X)", g_CrashSnapshot.threadId, g_CrashSnapshot.threadId);
    LogHook("Exception Code : 0x%08X", code);
    LogHook("Faulting RIP   : %p (Module: %ls)", rip, modNameBuf);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        LogHook("Access Violation: Attempt to %s memory at address %p",
            accessType == 0 ? "READ" : (accessType == 1 ? "WRITE" : "EXECUTE"), (void*)faultAddr);
    }
    const CONTEXT& ctx = g_CrashSnapshot.contextRecord;
    LogHook("Registers:");
    LogHook("  RAX=%p  RBX=%p  RCX=%p  RDX=%p", (void*)ctx.Rax, (void*)ctx.Rbx, (void*)ctx.Rcx, (void*)ctx.Rdx);
    LogHook("  RSI=%p  RDI=%p  RSP=%p  RBP=%p", (void*)ctx.Rsi, (void*)ctx.Rdi, (void*)ctx.Rsp, (void*)ctx.Rbp);
    LogHook("  R8 =%p  R9 =%p  R10=%p  R11=%p", (void*)ctx.R8, (void*)ctx.R9, (void*)ctx.R10, (void*)ctx.R11);
    LogHook("  R12=%p  R13=%p  R14=%p  R15=%p", (void*)ctx.R12, (void*)ctx.R13, (void*)ctx.R14, (void*)ctx.R15);

    LogHook("Stack Frames (Top %zu):", g_CrashSnapshot.stackDepth);
    for (size_t i = 0; i < g_CrashSnapshot.stackDepth; ++i) {
        void* frame = g_CrashSnapshot.stackSnapshot[i];
        if (frame) {
            wchar_t mBuf[MAX_PATH] = {0};
            if (GetCallerModuleName(frame, mBuf, MAX_PATH)) {
                LogHook("  [RSP+0x%02zX] %p (%ls)", i * 8, frame, mBuf);
            } else {
                LogHook("  [RSP+0x%02zX] %p", i * 8, frame);
            }
        }
    }
    LogHook("====================================================================");

    // 自动写入 MiniDump（在 Worker 线程上下文中安全调用，明确从 System32 加载系统组件）
    wchar_t szSysDir[MAX_PATH] = {0};
    GetSystemDirectoryW(szSysDir, MAX_PATH);
    std::wstring dbgHelpPath = std::wstring(szSysDir) + L"\\dbghelp.dll";
    HMODULE hDbgHelp = LoadLibraryExW(dbgHelpPath.c_str(), NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hDbgHelp) {
        hDbgHelp = LoadLibraryW(dbgHelpPath.c_str());
    }
    if (hDbgHelp) {
        typedef BOOL(WINAPI* fnMiniDumpWriteDump)(HANDLE, DWORD, HANDLE, int, PVOID, PVOID, PVOID);
        fnMiniDumpWriteDump pDump = (fnMiniDumpWriteDump)GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
        if (pDump) {
            std::wstring dumpPath = GetBinDirectory() + L"captured_crash.dmp";
            HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                EXCEPTION_POINTERS ep;
                EXCEPTION_RECORD er = {0};
                er.ExceptionCode = code;
                er.ExceptionAddress = rip;
                er.NumberParameters = 2;
                er.ExceptionInformation[0] = accessType;
                er.ExceptionInformation[1] = faultAddr;

                CONTEXT ctxCopy = ctx;
                ep.ExceptionRecord = &er;
                ep.ContextRecord = &ctxCopy;

                struct {
                    DWORD ThreadId;
                    PEXCEPTION_POINTERS ExceptionPointers;
                    BOOL ClientPointers;
                } exInfo;
                exInfo.ThreadId = g_CrashSnapshot.threadId;
                exInfo.ExceptionPointers = &ep;
                // 默认生成轻量级 MiniDumpNormal (0x00000000)，开发模式通过环境变量 CS2_TRANSLATOR_FULL_DUMP=1 开启 FullMemory
                int dumpType = 0; // MiniDumpNormal
                wchar_t envBuf[16] = {0};
                if (GetEnvironmentVariableW(L"CS2_TRANSLATOR_FULL_DUMP", envBuf, 16) > 0 && envBuf[0] == L'1') {
                    dumpType = 2; // MiniDumpWithFullMemory
                }
                pDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, &exInfo, NULL, NULL);
                CloseHandle(hFile);
                LogHook("[DUMP] %s minidump saved to: captured_crash.dmp", (dumpType == 2 ? "Full" : "Standard"));
            }
        }
    }
}

static DWORD WINAPI ToolsHookThread(LPVOID lpParam) {
    LogHook("[THREAD] ToolsHookThread started");

    // 在后台独立线程中预加载字典与扫描工具模块
    EnsureDictionaryLoaded();
    ScanKnownToolModules();

    HANDLE waitHandles[2] = { g_hWakeHookEvent, g_hCrashReportEvent };

    while (!g_bStopHookThread.load(std::memory_order_relaxed)) {
        if (!g_bWidgetsHooked.load(std::memory_order_relaxed) || !g_bGuiHooked.load(std::memory_order_relaxed)) {
            TryHookQtToolsModules();
        }
        ScanKnownToolModules();

        // 检查是否有崩溃报告待异步处理
        if (g_CrashSnapshot.captured.load(std::memory_order_acquire)) {
            ProcessCrashReportAsync();
            break;
        }

        DWORD timeout = (g_bWidgetsHooked.load() && g_bGuiHooked.load()) ? 500 : 50;
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, timeout);
        if (waitRes == WAIT_OBJECT_0 + 1) {
            ProcessCrashReportAsync();
            break;
        }
    }

    if (g_CrashSnapshot.captured.load(std::memory_order_acquire)) {
        ProcessCrashReportAsync();
    }

    LogHook("[THREAD] ToolsHookThread finishing (widgets=%d, gui=%d, stopRequested=%d)",
        g_bWidgetsHooked.load(), g_bGuiHooked.load(), g_bStopHookThread.load());
    return 0;
}

// 基于调用者地址判断是否属于 Valve Workshop Tools/Hammer 工具链模块（安全加锁拷贝）
static bool IsToolAddress(void* rip) {
    if (!rip) return false;
    wchar_t stem[64] = {0};
    if (GetCallerModuleName(rip, stem, 64)) {
        if (wcscmp(stem, L"hammer") == 0 || wcscmp(stem, L"modeldoc_editor") == 0 || wcscmp(stem, L"pet") == 0 ||
            wcscmp(stem, L"met") == 0 || wcscmp(stem, L"sfm") == 0 || wcscmp(stem, L"postprocessing") == 0 ||
            wcscmp(stem, L"smartprops_editor") == 0 || wcscmp(stem, L"pulse_editor") == 0 ||
            wcsstr(stem, L"subtool") != nullptr) {
            return true;
        }
    }
    return false;
}

// 极简 VEH 异常拦截器：严格保持 0 锁、0 LoadLibrary、0 文件 I/O、0 MiniDump
static LONG WINAPI DiagnosticCrashLoggerVEH(PEXCEPTION_POINTERS pExceptionInfo) {
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord || !pExceptionInfo->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;

    // 1. 精准工具模块低位空指针写守卫 (Targeted Tool Guard for Valve Workshop Tools null/dummy writes)
    // 修复 Valve 工具链在部分未初始化组件下的空写崩溃 BUG，安全跳过故障指令继续执行
    if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR faultAddr = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        void* rip = (void*)pExceptionInfo->ContextRecord->Rip;

        if (faultAddr < 0x10000 && IsToolAddress(rip)) {
            hde64s hs;
            unsigned int len = hde64_disasm(rip, &hs);
            if (!(hs.flags & F_ERROR) && len > 0 && len <= 15) {
                pExceptionInfo->ContextRecord->Rip += len;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // 2. 严重异常原子最小化快照（100% 纯寄存器与栈内存抓取，零系统锁与文件操作，交给 Worker 异步处理）
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_DATATYPE_MISALIGNMENT) {
        bool expected = false;
        if (g_CrashSnapshot.captured.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            g_CrashSnapshot.threadId = GetCurrentThreadId();
            g_CrashSnapshot.processId = GetCurrentProcessId();
            g_CrashSnapshot.exceptionCode = code;
            g_CrashSnapshot.rip = (void*)pExceptionInfo->ContextRecord->Rip;
            if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
                g_CrashSnapshot.accessType = (int)pExceptionInfo->ExceptionRecord->ExceptionInformation[0];
                g_CrashSnapshot.faultAddr = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
            }
            g_CrashSnapshot.contextRecord = *pExceptionInfo->ContextRecord;

            // 纯栈指针无锁快照
            void** rsp = (void**)pExceptionInfo->ContextRecord->Rsp;
            size_t depth = 0;
            if (rsp) {
                for (int i = 0; i < 32; ++i) {
                    void* frame = nullptr;
                    if (SafeReadPointer(&rsp[i], frame) && frame) {
                        g_CrashSnapshot.stackSnapshot[depth++] = frame;
                    }
                }
            }
            g_CrashSnapshot.stackDepth = depth;

            // 唤醒后台 Worker 线程进行复杂格式化与写盘
            if (g_hCrashReportEvent) {
                SetEvent(g_hCrashReportEvent);
            }
        }
    }

    // 允许异常在宿主进程中继续正常传播
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(dllexport) bool InitializeTranslator() {
    if (g_bTranslatorInitialized.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_TranslatorInitMutex);
    if (g_bTranslatorInitialized.load(std::memory_order_relaxed)) {
        return true;
    }

    LogHook("[INIT] InitializeTranslator invoked outside Loader Lock");

    // 1. 统一由 HookManager 管理 MinHook 初始化与 VEH 异常守卫
    if (!HookManager::Instance().Initialize(DiagnosticCrashLoggerVEH)) {
        LogHook("[INIT] HookManager::Initialize failed!");
        return false;
    }

    HMODULE hQtCore = GetModuleHandleW(L"Qt5Core.dll");
    if (!hQtCore) {
        LogHook("[INIT] GetModuleHandleW(Qt5Core.dll) failed!");
        return false;
    }

    g_pfn_fromUtf8 = (fnQString_fromUtf8)GetProcAddress(hQtCore, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
    g_pfn_utf16 = (fnQString_utf16)GetProcAddress(hQtCore, "?utf16@QString@@QEBAPEBGXZ");
    g_pfn_QString_dtor = (fnQString_dtor)GetProcAddress(hQtCore, "??1QString@@QEAA@XZ");

    // 优先通过 PePatcher 统一安全读取 LCLZ 补丁元数据头中的原始真实 QMetaObject::tr 地址
    PatchInfo patchInfo;
    if (PePatcher::GetPatchInfoFromMemory(hQtCore, patchInfo) && patchInfo.origTrRva != 0) {
        g_o_QMetaObject_tr = (fnQMetaObject_tr)((uint8_t*)hQtCore + patchInfo.origTrRva);
        LogHook("[INIT] EAT Redirect: real QMetaObject::tr resolved from LCLZ header at %p", g_o_QMetaObject_tr);
    } else {
        void* pTr = (void*)GetProcAddress(hQtCore, "?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z");
        if (pTr && g_pfn_fromUtf8 && !g_o_QMetaObject_tr) {
            bool ok = HookManager::Instance().InstallHook(pTr, (void*)hk_QMetaObject_tr, (void**)&g_o_QMetaObject_tr, "QMetaObject::tr");
            LogHook("[INIT] Hooked QMetaObject::tr via MinHook, result=%d, orig=%p", ok, g_o_QMetaObject_tr);
        }
    }

    LogHook("[INIT] Qt5Core=%p, fromUtf8=%p, utf16=%p, dtor=%p, orig_tr=%p", hQtCore, g_pfn_fromUtf8, g_pfn_utf16, g_pfn_QString_dtor, g_o_QMetaObject_tr);

    // 2. 创建异步唤醒事件与崩溃报告事件，并注册 DLL 通知
    if (!g_hWakeHookEvent) {
        g_hWakeHookEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    if (!g_hCrashReportEvent) {
        g_hCrashReportEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    HookManager::Instance().RegisterDllNotification(OnDllNotification);

    // 3. 启动后台线程异步完成所有耗时操作与挂钩（彻底脱离 DllMain 与 Loader Lock）
    if (!g_hToolsHookThread) {
        g_hToolsHookThread = CreateThread(NULL, 0, ToolsHookThread, NULL, 0, NULL);
        if (!g_hToolsHookThread) {
            LogHook("[INIT] CreateThread for ToolsHookThread failed!");
            return false;
        }
        LogHook("[INIT] ToolsHookThread spawned successfully");
    }

    g_bTranslatorInitialized.store(true, std::memory_order_release);
    return true;
}

extern "C" __declspec(dllexport) void ShutdownTranslator() {
    std::lock_guard<std::mutex> lock(g_TranslatorInitMutex);
    if (!g_bTranslatorInitialized.load(std::memory_order_relaxed)) {
        return;
    }
    g_bTranslatorInitialized.store(false, std::memory_order_release);

    LogHook("[SHUTDOWN] ShutdownTranslator invoked");

    // 1. Stop: 发出停止信号并唤醒后台 Worker 线程
    g_bStopHookThread.store(true, std::memory_order_release);
    if (g_hWakeHookEvent) {
        SetEvent(g_hWakeHookEvent);
    }

    // 2. Worker 真正退出：严谨等待直到 WAIT_OBJECT_0，绝对不能超时后继续卸载
    if (g_hToolsHookThread) {
        DWORD waitRes = WaitForSingleObject(g_hToolsHookThread, INFINITE);
        if (waitRes != WAIT_OBJECT_0) {
            LogHook("[SHUTDOWN] FATAL: Background worker thread wait failed (code=0x%08X), aborting unhook to prevent memory corruption!", waitRes);
            return;
        }
        CloseHandle(g_hToolsHookThread);
        g_hToolsHookThread = NULL;
    }
    if (g_hWakeHookEvent) {
        CloseHandle(g_hWakeHookEvent);
        g_hWakeHookEvent = NULL;
    }
    if (g_hCrashReportEvent) {
        CloseHandle(g_hCrashReportEvent);
        g_hCrashReportEvent = NULL;
    }

    // 3. 统一执行完整生命周期退出：
    //    Unregister callback -> Disable Hook -> Remove Hook -> Remove VEH -> MinHook Uninitialize
    HookManager::Instance().Shutdown();
    LogHook("[SHUTDOWN] ShutdownTranslator completed cleanly");
}

extern "C" __declspec(dllexport) void InitQtCoreQmTranslator() {
    InitializeTranslator();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
    }
    return TRUE;
}
