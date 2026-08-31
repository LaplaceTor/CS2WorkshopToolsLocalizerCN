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

    // 探测 DLL 是否已包含 LCLZ 补丁标记与元数据
    static bool GetPatchInfo(const std::wstring& dllPath, PatchInfo& outInfo, std::wstring& outError);
};

