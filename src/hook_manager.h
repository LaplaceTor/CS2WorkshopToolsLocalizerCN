#pragma once
#include <windows.h>

class HookManager {
public:
    // 初始化 MinHook 引擎与系统异常守卫 / 模块监控
    static bool Initialize();

    // 搜索并安装所有 Qt 模块（Qt5Core, Qt5Widgets, Qt5Gui）的函数 Hook
    static bool InstallHooks();

    // 禁用/移除当前安装的所有 Hook
    static bool RemoveHooks();

    // 卸载 Hook 引擎并清理全局资源
    static void Shutdown();

    // 通用 Hook 创建与启用辅助函数
    static bool CreateAndEnableHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName = nullptr);
};

