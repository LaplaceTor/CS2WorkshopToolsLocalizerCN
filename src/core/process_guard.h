#pragma once

#include <windows.h>

class ProcessGuard {
public:
    // 检查宿主进程环境：若宿主为 cs2.exe 且未携带 -tools 参数，则弹出警告并强制中止进程
    static void EnforceCs2ToolsEnvironment();

    // 辅助判定函数：判断给定命令行是否包含 -tools 参数
    static bool HasToolsArgument(const wchar_t* cmdLine);

    // 辅助判定函数：判断当前进程是否为 cs2.exe
    static bool IsCs2HostProcess();
};
