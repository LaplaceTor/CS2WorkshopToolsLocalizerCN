#pragma once

#include <windows.h>

class ProcessGuard {
public:
    static void EnforceCs2ToolsEnvironment();

    static bool HasToolsArgument(const wchar_t* cmdLine);

    static bool IsCs2HostProcess();
};
