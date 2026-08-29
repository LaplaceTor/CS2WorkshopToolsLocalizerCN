#include "cs2_detector.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>

namespace fs = std::filesystem;

bool Cs2Detector::IsValidCs2Root(const std::wstring& rootPath) {
    if (rootPath.empty()) return false;
    try {
        fs::path p(rootPath);
        fs::path bin = p / L"game" / L"bin" / L"win64";
        fs::path cs2Exe = bin / L"cs2.exe";
        fs::path qt5Core = bin / L"Qt5Core.dll";
        return fs::exists(cs2Exe) || fs::exists(qt5Core) || fs::exists(bin);
    } catch (...) {
        return false;
    }
}

std::wstring Cs2Detector::GetWin64BinDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return (p / L"game" / L"bin" / L"win64").wstring();
}

std::wstring Cs2Detector::GetAddonsDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return (p / L"content" / L"csgo_addons").wstring();
}

std::vector<std::wstring> Cs2Detector::GetAvailableAddons(const std::wstring& cs2Root) {
    std::vector<std::wstring> addons;
    try {
        fs::path addonsPath = fs::path(cs2Root) / L"content" / L"csgo_addons";
        if (fs::exists(addonsPath) && fs::is_directory(addonsPath)) {
            for (const auto& entry : fs::directory_iterator(addonsPath)) {
                if (entry.is_directory()) {
                    std::wstring name = entry.path().filename().wstring();
                    addons.push_back(name);
                }
            }
        }
    } catch (...) {}
    std::sort(addons.begin(), addons.end());
    return addons;
}

bool Cs2Detector::CheckRegistryUninstall(std::wstring& outPath) {
    const wchar_t* subkeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730"
    };

    HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    for (HKEY root : roots) {
        for (const wchar_t* subkey : subkeys) {
            HKEY hKey = NULL;
            if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                wchar_t buffer[MAX_PATH] = {0};
                DWORD bufSize = sizeof(buffer);
                DWORD type = 0;
                if (RegQueryValueExW(hKey, L"InstallLocation", NULL, &type, (LPBYTE)buffer, &bufSize) == ERROR_SUCCESS) {
                    RegCloseKey(hKey);
                    if (buffer[0] != L'\0' && IsValidCs2Root(buffer)) {
                        outPath = buffer;
                        return true;
                    }
                }
                RegCloseKey(hKey);
            }
        }
    }
    return false;
}

bool Cs2Detector::ParseSteamLibraryFolders(const std::wstring& vdfPath, std::vector<std::wstring>& outLibraries) {
    try {
        std::ifstream file(fs::path(vdfPath), std::ios::binary);
        if (!file.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // 匹配 "path" "..."
        std::regex pathRegex(R"re("path"\s+"([^"]+)")re");
        auto words_begin = std::sregex_iterator(content.begin(), content.end(), pathRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            std::string pStr = match[1].str();
            // 替换双反斜杠 \\ 为 单反斜杠
            std::string cleanPath;
            for (size_t k = 0; k < pStr.length(); ++k) {
                if (pStr[k] == '\\' && k + 1 < pStr.length() && pStr[k+1] == '\\') {
                    cleanPath.push_back('\\');
                    k++;
                } else {
                    cleanPath.push_back(pStr[k]);
                }
            }
            // 转换为 wstring
            int wlen = MultiByteToWideChar(CP_UTF8, 0, cleanPath.c_str(), -1, NULL, 0);
            if (wlen > 0) {
                std::wstring wpath(wlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, cleanPath.c_str(), -1, &wpath[0], wlen);
                while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
                outLibraries.push_back(wpath);
            }
        }
        return !outLibraries.empty();
    } catch (...) {
        return false;
    }
}

bool Cs2Detector::CheckRegistrySteam(std::wstring& outPath) {
    struct SteamRegQuery {
        HKEY root;
        const wchar_t* subkey;
        const wchar_t* valName;
    };

    SteamRegQuery queries[] = {
        { HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath" }
    };

    std::wstring steamPath;
    for (const auto& q : queries) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(q.root, q.subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buffer[MAX_PATH] = {0};
            DWORD bufSize = sizeof(buffer);
            DWORD type = 0;
            if (RegQueryValueExW(hKey, q.valName, NULL, &type, (LPBYTE)buffer, &bufSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                if (buffer[0] != L'\0') {
                    steamPath = buffer;
                    break;
                }
            }
            RegCloseKey(hKey);
        }
    }

    if (steamPath.empty()) return false;

    // 检查 steamapps/libraryfolders.vdf
    fs::path vdfPath = fs::path(steamPath) / L"steamapps" / L"libraryfolders.vdf";
    std::vector<std::wstring> libraries;
    libraries.push_back(steamPath);

    if (fs::exists(vdfPath)) {
        ParseSteamLibraryFolders(vdfPath.wstring(), libraries);
    }

    for (const auto& lib : libraries) {
        fs::path cand = fs::path(lib) / L"steamapps" / L"common" / L"Counter-Strike Global Offensive";
        if (IsValidCs2Root(cand.wstring())) {
            outPath = cand.wstring();
            return true;
        }
    }

    return false;
}

bool Cs2Detector::CheckCommonDrivePaths(std::wstring& outPath) {
    const wchar_t* drives[] = { L"C:", L"D:", L"E:", L"F:", L"G:", L"H:" };
    const wchar_t* prefixes[] = {
        L"SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive",
        L"Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive",
        L"Program Files\\Steam\\steamapps\\common\\Counter-Strike Global Offensive",
        L"Steam\\steamapps\\common\\Counter-Strike Global Offensive",
        L"Games\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive"
    };

    for (const wchar_t* drive : drives) {
        for (const wchar_t* prefix : prefixes) {
            fs::path cand = fs::path(drive) / L"\\" / prefix;
            if (IsValidCs2Root(cand.wstring())) {
                outPath = cand.wstring();
                return true;
            }
        }
    }
    return false;
}

bool Cs2Detector::DetectCs2(std::wstring& outCs2Root) {
    if (CheckRegistryUninstall(outCs2Root)) return true;
    if (CheckRegistrySteam(outCs2Root)) return true;
    if (CheckCommonDrivePaths(outCs2Root)) return true;
    return false;
}

