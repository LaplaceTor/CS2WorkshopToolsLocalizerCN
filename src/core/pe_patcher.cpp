#include "core/pe_patcher.h"
#include <windows.h>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <initializer_list>



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

namespace {

// ===========================================================================
// PE 补丁相关常量
// 此前这些值以裸数字与魔法字符串形式散落在 PatchQtCore 与各探测函数里，
// 现在集中定义，改 PE 布局时只需动这一处。
// ===========================================================================
constexpr WORD        kMaxSections          = 96;      // NumberOfSections 合理上限（PE 规范上限）
constexpr uint64_t    kCaveAlignment        = 16;      // Code Cave 起始地址对齐粒度
constexpr uint64_t    kMaxRva32             = 0xFFFFFFFFULL;
constexpr int         kMaxImportThunks      = 4096;    // 单个导入描述符最多扫描的 thunk 数
constexpr size_t      kMaxImportDllNameLen  = 128;
constexpr size_t      kMaxImportFuncNameLen = 64;
constexpr size_t      kMaxExportSymNameLen  = 128;
constexpr char        kLclzMagic[5]         = "LCLZ";  // 补丁元数据魔数
constexpr uint32_t    kPatchVersion         = 3;       // v3: 无状态纯 EAT 重定向，零 .data 污染，无 EntryPoint 劫持
constexpr const char* kTrExportSymbol       = "?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z";
constexpr const char* kInjectDllName        = "qtcore_qm.dll";
constexpr const char* kInjectEntryName      = "tr";

// ---------------------------------------------------------------------------
// 基础小工具
// ---------------------------------------------------------------------------

bool HasLclzMagic(const PatchHeader* header) {
    return header != nullptr && std::memcmp(header->magic, kLclzMagic, 4) == 0;
}

const IMAGE_SECTION_HEADER* FindSection(
    const IMAGE_SECTION_HEADER* sections, WORD count, const char* name)
{
    if (!sections) return nullptr;
    for (WORD i = 0; i < count; ++i) {
        if (std::memcmp(sections[i].Name, name, 5) == 0) {
            return &sections[i];
        }
    }
    return nullptr;
}

// Code Cave 起始 RVA：节区虚拟末尾向上对齐到 kCaveAlignment
uint64_t CaveRvaOf(const IMAGE_SECTION_HEADER* sec) {
    const uint64_t endRva = static_cast<uint64_t>(sec->VirtualAddress)
                          + static_cast<uint64_t>(sec->Misc.VirtualSize);
    return (endRva + kCaveAlignment - 1) & ~(kCaveAlignment - 1);
}

bool ComputeRel32(uint64_t targetRva, uint64_t nextInstrRva, int32_t& outDisp,
                  const wchar_t* ctx, std::wstring& outError) {
    const int64_t diff = static_cast<int64_t>(targetRva) - static_cast<int64_t>(nextInstrRva);
    if (diff < INT32_MIN || diff > INT32_MAX) {
        outError = std::wstring(L"相对偏移计算溢出 32 位整型范围: ") + ctx;
        return false;
    }
    outDisp = static_cast<int32_t>(diff);
    return true;
}

bool ComputeRel8(size_t targetIdx, size_t nextInstrIdx, uint8_t& outDisp,
                 const wchar_t* ctx, std::wstring& outError) {
    const int64_t diff = static_cast<int64_t>(targetIdx) - static_cast<int64_t>(nextInstrIdx);
    if (diff < -128 || diff > 127) {
        outError = std::wstring(L"短跳转相对偏移计算溢出 8 位整型范围: ") + ctx;
        return false;
    }
    outDisp = static_cast<uint8_t>(static_cast<int8_t>(diff));
    return true;
}

void EmitDisp32(std::vector<uint8_t>& code, int32_t disp) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&disp);
    code.insert(code.end(), p, p + sizeof(disp));
}

// 追加一条「opcode + rel32 位移」指令。
// instrTotalLen 为该指令总长度（含 4 字节位移字段），用于推算下一条指令起始 RVA。
// 此前这段样板在 PatchQtCore 里被复制了 13 次，每处都在手算 RVA 并做指针强转。
bool EmitRel32(std::vector<uint8_t>& code, DWORD codeBaseRva,
               std::initializer_list<uint8_t> opcode, size_t instrTotalLen,
               uint64_t targetRva, const wchar_t* ctx, std::wstring& outError)
{
    const uint64_t currRva = static_cast<uint64_t>(codeBaseRva) + code.size();
    int32_t disp = 0;
    if (!ComputeRel32(targetRva, currRva + instrTotalLen, disp, ctx, outError)) {
        return false;
    }
    code.insert(code.end(), opcode.begin(), opcode.end());
    EmitDisp32(code, disp);
    return true;
}

// 回填已发射的短跳转指令（0x74 xx）的 8 位位移，fromIdx 为 opcode 下标
bool BackfillRel8(std::vector<uint8_t>& code, size_t fromIdx, size_t targetIdx,
                  const wchar_t* ctx, std::wstring& outError) {
    return ComputeRel8(targetIdx, fromIdx + 2, code[fromIdx + 1], ctx, outError);
}

bool ReadWholeFile(const std::wstring& path, const wchar_t* openFailMsg,
                   std::vector<uint8_t>& outBuffer, std::wstring& outError) {
    std::ifstream inFile(path, std::ios::binary | std::ios::ate);
    if (!inFile.is_open()) {
        outError = std::wstring(openFailMsg) + path;
        return false;
    }

    const std::streamsize fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    if (fileSize < static_cast<std::streamsize>(sizeof(IMAGE_DOS_HEADER))) {
        outError = L"文件过小，不是有效的 PE 文件";
        return false;
    }

    outBuffer.resize(static_cast<size_t>(fileSize));
    if (!inFile.read(reinterpret_cast<char*>(outBuffer.data()), fileSize)) {
        outError = L"读取源文件失败";
        return false;
    }
    inFile.close();
    return true;
}

// ---------------------------------------------------------------------------
// 1. PE 结构解析
// ---------------------------------------------------------------------------

struct PeImage {
    const IMAGE_NT_HEADERS64* nt = nullptr;
    size_t ntOffset = 0;
    WORD numSections = 0;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    const IMAGE_SECTION_HEADER* text = nullptr;
    const IMAGE_SECTION_HEADER* data = nullptr;
};

// 严格校验 DOS / NT / Optional 头与节表，并定位 .text 与 .data
bool ParsePeImage(const SafePeReader& reader, size_t fileSize, PeImage& out, std::wstring& outError) {
    const IMAGE_DOS_HEADER* dosHeader = reader.ReadStruct<IMAGE_DOS_HEADER>(0);
    if (!dosHeader || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        outError = L"无效的 DOS 签名 (IMAGE_DOS_SIGNATURE)";
        return false;
    }

    if (dosHeader->e_lfanew <= 0) {
        outError = L"无效的 DOS e_lfanew 偏移 (必须大于 0)";
        return false;
    }

    const size_t ntHeaderOff = static_cast<size_t>(dosHeader->e_lfanew);
    if (!reader.InBounds(ntHeaderOff, sizeof(IMAGE_NT_HEADERS64))) {
        outError = L"NT 头部偏移超出文件边界";
        return false;
    }

    const IMAGE_NT_HEADERS64* nt = reader.ReadStruct<IMAGE_NT_HEADERS64>(ntHeaderOff);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        outError = L"无效的 NT 签名 (IMAGE_NT_SIGNATURE)";
        return false;
    }

    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        outError = L"仅支持 64 位 (x64 / AMD64) PE 动态库";
        return false;
    }

    const WORD numSections = nt->FileHeader.NumberOfSections;
    if (numSections == 0 || numSections > kMaxSections) {
        outError = L"异常的节区数量 (NumberOfSections)";
        return false;
    }

    if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        outError = L"无效的 OptionalHeader 大小 (SizeOfOptionalHeader)";
        return false;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = L"仅支持 PE32+ (64 位) 格式";
        return false;
    }

    if (nt->OptionalHeader.SizeOfHeaders > fileSize) {
        outError = L"SizeOfHeaders 超出文件边界";
        return false;
    }

    if (nt->OptionalHeader.SizeOfImage == 0) {
        outError = L"无效的 SizeOfImage (为 0)";
        return false;
    }

    const size_t secHeadersOff = ntHeaderOff
        + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt->FileHeader.SizeOfOptionalHeader;
    if (!reader.InBounds(secHeadersOff, sizeof(IMAGE_SECTION_HEADER) * numSections)) {
        outError = L"节区头部数组超出文件边界";
        return false;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < numSections; ++i) {
        // 校验每个 section 的物理映射范围合法性
        if (sections[i].SizeOfRawData > 0) {
            if (!reader.InBounds(sections[i].PointerToRawData, sections[i].SizeOfRawData)) {
                outError = L"节区数据范围超出文件物理边界";
                return false;
            }
        }
    }

    out.nt = nt;
    out.ntOffset = ntHeaderOff;
    out.numSections = numSections;
    out.sections = sections;
    out.text = FindSection(sections, numSections, ".text");
    out.data = FindSection(sections, numSections, ".data");

    if (!out.text) {
        outError = L"未在 PE 文件中找到 .text 节";
        return false;
    }
    if (!out.data) {
        outError = L"未在 PE 文件中找到 .data 节";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2. 恢复原始入口点与 QMetaObject::tr 的 RVA
//    （重复打补丁时从 LCLZ v3 补丁头还原）
// ---------------------------------------------------------------------------
void RestoreOriginalEntryPoint(
    const SafePeReader& reader,
    const PeImage& img,
    const std::vector<uint8_t>& buffer,
    uint64_t textEndRva,
    DWORD caveRva,
    DWORD& inOutEntryRva,
    DWORD& inOutTrRva)
{
    (void)textEndRva;
    auto optCaveHeaderOff = PePatcher::RvaToFileOffset(img.nt, caveRva, buffer.size(), sizeof(PatchHeader));
    if (optCaveHeaderOff) {
        const PatchHeader* header = reader.ReadStruct<PatchHeader>(*optCaveHeaderOff);
        if (HasLclzMagic(header) && header->version == kPatchVersion) {
            if (header->origTrRva != 0) {
                inOutTrRva = header->origTrRva;
            }
            if (header->originalEntryRva != 0) {
                inOutEntryRva = header->originalEntryRva;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 3. 导入表：定位 KERNEL32 的 LoadLibraryA / GetProcAddress 在 IAT 中的 RVA
// ---------------------------------------------------------------------------

struct ImportSlots {
    DWORD loadLibraryA = 0;
    DWORD getProcAddress = 0;
};

bool ResolveImportSlots(const SafePeReader& reader, const PeImage& img,
                        size_t fileSize, ImportSlots& out, std::wstring& outError)
{
    const IMAGE_DATA_DIRECTORY importDataDir =
        img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir.VirtualAddress == 0 || importDataDir.Size == 0) {
        outError = L"PE 文件缺少导入表 (IMAGE_DIRECTORY_ENTRY_IMPORT)";
        return false;
    }

    auto optImportOff = PePatcher::RvaToFileOffset(
        img.nt, importDataDir.VirtualAddress, fileSize, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (!optImportOff) {
        outError = L"导入表偏移超出文件物理边界";
        return false;
    }

    size_t currDescOff = *optImportOff;
    while (reader.InBounds(currDescOff, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        const IMAGE_IMPORT_DESCRIPTOR* importDesc = reader.ReadStruct<IMAGE_IMPORT_DESCRIPTOR>(currDescOff);
        if (!importDesc || importDesc->Name == 0) {
            break;
        }

        auto optNameOff = PePatcher::RvaToFileOffset(img.nt, importDesc->Name, fileSize, 1);
        if (optNameOff) {
            const char* dllName = reader.ReadNullTerminatedString(*optNameOff, kMaxImportDllNameLen);
            if (dllName) {
                std::string dllNameLower = dllName;
                std::transform(dllNameLower.begin(), dllNameLower.end(), dllNameLower.begin(), ::tolower);

                if (dllNameLower.find("kernel32") != std::string::npos) {
                    const DWORD thunkRva = importDesc->OriginalFirstThunk
                        ? importDesc->OriginalFirstThunk : importDesc->FirstThunk;
                    const DWORD iatRva = importDesc->FirstThunk;

                    auto optThunkOff = PePatcher::RvaToFileOffset(
                        img.nt, thunkRva, fileSize, sizeof(IMAGE_THUNK_DATA64));
                    if (optThunkOff) {
                        const size_t thunkOff = *optThunkOff;
                        int idx = 0;
                        while (idx < kMaxImportThunks &&
                               reader.InBounds(thunkOff + idx * sizeof(IMAGE_THUNK_DATA64), sizeof(IMAGE_THUNK_DATA64)))
                        {
                            const IMAGE_THUNK_DATA64* thunkData =
                                reader.ReadStruct<IMAGE_THUNK_DATA64>(thunkOff + idx * sizeof(IMAGE_THUNK_DATA64));
                            if (!thunkData || thunkData->u1.AddressOfData == 0) {
                                break;
                            }

                            if (!(thunkData->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                                auto optImpByNameOff = PePatcher::RvaToFileOffset(
                                    img.nt, static_cast<DWORD>(thunkData->u1.AddressOfData),
                                    fileSize, sizeof(IMAGE_IMPORT_BY_NAME));
                                if (optImpByNameOff) {
                                    const size_t impByNameOff = *optImpByNameOff;
                                    const IMAGE_IMPORT_BY_NAME* impName = reader.ReadStruct<IMAGE_IMPORT_BY_NAME>(impByNameOff);
                                    if (impName) {
                                        const char* funcNameStr = reader.ReadNullTerminatedString(
                                            impByNameOff + FIELD_OFFSET(IMAGE_IMPORT_BY_NAME, Name),
                                            kMaxImportFuncNameLen);
                                        if (funcNameStr) {
                                            if (std::strcmp(funcNameStr, "LoadLibraryA") == 0) {
                                                out.loadLibraryA = iatRva + idx * sizeof(IMAGE_THUNK_DATA64);
                                            } else if (std::strcmp(funcNameStr, "GetProcAddress") == 0) {
                                                out.getProcAddress = iatRva + idx * sizeof(IMAGE_THUNK_DATA64);
                                            }
                                        }
                                    }
                                }
                            }
                            idx++;
                        }
                    }
                    if (out.loadLibraryA != 0 && out.getProcAddress != 0) break;
                }
            }
        }
        currDescOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    if (out.loadLibraryA == 0 || out.getProcAddress == 0) {
        outError = L"未在导入表中检索到有效的 LoadLibraryA 或 GetProcAddress IAT 条目";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 4. 导出表：定位 ?tr@QMetaObject 条目及其在 EAT 中的文件偏移
// ---------------------------------------------------------------------------

struct TrExportSlot {
    size_t eatFileOffset = 0;   // EAT 中该条目所在的文件偏移，用于改写函数 RVA
};

bool ResolveTrExport(const SafePeReader& reader, const PeImage& img,
                     const std::vector<uint8_t>& buffer,
                     DWORD& inOutTrRva, TrExportSlot& out, std::wstring& outError)
{
    const IMAGE_DATA_DIRECTORY exportDataDir =
        img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDataDir.VirtualAddress == 0 || exportDataDir.Size == 0) {
        outError = L"未在导出表中检索到 "
            + std::wstring(kTrExportSymbol, kTrExportSymbol + std::strlen(kTrExportSymbol)) + L" 条目";
        return false;
    }

    auto optExportOff = PePatcher::RvaToFileOffset(
        img.nt, exportDataDir.VirtualAddress, buffer.size(), sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!optExportOff) {
        outError = L"未在导出表中检索到 "
            + std::wstring(kTrExportSymbol, kTrExportSymbol + std::strlen(kTrExportSymbol)) + L" 条目";
        return false;
    }

    const IMAGE_EXPORT_DIRECTORY* expDir = reader.ReadStruct<IMAGE_EXPORT_DIRECTORY>(*optExportOff);
    if (!expDir) {
        outError = L"未在导出表中检索到 "
            + std::wstring(kTrExportSymbol, kTrExportSymbol + std::strlen(kTrExportSymbol)) + L" 条目";
        return false;
    }

    auto optFunctionsOff = PePatcher::RvaToFileOffset(
        img.nt, expDir->AddressOfFunctions, buffer.size(), expDir->NumberOfFunctions * sizeof(DWORD));
    auto optNamesOff = PePatcher::RvaToFileOffset(
        img.nt, expDir->AddressOfNames, buffer.size(), expDir->NumberOfNames * sizeof(DWORD));
    auto optOrdinalsOff = PePatcher::RvaToFileOffset(
        img.nt, expDir->AddressOfNameOrdinals, buffer.size(), expDir->NumberOfNames * sizeof(WORD));

    if (optFunctionsOff && optNamesOff && optOrdinalsOff) {
        const DWORD* pFunctions = reinterpret_cast<const DWORD*>(buffer.data() + *optFunctionsOff);
        const DWORD* pNames = reinterpret_cast<const DWORD*>(buffer.data() + *optNamesOff);
        const WORD* pOrdinals = reinterpret_cast<const WORD*>(buffer.data() + *optOrdinalsOff);

        for (DWORD i = 0; i < expDir->NumberOfNames; ++i) {
            auto optNameOff = PePatcher::RvaToFileOffset(img.nt, pNames[i], buffer.size(), 1);
            if (optNameOff) {
                const char* symName = reader.ReadNullTerminatedString(*optNameOff, kMaxExportSymNameLen);
                if (symName && std::strcmp(symName, kTrExportSymbol) == 0) {
                    const WORD ordIndex = pOrdinals[i];
                    if (ordIndex >= expDir->NumberOfFunctions) {
                        outError = L"导出表中函数序号超出 NumberOfFunctions 范围";
                        return false;
                    }
                    if (inOutTrRva == 0) {
                        inOutTrRva = pFunctions[ordIndex];
                    }
                    out.eatFileOffset = *optFunctionsOff + ordIndex * sizeof(DWORD);
                    break;
                }
            }
        }
    }

    if (inOutTrRva == 0 || out.eatFileOffset == 0) {
        outError = L"未在导出表中检索到 "
            + std::wstring(kTrExportSymbol, kTrExportSymbol + std::strlen(kTrExportSymbol)) + L" 条目";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 5. 内存布局计算
//
// [.text 节，严格保持只读可执行 RX，绝不改为可写，零 .data 污染]
//   PatchHeader("LCLZ", v3) | "qtcore_qm.dll\0" | "tr\0" | trShellcode
// ---------------------------------------------------------------------------

struct CaveLayout {
    DWORD headerRva = 0;
    DWORD dllNameRva = 0;
    DWORD funcNameRva = 0;
    DWORD trCodeStartRva = 0;
};

bool PrepareCaveLayout(const PeImage& img, std::vector<uint8_t>& buffer,
                       DWORD caveRva, CaveLayout& out, std::wstring& outError)
{
    (void)img;
    (void)buffer;
    (void)outError;

    const std::string dllName = kInjectDllName;
    const std::string funcName = kInjectEntryName;
    const DWORD dllNameLen = static_cast<DWORD>(dllName.length() + 1);
    const DWORD funcNameLen = static_cast<DWORD>(funcName.length() + 1);

    out.headerRva = caveRva;
    out.dllNameRva = out.headerRva + sizeof(PatchHeader);
    out.funcNameRva = out.dllNameRva + dllNameLen;

    const DWORD totalTextConstLen = (out.funcNameRva + funcNameLen - caveRva);
    out.trCodeStartRva = (caveRva + totalTextConstLen + 15) & ~15;

    return true;
}

// ---------------------------------------------------------------------------
// 6. 构建 QMetaObject::tr 的无状态延迟引导 Shellcode (LCLZ v3)
//    - 脱离 Loader Lock 首次调用时动态引导注入 DLL
//    - 纯栈保护寄存器与易失寄存器，维持 16 字节栈对齐
//    - 零 .data 持久状态存储，完全避免对宿主模块全局内存的踩踏
//    - 加载失败立即通过 int 3 (0xCC) 断点中断严格拦截
// ---------------------------------------------------------------------------
bool BuildTrShellcode(const CaveLayout& layout, const ImportSlots& imports,
                      DWORD codeStartRva, DWORD origTrRva,
                      std::vector<uint8_t>& out, std::wstring& outError)
{
    (void)origTrRva;
    out.clear();

    // 1. 保护入参寄存器与调用者易失寄存器 (push rax, rcx, rdx, r8, r9)
    // 5 次 push = 40 字节。
    // 进入函数时，CALL 指令压入了 8 字节 Return Address (此时 RSP 为 8 mod 16)；
    // 5 次 push 后，RSP 相对原调用点偏移 48 字节，正好满足 16 字节对齐 (0 mod 16)。
    const uint8_t pushRegs[] = { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51 };
    out.insert(out.end(), pushRegs, pushRegs + sizeof(pushRegs));

    // 2. 分配 32 字节影子空间 (Shadow Space): sub rsp, 0x20 (仍保持 16 字节对齐)
    const uint8_t subRsp[] = { 0x48, 0x83, 0xec, 0x20 };
    out.insert(out.end(), subRsp, subRsp + sizeof(subRsp));

    // 3. LoadLibraryA("qtcore_qm.dll")
    // lea rcx, [rip + dllNameDisp]
    if (!EmitRel32(out, codeStartRva, { 0x48, 0x8d, 0x0d }, 7, layout.dllNameRva,
                   L"trShellcode dllName", outError)) return false;
    // call qword ptr [rip + loadLibIatDisp]
    if (!EmitRel32(out, codeStartRva, { 0xff, 0x15 }, 6, imports.loadLibraryA,
                   L"trShellcode iatLoadLib", outError)) return false;

    // test rax, rax
    const uint8_t testRax[] = { 0x48, 0x85, 0xc0 };
    out.insert(out.end(), testRax, testRax + sizeof(testRax));

    // jz fail_fast (短跳转 0x74，位移稍后回填)
    const size_t jzLoadFail = out.size();
    out.push_back(0x74);
    out.push_back(0x00);

    // 4. GetProcAddress(hDll, "tr")
    // mov rcx, rax
    const uint8_t movRcxRax[] = { 0x48, 0x89, 0xc1 };
    out.insert(out.end(), movRcxRax, movRcxRax + sizeof(movRcxRax));

    // lea rdx, [rip + funcNameDisp]
    if (!EmitRel32(out, codeStartRva, { 0x48, 0x8d, 0x15 }, 7, layout.funcNameRva,
                   L"trShellcode funcName", outError)) return false;
    // call qword ptr [rip + getProcIatDisp]
    if (!EmitRel32(out, codeStartRva, { 0xff, 0x15 }, 6, imports.getProcAddress,
                   L"trShellcode iatGetProc", outError)) return false;

    out.insert(out.end(), testRax, testRax + sizeof(testRax));

    // jz fail_fast (短跳转 0x74，位移稍后回填)
    const size_t jzGetProcFail = out.size();
    out.push_back(0x74);
    out.push_back(0x00);

    // 5. 保存目标函数指针到易失寄存器 r11 (mov r11, rax)
    const uint8_t movR11Rax[] = { 0x49, 0x89, 0xc3 };
    out.insert(out.end(), movR11Rax, movR11Rax + sizeof(movR11Rax));

    // 6. 恢复栈空间与入参寄存器
    const uint8_t addRsp[] = { 0x48, 0x83, 0xc4, 0x20 };
    out.insert(out.end(), addRsp, addRsp + sizeof(addRsp));

    // pop r9, r8, rdx, rcx, rax
    const uint8_t popRegs[] = { 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58 };
    out.insert(out.end(), popRegs, popRegs + sizeof(popRegs));

    // 7. 跳转至目标函数: jmp r11 (41 ff e3)
    // 此时栈顶正好是原始调用者的返回地址，且第 5 参数 [RSP+0x28] 保持原样透传
    const uint8_t jmpR11[] = { 0x41, 0xff, 0xe3 };
    out.insert(out.end(), jmpR11, jmpR11 + sizeof(jmpR11));

    // 8. 严格报错熔断拦截点 (fail_fast: int 3)
    const size_t failFastIdx = out.size();
    if (!BackfillRel8(out, jzLoadFail, failFastIdx, L"trShellcode jzLoadFail", outError)) return false;
    if (!BackfillRel8(out, jzGetProcFail, failFastIdx, L"trShellcode jzGetProcFail", outError)) return false;

    // int 3 (0xCC) 立即触发调试中断，坚决暴露部署环境丢失模块问题
    out.push_back(0xcc);

    return true;
}

// ---------------------------------------------------------------------------
// 8. 原子落盘：先写临时文件再整体替换，避免中断产生半写损坏
// ---------------------------------------------------------------------------
bool CommitPatchedFile(const std::vector<uint8_t>& buffer, const std::wstring& dstPath,
                       std::wstring& outError)
{
    const std::wstring tmpPath = dstPath + L".tmp";
    {
        std::ofstream outFile(tmpPath, std::ios::binary);
        if (!outFile.is_open()) {
            outError = L"无法写入临时文件: " + tmpPath;
            return false;
        }
        if (!outFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
            outError = L"写入目标文件数据失败";
            outFile.close();
            DeleteFileW(tmpPath.c_str());
            return false;
        }
    }

    if (!MoveFileExW(tmpPath.c_str(), dstPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        outError = L"提交目标文件失败: " + dstPath;
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

} // namespace

// ===========================================================================
// 主流程：读文件 → 校验 PE → 还原原始入口 → 解析 IAT/EAT → 计算布局
//          → 生成两端 Shellcode → 写回 → 原子落盘
// （各步骤的具体实现见上方匿名命名空间中的专职函数）
// ===========================================================================
bool PePatcher::PatchQtCore(const std::wstring& srcDllPath, const std::wstring& dstDllPath, std::wstring& outError) {
    std::vector<uint8_t> buffer;
    if (!ReadWholeFile(srcDllPath, L"无法打开源 Qt5Core.dll: ", buffer, outError)) {
        return false;
    }

    SafePeReader reader(buffer.data(), buffer.size());

    PeImage img;
    if (!ParsePeImage(reader, buffer.size(), img, outError)) {
        return false;
    }

    // Code Cave 起始 RVA（使用 64 位整型运算防止溢出）
    const uint64_t textEndRva = static_cast<uint64_t>(img.text->VirtualAddress)
                              + static_cast<uint64_t>(img.text->Misc.VirtualSize);
    const uint64_t caveRva64 = CaveRvaOf(img.text);
    if (caveRva64 > kMaxRva32) {
        outError = L"Code Cave RVA 溢出 32 位地址空间";
        return false;
    }
    const DWORD caveRva = static_cast<DWORD>(caveRva64);

    // 还原真实入口点与 QMetaObject::tr RVA（支持重复打补丁与历史旧补丁）
    DWORD origEntryPointRva = img.nt->OptionalHeader.AddressOfEntryPoint;
    DWORD origTrRva = 0;
    RestoreOriginalEntryPoint(reader, img, buffer, textEndRva, caveRva, origEntryPointRva, origTrRva);

    ImportSlots imports;
    if (!ResolveImportSlots(reader, img, buffer.size(), imports, outError)) {
        return false;
    }

    TrExportSlot trExport;
    if (!ResolveTrExport(reader, img, buffer, origTrRva, trExport, outError)) {
        return false;
    }

    CaveLayout layout;
    if (!PrepareCaveLayout(img, buffer, caveRva, layout, outError)) {
        return false;
    }

    std::vector<uint8_t> trShellcode;
    if (!BuildTrShellcode(layout, imports, layout.trCodeStartRva, origTrRva, trShellcode, outError)) {
        return false;
    }

    // 校验 Code Cave 是否超出 .text 节大小与文件边界
    const DWORD totalCaveBytesNeeded = (layout.trCodeStartRva - caveRva) + static_cast<DWORD>(trShellcode.size());
    auto optCaveWriteOff = RvaToFileOffset(img.nt, caveRva, buffer.size(), totalCaveBytesNeeded);
    if (!optCaveWriteOff) {
        outError = L".text 节末尾剩余空间不足以容纳 Code Cave";
        return false;
    }

    const size_t caveOff = *optCaveWriteOff;
    const size_t trCodeStartOff = caveOff + (layout.trCodeStartRva - caveRva);

    const std::string dllName = kInjectDllName;
    const std::string funcName = kInjectEntryName;
    const DWORD dllNameLen = static_cast<DWORD>(dllName.length() + 1);
    const DWORD funcNameLen = static_cast<DWORD>(funcName.length() + 1);

    PatchHeader patchHdr;
    std::memcpy(patchHdr.magic, kLclzMagic, 4);
    patchHdr.version = kPatchVersion;
    patchHdr.originalEntryRva = origEntryPointRva;
    patchHdr.origTrRva = origTrRva;
    patchHdr.payloadSize = totalCaveBytesNeeded;

    std::memset(buffer.data() + caveOff, 0, totalCaveBytesNeeded);
    std::memcpy(buffer.data() + caveOff, &patchHdr, sizeof(PatchHeader));
    std::memcpy(buffer.data() + caveOff + (layout.dllNameRva - caveRva), dllName.c_str(), dllNameLen);
    std::memcpy(buffer.data() + caveOff + (layout.funcNameRva - caveRva), funcName.c_str(), funcNameLen);
    std::memcpy(buffer.data() + trCodeStartOff, trShellcode.data(), trShellcode.size());

    // 注意：OptionalHeader.AddressOfEntryPoint 完全保持官方原生原样，绝对不修改！
    // 仅更新导出表中 ?tr@QMetaObject 指向延迟引导 Shellcode
    *reinterpret_cast<DWORD*>(buffer.data() + trExport.eatFileOffset) = layout.trCodeStartRva;

    // .text 保持原生 RX；.data 保持 100% 纯净零写入，彻底杜绝全局对象野指针与析构崩溃！

    return CommitPatchedFile(buffer, dstDllPath, outError);
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
    if (numSections == 0 || numSections > kMaxSections) {
        outError = L"异常的节区数量";
        return false;
    }

    size_t secHeadersOff = ntHeaderOff + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + ntHeaders->FileHeader.SizeOfOptionalHeader;
    if (!reader.InBounds(secHeadersOff, sizeof(IMAGE_SECTION_HEADER) * numSections)) {
        outError = L"节区头部数组超出文件边界";
        return false;
    }

    const IMAGE_SECTION_HEADER* textSec =
        FindSection(IMAGE_FIRST_SECTION(ntHeaders), numSections, ".text");
    if (!textSec) {
        outError = L"未找到 .text 节";
        return false;
    }

    const uint64_t caveRva64 = CaveRvaOf(textSec);
    if (caveRva64 <= kMaxRva32) {
        DWORD caveRva = static_cast<DWORD>(caveRva64);
        auto optCaveOff = RvaToFileOffset(ntHeaders, caveRva, buffer.size(), sizeof(PatchHeader));

        if (optCaveOff) {
            const PatchHeader* pHeader = reader.ReadStruct<PatchHeader>(*optCaveOff);
            if (HasLclzMagic(pHeader) && pHeader->version == kPatchVersion) {
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
    if (numSections == 0 || numSections > kMaxSections) return false;

    size_t secArrayOffset = static_cast<size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < numSections; ++i) {
        const IMAGE_SECTION_HEADER* sec = reader.ReadStruct<IMAGE_SECTION_HEADER>(secArrayOffset + i * sizeof(IMAGE_SECTION_HEADER));
        if (!sec) continue;

        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            uint32_t textEndRva = sec->VirtualAddress + sec->Misc.VirtualSize;
            uint32_t caveRva = (textEndRva + 15) & ~15;
            const PatchHeader* pH = reader.ReadStruct<PatchHeader>(caveRva);
            if (HasLclzMagic(pH) && pH->version == kPatchVersion && pH->origTrRva != 0) {
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


