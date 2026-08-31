#include "cs2_detector.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace fs = std::filesystem;

bool Cs2Detector::IsProcessRunning(unsigned long pid) {
    if (pid == 0) return false;
    HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (hProcess == NULL) {
        return false;
    }
    DWORD waitRes = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);
    return (waitRes == WAIT_TIMEOUT);
}

bool Cs2Detector::IsCs2ProcessRunning() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    bool isRunning = false;
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"cs2.exe") == 0) {
                isRunning = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return isRunning;
}

bool Cs2Detector::IsValidCs2Root(const std::wstring& rootPath) {
    if (rootPath.empty()) return false;
    QDir dir(QString::fromStdWString(rootPath));
    return dir.exists("game/bin/win64/cs2.exe") || dir.exists("game/bin/win64/Qt5Core.dll") || dir.exists("game/bin/win64");
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
    QString addonsPath = QString::fromStdWString(cs2Root) + "/content/csgo_addons";
    QDir dir(addonsPath);
    if (dir.exists()) {
        QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        addons.reserve(entries.size());
        for (const QString& entry : entries) {
            addons.push_back(entry.toStdWString());
        }
    }
    return addons;
}

bool Cs2Detector::CheckRegistryUninstall(std::wstring& outPath) {
    const QString subkeys[] = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730",
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730"
    };

    for (const QString& subkey : subkeys) {
        QSettings reg(subkey, QSettings::NativeFormat);
        QString loc = reg.value("InstallLocation").toString();
        if (!loc.isEmpty()) {
            std::wstring wloc = loc.toStdWString();
            if (IsValidCs2Root(wloc)) {
                outPath = wloc;
                return true;
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
    const QString subkeys[] = {
        "HKEY_CURRENT_USER\\Software\\Valve\\Steam",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"
    };

    QString steamPath;
    for (const QString& subkey : subkeys) {
        QSettings reg(subkey, QSettings::NativeFormat);
        QString p = reg.value("SteamPath").toString();
        if (p.isEmpty()) {
            p = reg.value("InstallPath").toString();
        }
        if (!p.isEmpty()) {
            steamPath = p;
            break;
        }
    }

    if (steamPath.isEmpty()) return false;

    std::wstring wSteamPath = steamPath.toStdWString();
    fs::path vdfPath = fs::path(wSteamPath) / L"steamapps" / L"libraryfolders.vdf";
    std::vector<std::wstring> libraries;
    libraries.push_back(wSteamPath);

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

