#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <windows.h>

#pragma pack(push, 1)
struct PatchHeader {
    char     magic[4];          // "LCLZ"
    uint32_t version;           // 2
    uint32_t originalEntryRva;  // 原始未修改的 EntryPoint RVA
    uint32_t origTrRva;         // 原始 QMetaObject::tr RVA
    uint32_t payloadSize;       // Header + Strings + Shellcode 总大小
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
    // 统一安全的 RVA 转文件物理偏移函数（带全面边界与 Section 范围验证）
    static std::optional<size_t> RvaToFileOffset(
        const IMAGE_NT_HEADERS64* nt,
        DWORD rva,
        size_t fileSize,
        size_t requiredSize = 1);

    // 修补 64位 Qt5Core.dll，在 .text 节的代码洞中注入 LoadLibraryA("qtcore_qm.dll")
    static bool PatchQtCore(const std::wstring& srcDllPath, const std::wstring& dstDllPath, std::wstring& outError);

    // 探测文件形式的 DLL 是否已包含 LCLZ 补丁标记与元数据
    static bool GetPatchInfo(const std::wstring& dllPath, PatchInfo& outInfo, std::wstring& outError);

    // 从内存中已加载的模块安全获取 LCLZ 补丁元数据与原始 QMetaObject::tr 地址 (统一 SafePeReader)
    static bool GetPatchInfoFromMemory(HMODULE hMod, PatchInfo& outInfo);

    // 从内存中安全获取模块的 SizeOfImage (通过 SafePeReader 解析 OptionalHeader，0 API 调用)
    static uint32_t GetModuleSizeOfImage(HMODULE hMod);
};

