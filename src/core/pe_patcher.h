#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma pack(push, 1)
struct PatchHeader {
    char     magic[4];
    uint32_t version;
    uint32_t originalEntryRva;
    uint32_t origTrRva;
    uint32_t payloadSize;
};
#pragma pack(pop)

struct PatchInfo {
    bool isPatched = false;
    uint32_t version = 0;
    uint32_t originalEntryRva = 0;
    uint32_t origTrRva = 0;
    uint32_t payloadSize = 0;
};

class SafePeReader {
public:
    const uint8_t* m_data;
    size_t m_size;

    SafePeReader(const uint8_t* pData, size_t s) : m_data(pData), m_size(s) {}

    bool InBounds(size_t offset, size_t len) const {
        if (offset > m_size) return false;
        if (len > m_size - offset) return false;
        return true;
    }

    template<typename T>
    const T* ReadStruct(size_t offset) const {
        if (!InBounds(offset, sizeof(T))) return nullptr;
        return reinterpret_cast<const T*>(m_data + offset);
    }

    const char* ReadNullTerminatedString(size_t offset, size_t maxLen = 256) const {
        if (offset >= m_size) return nullptr;
        size_t limit = (std::min)(m_size - offset, maxLen);
        for (size_t i = 0; i < limit; ++i) {
            if (m_data[offset + i] == '\0') {
                return reinterpret_cast<const char*>(m_data + offset);
            }
        }
        return nullptr;
    }
};

class PePatcher {
public:
    static std::optional<size_t> RvaToFileOffset(
        const IMAGE_NT_HEADERS64* nt,
        DWORD rva,
        size_t fileSize,
        size_t requiredSize = 1);

    static bool PatchQtCore(const std::wstring& srcDllPath, const std::wstring& dstDllPath, std::wstring& outError);

    static bool GetPatchInfo(const std::wstring& dllPath, PatchInfo& outInfo, std::wstring& outError);

    static bool GetPatchInfoFromMemory(HMODULE hMod, PatchInfo& outInfo);

    static uint32_t GetModuleSizeOfImage(HMODULE hMod);
};

