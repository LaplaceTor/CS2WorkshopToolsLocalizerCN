#include "hammer_ipc.h"

HWND HammerIpc::FindIpcWindow() {
    HWND hWnd = FindWindowExW(HWND_MESSAGE, NULL, kWindowClass, kWindowTitle);
    if (!hWnd) {
        hWnd = FindWindowW(kWindowClass, kWindowTitle);
    }
    return hWnd;
}

bool HammerIpc::Send(unsigned int msgId) {
    const HWND hWnd = FindIpcWindow();
    if (!hWnd) {
        return false;
    }
    return (PostMessageW(hWnd, msgId, 0, 0) != FALSE);
}
