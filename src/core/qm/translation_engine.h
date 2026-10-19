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
    StaticUI,       // QMetaObject::tr, QAction, QAbstractButton, QLabel, QWidget::setWindowTitle, QGroupBox::setTitle
    ItemWidget,     // QTableWidgetItem, QListWidgetItem, QTreeWidgetItem, QComboBox
    Painter         // QPainter::drawText
};

// 预扫描调用者模块地址区间快照（100% 纯无锁 Lock-Free Snapshot，VEH / Hook 绝对安全）
struct ModuleRangeEntry {
    uintptr_t base;
    uintptr_t end;
    wchar_t stem[32]; // 固定长度 POD，无堆分配，支持安全并发复制
};

constexpr size_t MAX_CACHED_MODULE_RANGES = 128;

struct ModuleRangeSnapshot {
    size_t count;
    ModuleRangeEntry entries[MAX_CACHED_MODULE_RANGES];
};

// 获取 CS2 根二进制目录 (game/bin/win64/)
std::wstring GetBinDirectory();

// 模块地址与快照查询管理
bool GetCallerModuleName(void* callerAddr, wchar_t* outStem, size_t stemMaxLen);
bool ResolveCallerModuleName(void* directCallerAddr, wchar_t* outStem, size_t stemMaxLen);
bool AddModuleRange(HMODULE hMod, const std::wstring& overrideStem = L"");
bool RemoveModuleRangeByBase(uintptr_t base);
void InvalidateUnloadedModules();
void ScanKnownToolModules();

// 词典引擎生命周期与控制
void LoadMasterTranslations();
void EnsureDictionaryLoaded();
void ClearAllTranslations();
bool IsTranslationEnabled();
void SetTranslationEnabled(bool enabled);

// 核心翻译查询接口
bool FindTranslationScoped(void* callerAddr, const char* text, std::string& outResult, TranslationSource source = TranslationSource::StaticUI);
bool FindTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult, TranslationSource source = TranslationSource::StaticUI);

// 反向翻译（还原英文）
bool FindReverseTranslationScoped(void* callerAddr, const char* text, std::string& outResult);
bool FindReverseTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult);

// 文件路径与文件名保护检测
bool IsFilePathOrIdentifier(const std::string& s);

} // namespace qm
