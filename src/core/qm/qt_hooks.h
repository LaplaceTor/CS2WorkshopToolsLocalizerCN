#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qm {

// 初始化并挂钩 Qt 核心 (Qt5Core: QMetaObject::tr 等基础导出解析)
bool InitializeQtCoreHooks();

// 挂钩 Qt5Widgets 与 Qt5Gui 界面控件
bool TryHookQtToolsModules();

// 检查是否已完成 Widgets/Gui 挂钩
bool AreWidgetsHooked();
bool AreGuiHooked();

// 自动探测并触发 Hammer 原生 'Reload .FGD Files' QAction
void TriggerReloadFgdAction();

// EAT 延迟引导入口 tr / hk_QMetaObject_tr
extern "C" __declspec(dllexport) void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);
extern "C" __declspec(dllexport) void* __fastcall tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);

} // namespace qm
