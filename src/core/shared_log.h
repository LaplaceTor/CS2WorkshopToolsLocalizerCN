#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace shared_log {

constexpr const wchar_t* kSharedLogMemName = L"Local\\CS2_Hammer_Localizer_SharedLog";
constexpr size_t kSharedLogCapacity = 128 * 1024; // 128 KB 环形缓冲区

#pragma pack(push, 4)
struct SharedLogHeader {
    uint32_t magic;         // 0x5A4C434C ('LCLZ')
    uint32_t writeSeq;      // 单调递增写入总字节数
    uint32_t writeOffset;   // 当前写入环形偏移
    char buffer[kSharedLogCapacity];
};
#pragma pack(pop)

// 注入端写入函数实现与状态管理
class SharedLogWriter {
public:
    static SharedLogWriter& Instance() {
        static SharedLogWriter s_inst;
        return s_inst;
    }

    void Log(const char* fmt, va_list args) {
        if (!fmt) return;
        char formatted[1024] = {0};
        int len = vsnprintf(formatted, sizeof(formatted) - 2, fmt, args);
        if (len <= 0) return;
        formatted[len] = '\n';
        formatted[len + 1] = '\0';
        len += 1;

        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureMapping();
        if (!m_pHeader) return;

        for (int i = 0; i < len; ++i) {
            uint32_t offset = m_pHeader->writeOffset;
            m_pHeader->buffer[offset] = formatted[i];
            m_pHeader->writeOffset = (offset + 1) % static_cast<uint32_t>(kSharedLogCapacity);
        }

        // 内存屏障保证内容先于 sequence 更新对其它核可见
        MemoryBarrier();
        m_pHeader->writeSeq += static_cast<uint32_t>(len);
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pHeader) {
            UnmapViewOfFile(m_pHeader);
            m_pHeader = nullptr;
        }
        if (m_hMap) {
            CloseHandle(m_hMap);
            m_hMap = NULL;
        }
    }

private:
    SharedLogWriter() = default;
    ~SharedLogWriter() {
        Shutdown();
    }

    void EnsureMapping() {
        if (m_pHeader) return;
        m_hMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            sizeof(SharedLogHeader),
            kSharedLogMemName
        );
        if (m_hMap) {
            m_pHeader = reinterpret_cast<SharedLogHeader*>(
                MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedLogHeader))
            );
            if (m_pHeader) {
                if (m_pHeader->magic != 0x5A4C434C) {
                    m_pHeader->magic = 0x5A4C434C;
                    m_pHeader->writeSeq = 0;
                    m_pHeader->writeOffset = 0;
                }
            }
        }
    }

    HANDLE m_hMap = NULL;
    SharedLogHeader* m_pHeader = nullptr;
    std::mutex m_mutex;
};

inline void LogHook(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    SharedLogWriter::Instance().Log(fmt, args);
    va_end(args);
}

inline void ShutdownSharedLog() {
    SharedLogWriter::Instance().Shutdown();
}

} // namespace shared_log
