#pragma once
#include <windows.h>
#include <winternl.h>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

#ifndef LDR_DLL_NOTIFICATION_REASON_LOADED
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
#define LDR_DLL_NOTIFICATION_REASON_UNLOADED 2

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    PLDR_DLL_NOTIFICATION_DATA NotificationData,
    PVOID Context
);
#endif

struct HookEntry {
    void* target = nullptr;
    void* detour = nullptr;
    void** original = nullptr;
    std::string name;
    bool enabled = false;
};

class HookManager {
public:
    static HookManager& Instance();

    // 1. 初始化 MinHook 引擎与异常守卫 (VEH)
    bool Initialize(PVECTORED_EXCEPTION_HANDLER pVehHandler = nullptr);

    // 2. 注册 DLL 加载/卸载全局通知回调（LdrRegisterDllNotification）
    bool RegisterDllNotification(PLDR_DLL_NOTIFICATION_FUNCTION pfnCallback, PVOID pContext = nullptr);

    // 3. 显式注销 DLL 通知回调（LdrUnregisterDllNotification）
    void UnregisterDllNotification();

    // 4. 显式注销 VEH 异常过滤器
    void UnregisterVeh();

    // 5. 安装并启用单个 Hook，统一记录到 m_hooks
    bool InstallHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName = nullptr);

    // 6. 禁用并移除所有 Hook（保留 MinHook 上下文）
    void UninstallHooks();

    // 7. 完整的统一生命周期退出：
    //    LdrUnregisterDllNotification -> disable/remove hooks -> free trampolines -> RemoveVectoredExceptionHandler -> MH_Uninitialize
    void Shutdown();

    // 8. 紧急停用所有 Hook（供 VEH 异常处理使用，防止死锁与循环崩溃）
    void EmergencyDisableAllHooks();

    // 9. 检查指定地址是否属于已安装的 Hook Detour 或 Target
    bool IsHookAddress(void* addr);

    // 状态查询
    bool IsInitialized() const;
    size_t GetHookCount() const;

    // 静态便捷方法
    static bool CreateAndEnableHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName = nullptr) {
        return Instance().InstallHook(pTarget, pDetour, ppOriginal, hookName);
    }
    static void ShutdownAll() { Instance().Shutdown(); }

private:
    HookManager() = default;
    ~HookManager();

    std::mutex m_mutex;
    std::vector<HookEntry> m_hooks;
    void* m_pVehHandle = nullptr;
    void* m_pDllNotificationCookie = nullptr;
    std::atomic<bool> m_bInitialized{false};
};



