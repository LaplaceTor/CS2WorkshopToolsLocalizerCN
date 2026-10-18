#include "core/qm/fgd_memory_hook.h"
#include "core/qm/translation_engine.h"
#include "core/dictionary_compiler.h"
#include "core/fgd_core.h"
#include "core/hook_manager.h"
#include "core/shared_log.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace qm {

namespace {

enum BufferSeekType_t {
    SEEK_HEAD = 0,
    SEEK_CURRENT = 1,
    SEEK_TAIL = 2
};

typedef void (__fastcall *fnCUtlBuffer_EnsureCapacity)(void* pBuffer, int num);
typedef void (__fastcall *fnCUtlBuffer_Put)(void* pBuffer, const void* pMem, int size);
typedef void (__fastcall *fnCUtlBuffer_SeekPut)(void* pBuffer, int type, int offset);
typedef void (__fastcall *fnCUtlBuffer_SeekGet)(void* pBuffer, int type, int offset);

typedef bool (__fastcall *fnReadFileToBuffer)(
    void* pThis,
    const char* pFileName,
    const char* pPathID,
    void* pBuffer,
    int maxBytes,
    int startingOffset,
    void* pfnAlloc
);

typedef int (__fastcall *fnV_stricmp_fast)(const char* s1, const char* s2);

fnCUtlBuffer_EnsureCapacity g_pfnCUtlBuffer_EnsureCapacity = nullptr;
fnCUtlBuffer_Put g_pfnCUtlBuffer_Put = nullptr;
fnCUtlBuffer_SeekPut g_pfnCUtlBuffer_SeekPut = nullptr;
fnCUtlBuffer_SeekGet g_pfnCUtlBuffer_SeekGet = nullptr;

fnReadFileToBuffer g_o_ReadFileToBuffer = nullptr;
std::atomic<bool> g_bFileSystemHooked{false};
std::atomic<bool> g_bFileSystemHookFailed{false};

fnV_stricmp_fast g_o_V_stricmp_fast = nullptr;
std::atomic<bool> g_bTier0Hooked{false};

std::unordered_map<std::string, std::string> g_FgdMemoryCache;
std::mutex g_FgdCacheMutex;
std::atomic<bool> g_bFgdCacheLoaded{false};

std::unordered_map<std::string, std::string> g_FgdDict;
FgdOverrideData g_FgdOverrideData;
std::mutex g_FgdDictMutex;
std::mutex g_FgdLoadMutex;
std::atomic<bool> g_bFgdDictLoaded{false};

std::string NormalizeFgdKey(const std::string& path) {
    if (path.empty()) return "";
    std::string key = path;
    for (char& c : key) {
        if (c == '\\') c = '/';
        c = (char)::tolower((unsigned char)c);
    }
    size_t lastSlash = key.find_last_of('/');
    if (lastSlash != std::string::npos) {
        return key.substr(lastSlash + 1);
    }
    return key;
}

bool IsFgdPath(const char* pFileName) {
    if (!pFileName) return false;
    size_t len = strlen(pFileName);
    if (len < 4) return false;
    const char* ext = pFileName + len - 4;
    return (_stricmp(ext, ".fgd") == 0);
}

void LoadFgdOverridePureCpp(const std::wstring& path, FgdOverrideData& outOverride) {
    std::wstring err;
    if (!DictionaryCompiler::ParseFgdOverrideFile(path, outOverride, err)) {
        shared_log::LogHook("[FGD] ParseFgdOverrideFile failed for %ls: %ls", path.c_str(), err.c_str());
    }
}

bool SafeGetBufferMemory(void* pBuffer, char*& outPtr, int& outSize) {
    if (!pBuffer) return false;
    __try {
        outPtr = (char*)((void**)pBuffer)[1];
        outSize = *(int*)((uint8_t*)pBuffer + 0x14);
        return (outPtr != nullptr && outSize > 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        outPtr = nullptr;
        outSize = 0;
        return false;
    }
}

bool __fastcall hk_ReadFileToBuffer(
    void* pThis,
    const char* pFileName,
    const char* pPathID,
    void* pBuffer,
    int maxBytes,
    int startingOffset,
    void* pfnAlloc
) {
    bool bRet = g_o_ReadFileToBuffer(pThis, pFileName, pPathID, pBuffer, maxBytes, startingOffset, pfnAlloc);
    if (!bRet || !pFileName || !pBuffer) return bRet;
    if (!IsTranslationEnabled()) return bRet;

    if (!IsFgdPath(pFileName)) {
        return bRet;
    }

    std::string normKey = NormalizeFgdKey(pFileName);
    std::string transData;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(g_FgdCacheMutex);
        auto it = g_FgdMemoryCache.find(normKey);
        if (it != g_FgdMemoryCache.end() && !it->second.empty()) {
            transData = it->second;
            found = true;
        }
    }

    // JIT 即时兜底：如果缓存还未就绪，现场翻译
    if (!found) {
        EnsureFgdDictLoaded();
        char* pRawData = nullptr;
        int rawSize = 0;
        if (SafeGetBufferMemory(pBuffer, pRawData, rawSize) && pRawData && rawSize > 0) {
            std::string rawContent(pRawData, static_cast<size_t>(rawSize));
            std::string jitTrans;
            std::unordered_map<std::string, std::string> fgdDictCopy;
            FgdOverrideData fgdOverrideCopy;
            {
                std::lock_guard<std::mutex> dictLock(g_FgdDictMutex);
                fgdDictCopy = g_FgdDict;
                fgdOverrideCopy = g_FgdOverrideData;
            }
            if (FgdCore::TranslateContent(rawContent, jitTrans, fgdDictCopy, fgdOverrideCopy) && !jitTrans.empty()) {
                transData = std::move(jitTrans);
                found = true;
                std::lock_guard<std::mutex> lock(g_FgdCacheMutex);
                g_FgdMemoryCache[normKey] = transData;
                shared_log::LogHook("[FGD] JIT translated '%s' (%d -> %zu bytes)", pFileName, rawSize, transData.size());
            }
        }
    }

    if (found && !transData.empty()) {
        if (g_pfnCUtlBuffer_SeekPut && g_pfnCUtlBuffer_EnsureCapacity && g_pfnCUtlBuffer_Put && g_pfnCUtlBuffer_SeekGet) {
            g_pfnCUtlBuffer_SeekPut(pBuffer, SEEK_HEAD, 0);
            g_pfnCUtlBuffer_EnsureCapacity(pBuffer, static_cast<int>(transData.size()) + 2);
            g_pfnCUtlBuffer_Put(pBuffer, transData.data(), static_cast<int>(transData.size()));
            char* pMem = (char*)((void**)pBuffer)[1];
            if (pMem) {
                pMem[transData.size()] = '\0';
            }
            g_pfnCUtlBuffer_SeekGet(pBuffer, SEEK_HEAD, 0);

            shared_log::LogHook("[FGD] In-memory injected '%s' (%zu bytes) into CUtlBuffer at %p",
                    pFileName, transData.size(), pBuffer);
            return true;
        }
    }

    return bRet;
}

int __fastcall hk_V_stricmp_fast(const char* s1, const char* s2) {
    if (!s1 || !s2) {
        if (g_o_V_stricmp_fast) return g_o_V_stricmp_fast(s1, s2);
        return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    }

    int res = g_o_V_stricmp_fast ? g_o_V_stricmp_fast(s1, s2) : _stricmp(s1, s2);
    if (res == 0) return 0;

    void* caller = _ReturnAddress();
    wchar_t stem[64] = {0};
    if (GetCallerModuleName(caller, stem, 64) && _wcsicmp(stem, L"hammer") == 0) {
        std::string trans;
        if (FindTranslationScoped(caller, s1, trans, TranslationSource::StaticUI) && trans == s2) return 0;
        if (FindTranslationScoped(caller, s2, trans, TranslationSource::StaticUI) && trans == s1) return 0;

        std::string rev;
        if (FindReverseTranslationScoped(caller, s1, rev) && _stricmp(rev.c_str(), s2) == 0) return 0;
        if (FindReverseTranslationScoped(caller, s2, rev) && _stricmp(rev.c_str(), s1) == 0) return 0;

        if (MatchFgdStringPair(s1, s2)) return 0;
    }

    return res;
}

} // namespace

bool MatchFgdStringPair(const char* s1, const char* s2) {
    if (!s1 || !s2) return false;
    std::lock_guard<std::mutex> lock(g_FgdDictMutex);
    auto itFgd1 = g_FgdDict.find(s1);
    if (itFgd1 != g_FgdDict.end() && itFgd1->second == s2) return true;
    auto itFgd2 = g_FgdDict.find(s2);
    if (itFgd2 != g_FgdDict.end() && itFgd2->second == s1) return true;
    return false;
}

bool LoadFgdDictionariesFromDisk(bool bForce) {
    if (!bForce && g_bFgdDictLoaded.load(std::memory_order_acquire)) return true;

    std::lock_guard<std::mutex> loadLock(g_FgdLoadMutex);
    if (!bForce && g_bFgdDictLoaded.load(std::memory_order_acquire)) return true;

    std::wstring binDir = GetBinDirectory();
    std::wstring appDir = L"";
    bool useMachineTrans = true;

    std::wstring appDirPointerPath = binDir + L"localizer_appdir.txt";
    FILE* fpPointer = _wfopen(appDirPointerPath.c_str(), L"r, ccs=UTF-8");
    if (!fpPointer) fpPointer = _wfopen(appDirPointerPath.c_str(), L"r");
    if (fpPointer) {
        wchar_t lineBuf[MAX_PATH] = {0};
        if (fgetws(lineBuf, MAX_PATH, fpPointer)) {
            size_t len = wcslen(lineBuf);
            while (len > 0 && (lineBuf[len - 1] == L'\r' || lineBuf[len - 1] == L'\n' || lineBuf[len - 1] == L' ' || lineBuf[len - 1] == L'\t')) {
                lineBuf[--len] = L'\0';
            }
            if (len > 0 && (lineBuf[len - 1] == L'\\' || lineBuf[len - 1] == L'/')) {
                lineBuf[--len] = L'\0';
            }
            if (len > 0) appDir = lineBuf;
        }
        while (fgetws(lineBuf, MAX_PATH, fpPointer)) {
            std::wstring opt = lineBuf;
            if (opt.find(L"use_machine_trans=0") != std::wstring::npos) useMachineTrans = false;
            else if (opt.find(L"use_machine_trans=1") != std::wstring::npos) useMachineTrans = true;
        }
        fclose(fpPointer);
    }

    if (appDir.empty()) {
        shared_log::LogHook("[FGD] Error: Cannot find launcher appDir pointer for FGD dictionaries from %ls", appDirPointerPath.c_str());
        return false;
    }

    std::wstring fgdDictPath = appDir + L"\\translations\\fgd_translations.jsonc";
    std::wstring fgdOverridePath = appDir + L"\\translations\\fgd_override.jsonc";
    std::wstring fgdFallbackPath = L"";

    if (GetFileAttributesW(fgdDictPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        shared_log::LogHook("[FGD] Error: fgd_translations.jsonc not found at %ls", fgdDictPath.c_str());
        fgdDictPath.clear();
    }
    if (GetFileAttributesW(fgdOverridePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fgdOverridePath.clear();
    }
    if (useMachineTrans) {
        std::wstring pFallback = appDir + L"\\translations\\fgd_fallback.jsonc";
        if (GetFileAttributesW(pFallback.c_str()) != INVALID_FILE_ATTRIBUTES) {
            fgdFallbackPath = pFallback;
        }
    }

    std::unordered_map<std::string, std::string> newFgdDict;
    FgdOverrideData newOverride;
    bool parsed = true;

    if (!fgdDictPath.empty()) {
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> dummyScoped;
        std::wstring err;
        if (!DictionaryCompiler::ParseJsoncFileToMaps(fgdDictPath, newFgdDict, dummyScoped, err, fgdFallbackPath)) {
            shared_log::LogHook("[FGD] Failed to parse fgd_translations.jsonc: %ls", err.c_str());
            parsed = false;
        }
    }
    if (!fgdOverridePath.empty()) {
        LoadFgdOverridePureCpp(fgdOverridePath, newOverride);
    }

    if (!parsed) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_FgdDictMutex);
        g_FgdDict.swap(newFgdDict);
        g_FgdOverrideData = std::move(newOverride);
    }

    shared_log::LogHook("[FGD] %s %zu FGD translation keys (dict=%ls, fallback=%ls), %zu override keys",
            bForce ? "Reloaded" : "Loaded",
            g_FgdDict.size(), fgdDictPath.empty() ? L"none" : fgdDictPath.c_str(), fgdFallbackPath.empty() ? L"none" : fgdFallbackPath.c_str(),
            g_FgdOverrideData.globalProperties.size() + g_FgdOverrideData.ioOverrides.size() + g_FgdOverrideData.classDescriptions.size());

    g_bFgdDictLoaded.store(true, std::memory_order_release);
    return true;
}

void EnsureFgdDictLoaded() {
    LoadFgdDictionariesFromDisk(false);
}

void PrecompileFgdBuffers() {
    EnsureFgdDictLoaded();

    std::unordered_map<std::string, std::string> fgdDictSnapshot;
    FgdOverrideData fgdOverrideSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_FgdDictMutex);
        fgdDictSnapshot = g_FgdDict;
        fgdOverrideSnapshot = g_FgdOverrideData;
    }

    std::wstring binDir = GetBinDirectory();
    fs::path binPath(binDir);
    fs::path gameRoot = binPath.parent_path().parent_path();

    std::unordered_map<std::string, std::string> newCache;
    size_t fileCount = 0;

    std::vector<fs::path> targetDirs = {
        gameRoot / "game" / "core",
        gameRoot / "game" / "csgo",
        gameRoot / "game" / "csgo_core"
    };

    for (const auto& dir : targetDirs) {
        if (!fs::exists(dir)) continue;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".fgd") {
                    std::ifstream inFile(entry.path(), std::ios::binary);
                    if (inFile.is_open()) {
                        std::string rawContent((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                        inFile.close();

                        std::string transContent;
                        if (FgdCore::TranslateContent(rawContent, transContent, fgdDictSnapshot, fgdOverrideSnapshot) && !transContent.empty()) {
                            std::string filenameLower = entry.path().filename().string();
                            std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);
                            newCache[filenameLower] = transContent;

                            std::string relPath = fs::relative(entry.path(), gameRoot).string();
                            for (char& c : relPath) {
                                if (c == '\\') c = '/';
                                c = (char)::tolower((unsigned char)c);
                            }
                            newCache[relPath] = transContent;
                            fileCount++;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            shared_log::LogHook("[FGD] Exception scanning FGD directory: %s", e.what());
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_FgdCacheMutex);
        g_FgdMemoryCache = std::move(newCache);
        g_bFgdCacheLoaded.store(true, std::memory_order_release);
    }
    shared_log::LogHook("[FGD] Precompiled %zu FGD files into in-memory cache", fileCount);
}

bool TryHookTier0() {
    if (g_bTier0Hooked.load(std::memory_order_relaxed)) return true;

    HMODULE hTier0 = GetModuleHandleW(L"tier0.dll");
    if (!hTier0) {
        hTier0 = LoadLibraryW(L"tier0.dll");
    }
    if (!hTier0) return false;

    void* pVStricmpFast = (void*)GetProcAddress(hTier0, "V_stricmp_fast");
    if (pVStricmpFast) {
        if (HookManager::Instance().InstallHook(
            pVStricmpFast,
            (void*)hk_V_stricmp_fast,
            (void**)&g_o_V_stricmp_fast,
            "tier0::V_stricmp_fast"
        )) {
            g_bTier0Hooked.store(true, std::memory_order_release);
            shared_log::LogHook("[HOOK] Successfully hooked tier0!V_stricmp_fast at %p", pVStricmpFast);
            return true;
        }
    }
    return false;
}

bool TryHookFileSystem() {
    if (g_bFileSystemHooked.load(std::memory_order_relaxed)) return true;
    if (g_bFileSystemHookFailed.load(std::memory_order_relaxed)) return false;

    HMODULE hFileSystem = GetModuleHandleW(L"filesystem_stdio.dll");
    if (!hFileSystem) {
        hFileSystem = LoadLibraryW(L"filesystem_stdio.dll");
    }
    if (!hFileSystem) {
        return false;
    }

    typedef void* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
    CreateInterfaceFn pfnCreateInterface = (CreateInterfaceFn)GetProcAddress(hFileSystem, "CreateInterface");
    if (!pfnCreateInterface) {
        shared_log::LogHook("[FGD] filesystem_stdio.dll has no CreateInterface export!");
        return false;
    }

    void* pFileSystem = pfnCreateInterface("VFileSystem017", nullptr);
    if (!pFileSystem) {
        shared_log::LogHook("[FGD] CreateInterface('VFileSystem017') returned null!");
        return false;
    }

    void** vtable = *(void***)pFileSystem;
    if (!vtable) {
        shared_log::LogHook("[FGD] VFileSystem017 has null vtable!");
        return false;
    }

    void* pTargetReadFile = vtable[25];
    if (!pTargetReadFile) {
        shared_log::LogHook("[FGD] vtable[25] is null!");
        return false;
    }

    HMODULE hTier0 = GetModuleHandleW(L"tier0.dll");
    if (!hTier0) {
        hTier0 = LoadLibraryW(L"tier0.dll");
    }
    if (hTier0) {
        g_pfnCUtlBuffer_EnsureCapacity = (fnCUtlBuffer_EnsureCapacity)GetProcAddress(hTier0, "?EnsureCapacity@CUtlBuffer@@QEAAXH@Z");
        g_pfnCUtlBuffer_Put = (fnCUtlBuffer_Put)GetProcAddress(hTier0, "?Put@CUtlBuffer@@QEAAXPEBXH@Z");
        g_pfnCUtlBuffer_SeekPut = (fnCUtlBuffer_SeekPut)GetProcAddress(hTier0, "?SeekPut@CUtlBuffer@@QEAAXW4SeekType_t@1@H@Z");
        g_pfnCUtlBuffer_SeekGet = (fnCUtlBuffer_SeekGet)GetProcAddress(hTier0, "?SeekGet@CUtlBuffer@@QEAAXW4SeekType_t@1@H@Z");
    }

    if (!g_pfnCUtlBuffer_EnsureCapacity || !g_pfnCUtlBuffer_Put || !g_pfnCUtlBuffer_SeekPut || !g_pfnCUtlBuffer_SeekGet) {
        shared_log::LogHook("[FGD] Failed to resolve CUtlBuffer exports from tier0.dll!");
        g_bFileSystemHookFailed.store(true, std::memory_order_release);
        MessageBoxW(
            NULL,
            L"CS2 创意工坊工具汉化模块未能成功解析 tier0.dll 中的 CUtlBuffer 接口。\n\n"
            L"FGD 实体汉化可能无法在内存中完全生效，请检查游戏是否发生更新。",
            L"CS2 Workshop Tools Localizer - 警告",
            MB_ICONWARNING | MB_OK
        );
        return false;
    }

    bool ok = HookManager::Instance().InstallHook(
        pTargetReadFile,
        (void*)hk_ReadFileToBuffer,
        (void**)&g_o_ReadFileToBuffer,
        "VFileSystem017::ReadFileToBuffer"
    );

    if (ok) {
        g_bFileSystemHooked.store(true, std::memory_order_release);
        shared_log::LogHook("[FGD] Successfully hooked VFileSystem017::ReadFileToBuffer at %p", pTargetReadFile);
    } else {
        g_bFileSystemHookFailed.store(true, std::memory_order_release);
        shared_log::LogHook("[FGD] Failed to hook VFileSystem017::ReadFileToBuffer at %p!", pTargetReadFile);
        MessageBoxW(
            NULL,
            L"CS2 创意工坊工具汉化模块未能成功挂钩 VFileSystem017::ReadFileToBuffer。\n\n"
            L"原因：当前游戏版本的文件系统接口特征可能发生了变动。\n"
            L"FGD 实体汉化将临时降级为原版英文显示，建议检查启动器是否有新版本更新。",
            L"CS2 Workshop Tools Localizer - 警告",
            MB_ICONWARNING | MB_OK
        );
    }
    TryHookTier0();
    return ok;
}

} // namespace qm
