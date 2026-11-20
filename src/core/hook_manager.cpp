#include "core/hook_manager.h"
#include "MinHook.h"
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
    return InitializeLocked(pVehHandler);
}

bool HookManager::InitializeLocked(PVECTORED_EXCEPTION_HANDLER pVehHandler) {
    if (m_bInitialized.load()) {
        return true;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }

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

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bInitialized.load() && !InitializeLocked(nullptr)) {
        return false;
    }

    auto it = std::find_if(m_hooks.begin(), m_hooks.end(), [pTarget](const HookEntry& h) {
        return h.target == pTarget;
    });

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
        if (it != m_hooks.end()) {
            m_hooks.erase(it);
        }
        return false;
    }

    if (it != m_hooks.end()) {
        it->detour = pDetour;
        it->original = ppOriginal;
        if (hookName && hookName[0] != '\0') it->name = hookName;
        it->enabled = true;
    } else {
        HookEntry entry;
        entry.target = pTarget;
        entry.detour = pDetour;
        entry.original = ppOriginal;
        entry.name = hookName ? hookName : "";
        entry.enabled = true;
        m_hooks.push_back(entry);
    }
    return true;
}

bool HookManager::UninstallHook(void* pTarget) {
    if (!pTarget) return false;
    std::lock_guard<std::mutex> lock(m_mutex);

    MH_STATUS disStatus = MH_DisableHook(pTarget);
    (void)disStatus;

    MH_STATUS remStatus = MH_RemoveHook(pTarget);
    if (remStatus == MH_ERROR_ENABLED) {
        MH_DisableHook(pTarget);
        MH_RemoveHook(pTarget);
    }

    for (auto it = m_hooks.begin(); it != m_hooks.end(); ++it) {
        if (it->target == pTarget) {
            it->enabled = false;
            m_hooks.erase(it);
            break;
        }
    }
    return true;
}

void HookManager::UninstallHooks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bInitialized.load()) {
        return;
    }

    for (auto& hook : m_hooks) {
        if (hook.target) {
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
    UnregisterDllNotificationLocked();
}

void HookManager::UnregisterDllNotificationLocked() {
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
    UnregisterVehLocked();
}

void HookManager::UnregisterVehLocked() {
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

    UnregisterDllNotificationLocked();

    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            MH_DisableHook(hook.target);
            hook.enabled = false;
        }
    }
    MH_DisableHook(MH_ALL_HOOKS);

    for (auto& hook : m_hooks) {
        if (hook.target) {
            MH_RemoveHook(hook.target);
        }
    }
    m_hooks.clear();

    UnregisterVehLocked();

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
    MH_DisableHook(MH_ALL_HOOKS);

    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    for (auto& hook : m_hooks) {
        if (hook.enabled && hook.target) {
            hook.enabled = false;
        }
    }
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

