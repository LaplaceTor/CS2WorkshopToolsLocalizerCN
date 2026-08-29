#include "pe_patcher.h"
#include <windows.h>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

bool PePatcher::PatchQtCore(const std::wstring& srcDllPath, const std::wstring& dstDllPath, std::wstring& outError) {
    std::ifstream inFile(srcDllPath, std::ios::binary | std::ios::ate);
    if (!inFile.is_open()) {
        outError = L"无法打开源 Qt5Core.dll: " + srcDllPath;
        return false;
    }

    std::streamsize fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    if (fileSize < (std::streamsize)sizeof(IMAGE_DOS_HEADER)) {
        outError = L"文件过小，不是有效的 PE 文件";
        return false;
    }

    std::vector<uint8_t> buffer(fileSize);
    if (!inFile.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        outError = L"读取源文件失败";
        return false;
    }
    inFile.close();

    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(buffer.data());
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        outError = L"无效的 DOS 签名";
        return false;
    }

    if (dosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > buffer.size()) {
        outError = L"无效的 NT 头部偏移";
        return false;
    }

    PIMAGE_NT_HEADERS64 ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(buffer.data() + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        outError = L"无效的 NT 签名";
        return false;
    }

    if (ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = L"仅支持 64 位 PE 动态库 (PE32+)";
        return false;
    }

    DWORD origEntryPointRva = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    WORD numSections = ntHeaders->FileHeader.NumberOfSections;
    PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(ntHeaders);

    auto rvaToFileOffset = [&](DWORD rva) -> DWORD {
        for (WORD i = 0; i < numSections; ++i) {
            DWORD secVa = sections[i].VirtualAddress;
            DWORD secRawSize = sections[i].SizeOfRawData;
            DWORD secVirtSize = sections[i].Misc.VirtualSize;
            DWORD secSpan = (std::max)(secRawSize, secVirtSize);
            if (rva >= secVa && rva < secVa + secSpan) {
                return sections[i].PointerToRawData + (rva - secVa);
            }
        }
        return 0;
    };

    // 寻找 .text 节
    PIMAGE_SECTION_HEADER textSec = nullptr;
    for (WORD i = 0; i < numSections; ++i) {
        if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
            textSec = &sections[i];
            break;
        }
    }

    if (!textSec) {
        outError = L"未在 PE 文件中找到 .text 节";
        return false;
    }

    // 寻找 KERNEL32.dll 中的 LoadLibraryA 的 IAT RVA
    IMAGE_DATA_DIRECTORY importDataDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir.VirtualAddress == 0 || importDataDir.Size == 0) {
        outError = L"PE 文件缺少导入表";
        return false;
    }

    DWORD importOffset = rvaToFileOffset(importDataDir.VirtualAddress);
    if (importOffset == 0 || importOffset >= buffer.size()) {
        outError = L"导入表偏移无效";
        return false;
    }

    DWORD iatLoadLibRva = 0;
    PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(buffer.data() + importOffset);

    while (importDesc->Name != 0) {
        DWORD nameOff = rvaToFileOffset(importDesc->Name);
        if (nameOff != 0 && nameOff < buffer.size()) {
            const char* dllName = reinterpret_cast<const char*>(buffer.data() + nameOff);
            std::string dllNameLower = dllName;
            std::transform(dllNameLower.begin(), dllNameLower.end(), dllNameLower.begin(), ::tolower);

            if (dllNameLower.find("kernel32") != std::string::npos) {
                DWORD thunkRva = importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk;
                DWORD iatRva = importDesc->FirstThunk;

                DWORD thunkOff = rvaToFileOffset(thunkRva);
                if (thunkOff != 0) {
                    PIMAGE_THUNK_DATA64 thunkData = reinterpret_cast<PIMAGE_THUNK_DATA64>(buffer.data() + thunkOff);
                    int idx = 0;
                    while (thunkData->u1.AddressOfData != 0) {
                        if (!(thunkData->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                            DWORD importByNameOff = rvaToFileOffset(static_cast<DWORD>(thunkData->u1.AddressOfData));
                            if (importByNameOff != 0 && importByNameOff < buffer.size()) {
                                PIMAGE_IMPORT_BY_NAME impName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(buffer.data() + importByNameOff);
                                if (std::strcmp(reinterpret_cast<const char*>(impName->Name), "LoadLibraryA") == 0) {
                                    iatLoadLibRva = iatRva + idx * sizeof(IMAGE_THUNK_DATA64);
                                    break;
                                }
                            }
                        }
                        idx++;
                        thunkData++;
                    }
                }
                if (iatLoadLibRva != 0) break;
            }
        }
        importDesc++;
    }

    if (iatLoadLibRva == 0) {
        outError = L"未在导入表中检索到 LoadLibraryA 条目";
        return false;
    }

    // 计算 Code Cave
    DWORD caveRva = (textSec->VirtualAddress + textSec->Misc.VirtualSize + 15) & ~15;
    const std::string dllName = "qtcore_qm.dll";
    DWORD dllNameLen = static_cast<DWORD>(dllName.length() + 1); // 包含 '\0'
    DWORD dllNameRva = caveRva;

    DWORD codeStartRva = caveRva + dllNameLen + ((8 - (dllNameLen % 8)) % 8);
    DWORD codeStartOff = textSec->PointerToRawData + (codeStartRva - textSec->VirtualAddress);

    // 构建 Shellcode
    std::vector<uint8_t> shellcode;

    // 1. 保护寄存器 (8 个 push，共 64 字节)
    // push rax(0x50), rcx(0x51), rdx(0x52), rbx(0x53), r8(0x41,0x50), r9(0x41,0x51), r10(0x41,0x52), r11(0x41,0x53)
    const uint8_t pushRegs[] = { 0x50, 0x51, 0x52, 0x53, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 };
    shellcode.insert(shellcode.end(), pushRegs, pushRegs + sizeof(pushRegs));

    // 2. 栈对齐：sub rsp, 0x28
    const uint8_t subRsp[] = { 0x48, 0x83, 0xec, 0x28 };
    shellcode.insert(shellcode.end(), subRsp, subRsp + sizeof(subRsp));

    // 3. lea rcx, [rip + disp32] (指向 dll_name_rva)
    // 指令长度 7 字节 (48 8d 0d + 4字节 disp32)
    DWORD currRva = codeStartRva + static_cast<DWORD>(shellcode.size());
    int32_t dispName = static_cast<int32_t>(dllNameRva) - static_cast<int32_t>(currRva + 7);
    shellcode.push_back(0x48);
    shellcode.push_back(0x8d);
    shellcode.push_back(0x0d);
    shellcode.insert(shellcode.end(), reinterpret_cast<uint8_t*>(&dispName), reinterpret_cast<uint8_t*>(&dispName) + 4);

    // 4. call qword ptr [rip + disp32] (调用 IAT 中的 LoadLibraryA)
    // 指令长度 6 字节 (ff 15 + 4字节 disp32)
    currRva = codeStartRva + static_cast<DWORD>(shellcode.size());
    int32_t dispIat = static_cast<int32_t>(iatLoadLibRva) - static_cast<int32_t>(currRva + 6);
    shellcode.push_back(0xff);
    shellcode.push_back(0x15);
    shellcode.insert(shellcode.end(), reinterpret_cast<uint8_t*>(&dispIat), reinterpret_cast<uint8_t*>(&dispIat) + 4);

    // 5. 恢复栈：add rsp, 0x28
    const uint8_t addRsp[] = { 0x48, 0x83, 0xc4, 0x28 };
    shellcode.insert(shellcode.end(), addRsp, addRsp + sizeof(addRsp));

    // 6. 恢复寄存器 (8 个 pop)
    // pop r11, r10, r9, r8, rbx, rdx, rcx, rax
    const uint8_t popRegs[] = { 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5b, 0x5a, 0x59, 0x58 };
    shellcode.insert(shellcode.end(), popRegs, popRegs + sizeof(popRegs));

    // 7. jmp disp32 (跳回原 EntryPoint)
    // 指令长度 5 字节 (e9 + 4字节 disp32)
    currRva = codeStartRva + static_cast<DWORD>(shellcode.size());
    int32_t dispBack = static_cast<int32_t>(origEntryPointRva) - static_cast<int32_t>(currRva + 5);
    shellcode.push_back(0xe9);
    shellcode.insert(shellcode.end(), reinterpret_cast<uint8_t*>(&dispBack), reinterpret_cast<uint8_t*>(&dispBack) + 4);

    // 校验 Code Cave 是否超出 .text 节大小
    DWORD caveOff = textSec->PointerToRawData + (caveRva - textSec->VirtualAddress);
    DWORD totalCaveBytesNeeded = (codeStartOff - caveOff) + static_cast<DWORD>(shellcode.size());

    if (caveOff + totalCaveBytesNeeded > textSec->PointerToRawData + textSec->SizeOfRawData) {
        outError = L".text 节末尾剩余空间不足以容纳 Code Cave";
        return false;
    }

    // 写入 DLL 名称与 Shellcode
    std::memcpy(buffer.data() + caveOff, dllName.c_str(), dllNameLen);
    std::memcpy(buffer.data() + codeStartOff, shellcode.data(), shellcode.size());

    // 更新 EntryPoint
    ntHeaders->OptionalHeader.AddressOfEntryPoint = codeStartRva;

    // 写入目标文件
    std::ofstream outFile(dstDllPath, std::ios::binary);
    if (!outFile.is_open()) {
        outError = L"无法写入目标文件: " + dstDllPath;
        return false;
    }

    if (!outFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
        outError = L"写入目标文件数据失败";
        return false;
    }
    outFile.close();

    return true;
}

