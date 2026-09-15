#include "core/cs2_detector.h"
#include "core/path_constants.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <algorithm>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFile>
#include <QRegularExpression>
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
    const fs::path bin = paths::Win64Bin(rootPath);
    return fs::exists(bin / paths::kCs2Exe)
        || fs::exists(bin / paths::kQt5CoreDll)
        || fs::exists(bin);
}

std::wstring Cs2Detector::GetWin64BinDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return paths::Win64Bin(p).wstring();
}

std::wstring Cs2Detector::GetAddonsDir(const std::wstring& cs2Root) {
    fs::path p(cs2Root);
    return paths::Addons(p).wstring();
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
    // Steam VDF 为 UTF-8 文本
    QFile file(QString::fromStdWString(vdfPath));
    if (!file.open(QIODevice::ReadOnly)) return false;
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    // 匹配 "path" "..."
    static const QRegularExpression pathRegex(
        QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
    QRegularExpressionMatchIterator it = pathRegex.globalMatch(content);
    while (it.hasNext()) {
        // VDF 中路径分隔符写作转义的双反斜杠
        QString p = it.next().captured(1).replace(
            QStringLiteral("\\\\"),
            QStringLiteral("\\"));
        if (!p.isEmpty()) {
            outLibraries.push_back(p.toStdWString());
        }
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

    std::wstring wSteamPath = steamPath.toStdWString();
    fs::path vdfPath = fs::path(wSteamPath) / paths::kSteamAppsDir / paths::kLibraryFoldersVdf;
    std::vector<std::wstring> libraries;
    libraries.push_back(wSteamPath);

    if (fs::exists(vdfPath)) {
        ParseSteamLibraryFolders(vdfPath.wstring(), libraries);
    }

    for (const auto& lib : libraries) {
        fs::path cand = fs::path(lib) / paths::kSteamAppsDir / paths::kCommonDir / paths::kCs2InstallDir;
        if (IsValidCs2Root(cand.wstring())) {
            outPath = cand.wstring();
            return true;
        }
    }

    return false;
}

bool Cs2Detector::CheckCommonDrivePaths(std::wstring& outPath) {
    // 盘符根目录下的 Steam 常见安装位置，统一拼上 steamapps/common/<CS2 安装目录名>
    const wchar_t* steamRoots[] = {
        L"SteamLibrary",
        L"Program Files (x86)\\Steam",
        L"Program Files\\Steam",
        L"Steam",
        L"Games\\SteamLibrary"
    };

    // 枚举系统全部盘符，替代硬编码的 C:~H:
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
    if (CheckRegistryUninstall(outCs2Root)) return true;
    if (CheckRegistrySteam(outCs2Root)) return true;
    if (CheckCommonDrivePaths(outCs2Root)) return true;
    return false;
}

