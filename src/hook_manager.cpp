#include "hook_manager.h"
#include "../third_party/minhook/include/MinHook.h"
#include <stdio.h>

static bool g_bHookManagerInitialized = false;

bool HookManager::Initialize() {
    if (g_bHookManagerInitialized) {
        return true;
    }

    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        g_bHookManagerInitialized = true;
        return true;
    }

    return false;
}

bool HookManager::CreateAndEnableHook(void* pTarget, void* pDetour, void** ppOriginal, const char* hookName) {
    if (!pTarget || !pDetour) {
        return false;
    }

    if (!g_bHookManagerInitialized) {
        if (!Initialize()) {
            return false;
        }
    }

    MH_STATUS createStatus = MH_CreateHook(pTarget, pDetour, ppOriginal);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        return false;
    }

    MH_STATUS enableStatus = MH_EnableHook(pTarget);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        return false;
    }

    return true;
}

bool HookManager::RemoveHooks() {
    if (!g_bHookManagerInitialized) {
        return true;
    }

    MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
    return (status == MH_OK);
}

void HookManager::Shutdown() {
    if (!g_bHookManagerInitialized) {
        return;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_bHookManagerInitialized = false;
}

