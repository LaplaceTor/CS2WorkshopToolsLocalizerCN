#include "core/process_monitor.h"

#include "core/cs2_detector.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool ProcessMonitor::IsRunning(const Source& src) {
    if (src.qProcessRunning) {
        return true;
    }

    if (src.nativeHandle != nullptr) {
        const DWORD waitRes = WaitForSingleObject(static_cast<HANDLE>(src.nativeHandle), 0);
        return (waitRes == WAIT_TIMEOUT);
    }

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
