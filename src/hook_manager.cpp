#include "hook_manager.h"
#include "../third_party/minhook/include/MinHook.h"
#include <stdio.h>
#include <algorithm>

HookManager& HookManager::Instance() {
    static HookManager s_instance;
    return s_instance;
}

HookManager::~HookManager() {
    Shutdown();
}

bool HookManager::Initialize(PVECTORED_EXCEPTION_HANDLER pVehHandler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bInitialized.load()) {
        return true;
    }

    // 1. 初始化 MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }

    // 2. 统一注册并管理 VEH 异常守卫
    if (pVehHandler != nullptr && m_pVehHandle == nullptr) {
        m_pVehHandle = AddVectoredExceptionHandler(1, pVehHandler);
    }

    m_bInitialized.store(true);
    return true;
}

bool HookManager::InstallHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName) {
    if (!pTarget || !pDetour) {
        return false;
    }

    if (!m_bInitialized.load()) {
        if (!Initialize(nullptr)) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    MH_STATUS createStatus = MH_CreateHook(pTarget, pDetour, ppOriginal);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        return false;
    }

    MH_STATUS enableStatus = MH_EnableHook(pTarget);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        MH_RemoveHook(pTarget);
        if (ppOriginal) {
            *ppOriginal = nullptr;
        }
        return false;
    }

    HookEntry entry;
    entry.target = pTarget;
    entry.detour = pDetour;
    entry.original = ppOriginal;
    entry.name = hookName ? hookName : "";
    entry.enabled = true;

    m_hooks.push_back(entry);
    return true;
}

void HookManager::UninstallHooks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bInitialized.load()) {
        return;
    }

    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            MH_DisableHook(hook.target);
            MH_RemoveHook(hook.target);
            hook.enabled = false;
        }
    }
    m_hooks.clear();
}

typedef NTSTATUS (NTAPI *pfnLdrRegisterDllNotification)(
    ULONG Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID *Cookie
);

typedef NTSTATUS (NTAPI *pfnLdrUnregisterDllNotification)(
    PVOID Cookie
);

bool HookManager::RegisterDllNotification(PLDR_DLL_NOTIFICATION_FUNCTION pfnCallback, PVOID pContext) {
    if (!pfnCallback) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pDllNotificationCookie != nullptr) {
        return true;
    }

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return false;

    pfnLdrRegisterDllNotification pRegister = 
        (pfnLdrRegisterDllNotification)GetProcAddress(hNtdll, "LdrRegisterDllNotification");
    if (!pRegister) return false;

    PVOID cookie = nullptr;
    NTSTATUS status = pRegister(0, pfnCallback, pContext, &cookie);
    if (status == 0) {
        m_pDllNotificationCookie = cookie;
        return true;
    }
    return false;
}

void HookManager::UnregisterDllNotification() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pDllNotificationCookie != nullptr) {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            pfnLdrUnregisterDllNotification pUnregister = 
                (pfnLdrUnregisterDllNotification)GetProcAddress(hNtdll, "LdrUnregisterDllNotification");
            if (pUnregister) {
                pUnregister(m_pDllNotificationCookie);
            }
        }
        m_pDllNotificationCookie = nullptr;
    }
}

void HookManager::UnregisterVeh() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pVehHandle != nullptr) {
        RemoveVectoredExceptionHandler(m_pVehHandle);
        m_pVehHandle = nullptr;
    }
}

void HookManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bInitialized.load()) {
        return;
    }

    // 1. 注销 DLL 加载通知回调
    if (m_pDllNotificationCookie != nullptr) {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            pfnLdrUnregisterDllNotification pUnregister = 
                (pfnLdrUnregisterDllNotification)GetProcAddress(hNtdll, "LdrUnregisterDllNotification");
            if (pUnregister) {
                pUnregister(m_pDllNotificationCookie);
            }
        }
        m_pDllNotificationCookie = nullptr;
    }

    // 2. 禁用并移除每一个已注册 Hook
    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            MH_DisableHook(hook.target);
            MH_RemoveHook(hook.target);
            hook.enabled = false;
        }
    }
    m_hooks.clear();

    // 3. 全局保底禁用所有 Hook
    MH_DisableHook(MH_ALL_HOOKS);

    // 4. 注销全局 VEH 异常过滤器
    if (m_pVehHandle != nullptr) {
        RemoveVectoredExceptionHandler(m_pVehHandle);
        m_pVehHandle = nullptr;
    }

    // 5. 卸载 MinHook 并释放所有蹦床（trampoline）内存空间
    MH_Uninitialize();

    m_bInitialized.store(false);
}

bool HookManager::IsInitialized() const {
    return m_bInitialized.load();
}

size_t HookManager::GetHookCount() const {
    return m_hooks.size();
}



