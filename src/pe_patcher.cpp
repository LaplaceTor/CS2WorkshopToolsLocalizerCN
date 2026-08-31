#include "pe_patcher.h"
#include <windows.h>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>



std::optional<size_t> PePatcher::RvaToFileOffset(
    const IMAGE_NT_HEADERS64* nt,
    DWORD rva,
    size_t fileSize,
    size_t requiredSize)
{
    if (!nt || fileSize == 0 || requiredSize == 0) {
        return std::nullopt;
    }

    WORD numSections = nt->FileHeader.NumberOfSections;
    if (numSections == 0 || numSections > 96) {
        return std::nullopt;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    if (!sections) {
        return std::nullopt;
    }

    for (WORD i = 0; i < numSections; ++i) {
        const auto& sec = sections[i];
        uint64_t secBegin = sec.VirtualAddress;
        uint64_t secRawSize = sec.SizeOfRawData;
        uint64_t secVirtSize = sec.Misc.VirtualSize;
        uint64_t secSpan = (std::max)(secRawSize, secVirtSize);
        uint64_t secEnd = secBegin + secSpan;
        uint64_t rva64 = rva;

        if (rva64 >= secBegin && rva64 < secEnd) {
            uint64_t offsetInSec = rva64 - secBegin;
            // 确保请求的偏移与长度完整落在文件的物理 raw data 范围内
            if (offsetInSec >= secRawSize) {
                return std::nullopt;
            }
            if (requiredSize > secRawSize - offsetInSec) {
                return std::nullopt;
            }
            uint64_t fileOffset = static_cast<uint64_t>(sec.PointerToRawData) + offsetInSec;
            if (fileOffset > fileSize || requiredSize > fileSize - fileOffset) {
                return std::nullopt;
            }
            return static_cast<size_t>(fileOffset);
        }
    }

    return std::nullopt;
}

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

    SafePeReader reader(buffer.data(), buffer.size());

    // 1. 严格校验 DOS Header
    const IMAGE_DOS_HEADER* dosHeader = reader.ReadStruct<IMAGE_DOS_HEADER>(0);
    if (!dosHeader || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        outError = L"无效的 DOS 签名 (IMAGE_DOS_SIGNATURE)";
        return false;
    }

    if (dosHeader->e_lfanew <= 0) {
        outError = L"无效的 DOS e_lfanew 偏移 (必须大于 0)";
        return false;
    }

    size_t ntHeaderOff = static_cast<size_t>(dosHeader->e_lfanew);
    if (!reader.InBounds(ntHeaderOff, sizeof(IMAGE_NT_HEADERS64))) {
        outError = L"NT 头部偏移超出文件边界";
        return false;
    }

    // 2. 严格校验 NT Headers
    const IMAGE_NT_HEADERS64* ntHeadersConst = reader.ReadStruct<IMAGE_NT_HEADERS64>(ntHeaderOff);
    if (!ntHeadersConst || ntHeadersConst->Signature != IMAGE_NT_SIGNATURE) {
        outError = L"无效的 NT 签名 (IMAGE_NT_SIGNATURE)";
        return false;
    }

    if (ntHeadersConst->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        outError = L"仅支持 64 位 (x64 / AMD64) PE 动态库";
        return false;
    }

    WORD numSections = ntHeadersConst->FileHeader.NumberOfSections;
    if (numSections == 0 || numSections > 96) {
        outError = L"异常的节区数量 (NumberOfSections)";
        return false;
    }

    if (ntHeadersConst->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        outError = L"无效的 OptionalHeader 大小 (SizeOfOptionalHeader)";
        return false;
    }

    if (ntHeadersConst->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = L"仅支持 PE32+ (64 位) 格式";
        return false;
    }

    if (ntHeadersConst->OptionalHeader.SizeOfHeaders > buffer.size()) {
        outError = L"SizeOfHeaders 超出文件边界";
        return false;
    }

    if (ntHeadersConst->OptionalHeader.SizeOfImage == 0) {
        outError = L"无效的 SizeOfImage (为 0)";
        return false;
    }

    // 3. 严格校验 Section Headers
    size_t secHeadersOff = ntHeaderOff + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + ntHeadersConst->FileHeader.SizeOfOptionalHeader;
    if (!reader.InBounds(secHeadersOff, sizeof(IMAGE_SECTION_HEADER) * numSections)) {
        outError = L"节区头部数组超出文件边界";
        return false;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeadersConst);
    const IMAGE_SECTION_HEADER* textSec = nullptr;
    const IMAGE_SECTION_HEADER* dataSec = nullptr;

    for (WORD i = 0; i < numSections; ++i) {
        // 校验每个 section 的物理映射范围合法性
        if (sections[i].SizeOfRawData > 0) {
            if (!reader.InBounds(sections[i].PointerToRawData, sections[i].SizeOfRawData)) {
                outError = L"节区数据范围超出文件物理边界";
                return false;
            }
        }
        if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
            textSec = &sections[i];
        } else if (std::memcmp(sections[i].Name, ".data", 5) == 0) {
            dataSec = &sections[i];
        }
    }

    if (!textSec) {
        outError = L"未在 PE 文件中找到 .text 节";
        return false;
    }

    if (!dataSec) {
        outError = L"未在 PE 文件中找到 .data 节";
        return false;
    }

    DWORD origEntryPointRva = ntHeadersConst->OptionalHeader.AddressOfEntryPoint;
    DWORD origTrRva = 0;

    // 4. Code Cave 起始 RVA 判定（使用 64 位整型运算防止溢出）
    uint64_t textSecEndRva64 = static_cast<uint64_t>(textSec->VirtualAddress) + static_cast<uint64_t>(textSec->Misc.VirtualSize);
    uint64_t caveRva64 = (textSecEndRva64 + 15) & ~15ULL;
    if (caveRva64 > 0xFFFFFFFFULL) {
        outError = L"Code Cave RVA 溢出 32 位地址空间";
        return false;
    }
    DWORD caveRva = static_cast<DWORD>(caveRva64);

    // 优先通过明确的 LCLZ 补丁元数据头 (PatchHeader) 判定与恢复原始入口点（100% 确定性、零误判）
    bool bFoundLclzMagic = false;
    auto optCaveHeaderOff = RvaToFileOffset(ntHeadersConst, caveRva, buffer.size(), sizeof(PatchHeader));
    if (optCaveHeaderOff) {
        const PatchHeader* pHeader = reader.ReadStruct<PatchHeader>(*optCaveHeaderOff);
        if (pHeader && std::memcmp(pHeader->magic, "LCLZ", 4) == 0 && (pHeader->version == 1 || pHeader->version == 2)) {
            uint64_t origEntry64 = pHeader->originalEntryRva;
            if (origEntry64 >= textSec->VirtualAddress && origEntry64 < textSecEndRva64) {
                origEntryPointRva = pHeader->originalEntryRva;
                bFoundLclzMagic = true;
            }
            if (pHeader->origTrRva != 0) {
                origTrRva = pHeader->origTrRva;
            }
        }
    }

    // 兼容历史遗留旧补丁（未写入 LCLZ 头）：仅在入口点位于 .text 尾部且非 LCLZ 时作为兜底解析
    if (!bFoundLclzMagic) {
        if (textSecEndRva64 >= 0x1000 && origEntryPointRva >= textSecEndRva64 - 0x1000 && origEntryPointRva < textSecEndRva64) {
            auto optEpOff = RvaToFileOffset(ntHeadersConst, origEntryPointRva, buffer.size(), 64);
            if (optEpOff) {
                size_t epOff = *optEpOff;
                for (size_t k = 0; k < 64; ++k) {
                    if (buffer[epOff + k] == 0xe9) {
                        int32_t jmpDisp = *reinterpret_cast<const int32_t*>(&buffer[epOff + k + 1]);
                        int64_t targetRva64 = static_cast<int64_t>(origEntryPointRva) + k + 5 + jmpDisp;
                        if (targetRva64 >= textSec->VirtualAddress && targetRva64 < static_cast<int64_t>(textSecEndRva64)) {
                            origEntryPointRva = static_cast<DWORD>(targetRva64);
                            break;
                        }
                    }
                }
            }
        }
    }

    // 5. 寻找 KERNEL32.dll 中的 LoadLibraryA 和 GetProcAddress 的 IAT RVA
    IMAGE_DATA_DIRECTORY importDataDir = ntHeadersConst->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir.VirtualAddress == 0 || importDataDir.Size == 0) {
        outError = L"PE 文件缺少导入表 (IMAGE_DIRECTORY_ENTRY_IMPORT)";
        return false;
    }

    auto optImportOff = RvaToFileOffset(ntHeadersConst, importDataDir.VirtualAddress, buffer.size(), sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (!optImportOff) {
        outError = L"导入表偏移超出文件物理边界";
        return false;
    }

    DWORD iatLoadLibRva = 0;
    DWORD iatGetProcRva = 0;
    size_t currDescOff = *optImportOff;

    while (reader.InBounds(currDescOff, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        const IMAGE_IMPORT_DESCRIPTOR* importDesc = reader.ReadStruct<IMAGE_IMPORT_DESCRIPTOR>(currDescOff);
        if (!importDesc || importDesc->Name == 0) {
            break;
        }

        auto optNameOff = RvaToFileOffset(ntHeadersConst, importDesc->Name, buffer.size(), 1);
        if (optNameOff) {
            const char* dllName = reader.ReadNullTerminatedString(*optNameOff, 128);
            if (dllName) {
                std::string dllNameLower = dllName;
                std::transform(dllNameLower.begin(), dllNameLower.end(), dllNameLower.begin(), ::tolower);

                if (dllNameLower.find("kernel32") != std::string::npos) {
                    DWORD thunkRva = importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk;
                    DWORD iatRva = importDesc->FirstThunk;

                    auto optThunkOff = RvaToFileOffset(ntHeadersConst, thunkRva, buffer.size(), sizeof(IMAGE_THUNK_DATA64));
                    if (optThunkOff) {
                        size_t thunkOff = *optThunkOff;
                        int idx = 0;
                        while (reader.InBounds(thunkOff + idx * sizeof(IMAGE_THUNK_DATA64), sizeof(IMAGE_THUNK_DATA64)) && idx < 4096) {
                            const IMAGE_THUNK_DATA64* thunkData = reader.ReadStruct<IMAGE_THUNK_DATA64>(thunkOff + idx * sizeof(IMAGE_THUNK_DATA64));
                            if (!thunkData || thunkData->u1.AddressOfData == 0) {
                                break;
                            }

                            if (!(thunkData->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                                auto optImpByNameOff = RvaToFileOffset(ntHeadersConst, static_cast<DWORD>(thunkData->u1.AddressOfData), buffer.size(), sizeof(IMAGE_IMPORT_BY_NAME));
                                if (optImpByNameOff) {
                                    size_t impByNameOff = *optImpByNameOff;
                                    const IMAGE_IMPORT_BY_NAME* impName = reader.ReadStruct<IMAGE_IMPORT_BY_NAME>(impByNameOff);
                                    if (impName) {
                                        const char* funcNameStr = reader.ReadNullTerminatedString(impByNameOff + FIELD_OFFSET(IMAGE_IMPORT_BY_NAME, Name), 64);
                                        if (funcNameStr) {
                                            if (std::strcmp(funcNameStr, "LoadLibraryA") == 0) {
                                                iatLoadLibRva = iatRva + idx * sizeof(IMAGE_THUNK_DATA64);
                                            } else if (std::strcmp(funcNameStr, "GetProcAddress") == 0) {
                                                iatGetProcRva = iatRva + idx * sizeof(IMAGE_THUNK_DATA64);
                                            }
                                        }
                                    }
                                }
                            }
                            idx++;
                        }
                    }
                    if (iatLoadLibRva != 0 && iatGetProcRva != 0) break;
                }
            }
        }
        currDescOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    if (iatLoadLibRva == 0 || iatGetProcRva == 0) {
        outError = L"未在导入表中检索到有效的 LoadLibraryA 或 GetProcAddress IAT 条目";
        return false;
    }

    // 6. 寻找 ?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z 在 Export Table 中的条目与 RVA
    size_t trEatFileOffset = 0;
    IMAGE_DATA_DIRECTORY exportDataDir = ntHeadersConst->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDataDir.VirtualAddress != 0 && exportDataDir.Size != 0) {
        auto optExportOff = RvaToFileOffset(ntHeadersConst, exportDataDir.VirtualAddress, buffer.size(), sizeof(IMAGE_EXPORT_DIRECTORY));
        if (optExportOff) {
            const IMAGE_EXPORT_DIRECTORY* expDir = reader.ReadStruct<IMAGE_EXPORT_DIRECTORY>(*optExportOff);
            if (expDir) {
                auto optFunctionsOff = RvaToFileOffset(ntHeadersConst, expDir->AddressOfFunctions, buffer.size(), expDir->NumberOfFunctions * sizeof(DWORD));
                auto optNamesOff = RvaToFileOffset(ntHeadersConst, expDir->AddressOfNames, buffer.size(), expDir->NumberOfNames * sizeof(DWORD));
                auto optOrdinalsOff = RvaToFileOffset(ntHeadersConst, expDir->AddressOfNameOrdinals, buffer.size(), expDir->NumberOfNames * sizeof(WORD));

                if (optFunctionsOff && optNamesOff && optOrdinalsOff) {
                    const DWORD* pFunctions = reinterpret_cast<const DWORD*>(buffer.data() + *optFunctionsOff);
                    const DWORD* pNames = reinterpret_cast<const DWORD*>(buffer.data() + *optNamesOff);
                    const WORD* pOrdinals = reinterpret_cast<const WORD*>(buffer.data() + *optOrdinalsOff);

                    for (DWORD i = 0; i < expDir->NumberOfNames; ++i) {
                        auto optNameOff = RvaToFileOffset(ntHeadersConst, pNames[i], buffer.size(), 1);
                        if (optNameOff) {
                            const char* symName = reader.ReadNullTerminatedString(*optNameOff, 128);
                            if (symName && std::strcmp(symName, "?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z") == 0) {
                                WORD ordIndex = pOrdinals[i];
                                if (ordIndex >= expDir->NumberOfFunctions) {
                                    outError = L"导出表中函数序号超出 NumberOfFunctions 范围";
                                    return false;
                                }
                                if (origTrRva == 0) {
                                    origTrRva = pFunctions[ordIndex];
                                }
                                trEatFileOffset = *optFunctionsOff + ordIndex * sizeof(DWORD);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (origTrRva == 0 || trEatFileOffset == 0) {
        outError = L"未在导出表中检索到 ?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z 条目";
        return false;
    }

    // 7. 内存布局计算：
    // [在 .text 节 (严格保持只读可执行 RX，绝不修改为可写)]:
    // - PatchHeader ("LCLZ", 20 bytes)
    // - "qtcore_qm.dll\0"
    // - "tr\0"
    // - epCodeStartRva (仅记录标志并 jump 原 EntryPoint，绝不调用任何 API)
    // - trCodeStartRva (在真正脱离 Loader Lock 后的首次 Qt API 调用触发延迟引导)
    //
    // [在 .data 节 (原生可读写 RW，安全存放可变状态数据)]:
    // - uint8_t  g_bNeedsInit (0)
    // - uint8_t  g_bTrHooked (0)
    // - uint64_t g_pfnDetourPtr (0)

    const std::string dllName = "qtcore_qm.dll";
    const std::string funcName = "tr";
    DWORD dllNameLen = static_cast<DWORD>(dllName.length() + 1);
    DWORD funcNameLen = static_cast<DWORD>(funcName.length() + 1);

    // .text 只读数据与代码 RVA
    DWORD headerRva = caveRva;
    DWORD dllNameRva = headerRva + sizeof(PatchHeader);
    DWORD funcNameRva = dllNameRva + dllNameLen;
    DWORD totalTextConstLen = (funcNameRva + funcNameLen - caveRva);
    DWORD epCodeStartRva = (caveRva + totalTextConstLen + 15) & ~15;

    // .data 可写状态变量 RVA (位于 .data 节末尾可写空间)
    uint64_t dataSecEndRva64 = static_cast<uint64_t>(dataSec->VirtualAddress) + static_cast<uint64_t>(dataSec->Misc.VirtualSize);
    uint64_t dataCaveRva64 = (dataSecEndRva64 + 15) & ~15ULL;
    if (dataCaveRva64 > 0xFFFFFFFFULL) {
        outError = L".data 节变量 RVA 溢出 32 位整型范围";
        return false;
    }
    DWORD initFlagRva = static_cast<DWORD>(dataCaveRva64);
    DWORD trHookedFlagRva = initFlagRva + 1;
    DWORD detourPtrRva = (trHookedFlagRva + 1 + 7) & ~7; // 8 字节对齐
    DWORD totalDataBytesNeeded = (detourPtrRva + 8 - initFlagRva);

    auto optDataWriteOff = RvaToFileOffset(ntHeadersConst, initFlagRva, buffer.size(), totalDataBytesNeeded);
    if (!optDataWriteOff) {
        uint64_t dataRawEndRva64 = static_cast<uint64_t>(dataSec->VirtualAddress) + static_cast<uint64_t>(dataSec->SizeOfRawData);
        if (dataRawEndRva64 >= static_cast<uint64_t>(dataSec->VirtualAddress) + totalDataBytesNeeded) {
            initFlagRva = static_cast<DWORD>((dataRawEndRva64 - totalDataBytesNeeded) & ~7);
            trHookedFlagRva = initFlagRva + 1;
            detourPtrRva = (trHookedFlagRva + 1 + 7) & ~7;
            optDataWriteOff = RvaToFileOffset(ntHeadersConst, initFlagRva, buffer.size(), totalDataBytesNeeded);
        }
    }
    if (!optDataWriteOff) {
        outError = L"无法在 .data 节区中定位有效的物理可写空间以存放补丁状态变量";
        return false;
    }
    std::memset(buffer.data() + *optDataWriteOff, 0, totalDataBytesNeeded);

    auto SafeComputeRel32 = [](uint64_t targetRva, uint64_t nextInstrRva, int32_t& outDisp, const wchar_t* ctx, std::wstring& err) -> bool {
        int64_t diff = static_cast<int64_t>(targetRva) - static_cast<int64_t>(nextInstrRva);
        if (diff < INT32_MIN || diff > INT32_MAX) {
            err = std::wstring(L"相对偏移计算溢出 32 位整型范围: ") + ctx;
            return false;
        }
        outDisp = static_cast<int32_t>(diff);
        return true;
    };

    auto SafeComputeRel8 = [](size_t targetIdx, size_t nextInstrIdx, uint8_t& outDisp, const wchar_t* ctx, std::wstring& err) -> bool {
        int64_t diff = static_cast<int64_t>(targetIdx) - static_cast<int64_t>(nextInstrIdx);
        if (diff < -128 || diff > 127) {
            err = std::wstring(L"短跳转相对偏移计算溢出 8 位整型范围: ") + ctx;
            return false;
        }
        outDisp = static_cast<uint8_t>(static_cast<int8_t>(diff));
        return true;
    };

    // 8. 构建纯净 EntryPoint Shellcode (仅记录需要初始化并立即返回原 EntryPoint，绝不在 Loader Lock 中执行任何 API 或 I/O)
    std::vector<uint8_t> epShellcode;

    // cmp edx, 1 (DLL_PROCESS_ATTACH)
    epShellcode.push_back(0x83);
    epShellcode.push_back(0xfa);
    epShellcode.push_back(0x01);

    // jne origEntryPoint (2 字节 short jump 或 6 字节 near jump)
    DWORD currEpRva = epCodeStartRva + static_cast<DWORD>(epShellcode.size());
    int32_t dispSkip = 0;
    if (!SafeComputeRel32(origEntryPointRva, currEpRva + 6, dispSkip, L"epShellcode jne origEntryPoint", outError)) {
        return false;
    }
    epShellcode.push_back(0x0f);
    epShellcode.push_back(0x85);
    epShellcode.insert(epShellcode.end(), reinterpret_cast<uint8_t*>(&dispSkip), reinterpret_cast<uint8_t*>(&dispSkip) + 4);

    // mov byte ptr [rip + dispInitFlag], 1 (仅在内存 Code Cave 中标记状态，耗时 5ns)
    currEpRva = epCodeStartRva + static_cast<DWORD>(epShellcode.size());
    int32_t dispInitFlag = 0;
    if (!SafeComputeRel32(initFlagRva, currEpRva + 7, dispInitFlag, L"epShellcode initFlag", outError)) {
        return false;
    }
    epShellcode.push_back(0xc6);
    epShellcode.push_back(0x05);
    epShellcode.insert(epShellcode.end(), reinterpret_cast<uint8_t*>(&dispInitFlag), reinterpret_cast<uint8_t*>(&dispInitFlag) + 4);
    epShellcode.push_back(0x01);

    // jmp origEntryPoint (直接跳回原始 EntryPoint)
    currEpRva = epCodeStartRva + static_cast<DWORD>(epShellcode.size());
    int32_t dispBack = 0;
    if (!SafeComputeRel32(origEntryPointRva, currEpRva + 5, dispBack, L"epShellcode jmp origEntryPoint", outError)) {
        return false;
    }
    epShellcode.push_back(0xe9);
    epShellcode.insert(epShellcode.end(), reinterpret_cast<uint8_t*>(&dispBack), reinterpret_cast<uint8_t*>(&dispBack) + 4);

    DWORD trCodeStartRva = epCodeStartRva + static_cast<DWORD>(epShellcode.size());

    // 9. 构建脱离 Loader Lock 后的首个 Qt API (QMetaObject::tr) 延迟引导 Shellcode
    std::vector<uint8_t> trShellcode;

    // 0. cmp byte ptr [rip + dispTrHooked], 1 (已初始化则直接跳转 Detour)
    DWORD currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispTrHooked = 0;
    if (!SafeComputeRel32(trHookedFlagRva, currTrRva + 7, dispTrHooked, L"trShellcode trHookedFlag", outError)) {
        return false;
    }
    trShellcode.push_back(0x80);
    trShellcode.push_back(0x3d);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispTrHooked), reinterpret_cast<uint8_t*>(&dispTrHooked) + 4);
    trShellcode.push_back(0x01);

    // je jump_detour (短跳转 0x74)
    size_t jeDetourIdx = trShellcode.size();
    trShellcode.push_back(0x74);
    trShellcode.push_back(0x00); // 待回填 1 字节相对偏移

    // 1. 保护所有参数寄存器与易失寄存器 (rax, rcx, rdx, rbx, r8, r9, r10, r11)
    const uint8_t pushRegs[] = { 0x50, 0x51, 0x52, 0x53, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 };
    trShellcode.insert(trShellcode.end(), pushRegs, pushRegs + sizeof(pushRegs));

    // 2. 栈对齐：sub rsp, 0x28
    const uint8_t subRsp[] = { 0x48, 0x83, 0xec, 0x28 };
    trShellcode.insert(trShellcode.end(), subRsp, subRsp + sizeof(subRsp));

    // 3. LoadLibraryA("qtcore_qm.dll") (在脱离 Loader Lock 后的普通工作线程中安全调用)
    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispDllName = 0;
    if (!SafeComputeRel32(dllNameRva, currTrRva + 7, dispDllName, L"trShellcode dllName", outError)) {
        return false;
    }
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x8d);
    trShellcode.push_back(0x0d);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispDllName), reinterpret_cast<uint8_t*>(&dispDllName) + 4);

    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispLoadLib = 0;
    if (!SafeComputeRel32(iatLoadLibRva, currTrRva + 6, dispLoadLib, L"trShellcode iatLoadLib", outError)) {
        return false;
    }
    trShellcode.push_back(0xff);
    trShellcode.push_back(0x15);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispLoadLib), reinterpret_cast<uint8_t*>(&dispLoadLib) + 4);

    // test rax, rax
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x85);
    trShellcode.push_back(0xc0);

    // jz fallback_exit (短跳转 0x74)
    size_t jzLoadFail = trShellcode.size();
    trShellcode.push_back(0x74);
    trShellcode.push_back(0x00);

    // 4. GetProcAddress(hDll, "tr")
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x89);
    trShellcode.push_back(0xc1); // mov rcx, rax

    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispTrFuncName = 0;
    if (!SafeComputeRel32(funcNameRva, currTrRva + 7, dispTrFuncName, L"trShellcode funcName", outError)) {
        return false;
    }
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x8d);
    trShellcode.push_back(0x15);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispTrFuncName), reinterpret_cast<uint8_t*>(&dispTrFuncName) + 4);

    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispGetProc = 0;
    if (!SafeComputeRel32(iatGetProcRva, currTrRva + 6, dispGetProc, L"trShellcode iatGetProc", outError)) {
        return false;
    }
    trShellcode.push_back(0xff);
    trShellcode.push_back(0x15);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispGetProc), reinterpret_cast<uint8_t*>(&dispGetProc) + 4);

    // test rax, rax
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x85);
    trShellcode.push_back(0xc0);

    // jz fallback_exit (短跳转 0x74)
    size_t jzGetProcFail = trShellcode.size();
    trShellcode.push_back(0x74);
    trShellcode.push_back(0x00);

    // 5. 保存解析出的 detour 函数指针并置标志位
    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispDetour = 0;
    if (!SafeComputeRel32(detourPtrRva, currTrRva + 7, dispDetour, L"trShellcode detourPtr", outError)) {
        return false;
    }
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x89);
    trShellcode.push_back(0x05);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispDetour), reinterpret_cast<uint8_t*>(&dispDetour) + 4);

    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispTrHooked2 = 0;
    if (!SafeComputeRel32(trHookedFlagRva, currTrRva + 7, dispTrHooked2, L"trShellcode trHookedFlag2", outError)) {
        return false;
    }
    trShellcode.push_back(0xc6);
    trShellcode.push_back(0x05);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispTrHooked2), reinterpret_cast<uint8_t*>(&dispTrHooked2) + 4);
    trShellcode.push_back(0x01);

    // fallback_exit 目标点
    size_t fallbackIdx = trShellcode.size();
    if (!SafeComputeRel8(fallbackIdx, jzLoadFail + 2, trShellcode[jzLoadFail + 1], L"trShellcode jzLoadFail", outError)) {
        return false;
    }
    if (!SafeComputeRel8(fallbackIdx, jzGetProcFail + 2, trShellcode[jzGetProcFail + 1], L"trShellcode jzGetProcFail", outError)) {
        return false;
    }

    // 6. 恢复栈与寄存器
    const uint8_t addRsp[] = { 0x48, 0x83, 0xc4, 0x28 };
    trShellcode.insert(trShellcode.end(), addRsp, addRsp + sizeof(addRsp));

    const uint8_t popRegs[] = { 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5b, 0x5a, 0x59, 0x58 };
    trShellcode.insert(trShellcode.end(), popRegs, popRegs + sizeof(popRegs));

    // jump_detour 目标点
    size_t jumpDetourIdx = trShellcode.size();
    if (!SafeComputeRel8(jumpDetourIdx, jeDetourIdx + 2, trShellcode[jeDetourIdx + 1], L"trShellcode jeDetour", outError)) {
        return false;
    }

    // 7. cmp qword ptr [rip + dispDetourPtr], 0
    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispDetour2 = 0;
    if (!SafeComputeRel32(detourPtrRva, currTrRva + 8, dispDetour2, L"trShellcode detourPtr2", outError)) {
        return false;
    }
    trShellcode.push_back(0x48);
    trShellcode.push_back(0x83);
    trShellcode.push_back(0x3d);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispDetour2), reinterpret_cast<uint8_t*>(&dispDetour2) + 4);
    trShellcode.push_back(0x00);

    // je jump_orig (74 06)
    trShellcode.push_back(0x74);
    trShellcode.push_back(0x06);

    // jmp qword ptr [rip + dispDetourJump] (ff 25 disp32)
    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispDetourJump = 0;
    if (!SafeComputeRel32(detourPtrRva, currTrRva + 6, dispDetourJump, L"trShellcode detourJump", outError)) {
        return false;
    }
    trShellcode.push_back(0xff);
    trShellcode.push_back(0x25);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispDetourJump), reinterpret_cast<uint8_t*>(&dispDetourJump) + 4);

    // jump_orig: jmp origTrRva (e9 dispOrigTr)
    currTrRva = trCodeStartRva + static_cast<DWORD>(trShellcode.size());
    int32_t dispOrigTr = 0;
    if (!SafeComputeRel32(origTrRva, currTrRva + 5, dispOrigTr, L"trShellcode origTr", outError)) {
        return false;
    }
    trShellcode.push_back(0xe9);
    trShellcode.insert(trShellcode.end(), reinterpret_cast<uint8_t*>(&dispOrigTr), reinterpret_cast<uint8_t*>(&dispOrigTr) + 4);

    // 10. 统一使用 RvaToFileOffset 校验 Code Cave 是否超出 .text 节大小与文件边界
    DWORD totalCaveBytesNeeded = (trCodeStartRva - caveRva) + static_cast<DWORD>(trShellcode.size());
    auto optCaveWriteOff = RvaToFileOffset(ntHeadersConst, caveRva, buffer.size(), totalCaveBytesNeeded);
    if (!optCaveWriteOff) {
        outError = L".text 节末尾剩余空间不足以容纳 Code Cave";
        return false;
    }

    size_t caveOff = *optCaveWriteOff;
    size_t epCodeStartOff = caveOff + (epCodeStartRva - caveRva);
    size_t trCodeStartOff = caveOff + (trCodeStartRva - caveRva);

    // 写入 LCLZ 补丁元数据头 (PatchHeader)
    PatchHeader patchHdr;
    std::memcpy(patchHdr.magic, "LCLZ", 4);
    patchHdr.version = 2;
    patchHdr.originalEntryRva = origEntryPointRva;
    patchHdr.origTrRva = origTrRva;
    patchHdr.payloadSize = totalCaveBytesNeeded;

    std::memset(buffer.data() + caveOff, 0, totalCaveBytesNeeded);
    std::memcpy(buffer.data() + caveOff, &patchHdr, sizeof(PatchHeader));
    std::memcpy(buffer.data() + caveOff + (dllNameRva - caveRva), dllName.c_str(), dllNameLen);
    std::memcpy(buffer.data() + caveOff + (funcNameRva - caveRva), funcName.c_str(), funcNameLen);
    std::memcpy(buffer.data() + epCodeStartOff, epShellcode.data(), epShellcode.size());
    std::memcpy(buffer.data() + trCodeStartOff, trShellcode.data(), trShellcode.size());

    // 11. 更新 EntryPoint 指向纯净轻量标记 Shellcode
    IMAGE_NT_HEADERS64* ntHeadersMut = reinterpret_cast<IMAGE_NT_HEADERS64*>(buffer.data() + ntHeaderOff);
    ntHeadersMut->OptionalHeader.AddressOfEntryPoint = epCodeStartRva;

    // 12. 更新 Export Address Table 中的 ?tr@QMetaObject 指向脱离 Loader Lock 后的延迟引导 Shellcode
    *reinterpret_cast<DWORD*>(buffer.data() + trEatFileOffset) = trCodeStartRva;

    // 13. 注意：.text 节严格保持原生 RX 属性 (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)，绝不赋予写权限！
    // 所有可变状态（initFlag, trHookedFlag, detourPtr）均已安全安置于 .data 节区中。

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

bool PePatcher::GetPatchInfo(const std::wstring& dllPath, PatchInfo& outInfo, std::wstring& outError) {
    std::ifstream inFile(dllPath, std::ios::binary | std::ios::ate);
    if (!inFile.is_open()) {
        outError = L"无法打开文件: " + dllPath;
        return false;
    }

    std::streamsize fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    if (fileSize < static_cast<std::streamsize>(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64))) {
        outError = L"文件过小";
        return false;
    }

    std::vector<uint8_t> buffer(fileSize);
    if (!inFile.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        outError = L"读取文件失败";
        return false;
    }
    inFile.close();

    SafePeReader reader(buffer.data(), buffer.size());

    const IMAGE_DOS_HEADER* dosHeader = reader.ReadStruct<IMAGE_DOS_HEADER>(0);
    if (!dosHeader || dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
        outError = L"无效的 DOS 头部";
        return false;
    }

    size_t ntHeaderOff = static_cast<size_t>(dosHeader->e_lfanew);
    if (!reader.InBounds(ntHeaderOff, sizeof(IMAGE_NT_HEADERS64))) {
        outError = L"NT 头部超出文件边界";
        return false;
    }

    const IMAGE_NT_HEADERS64* ntHeaders = reader.ReadStruct<IMAGE_NT_HEADERS64>(ntHeaderOff);
    if (!ntHeaders || ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = L"非 64 位 PE 文件";
        return false;
    }

    WORD numSections = ntHeaders->FileHeader.NumberOfSections;
    if (numSections == 0 || numSections > 96) {
        outError = L"异常的节区数量";
        return false;
    }

    size_t secHeadersOff = ntHeaderOff + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + ntHeaders->FileHeader.SizeOfOptionalHeader;
    if (!reader.InBounds(secHeadersOff, sizeof(IMAGE_SECTION_HEADER) * numSections)) {
        outError = L"节区头部数组超出文件边界";
        return false;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
    const IMAGE_SECTION_HEADER* textSec = nullptr;

    for (WORD i = 0; i < numSections; ++i) {
        if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
            textSec = &sections[i];
            break;
        }
    }

    if (!textSec) {
        outError = L"未找到 .text 节";
        return false;
    }

    uint64_t textSecEndRva64 = static_cast<uint64_t>(textSec->VirtualAddress) + static_cast<uint64_t>(textSec->Misc.VirtualSize);
    uint64_t caveRva64 = (textSecEndRva64 + 15) & ~15ULL;
    if (caveRva64 <= 0xFFFFFFFFULL) {
        DWORD caveRva = static_cast<DWORD>(caveRva64);
        auto optCaveOff = RvaToFileOffset(ntHeaders, caveRva, buffer.size(), sizeof(PatchHeader));

        if (optCaveOff) {
            const PatchHeader* pHeader = reader.ReadStruct<PatchHeader>(*optCaveOff);
            if (pHeader && std::memcmp(pHeader->magic, "LCLZ", 4) == 0) {
                outInfo.isPatched = true;
                outInfo.version = pHeader->version;
                outInfo.originalEntryRva = pHeader->originalEntryRva;
                outInfo.origTrRva = pHeader->origTrRva;
                outInfo.payloadSize = pHeader->payloadSize;
                return true;
            }
        }
    }

    outInfo.isPatched = false;
    return true;
}

uint32_t PePatcher::GetModuleSizeOfImage(HMODULE hMod) {
    if (!hMod) return 0;
    SafePeReader reader(reinterpret_cast<const uint8_t*>(hMod), 0x100000000ULL /* 4GB memory view limit */);
    const IMAGE_DOS_HEADER* dos = reader.ReadStruct<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return 0;
    }
    const IMAGE_NT_HEADERS64* nt = reader.ReadStruct<IMAGE_NT_HEADERS64>(static_cast<size_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

bool PePatcher::GetPatchInfoFromMemory(HMODULE hMod, PatchInfo& outInfo) {
    outInfo = PatchInfo{};
    if (!hMod) return false;

    uint32_t sizeOfImage = GetModuleSizeOfImage(hMod);
    if (sizeOfImage == 0) return false;

    SafePeReader reader(reinterpret_cast<const uint8_t*>(hMod), sizeOfImage);
    const IMAGE_DOS_HEADER* dos = reader.ReadStruct<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;

    const IMAGE_NT_HEADERS64* nt = reader.ReadStruct<IMAGE_NT_HEADERS64>(static_cast<size_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;

    WORD numSections = nt->FileHeader.NumberOfSections;
    if (numSections == 0 || numSections > 96) return false;

    size_t secArrayOffset = static_cast<size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < numSections; ++i) {
        const IMAGE_SECTION_HEADER* sec = reader.ReadStruct<IMAGE_SECTION_HEADER>(secArrayOffset + i * sizeof(IMAGE_SECTION_HEADER));
        if (!sec) continue;

        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            uint32_t textEndRva = sec->VirtualAddress + sec->Misc.VirtualSize;
            uint32_t caveRva = (textEndRva + 15) & ~15;
            const PatchHeader* pH = reader.ReadStruct<PatchHeader>(caveRva);
            if (pH && std::memcmp(pH->magic, "LCLZ", 4) == 0 && pH->origTrRva != 0) {
                outInfo.isPatched = true;
                outInfo.version = pH->version;
                outInfo.originalEntryRva = pH->originalEntryRva;
                outInfo.origTrRva = pH->origTrRva;
                outInfo.payloadSize = pH->payloadSize;
                return true;
            }
        }
    }
    return false;
}


