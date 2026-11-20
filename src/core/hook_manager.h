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

    bool Initialize(PVECTORED_EXCEPTION_HANDLER pVehHandler = nullptr);

    bool RegisterDllNotification(PLDR_DLL_NOTIFICATION_FUNCTION pfnCallback, PVOID pContext = nullptr);

    void UnregisterDllNotification();

    void UnregisterVeh();

    bool InstallHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName = nullptr);

    bool UninstallHook(void* pTarget);

    void UninstallHooks();

    void Shutdown();

    void EmergencyDisableAllHooks();

    bool IsHookAddress(void* addr);

    bool IsInitialized() const;
    size_t GetHookCount() const;

    static bool CreateAndEnableHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName = nullptr) {
        return Instance().InstallHook(pTarget, pDetour, ppOriginal, hookName);
    }
    static void ShutdownAll() { Instance().Shutdown(); }

private:
    HookManager() = default;
    ~HookManager();

    bool InitializeLocked(PVECTORED_EXCEPTION_HANDLER pVehHandler);
    void UnregisterDllNotificationLocked();
    void UnregisterVehLocked();

    mutable std::mutex m_mutex;
    std::vector<HookEntry> m_hooks;
    void* m_pVehHandle = nullptr;
    void* m_pDllNotificationCookie = nullptr;
    std::atomic<bool> m_bInitialized{false};
};

