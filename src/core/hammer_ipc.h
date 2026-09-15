#pragma once

// 与运行中的 HAMMER 进程通信。
//
// HAMMER 侧会注册一个「仅消息窗口」(HWND_MESSAGE) 作为 IPC 端点，
// 启动器通过向其投递自定义 WM_ 消息来下发指令（切换语言 / 热重载词典）。
//
// 原先窗口查找与消息投递散落在 mainwindow 的两处，这里收口为单点。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class HammerIpc {
public:
    // IPC 窗口的类名与标题（必须与 HAMMER 侧注册时一致）
    static constexpr const wchar_t* kWindowClass = L"CS2_HAMMER_LOCALIZER_IPC";
    static constexpr const wchar_t* kWindowTitle = L"CS2_Hammer_Localizer_MsgWnd";

    // 自定义消息 ID
    static constexpr unsigned int kMsgToggleLang = WM_USER + 101;  // 切换原文 / 翻译
    static constexpr unsigned int kMsgReloadDict = WM_USER + 102;  // 热重载 FGD + Qt 词典

    // 查找 HAMMER 注册的 IPC 窗口。
    // 优先在仅消息窗口(HWND_MESSAGE)中查找，失败时退回全局查找。未找到返回 nullptr。
    static HWND FindIpcWindow();

    // 向 HAMMER 投递一条命令；窗口不存在时返回 false
    static bool Send(unsigned int msgId);
};
