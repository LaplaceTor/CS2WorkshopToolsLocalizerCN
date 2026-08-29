#pragma once

#include <string>
#include <vector>

class Cs2Detector {
public:
    // 检测 CS2 根目录路径（返回 true 表示检测成功）
    static bool DetectCs2(std::wstring& outCs2Root);

    // 验证给定路径是否是有效的 CS2 安装目录
    static bool IsValidCs2Root(const std::wstring& rootPath);

    // 获取 CS2 的 win64 二进制目录
    static std::wstring GetWin64BinDir(const std::wstring& cs2Root);

    // 获取 CS2 的 content/csgo_addons 目录
    static std::wstring GetAddonsDir(const std::wstring& cs2Root);

    // 扫描 content/csgo_addons 下的所有可用 Addon 列表
    static std::vector<std::wstring> GetAvailableAddons(const std::wstring& cs2Root);

    // 检测系统当前是否有 CS2 进程 (cs2.exe) 在运行
    static bool IsCs2ProcessRunning();

private:
    static bool CheckRegistryUninstall(std::wstring& outPath);
    static bool CheckRegistrySteam(std::wstring& outPath);
    static bool CheckCommonDrivePaths(std::wstring& outPath);
    static bool ParseSteamLibraryFolders(const std::wstring& vdfPath, std::vector<std::wstring>& outLibraries);
};

