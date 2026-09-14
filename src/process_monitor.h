#pragma once

// HAMMER 进程存活性监控。
//
// 封装两件事：
//   1. 三路探测（QProcess 状态 / Win32 进程句柄 / PID）——三路是历史演进叠加的结果，
//      必须按固定优先级使用，不可合并。
//   2. "连续多次未探测到才算真的退出" 的阈值判定——单次采样失败可能是时序抖动。
//
// 不依赖 Qt 控件，便于单独测试与复用。

class ProcessMonitor {
public:
    // 进程的探测来源，按优先级使用（命中一路即返回，不再往下试）
    struct Source {
        bool          qProcessRunning = false;  // QProcess::state() == Running，由调用方读取后传入
        void*         nativeHandle    = nullptr; // Win32 进程句柄
        unsigned long pid             = 0;       // 进程 PID
    };

    explicit ProcessMonitor(int missThreshold = 2) : m_missThreshold(missThreshold) {}

    // 探测进程当前是否在运行
    static bool IsRunning(const Source& src);

    // 采样一次并更新连续未命中计数，返回本次是否探测到进程
    bool Sample(const Source& src);

    // 连续未命中是否已达阈值（可认定进程已退出）
    bool ReachedTerminationThreshold() const { return m_missCount >= m_missThreshold; }

    int  MissCount() const { return m_missCount; }
    void Reset() { m_missCount = 0; }

private:
    int m_missThreshold;
    int m_missCount = 0;
};
