#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace qm {

bool LoadFgdDictionariesFromDisk(bool bForce = false);
void EnsureFgdDictLoaded();
void PrecompileFgdBuffers();

bool TryHookFileSystem();
bool TryHookTier0();

bool MatchFgdStringPair(const char* s1, const char* s2);

}
