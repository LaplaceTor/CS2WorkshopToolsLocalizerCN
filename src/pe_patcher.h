#pragma once

#include <string>

class PePatcher {
public:
    // 修补 64位 Qt5Core.dll，在 .text 节的代码洞中注入 LoadLibraryA("qtcore_qm.dll")
    static bool PatchQtCore(const std::wstring& srcDllPath, const std::wstring& dstDllPath, std::wstring& outError);
};

