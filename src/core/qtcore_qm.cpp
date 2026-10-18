#include <windows.h>
#include <winternl.h>
#include <atomic>
#include <mutex>
#include <cstdio>

#include "core/qm/translation_engine.h"
#include "core/qm/fgd_memory_hook.h"
#include "core/qm/qt_hooks.h"

#include "core/hammer_ipc.h"
#include "core/hook_manager.h"
#include "core/process_guard.h"
#include "core/shared_log.h"
#include "hde/hde64.h"

namespace {

HMODULE g_hModule = NULL;

std::atomic<bool> g_bTranslatorInitialized{false};
std::atomic<bool> g_bTranslatorInitFailed{false};
std::mutex g_TranslatorInitMutex;

HANDLE g_hToolsHookThread = NULL;
HANDLE g_hWakeHookEvent = NULL;
std::atomic<bool> g_bStopHookThread{false};

// VEH 空写跳过守卫计数（100% 原子操作，保持 VEH 内 0 锁 0 IO）
std::atomic<uint32_t> g_NullGuardSkipCount{0};
std::atomic<uint32_t> g_NullGuardWindowCount{0};
std::atomic<ULONGLONG> g_NullGuardWindowStart{0};
std::atomic<bool> g_NullGuardTripped{false};

// 切换语言：广播重绘所有 CS2/Hammer 窗口
void ToggleLanguage() {
    bool current = qm::IsTranslationEnabled();
    bool next = !current;
    qm::SetTranslationEnabled(next);
    shared_log::LogHook("[LANG] Translation toggled: %s", next ? "ENABLED (Chinese)" : "DISABLED (English)");

    // 自动触发 Hammer 原生 'Reload .FGD Files'，重读实体定义（中英双向即刻生效）
    qm::TriggerReloadFgdAction();

    // 异步广播重绘所有 CS2/Hammer 窗口（RDW_INVALIDATE 避免后台线程同步挂起 GUI 线程）
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) {
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
        }
        return TRUE;
    }, 0);
}

// 磁盘热重载：重新载入词典并刷新预编译 FGD
void ReloadTranslations() {
    shared_log::LogHook("[RELOAD] Reloading translations from disk...");
    qm::ClearAllTranslations();
    qm::LoadMasterTranslations();

    // 强制从磁盘重新解析 FGD 词典与覆盖词典
    qm::LoadFgdDictionariesFromDisk(true);

    // 重新预编译所有 FGD 内存缓存
    qm::PrecompileFgdBuffers();

    // 自动触发 Hammer 原生 'Reload .FGD Files'
    qm::TriggerReloadFgdAction();

    // 广播异步重绘所有 CS2/Hammer 窗口
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) {
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
        }
        return TRUE;
    }, 0);

    shared_log::LogHook("[RELOAD] Translations and FGD buffers reloaded successfully");
}

LRESULT CALLBACK LocalizerIpcWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == HammerIpc::kMsgToggleLang) {
        shared_log::LogHook("[IPC] Received WM_LOCALIZER_TOGGLE_LANG from launcher");
        ToggleLanguage();
        return qm::IsTranslationEnabled() ? 1 : 2;
    } else if (uMsg == HammerIpc::kMsgReloadDict) {
        shared_log::LogHook("[IPC] Received WM_LOCALIZER_RELOAD_DICT from launcher");
        ReloadTranslations();
        return 1;
    } else if (uMsg == HammerIpc::kMsgQueryStatus) {
        return qm::IsTranslationEnabled() ? 1 : 2;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

DWORD WINAPI ToolsHookThread(LPVOID lpParam) {
    shared_log::LogHook("[THREAD] ToolsHookThread started");

    // 注册并创建隐藏 IPC 消息窗口（供启动器远程发送控制指令）
    WNDCLASSEXW ipcWc = { sizeof(WNDCLASSEXW) };
    ipcWc.lpfnWndProc = LocalizerIpcWndProc;
    ipcWc.hInstance = g_hModule;
    ipcWc.lpszClassName = HammerIpc::kWindowClass;
    RegisterClassExW(&ipcWc);

    HWND hIpcWnd = CreateWindowExW(0, HammerIpc::kWindowClass, HammerIpc::kWindowTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hModule, NULL);
    shared_log::LogHook("[IPC] Message window created: %p", hIpcWnd);

    // 在后台独立线程中预加载字典与扫描工具模块
    qm::EnsureDictionaryLoaded();
    qm::ScanKnownToolModules();

    // 预编译内存中的 FGD 翻译缓存并挂钩文件系统
    qm::PrecompileFgdBuffers();
    qm::TryHookFileSystem();

    while (!g_bStopHookThread.load(std::memory_order_relaxed)) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!qm::AreWidgetsHooked() || !qm::AreGuiHooked()) {
            qm::TryHookQtToolsModules();
        }
        qm::TryHookFileSystem();
        qm::ScanKnownToolModules();

        // 汇总记录 VEH 空写守卫跳过情况
        {
            static uint32_t s_lastReported = 0;
            static bool s_trippedLogged = false;
            uint32_t skipTotal = g_NullGuardSkipCount.load(std::memory_order_relaxed);
            if (skipTotal != s_lastReported) {
                shared_log::LogHook("[GUARD] null-write guard skip count: %lu (+%lu this session, tripped=%d)",
                    (unsigned long)skipTotal,
                    (unsigned long)(skipTotal - s_lastReported),
                    (int)g_NullGuardTripped.load(std::memory_order_relaxed));
                s_lastReported = skipTotal;
            }
            if (!s_trippedLogged && g_NullGuardTripped.load(std::memory_order_relaxed)) {
                shared_log::LogHook("[GUARD] FATAL: null-write guard tripped (>=1000 skips within 10s window) — further access violations will propagate to the host!");
                s_trippedLogged = true;
            }
        }

        DWORD timeout = (qm::AreWidgetsHooked() && qm::AreGuiHooked()) ? 500 : 50;
        MsgWaitForMultipleObjectsEx(1, &g_hWakeHookEvent, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (hIpcWnd) {
        DestroyWindow(hIpcWnd);
    }
    UnregisterClassW(HammerIpc::kWindowClass, g_hModule);

    shared_log::LogHook("[THREAD] ToolsHookThread finishing (stopRequested=%d)", g_bStopHookThread.load());
    return 0;
}

VOID CALLBACK OnDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context) {
    if ((NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED || NotificationReason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) && g_hWakeHookEvent) {
        SetEvent(g_hWakeHookEvent);
    }
}

bool IsToolAddress(void* rip) {
    if (!rip) return false;
    wchar_t stem[64] = {0};
    if (qm::GetCallerModuleName(rip, stem, 64)) {
        if (wcscmp(stem, L"hammer") == 0 || wcscmp(stem, L"modeldoc_editor") == 0 || wcscmp(stem, L"pet") == 0 ||
            wcscmp(stem, L"met") == 0 || wcscmp(stem, L"sfm") == 0 || wcscmp(stem, L"postprocessing") == 0 ||
            wcscmp(stem, L"smartprops_editor") == 0 || wcscmp(stem, L"pulse_editor") == 0 ||
            wcsstr(stem, L"subtool") != nullptr) {
            return true;
        }
    }
    return false;
}

LONG WINAPI DiagnosticCrashLoggerVEH(PEXCEPTION_POINTERS pExceptionInfo) {
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord || !pExceptionInfo->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR faultAddr = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        void* rip = (void*)pExceptionInfo->ContextRecord->Rip;

        if (faultAddr < 0x10000 && IsToolAddress(rip)) {
            if (!g_NullGuardTripped.load(std::memory_order_relaxed)) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG windowStart = g_NullGuardWindowStart.load(std::memory_order_relaxed);
                if (windowStart == 0 || now - windowStart >= 10000) {
                    g_NullGuardWindowStart.store(now, std::memory_order_relaxed);
                    g_NullGuardWindowCount.store(0, std::memory_order_relaxed);
                }
                uint32_t windowCount = g_NullGuardWindowCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (windowCount < 1000) {
                    hde64s hs;
                    unsigned int len = hde64_disasm(rip, &hs);
                    if (!(hs.flags & F_ERROR) && len > 0 && len <= 15) {
                        pExceptionInfo->ContextRecord->Rip += len;
                        g_NullGuardSkipCount.fetch_add(1, std::memory_order_relaxed);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                } else {
                    g_NullGuardTripped.store(true, std::memory_order_relaxed);
                }
            }
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

extern "C" __declspec(dllexport) bool InitializeTranslator() {
    if (g_bTranslatorInitialized.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_bTranslatorInitFailed.load(std::memory_order_relaxed)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_TranslatorInitMutex);
    if (g_bTranslatorInitialized.load(std::memory_order_relaxed)) {
        return true;
    }
    if (g_bTranslatorInitFailed.load(std::memory_order_relaxed)) {
        return false;
    }

    shared_log::LogHook("[INIT] InitializeTranslator invoked outside Loader Lock");

    if (!HookManager::Instance().Initialize(DiagnosticCrashLoggerVEH)) {
        shared_log::LogHook("[INIT] HookManager::Initialize failed!");
        g_bTranslatorInitFailed.store(true, std::memory_order_release);
        return false;
    }

    if (!qm::InitializeQtCoreHooks()) {
        shared_log::LogHook("[INIT] InitializeQtCoreHooks failed!");
        g_bTranslatorInitFailed.store(true, std::memory_order_release);
        return false;
    }

    // 立即挂钩 VFileSystem017::ReadFileToBuffer
    qm::TryHookFileSystem();

    if (!g_hWakeHookEvent) {
        g_hWakeHookEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    HookManager::Instance().RegisterDllNotification(OnDllNotification);

    if (!g_hToolsHookThread) {
        g_hToolsHookThread = CreateThread(NULL, 0, ToolsHookThread, NULL, 0, NULL);
        if (!g_hToolsHookThread) {
            shared_log::LogHook("[INIT] CreateThread for ToolsHookThread failed!");
            g_bTranslatorInitFailed.store(true, std::memory_order_release);
            return false;
        }
        shared_log::LogHook("[INIT] ToolsHookThread spawned successfully");
    }

    g_bTranslatorInitialized.store(true, std::memory_order_release);
    return true;
}

extern "C" __declspec(dllexport) void ShutdownTranslator() {
    std::lock_guard<std::mutex> lock(g_TranslatorInitMutex);
    if (!g_bTranslatorInitialized.load(std::memory_order_relaxed)) {
        return;
    }
    g_bTranslatorInitialized.store(false, std::memory_order_release);

    shared_log::LogHook("[SHUTDOWN] ShutdownTranslator invoked");

    g_bStopHookThread.store(true, std::memory_order_release);
    if (g_hWakeHookEvent) {
        SetEvent(g_hWakeHookEvent);
    }

    if (g_hToolsHookThread) {
        DWORD waitRes = WaitForSingleObject(g_hToolsHookThread, INFINITE);
        if (waitRes != WAIT_OBJECT_0) {
            shared_log::LogHook("[SHUTDOWN] FATAL: Background worker thread wait failed (code=0x%08X), aborting unhook to prevent memory corruption!", waitRes);
            return;
        }
        CloseHandle(g_hToolsHookThread);
        g_hToolsHookThread = NULL;
    }
    if (g_hWakeHookEvent) {
        CloseHandle(g_hWakeHookEvent);
        g_hWakeHookEvent = NULL;
    }

    HookManager::Instance().Shutdown();
    shared_log::LogHook("[SHUTDOWN] ShutdownTranslator completed cleanly");

    shared_log::ShutdownSharedLog();
}

extern "C" __declspec(dllexport) void InitQtCoreQmTranslator() {
    InitializeTranslator();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        ProcessGuard::EnforceCs2ToolsEnvironment();
    }
    return TRUE;
}
