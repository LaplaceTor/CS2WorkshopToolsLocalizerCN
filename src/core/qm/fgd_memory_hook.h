#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace qm {

// 纯内存 FGD 词典加载与预编译
bool LoadFgdDictionariesFromDisk(bool bForce = false);
void EnsureFgdDictLoaded();
void PrecompileFgdBuffers();

// 文件系统与 tier0 挂钩
bool TryHookFileSystem();
bool TryHookTier0();

// 供 Tier0 比较兼容性检查的 FGD 词条匹配
bool MatchFgdStringPair(const char* s1, const char* s2);

} // namespace qm
