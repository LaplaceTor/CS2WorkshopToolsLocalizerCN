#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace capture {

constexpr const wchar_t* kSharedMemoryName = L"Local\\CS2_Hammer_Localizer_Untranslated";
constexpr uint32_t kSharedMemoryMagic = 0x48414D54; // 'HAMT'
constexpr uint32_t kSharedMemoryVersion = 1;
constexpr uint32_t kMaxUntranslatedEntries = 1024;
constexpr uint32_t kMaxTextLen = 256;
constexpr uint32_t kMaxModuleLen = 32;

#pragma pack(push, 8)
struct UntranslatedEntry {
    char text[kMaxTextLen];
    wchar_t module[kMaxModuleLen];
    uint32_t hitCount;
    uint64_t lastTimestamp; // GetTickCount64() ms
};

struct UntranslatedSharedMemoryHeader {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t generationId;
    volatile uint32_t entryCount;
    volatile uint32_t writeIndex;
    UntranslatedEntry entries[kMaxUntranslatedEntries];
};
#pragma pack(pop)

// 检查字符串是否包含中文汉字、全角标点或 CJK 字符（已汉化文本特征）
inline bool ContainsChinese(const char* text) {
    if (!text) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if (!*(p + 1)) break;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if (!*(p + 1) || !*(p + 2)) break;
            uint32_t cp = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs
                (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Extension A
                (cp >= 0x2E80 && cp <= 0x2EFF) ||  // CJK Radicals
                (cp >= 0x3000 && cp <= 0x303F) ||  // CJK 标点符号 (。 、 【 】 等)
                (cp >= 0xFF00 && cp <= 0xFFEF) ||  // 全角形式 (（ ） ： 等)
                (cp >= 0x2000 && cp <= 0x206F)) {  // 常用标点 (… “” 等)
                return true;
            }
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if (!*(p + 1) || !*(p + 2) || !*(p + 3)) break;
            uint32_t cp = ((*p & 0x07) << 18) | ((*(p + 1) & 0x3F) << 12) | ((*(p + 2) & 0x3F) << 6) | (*(p + 3) & 0x3F);
            if (cp >= 0x20000 && cp <= 0x2FA1F) {
                return true;
            }
            p += 4;
        } else {
            p++;
        }
    }
    return false;
}

// 供 qtcore_qm.dll 调用的写入端
class UntranslatedCaptureWriter {
public:
    UntranslatedCaptureWriter() = default;
    ~UntranslatedCaptureWriter() {
        cleanup();
    }

    bool init() {
        if (m_header) return true;

        m_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(UntranslatedSharedMemoryHeader),
            kSharedMemoryName
        );

        if (!m_hMapFile) {
            return false;
        }

        bool isFirstCreator = (GetLastError() != ERROR_ALREADY_EXISTS);

        m_header = static_cast<UntranslatedSharedMemoryHeader*>(
            MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UntranslatedSharedMemoryHeader))
        );

        if (!m_header) {
            CloseHandle(m_hMapFile);
            m_hMapFile = nullptr;
            return false;
        }

        if (isFirstCreator) {
            std::memset(m_header, 0, sizeof(UntranslatedSharedMemoryHeader));
            m_header->magic = kSharedMemoryMagic;
            m_header->version = kSharedMemoryVersion;
            m_header->generationId = 1;
            m_header->entryCount = 0;
            m_header->writeIndex = 0;
        }

        m_localGenerationId = m_header->generationId;
        return true;
    }

    void record(const wchar_t* moduleStem, const char* text) {
        if (!text || text[0] == '\0') return;
        size_t len = std::strlen(text);
        if (len < 2 || len >= kMaxTextLen) return;

        // 绝不捕获任何包含中文或全角字符的已翻译文本
        if (ContainsChinese(text)) return;

        // 基础清洗：必须包含至少一个英文字母，避免纯数字/符号
        bool hasAlpha = false;
        for (size_t i = 0; i < len; ++i) {
            if ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z')) {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha) return;

        if (!m_header) {
            if (!init()) return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // 检测启动器是否清空了捕获池（通过 generationId 递增感知）
        uint32_t curGen = m_header->generationId;
        if (curGen != m_localGenerationId) {
            m_localIndexMap.clear();
            m_header->entryCount = 0;
            m_header->writeIndex = 0;
            m_localGenerationId = curGen;
        }

        std::wstring modStr = (moduleStem && *moduleStem) ? moduleStem : L"unknown";
        char modBuf[kMaxModuleLen * 3] = {0};
        WideCharToMultiByte(CP_UTF8, 0, modStr.c_str(), -1, modBuf, sizeof(modBuf), nullptr, nullptr);
        std::string key = std::string(modBuf) + "\t" + std::string(text);

        uint64_t now = GetTickCount64();
        auto it = m_localIndexMap.find(key);
        if (it != m_localIndexMap.end()) {
            uint32_t slot = it->second;
            if (slot < kMaxUntranslatedEntries) {
                m_header->entries[slot].hitCount++;
                m_header->entries[slot].lastTimestamp = now;
            }
            return;
        }

        // 新条目：分配槽位
        uint32_t slot = 0;
        uint32_t curCount = m_header->entryCount;
        if (curCount < kMaxUntranslatedEntries) {
            slot = curCount;
            m_header->entryCount = curCount + 1;
        } else {
            // 环形覆盖
            slot = m_header->writeIndex % kMaxUntranslatedEntries;
            // 淘汰旧槽位的索引
            char oldModBuf[kMaxModuleLen * 3] = {0};
            WideCharToMultiByte(CP_UTF8, 0, m_header->entries[slot].module, -1, oldModBuf, sizeof(oldModBuf), nullptr, nullptr);
            std::string oldKey = std::string(oldModBuf) + "\t" + std::string(m_header->entries[slot].text);
            m_localIndexMap.erase(oldKey);
        }

        UntranslatedEntry& entry = m_header->entries[slot];
        strncpy_s(entry.text, kMaxTextLen, text, _TRUNCATE);
        wcsncpy_s(entry.module, kMaxModuleLen, modStr.c_str(), _TRUNCATE);
        entry.hitCount = 1;
        entry.lastTimestamp = now;

        m_localIndexMap[key] = slot;
        m_header->writeIndex = m_header->writeIndex + 1;
    }

private:
    void cleanup() {
        if (m_header) {
            UnmapViewOfFile(m_header);
            m_header = nullptr;
        }
        if (m_hMapFile) {
            CloseHandle(m_hMapFile);
            m_hMapFile = nullptr;
        }
    }

    HANDLE m_hMapFile{nullptr};
    UntranslatedSharedMemoryHeader* m_header{nullptr};
    uint32_t m_localGenerationId{0};
    std::unordered_map<std::string, uint32_t> m_localIndexMap;
    std::mutex m_mutex;
};

// 供启动器调用的读取端
class UntranslatedCaptureReader {
public:
    UntranslatedCaptureReader() = default;
    ~UntranslatedCaptureReader() {
        cleanup();
    }

    bool ensureConnected() {
        if (m_header) return true;

        m_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemoryName);
        if (!m_hMapFile) {
            // 若 DLL 尚未运行创建，也可以由启动器主动预先创建
            m_hMapFile = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                sizeof(UntranslatedSharedMemoryHeader),
                kSharedMemoryName
            );
            if (!m_hMapFile) return false;

            m_header = static_cast<UntranslatedSharedMemoryHeader*>(
                MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UntranslatedSharedMemoryHeader))
            );
            if (m_header) {
                std::memset(m_header, 0, sizeof(UntranslatedSharedMemoryHeader));
                m_header->magic = kSharedMemoryMagic;
                m_header->version = kSharedMemoryVersion;
                m_header->generationId = 1;
            }
            return m_header != nullptr;
        }

        m_header = static_cast<UntranslatedSharedMemoryHeader*>(
            MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UntranslatedSharedMemoryHeader))
        );
        return m_header != nullptr;
    }

    struct Item {
        std::string text;
        std::wstring module;
        uint32_t hitCount;
        uint64_t lastTimestamp;
    };

    std::vector<Item> fetchEntries() {
        std::vector<Item> result;
        if (!ensureConnected() || !m_header) return result;

        uint32_t count = m_header->entryCount;
        if (count > kMaxUntranslatedEntries) count = kMaxUntranslatedEntries;

        result.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& e = m_header->entries[i];
            if (e.text[0] != '\0') {
                result.push_back({
                    e.text,
                    e.module,
                    e.hitCount,
                    e.lastTimestamp
                });
            }
        }
        return result;
    }

    void clearAll() {
        if (!ensureConnected() || !m_header) return;
        m_header->generationId = m_header->generationId + 1;
        m_header->entryCount = 0;
        m_header->writeIndex = 0;
        std::memset(m_header->entries, 0, sizeof(m_header->entries));
    }

private:
    void cleanup() {
        if (m_header) {
            UnmapViewOfFile(m_header);
            m_header = nullptr;
        }
        if (m_hMapFile) {
            CloseHandle(m_hMapFile);
            m_hMapFile = nullptr;
        }
    }

    HANDLE m_hMapFile{nullptr};
    UntranslatedSharedMemoryHeader* m_header{nullptr};
};

} // namespace capture
