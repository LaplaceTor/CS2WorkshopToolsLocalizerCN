#include "core/cs2_detector.h"
#include "core/path_constants.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <algorithm>
#include <system_error>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFile>
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

unsigned long Cs2Detector::FindCs2ProcessId() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    unsigned long foundPid = 0;
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"cs2.exe") == 0) {
                foundPid = pe32.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return foundPid;
}

bool Cs2Detector::IsCs2ProcessRunning() {
    return FindCs2ProcessId() != 0;
}

bool Cs2Detector::IsValidCs2Root(const std::wstring& rootPath) {
    if (rootPath.empty()) return false;
    std::error_code ec;
    const fs::path bin = paths::Win64Bin(rootPath);
    return fs::is_regular_file(bin / paths::kCs2Exe, ec)
        && fs::is_regular_file(bin / paths::kQt5CoreDll, ec);
}

std::wstring Cs2Detector::NormalizeCs2RootCandidate(const std::wstring& selectedPath) {
    if (selectedPath.empty()) return L"";

    std::error_code ec;
    fs::path p = fs::path(selectedPath).lexically_normal();
    while (p.has_filename() && p.filename().empty() && p.has_parent_path()) {
        p = p.parent_path();
    }

    fs::path cur = p;
    for (int up = 0; up < 4; ++up) {
        if (IsValidCs2Root(cur.wstring())) {
            return cur.wstring();
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return selectedPath;
}

bool Cs2Detector::CheckRunningProcess(std::wstring& outPath) {
    const unsigned long pid = FindCs2ProcessId();
    if (pid == 0) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (hProcess == NULL) {
        return false;
    }

    wchar_t exePath[MAX_PATH * 4] = {0};
    DWORD size = static_cast<DWORD>(sizeof(exePath) / sizeof(exePath[0]));
    const BOOL ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size);
    CloseHandle(hProcess);
    if (!ok || size == 0) {
        return false;
    }

    fs::path root = fs::path(exePath).parent_path().parent_path().parent_path().parent_path();
    if (IsValidCs2Root(root.wstring())) {
        outPath = root.wstring();
        return true;
    }
    return false;
}

std::wstring Cs2Detector::GetWin64BinDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return paths::Win64Bin(p).wstring();
}

std::wstring Cs2Detector::GetAddonsDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return paths::Addons(p).wstring();
}

bool Cs2Detector::IsExcludedAddon(const std::wstring& addonName) {
    for (const wchar_t* excluded : paths::kExcludedAddons) {
        if (_wcsicmp(addonName.c_str(), excluded) == 0) {
            return true;
        }
    }
    return false;
}

bool Cs2Detector::IsExcludedAddon(const QString& addonName) {
    for (const wchar_t* excluded : paths::kExcludedAddons) {
        if (addonName.compare(QString::fromWCharArray(excluded), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> Cs2Detector::GetAvailableAddons(const std::wstring& cs2Root) {
    std::vector<std::wstring> addons;
    QString addonsPath = QString::fromStdWString(paths::Addons(cs2Root).wstring());
    QDir dir(addonsPath);
    if (dir.exists()) {
        QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        addons.reserve(entries.size());
        for (const QString& entry : entries) {
            if (!IsExcludedAddon(entry)) {
                addons.push_back(entry.toStdWString());
            }
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

bool Cs2Detector::ParseSteamLibraryFolders(const std::wstring& vdfPath, std::vector<SteamLibraryFolder>& outLibraries) {
    QFile file(QString::fromStdWString(vdfPath));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    int depth = 0;
    bool inLibBlock = false;
    bool inAppsBlock = false;
    SteamLibraryFolder currentLib;
    QString pendingKey;
    QString lastAtom;

    const int n = content.size();
    int i = 0;
    while (i < n) {
        const QChar ch = content[i];
        if (ch.isSpace()) {
            ++i;
            continue;
        }
        if (ch == '/' && i + 1 < n && content[i + 1] == '/') {
            while (i < n && content[i] != '\n') ++i;
            continue;
        }
        if (ch == '{') {
            if (depth == 1) {
                inLibBlock = true;
                currentLib = SteamLibraryFolder{};
            } else if (depth == 2 && inLibBlock && lastAtom.compare(QStringLiteral("apps"), Qt::CaseInsensitive) == 0) {
                inAppsBlock = true;
            }
            pendingKey.clear();
            lastAtom.clear();
            ++depth;
            ++i;
            continue;
        }
        if (ch == '}') {
            if (depth == 3 && inAppsBlock) {
                inAppsBlock = false;
            } else if (depth == 2 && inLibBlock) {
                if (!currentLib.path.empty()) {
                    outLibraries.push_back(currentLib);
                }
                inLibBlock = false;
            }
            pendingKey.clear();
            lastAtom.clear();
            if (depth > 0) --depth;
            ++i;
            continue;
        }
        if (ch == '"') {
            ++i;
            QString token;
            while (i < n) {
                const QChar c = content[i];
                if (c == '\\' && i + 1 < n) {
                    token.append(content[i + 1]);
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    ++i;
                    break;
                }
                token.append(c);
                ++i;
            }

            if (pendingKey.isEmpty()) {
                pendingKey = token;
                lastAtom = token;
            } else {
                if (depth == 2 && inLibBlock
                    && pendingKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) {
                    currentLib.path = token.trimmed().toStdWString();
                } else if (depth == 3 && inAppsBlock && pendingKey == QStringLiteral("730")) {
                    currentLib.hasCs2App = true;
                }
                pendingKey.clear();
                lastAtom.clear();
            }
            continue;
        }
        ++i;
    }

    return !outLibraries.empty();
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

    const std::wstring wSteamPath = steamPath.toStdWString();
    const fs::path vdfPath = fs::path(wSteamPath) / paths::kSteamAppsDir / paths::kLibraryFoldersVdf;
    std::vector<SteamLibraryFolder> libraries;

    if (fs::exists(vdfPath)) {
        ParseSteamLibraryFolders(vdfPath.wstring(), libraries);
    }

    auto tryLibrary = [&outPath](const std::wstring& libPath) -> bool {
        const fs::path cand = fs::path(libPath) / paths::kSteamAppsDir / paths::kCommonDir / paths::kCs2InstallDir;
        if (IsValidCs2Root(cand.wstring())) {
            outPath = cand.wstring();
            return true;
        }
        return false;
    };

    for (const auto& lib : libraries) {
        if (lib.hasCs2App && tryLibrary(lib.path)) {
            return true;
        }
    }

    for (const auto& lib : libraries) {
        if (!lib.hasCs2App && tryLibrary(lib.path)) {
            return true;
        }
    }
    if (tryLibrary(wSteamPath)) {
        return true;
    }

    return false;
}

bool Cs2Detector::CheckCommonDrivePaths(std::wstring& outPath) {
    const wchar_t* steamRoots[] = {
        L"SteamLibrary",
        L"Program Files (x86)\\Steam",
        L"Program Files\\Steam",
        L"Steam",
        L"Games\\SteamLibrary"
    };

    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo& drive : drives) {
        QString drivePath = drive.absoluteFilePath();
        if (!drivePath.endsWith('/')) {
            drivePath += '/';
        }
        for (const wchar_t* root : steamRoots) {
            const fs::path cand = fs::path(drivePath.toStdWString()) / root
                                / paths::kSteamAppsDir / paths::kCommonDir / paths::kCs2InstallDir;
            if (IsValidCs2Root(cand.wstring())) {
                outPath = cand.wstring();
                return true;
            }
        }
    }
    return false;
}

bool Cs2Detector::DetectCs2(std::wstring& outCs2Root) {
    if (CheckRunningProcess(outCs2Root)) return true;
    if (CheckRegistryUninstall(outCs2Root)) return true;
    if (CheckRegistrySteam(outCs2Root)) return true;
    if (CheckCommonDrivePaths(outCs2Root)) return true;
    return false;
}
