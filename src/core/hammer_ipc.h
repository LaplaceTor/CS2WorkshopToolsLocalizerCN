#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class HammerIpc {
public:
    static constexpr const wchar_t* kWindowClass = L"CS2_HAMMER_LOCALIZER_IPC";
    static constexpr const wchar_t* kWindowTitle = L"CS2_Hammer_Localizer_MsgWnd";

    static constexpr unsigned int kMsgToggleLang   = WM_USER + 101;
    static constexpr unsigned int kMsgReloadDict   = WM_USER + 102;
    static constexpr unsigned int kMsgQueryStatus  = WM_USER + 103;

    static HWND FindIpcWindow();

    static bool Send(unsigned int msgId);
};
