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
static std::unordered_map<HMODULE, std::wstring> g_ModuleStemCache;
static std::mutex g_DictMutex;
static std::mutex g_StemCacheMutex;
static std::once_flag g_dictInitFlag;

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

// 基于调用者指令地址安全获取所在模块的短名称 (Stem)
static std::wstring GetCallerModuleName(void* callerAddr) {
    if (!callerAddr) return L"";
    HMODULE hMod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)callerAddr, &hMod) && hMod) {
        {
            std::lock_guard<std::mutex> lock(g_StemCacheMutex);
            auto it = g_ModuleStemCache.find(hMod);
            if (it != g_ModuleStemCache.end()) {
                return it->second;
            }
        }
        wchar_t szPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(hMod, szPath, MAX_PATH)) {
            std::wstring stem = ExtractStem(szPath);
            std::lock_guard<std::mutex> lock(g_StemCacheMutex);
            g_ModuleStemCache[hMod] = stem;
            return stem;
        }
    }
    return L"";
}

// ==============================================================================
// 3. Qt 原生 QJsonDocument / QJsonParseError 字典解析器
// ==============================================================================
struct QJsonParseError {
    int offset;
    int error;
};

typedef void* (__fastcall *fnQByteArray_ctor)(void* pThis, const char* str, int size);
typedef void (__fastcall *fnQByteArray_dtor)(void* pThis);

typedef void* (__fastcall *fnQJsonDocument_fromJson)(void* pOutDoc, const void* pByteArray, QJsonParseError* pError);
typedef void (__fastcall *fnQJsonDocument_dtor)(void* pThis);
typedef bool (__fastcall *fnQJsonDocument_isObject)(const void* pThis);
typedef void* (__fastcall *fnQJsonDocument_object)(const void* pThis, void* pOutObject);
typedef void (__fastcall *fnQJsonObject_dtor)(void* pThis);
typedef void* (__fastcall *fnQJsonObject_keys)(const void* pThis, void* pOutStringList);
typedef void (__fastcall *fnQStringList_dtor)(void* pThis);

typedef void* (__fastcall *fnQJsonObject_value)(const void* pThis, void* pOutValue, const void* pKeyQString);
typedef void (__fastcall *fnQJsonValue_dtor)(void* pThis);
typedef bool (__fastcall *fnQJsonValue_isObject)(const void* pThis);
typedef bool (__fastcall *fnQJsonValue_isString)(const void* pThis);
typedef void* (__fastcall *fnQJsonValue_toObject)(const void* pThis, void* pOutObject);
typedef void* (__fastcall *fnQJsonValue_toString)(const void* pThis, void* pOutQString);

// QList<QString> 64-bit 内存布局
struct QListData_Qt5 {
    int ref;
    int alloc;
    int begin;
    int end;
    void* array[1];
};

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

static std::string StripJsonComments(const char* p, size_t length) {
    std::string out;
    out.reserve(length);
    const char* end = p + length;
    bool inQuote = false;
    bool escape = false;

    while (p < end) {
        if (inQuote) {
            char c = *p++;
            out.push_back(c);
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inQuote = false;
            }
        } else {
            if (*p == '"') {
                inQuote = true;
                out.push_back(*p++);
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
                    out.push_back(*p++);
                }
            } else {
                out.push_back(*p++);
            }
        }
    }
    return out;
}

// 基于 Qt 原生 QJsonDocument / QJsonParseError 的高性能字典解析器
static bool ParseSectionedJson(const char* jsonContent, size_t length) {
    g_CommonDict.clear();
    g_CommonCache.clear();
    g_ScopedDicts.clear();
    g_ScopedCaches.clear();

    if (!jsonContent || length == 0) return false;

    // 跳过 UTF-8 BOM
    if (length >= 3 && (unsigned char)jsonContent[0] == 0xEF && (unsigned char)jsonContent[1] == 0xBB && (unsigned char)jsonContent[2] == 0xBF) {
        jsonContent += 3;
        length -= 3;
    }

    HMODULE hQtCore = GetModuleHandleW(L"Qt5Core.dll");
    if (!hQtCore) {
        LogHook("[JSON] Qt5Core.dll not found in process!");
        return false;
    }

    auto pfnQByteArray_ctor = (fnQByteArray_ctor)GetProcAddress(hQtCore, "??0QByteArray@@QEAA@PEBDH@Z");
    auto pfnQByteArray_dtor = (fnQByteArray_dtor)GetProcAddress(hQtCore, "??1QByteArray@@QEAA@XZ");
    auto pfnQJsonDoc_fromJson = (fnQJsonDocument_fromJson)GetProcAddress(hQtCore, "?fromJson@QJsonDocument@@SA?AV1@AEBVQByteArray@@PEAUQJsonParseError@@@Z");
    auto pfnQJsonDoc_dtor = (fnQJsonDocument_dtor)GetProcAddress(hQtCore, "??1QJsonDocument@@QEAA@XZ");
    auto pfnQJsonDoc_isObject = (fnQJsonDocument_isObject)GetProcAddress(hQtCore, "?isObject@QJsonDocument@@QEBA_NXZ");
    auto pfnQJsonDoc_object = (fnQJsonDocument_object)GetProcAddress(hQtCore, "?object@QJsonDocument@@QEBA?AVQJsonObject@@XZ");
    auto pfnQJsonObject_dtor = (fnQJsonObject_dtor)GetProcAddress(hQtCore, "??1QJsonObject@@QEAA@XZ");
    auto pfnQJsonObject_keys = (fnQJsonObject_keys)GetProcAddress(hQtCore, "?keys@QJsonObject@@QEBA?AVQStringList@@XZ");
    auto pfnQStringList_dtor = (fnQStringList_dtor)GetProcAddress(hQtCore, "??1QStringList@@QEAA@XZ");
    if (!pfnQStringList_dtor) pfnQStringList_dtor = (fnQStringList_dtor)GetProcAddress(hQtCore, "??1?$QList@VQString@@@@QEAA@XZ");
    auto pfnQListData_dispose = (void(__fastcall*)(void*))GetProcAddress(hQtCore, "?dispose@QListData@@QEAAXXZ");

    auto pfnQString_dtor = (fnQString_dtor)GetProcAddress(hQtCore, "??1QString@@QEAA@XZ");
    auto pfnQString_utf16 = (fnQString_utf16)GetProcAddress(hQtCore, "?utf16@QString@@QEBAPEBGXZ");

    auto pfnQJsonObject_value = (fnQJsonObject_value)GetProcAddress(hQtCore, "?value@QJsonObject@@QEBA?AVQJsonValue@@AEBVQString@@@Z");
    auto pfnQJsonValue_dtor = (fnQJsonValue_dtor)GetProcAddress(hQtCore, "??1QJsonValue@@QEAA@XZ");
    auto pfnQJsonValue_isObject = (fnQJsonValue_isObject)GetProcAddress(hQtCore, "?isObject@QJsonValue@@QEBA_NXZ");
    auto pfnQJsonValue_isString = (fnQJsonValue_isString)GetProcAddress(hQtCore, "?isString@QJsonValue@@QEBA_NXZ");
    auto pfnQJsonValue_toObject = (fnQJsonValue_toObject)GetProcAddress(hQtCore, "?toObject@QJsonValue@@QEBA?AVQJsonObject@@XZ");
    auto pfnQJsonValue_toString = (fnQJsonValue_toString)GetProcAddress(hQtCore, "?toString@QJsonValue@@QEBA?AVQString@@XZ");

    if (!pfnQByteArray_ctor || !pfnQByteArray_dtor || !pfnQJsonDoc_fromJson || !pfnQJsonDoc_dtor ||
        !pfnQJsonDoc_isObject || !pfnQJsonDoc_object || !pfnQJsonObject_dtor || !pfnQJsonObject_keys ||
        !pfnQJsonObject_value || !pfnQJsonValue_dtor || !pfnQJsonValue_isObject || !pfnQJsonValue_isString ||
        !pfnQJsonValue_toObject || !pfnQJsonValue_toString || !pfnQString_dtor || !pfnQString_utf16) {
        LogHook("[JSON] Failed to resolve Qt5Core QJson symbols!");
        return false;
    }

    std::string cleanJson = StripJsonComments(jsonContent, length);

    // 1. 构建 QByteArray 并调用 QJsonDocument::fromJson
    char byteArrBuf[32] = {0};
    pfnQByteArray_ctor(byteArrBuf, cleanJson.c_str(), (int)cleanJson.size());

    char docBuf[32] = {0};
    QJsonParseError parseError = {0, 0};
    pfnQJsonDoc_fromJson(docBuf, byteArrBuf, &parseError);
    pfnQByteArray_dtor(byteArrBuf);

    if (parseError.error != 0) {
        LogHook("[JSON] QJsonDocument::fromJson failed! error code=%d at offset=%d", parseError.error, parseError.offset);
        pfnQJsonDoc_dtor(docBuf);
        return false;
    }

    if (!pfnQJsonDoc_isObject(docBuf)) {
        LogHook("[JSON] QJsonDocument root is not an object!");
        pfnQJsonDoc_dtor(docBuf);
        return false;
    }

    // 2. 提取根 QJsonObject
    char rootObjBuf[32] = {0};
    pfnQJsonDoc_object(docBuf, rootObjBuf);

    char rootListBuf[32] = {0};
    pfnQJsonObject_keys(rootObjBuf, rootListBuf);
    void** pRootListData = (void**)rootListBuf;
    QListData_Qt5* pRootD = (QListData_Qt5*)pRootListData[0];

    if (pRootD && pRootD->end > pRootD->begin) {
        int secCount = pRootD->end - pRootD->begin;
        for (int i = 0; i < secCount; ++i) {
            void* pSecKeyStr = &pRootD->array[pRootD->begin + i];
            std::string secNameUtf8 = WStringToUtf8(pfnQString_utf16(pSecKeyStr));
            std::wstring sectionName = NormalizeSectionName(secNameUtf8);

            char secValBuf[32] = {0};
            pfnQJsonObject_value(rootObjBuf, secValBuf, pSecKeyStr);

            if (pfnQJsonValue_isObject(secValBuf)) {
                // 子块对象 (Scoped Section)
                auto& targetMap = (sectionName == L"common" || sectionName == L"general") 
                                  ? g_CommonDict : g_ScopedDicts[sectionName];

                char subObjBuf[32] = {0};
                pfnQJsonValue_toObject(secValBuf, subObjBuf);

                char subListBuf[32] = {0};
                pfnQJsonObject_keys(subObjBuf, subListBuf);
                void** pSubListData = (void**)subListBuf;
                QListData_Qt5* pSubD = (QListData_Qt5*)pSubListData[0];

                if (pSubD && pSubD->end > pSubD->begin) {
                    int kvCount = pSubD->end - pSubD->begin;
                    for (int j = 0; j < kvCount; ++j) {
                        void* pSubKeyStr = &pSubD->array[pSubD->begin + j];
                        std::string keyUtf8 = WStringToUtf8(pfnQString_utf16(pSubKeyStr));

                        char itemValBuf[32] = {0};
                        pfnQJsonObject_value(subObjBuf, itemValBuf, pSubKeyStr);
                        if (pfnQJsonValue_isString(itemValBuf)) {
                            char transStrBuf[32] = {0};
                            pfnQJsonValue_toString(itemValBuf, transStrBuf);
                            std::string valUtf8 = WStringToUtf8(pfnQString_utf16(transStrBuf));
                            if (!keyUtf8.empty() && !valUtf8.empty()) {
                                targetMap[keyUtf8] = valUtf8;
                            }
                            pfnQString_dtor(transStrBuf);
                        }
                        pfnQJsonValue_dtor(itemValBuf);
                    }
                }

                if (pfnQStringList_dtor) pfnQStringList_dtor(subListBuf);
                else if (pfnQListData_dispose) pfnQListData_dispose(subListBuf);
                pfnQJsonObject_dtor(subObjBuf);
            } else if (pfnQJsonValue_isString(secValBuf)) {
                // 扁平根级键值对
                char transStrBuf[32] = {0};
                pfnQJsonValue_toString(secValBuf, transStrBuf);
                std::string valUtf8 = WStringToUtf8(pfnQString_utf16(transStrBuf));
                if (!secNameUtf8.empty() && !valUtf8.empty()) {
                    g_CommonDict[secNameUtf8] = valUtf8;
                }
                pfnQString_dtor(transStrBuf);
            }
            pfnQJsonValue_dtor(secValBuf);
        }
    }

    if (pfnQStringList_dtor) pfnQStringList_dtor(rootListBuf);
    else if (pfnQListData_dispose) pfnQListData_dispose(rootListBuf);
    pfnQJsonObject_dtor(rootObjBuf);
    pfnQJsonDoc_dtor(docBuf);

    LogHook("[JSON] QJsonDocument parsed successfully: %zu common entries, %zu scoped modules",
        g_CommonDict.size(), g_ScopedDicts.size());

    return (!g_CommonDict.empty() || !g_ScopedDicts.empty());
}

static void LoadMasterTranslations() {
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

static void EnsureDictionaryLoaded() {
    std::call_once(g_dictInitFlag, [] {
        LoadMasterTranslations();
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
    EnsureDictionaryLoaded();

    std::string textStr(text);

    // 1. 优先根据 callerAddr 判定发起调用的模块
    if (callerAddr != nullptr) {
        std::wstring callerStem = GetCallerModuleName(callerAddr);

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
    InitializeTranslator();
}

// 1. QMetaObject::tr (导出作为 EAT 延迟引导入口)
extern "C" __declspec(dllexport) void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    void* caller = _ReturnAddress();
    EnsureInitialized();
    if (sourceText && g_pfn_fromUtf8 && pOutQString) {
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

static bool TryHookQtToolsModules() {
    auto installHookSafe = [](void* pTarget, void* pDetour, void** ppOriginal, const char* name) -> bool {
        if (!*ppOriginal) {
            if (!pTarget) {
                LogHook("[HOOK] %s symbol not found!", name);
                return false;
            }
            LogHook("[HOOK] Calling InstallHook for %s (target=%p, detour=%p)...", name, pTarget, pDetour);
            bool ok = HookManager::Instance().InstallHook(pTarget, pDetour, ppOriginal, name);
            if (!ok) {
                LogHook("[HOOK] %s InstallHook failed!", name);
                return false;
            }
            LogHook("[HOOK] %s hooked successfully (orig=%p)", name, *ppOriginal);
        }
        return true;
    };

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

            bool success = true;
            success &= installHookSafe(pActionSetText,   (void*)hk_QAction_setText,          (void**)&g_o_QAction_setText,          "QAction::setText");
            success &= installHookSafe(pActionSetTip,    (void*)hk_QAction_setToolTip,       (void**)&g_o_QAction_setToolTip,       "QAction::setToolTip");
            success &= installHookSafe(pActionSetStatus, (void*)hk_QAction_setStatusTip,     (void**)&g_o_QAction_setStatusTip,     "QAction::setStatusTip");
            success &= installHookSafe(pActionSetWhats,  (void*)hk_QAction_setWhatsThis,     (void**)&g_o_QAction_setWhatsThis,     "QAction::setWhatsThis");
            success &= installHookSafe(pButtonSetText,   (void*)hk_QAbstractButton_setText,  (void**)&g_o_QAbstractButton_setText,  "QAbstractButton::setText");
            success &= installHookSafe(pLabelSetText,    (void*)hk_QLabel_setText,           (void**)&g_o_QLabel_setText,           "QLabel::setText");
            success &= installHookSafe(pSetTitle,        (void*)hk_QWidget_setWindowTitle,   (void**)&g_o_QWidget_setWindowTitle,   "QWidget::setWindowTitle");
            success &= installHookSafe(pTreeSetText,     (void*)hk_QTreeWidgetItem_setText,  (void**)&g_o_QTreeWidgetItem_setText,  "QTreeWidgetItem::setText");
            success &= installHookSafe(pTableSetText,    (void*)hk_QTableWidgetItem_setText, (void**)&g_o_QTableWidgetItem_setText, "QTableWidgetItem::setText");
            success &= installHookSafe(pListSetText,     (void*)hk_QListWidgetItem_setText,  (void**)&g_o_QListWidgetItem_setText,  "QListWidgetItem::setText");
            success &= installHookSafe(pComboAddItem,    (void*)hk_QComboBox_addItem,       (void**)&g_o_QComboBox_addItem,       "QComboBox::addItem");

            if (success) {
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

            bool success = true;
            success &= installHookSafe(pDrawRect,   (void*)hk_QPainter_drawText_Rect,   (void**)&g_o_QPainter_drawText_Rect,   "QPainter::drawText(Rect)");
            success &= installHookSafe(pDrawRectF,  (void*)hk_QPainter_drawText_RectF,  (void**)&g_o_QPainter_drawText_RectF,  "QPainter::drawText(RectF)");
            success &= installHookSafe(pDrawPointF, (void*)hk_QPainter_drawText_PointF, (void**)&g_o_QPainter_drawText_PointF, "QPainter::drawText(PointF)");

            if (success) {
                g_bGuiHooked.store(true);
                LogHook("[HOOK] Qt5Gui hooks installed successfully (all 3/3 OK)");
            }
        }
    }

    return (g_bWidgetsHooked.load() && g_bGuiHooked.load());
}

static VOID CALLBACK OnDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context) {
    if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED && NotificationData && NotificationData->Loaded.BaseDllName) {
        const UNICODE_STRING* baseName = NotificationData->Loaded.BaseDllName;
        if (baseName->Buffer && baseName->Length > 0) {
            std::wstring dllName(baseName->Buffer, baseName->Length / sizeof(wchar_t));
            std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::towlower);
            if (dllName == L"qt5widgets.dll" || dllName == L"qt5gui.dll") {
                LogHook("[NOTIFY] Ldr Dll Notification: loaded %ls, signaling hook thread", dllName.c_str());
                if (g_hWakeHookEvent) {
                    SetEvent(g_hWakeHookEvent);
                }
            }
        }
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

static void ProcessCrashReportAsync() {
    if (!g_CrashSnapshot.captured.load(std::memory_order_acquire)) {
        return;
    }

    DWORD code = g_CrashSnapshot.exceptionCode;
    void* rip = g_CrashSnapshot.rip;
    std::wstring modName = GetCallerModuleName(rip);
    int accessType = g_CrashSnapshot.accessType;
    ULONG_PTR faultAddr = g_CrashSnapshot.faultAddr;

    LogHook("================ [CRASH EXCEPTION CAPTURED (WORKER)] ================");
    LogHook("Thread ID      : %u (0x%X)", g_CrashSnapshot.threadId, g_CrashSnapshot.threadId);
    LogHook("Exception Code : 0x%08X", code);
    LogHook("Faulting RIP   : %p (Module: %ls)", rip, modName.c_str());
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
            std::wstring m = GetCallerModuleName(frame);
            if (!m.empty()) {
                LogHook("  [RSP+0x%02zX] %p (%ls)", i * 8, frame, m.c_str());
            } else {
                LogHook("  [RSP+0x%02zX] %p", i * 8, frame);
            }
        }
    }
    LogHook("====================================================================");

    // 自动写入 MiniDump（在 Worker 线程上下文中安全调用）
    HMODULE hDbgHelp = LoadLibraryA("dbghelp.dll");
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
                exInfo.ClientPointers = FALSE;
                pDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, 2 /* MiniDumpWithFullMemory */, &exInfo, NULL, NULL);
                CloseHandle(hFile);
                LogHook("[DUMP] Full minidump saved to: captured_crash.dmp");
            }
        }
    }
}

static DWORD WINAPI ToolsHookThread(LPVOID lpParam) {
    LogHook("[THREAD] ToolsHookThread started");

    // 在后台独立线程中预加载字典，确保不阻塞主线程/DllMain
    EnsureDictionaryLoaded();

    HANDLE waitHandles[2] = { g_hWakeHookEvent, g_hCrashReportEvent };

    while (!g_bStopHookThread.load(std::memory_order_relaxed)) {
        if (!g_bWidgetsHooked.load(std::memory_order_relaxed) || !g_bGuiHooked.load(std::memory_order_relaxed)) {
            TryHookQtToolsModules();
        }

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

// 基于调用者地址判断是否属于 Valve Workshop Tools/Hammer 工具链模块（安全判断）
static bool IsToolAddress(void* rip) {
    if (!rip) return false;
    HMODULE hMod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &hMod) && hMod) {
        wchar_t szPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(hMod, szPath, MAX_PATH)) {
            for (int i = 0; szPath[i]; ++i) {
                if (szPath[i] >= L'A' && szPath[i] <= L'Z') szPath[i] += 32;
            }
            if (wcsstr(szPath, L"\\tools\\") || wcsstr(szPath, L"hammer") ||
                wcsstr(szPath, L"pet.") || wcsstr(szPath, L"modeldoc") ||
                wcsstr(szPath, L"met.") || wcsstr(szPath, L"sfm.") ||
                wcsstr(szPath, L"postprocessing") || wcsstr(szPath, L"_subtool")) {
                return true;
            }
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

            // 紧急停用所有 Hook
            HookManager::Instance().EmergencyDisableAllHooks();

            // 唤醒后台 Worker 线程进行复杂格式化与写盘
            if (g_hCrashReportEvent) {
                SetEvent(g_hCrashReportEvent);
            }
        }
    }

    // 允许异常在宿主进程中继续正常传播
    return EXCEPTION_CONTINUE_SEARCH;
}

static std::atomic<bool> g_bTranslatorInitialized{false};
static std::mutex g_TranslatorInitMutex;

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

    // 优先从 LCLZ 补丁元数据头直接读取原始真实 QMetaObject::tr 地址（杜绝 EAT 重定向环境下的自死锁/死循环）
    const PatchHeader* pHeader = nullptr;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hQtCore;
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)hQtCore + dos->e_lfanew);
        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                uint32_t textEndRva = sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
                uint32_t caveRva = (textEndRva + 15) & ~15;
                const PatchHeader* pH = (const PatchHeader*)((uint8_t*)hQtCore + caveRva);
                if (memcmp(pH->magic, "LCLZ", 4) == 0 && pH->origTrRva != 0) {
                    pHeader = pH;
                }
                break;
            }
        }
    }

    if (pHeader && pHeader->origTrRva != 0) {
        g_o_QMetaObject_tr = (fnQMetaObject_tr)((uint8_t*)hQtCore + pHeader->origTrRva);
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
