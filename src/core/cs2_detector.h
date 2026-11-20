#pragma once

#include <string>
#include <vector>

class QString;

class Cs2Detector {
public:
    static bool DetectCs2(std::wstring& outCs2Root);

    static bool IsValidCs2Root(const std::wstring& rootPath);

    static std::wstring NormalizeCs2RootCandidate(const std::wstring& selectedPath);

    static std::wstring GetWin64BinDir(const std::wstring& cs2Root);

    static std::wstring GetAddonsDir(const std::wstring& cs2Root);

    static bool IsExcludedAddon(const std::wstring& addonName);
    static bool IsExcludedAddon(const QString& addonName);

    static std::vector<std::wstring> GetAvailableAddons(const std::wstring& cs2Root);

    static bool IsProcessRunning(unsigned long pid);

    static unsigned long FindCs2ProcessId();

    static bool IsCs2ProcessRunning();

private:
    struct SteamLibraryFolder {
        std::wstring path;
        bool hasCs2App = false;
    };

    static bool CheckRunningProcess(std::wstring& outPath);
    static bool CheckRegistryUninstall(std::wstring& outPath);
    static bool CheckRegistrySteam(std::wstring& outPath);
    static bool CheckCommonDrivePaths(std::wstring& outPath);
    static bool ParseSteamLibraryFolders(const std::wstring& vdfPath, std::vector<SteamLibraryFolder>& outLibraries);
};
