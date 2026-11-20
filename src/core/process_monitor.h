#pragma once

class ProcessMonitor {
public:
    struct Source {
        bool          qProcessRunning = false;
        void*         nativeHandle    = nullptr;
        unsigned long pid             = 0;
    };

    explicit ProcessMonitor(int missThreshold = 2) : m_missThreshold(missThreshold) {}

    static bool IsRunning(const Source& src);

    bool Sample(const Source& src);

    bool ReachedTerminationThreshold() const { return m_missCount >= m_missThreshold; }

    int  MissCount() const { return m_missCount; }
    void Reset() { m_missCount = 0; }

private:
    int m_missThreshold;
    int m_missCount = 0;
};
