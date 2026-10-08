#include "core/process_guard.h"
#include <cwctype>
#include <string>

bool ProcessGuard::IsCs2HostProcess() {
    wchar_t exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    const wchar_t* pFileName = exePath;
    for (DWORD i = 0; i < len; ++i) {
        if (exePath[i] == L'\\' || exePath[i] == L'/') {
            pFileName = &exePath[i + 1];
        }
    }

    return (_wcsicmp(pFileName, L"cs2.exe") == 0);
}

bool ProcessGuard::HasToolsArgument(const wchar_t* cmdLine) {
    if (!cmdLine || !*cmdLine) {
        return false;
    }

    const wchar_t* p = cmdLine;
    while (*p) {
        // 跳过前导空白字符
        while (*p && iswspace(*p)) {
            ++p;
        }
        if (!*p) {
            break;
        }

        // 解析单个参数 Token
        std::wstring token;
        bool inQuotes = false;

        while (*p) {
            if (*p == L'"') {
                inQuotes = !inQuotes;
                ++p;
            } else if (!inQuotes && iswspace(*p)) {
                break;
            } else {
                token.push_back(*p);
                ++p;
            }
        }

        // 判定 Token 是否为 -tools 或 /tools（不区分大小写）
        if (_wcsicmp(token.c_str(), L"-tools") == 0 || _wcsicmp(token.c_str(), L"/tools") == 0) {
            return true;
        }
    }

    return false;
}

void ProcessGuard::EnforceCs2ToolsEnvironment() {
    if (!IsCs2HostProcess()) {
        return;
    }

    const wchar_t* cmdLine = GetCommandLineW();
    if (!HasToolsArgument(cmdLine)) {
        const wchar_t* msg =
            L"CS2 汉化补丁安全防护：\n"
            L"检测到当前 CS2 正在普通游戏模式下启动（未携带 -tools 参数）。\n\n"
            L"由于底层 Qt 核心已被修改用于 Hammer 汉化，直接进行匹配或进入游戏可能导致崩溃或被判定违规。\n\n"
            L"程序已自动中止启动。请通过汉化启动器点击「还原原生」后再游玩 CS2，或使用启动器进入创意工坊工具模式。";
        const wchar_t* title = L"CS2 汉化安全防护";

        MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_TOPMOST | MB_SETFOREGROUND);
        TerminateProcess(GetCurrentProcess(), 1);
    }
}
