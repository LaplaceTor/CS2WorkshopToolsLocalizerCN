#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace qm {

enum class TranslationSource {
    StaticUI,
    ItemWidget,
    Painter
};

struct ModuleRangeEntry {
    uintptr_t base;
    uintptr_t end;
    wchar_t stem[32];
};

constexpr size_t MAX_CACHED_MODULE_RANGES = 128;

struct ModuleRangeSnapshot {
    size_t count;
    ModuleRangeEntry entries[MAX_CACHED_MODULE_RANGES];
};

std::wstring GetBinDirectory();

bool ReadAppDirPointer(const std::wstring& binDir,
                       std::wstring& outAppDir,
                       bool& outUseMachineTrans,
                       std::wstring* outPointerPath = nullptr);

bool GetCallerModuleName(void* callerAddr, wchar_t* outStem, size_t stemMaxLen);
bool ResolveCallerModuleName(void* directCallerAddr, wchar_t* outStem, size_t stemMaxLen);
bool AddModuleRange(HMODULE hMod, const std::wstring& overrideStem = L"");
bool RemoveModuleRangeByBase(uintptr_t base);
void InvalidateUnloadedModules();
void ScanKnownToolModules();

void LoadMasterTranslations();
void EnsureDictionaryLoaded();
void ClearAllTranslations();
bool IsTranslationEnabled();
void SetTranslationEnabled(bool enabled);

bool FindTranslationScoped(void* callerAddr, const char* text, std::string& outResult, TranslationSource source = TranslationSource::StaticUI);
bool FindTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult, TranslationSource source = TranslationSource::StaticUI);

bool FindReverseTranslationScoped(void* callerAddr, const char* text, std::string& outResult);
bool FindReverseTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult);

bool IsFilePathOrIdentifier(const std::string& s);

void RegisterProtectedFgdIoNames(const std::vector<std::string>& names);
void ExtractAndRegisterFgdIoNames(const std::string& fgdContent);
bool IsProtectedFgdIoName(const std::string& s);

}
