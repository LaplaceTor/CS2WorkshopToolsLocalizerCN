#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qm {

bool InitializeQtCoreHooks();

bool TryHookQtToolsModules();

bool AreWidgetsHooked();
bool AreGuiHooked();

void TriggerReloadFgdAction();

extern "C" __declspec(dllexport) void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);
extern "C" __declspec(dllexport) void* __fastcall tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);

}
