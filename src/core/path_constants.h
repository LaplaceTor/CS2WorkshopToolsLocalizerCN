#pragma once

// 集中定义本项目涉及的全部路径字面量与路径组合规则。
//
// 背景：CS2 的 `game/bin/win64` 与若干关键文件名原先散落在
// backup_manager / cs2_detector / mainwindow / debug_window 四个模块中重复拼接，
// 任何一处目录结构变化都需要改十几个地方。这里收口为单点定义。
//
// 仅依赖 <filesystem> 与 <string>，因此主程序与注入 DLL 均可使用。

#include <filesystem>
#include <string>

namespace paths {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 目录名
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kGameDir    = L"game";
inline constexpr const wchar_t* kBinDir     = L"bin";
inline constexpr const wchar_t* kWin64Dir   = L"win64";
inline constexpr const wchar_t* kContentDir = L"content";
inline constexpr const wchar_t* kAddonsDir  = L"csgo_addons";

// 本工具自己维护的工作目录（位于 CS2 根目录之外）
inline constexpr const wchar_t* kTranslationsDir = L"translations";
inline constexpr const wchar_t* kBackupDir       = L"backup";

// Steam 安装布局（用于自动检测 CS2 根目录）
inline constexpr const wchar_t* kSteamAppsDir     = L"steamapps";
inline constexpr const wchar_t* kCommonDir        = L"common";
inline constexpr const wchar_t* kCs2InstallDir    = L"Counter-Strike Global Offensive";
inline constexpr const wchar_t* kLibraryFoldersVdf = L"libraryfolders.vdf";

// ---------------------------------------------------------------------------
// 文件名
// ---------------------------------------------------------------------------
inline constexpr const wchar_t* kCs2Exe         = L"cs2.exe";
inline constexpr const wchar_t* kQt5CoreDll     = L"Qt5Core.dll";
inline constexpr const wchar_t* kQt5WidgetsDll  = L"Qt5Widgets.dll";
inline constexpr const wchar_t* kQt5GuiDll      = L"Qt5Gui.dll";
inline constexpr const wchar_t* kInjectDll      = L"qtcore_qm.dll";
inline constexpr const wchar_t* kQtDictFile     = L"qt_translations.jsonc";
inline constexpr const wchar_t* kBackupManifest = L"backup_manifest.json";

// ---------------------------------------------------------------------------
// 路径组合
// ---------------------------------------------------------------------------

// <cs2Root>/game/bin/win64
inline fs::path Win64Bin(const fs::path& cs2Root) {
    return cs2Root / kGameDir / kBinDir / kWin64Dir;
}

// <cs2Root>/game/bin/win64/cs2.exe
inline fs::path Cs2Exe(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kCs2Exe;
}

// <cs2Root>/game/bin/win64/Qt5Core.dll
inline fs::path Qt5Core(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kQt5CoreDll;
}

// <cs2Root>/game/bin/win64/Qt5Widgets.dll
inline fs::path Qt5Widgets(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kQt5WidgetsDll;
}

// <cs2Root>/content/csgo_addons
inline fs::path Addons(const fs::path& cs2Root) {
    return cs2Root / kContentDir / kAddonsDir;
}

// <dir>/backup_manifest.json
inline fs::path Manifest(const fs::path& dir) {
    return dir / kBackupManifest;
}

}  // namespace paths
