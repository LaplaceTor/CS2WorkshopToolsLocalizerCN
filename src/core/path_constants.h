#pragma once

#include <filesystem>
#include <string>

namespace paths {

namespace fs = std::filesystem;

inline constexpr const wchar_t* kGameDir    = L"game";
inline constexpr const wchar_t* kBinDir     = L"bin";
inline constexpr const wchar_t* kWin64Dir   = L"win64";
inline constexpr const wchar_t* kContentDir = L"content";
inline constexpr const wchar_t* kAddonsDir  = L"csgo_addons";

inline constexpr const wchar_t* kTranslationsDir = L"translations";
inline constexpr const wchar_t* kBackupDir       = L"backup";

inline constexpr const wchar_t* kSteamAppsDir     = L"steamapps";
inline constexpr const wchar_t* kCommonDir        = L"common";
inline constexpr const wchar_t* kCs2InstallDir    = L"Counter-Strike Global Offensive";
inline constexpr const wchar_t* kLibraryFoldersVdf = L"libraryfolders.vdf";

inline constexpr const wchar_t* kCs2Exe         = L"cs2.exe";
inline constexpr const wchar_t* kQt5CoreDll     = L"Qt5Core.dll";
inline constexpr const wchar_t* kQt5WidgetsDll  = L"Qt5Widgets.dll";
inline constexpr const wchar_t* kQt5GuiDll      = L"Qt5Gui.dll";
inline constexpr const wchar_t* kInjectDll      = L"qtcore_qm.dll";
inline constexpr const wchar_t* kResourceCopyExe = L"resourcecopy.exe";
inline constexpr const wchar_t* kQtDictFile     = L"qt_translations.jsonc";
inline constexpr const wchar_t* kBackupManifest = L"backup_manifest.json";

inline constexpr const wchar_t* const kExcludedAddons[] = {
    L"addon_template",
    L"cs_script_demo",
    L"workshop_items"
};

inline constexpr const wchar_t* const kAllDictFiles[] = {
    L"qt_translations.jsonc",
    L"qt_fallback.jsonc",
    L"fgd_translations.jsonc",
    L"fgd_override.jsonc",
    L"fgd_fallback.jsonc"
};

inline fs::path Win64Bin(const fs::path& cs2Root) {
    return cs2Root / kGameDir / kBinDir / kWin64Dir;
}

inline fs::path Cs2Exe(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kCs2Exe;
}

inline fs::path Qt5Core(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kQt5CoreDll;
}

inline fs::path Qt5Widgets(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kQt5WidgetsDll;
}

inline fs::path ResourceCopyExe(const fs::path& cs2Root) {
    return Win64Bin(cs2Root) / kResourceCopyExe;
}

inline fs::path Addons(const fs::path& cs2Root) {
    return cs2Root / kContentDir / kAddonsDir;
}

inline fs::path GameAddons(const fs::path& cs2Root) {
    return cs2Root / kGameDir / kAddonsDir;
}

inline fs::path Manifest(const fs::path& dir) {
    return dir / kBackupManifest;
}

}
