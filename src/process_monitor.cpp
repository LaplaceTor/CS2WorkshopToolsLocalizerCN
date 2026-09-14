#include "process_monitor.h"

#include "cs2_detector.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool ProcessMonitor::IsRunning(const Source& src) {
    // 1. QProcess 自身报告仍在运行，直接采信
    if (src.qProcessRunning) {
        return true;
    }

    // 2. 有原生进程句柄：句柄未触发信号(WAIT_TIMEOUT)即代表进程仍存活。
    //    注意句柄已触发时直接判定为未运行，不会继续退化到 PID 探测。
    if (src.nativeHandle != nullptr) {
        const DWORD waitRes = WaitForSingleObject(static_cast<HANDLE>(src.nativeHandle), 0);
        return (waitRes == WAIT_TIMEOUT);
    }

    // 3. 退化为按 PID 查进程快照
    if (src.pid > 0) {
        return Cs2Detector::IsProcessRunning(src.pid);
    }

    return false;
}

bool ProcessMonitor::Sample(const Source& src) {
    if (IsRunning(src)) {
        m_missCount = 0;
        return true;
    }

    ++m_missCount;
    return false;
}
