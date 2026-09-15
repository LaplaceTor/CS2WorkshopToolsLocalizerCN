#pragma once
// 全项目统一的 UTF-8 <-> UTF-16 转换入口。
//
// 背景：本项目的字符串在四套表示之间来回穿梭——
//   * Win32 API 与 std::filesystem 使用 UTF-16 (wchar_t / std::wstring)
//   * 磁盘上的 JSONC 词典与 FGD 使用 UTF-8 (std::string)
//   * Qt 内部使用 UTF-16 (QString)
// 此前每个文件各写各的 `static_cast<wchar_t>(c)` / `static_cast<char>(wc)` /
// 裸 `MultiByteToWideChar`，其中按字节强转的写法会把中文彻底截坏。
//
// 约定：
//   * 跨越 Win32 / 文件系统边界一律用 Utf8ToWide / WideToUtf8
//   * 禁止再写 static_cast<wchar_t>(char) 之类的裸强转
//   * QString 与 std::wstring 互转请用 Qt 自带接口（toStdWString / fromStdWString）
//
// 该头文件只依赖 Win32，因此注入 DLL（不链 Qt）与主程序都能使用。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <string_view>

namespace enc {

// UTF-8 -> UTF-16。转换失败时返回空串（调用方按失败处理，不要静默忽略）。
inline std::wstring Utf8ToWide(std::string_view u8) {
    if (u8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), out.data(), needed);
    return out;
}

// UTF-16 -> UTF-8。转换失败时返回空串。
inline std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace enc
