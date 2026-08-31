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

bool HookManager::UninstallHook(void* pTarget) {
    if (!pTarget) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    MH_DisableHook(pTarget);
    MH_RemoveHook(pTarget);

    for (auto it = m_hooks.begin(); it != m_hooks.end(); ++it) {
        if (it->target == pTarget) {
            m_hooks.erase(it);
            break;
        }
    }
    return true;
}

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

    // 1. 注销 DLL 加载通知回调 (Unregister callback)
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

    // 2. 禁用所有 Hook (Disable Hook)
    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            MH_DisableHook(hook.target);
            hook.enabled = false;
        }
    }
    MH_DisableHook(MH_ALL_HOOKS);

    // 3. 移除所有 Hook (Remove Hook)
    for (auto& hook : m_hooks) {
        if (hook.target) {
            MH_RemoveHook(hook.target);
        }
    }
    m_hooks.clear();

    // 4. 注销全局 VEH 异常过滤器
    if (m_pVehHandle != nullptr) {
        RemoveVectoredExceptionHandler(m_pVehHandle);
        m_pVehHandle = nullptr;
    }

    // 5. 卸载 MinHook 并释放所有蹦床（trampoline）内存空间 (MinHook Uninitialize)
    MH_Uninitialize();

    m_bInitialized.store(false);
}

bool HookManager::IsInitialized() const {
    return m_bInitialized.load();
}

size_t HookManager::GetHookCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hooks.size();
}

void HookManager::EmergencyDisableAllHooks() {
    // 1. 全局原子禁用所有 Hook（MinHook 内部自带原子临界区保护，完全不依赖 m_hooks）
    MH_DisableHook(MH_ALL_HOOKS);

    // 2. 尝试获取互斥锁；若 try_lock 失败则绝对不访问 m_hooks 容器以避免数据竞争
    if (!m_mutex.try_lock()) {
        return;
    }

    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            hook.enabled = false;
        }
    }
    m_mutex.unlock();
}

bool HookManager::IsHookAddress(void* addr) {
    if (!addr) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& hook : m_hooks) {
        if (hook.detour == addr || hook.target == addr) {
            return true;
        }
    }
    return false;
}



