#pragma once

#include <string>
#include <vector>

class BackupManager {
public:
    // 递归备份 CS2 目录下的所有 FGD 文件到 backupDir（按相对路径）
    static bool BackupFgdFiles(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError);

    // 备份 Qt5Core.dll 到 backupDir/game/bin/win64/Qt5Core.dll
    static bool BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);

    // 从 backupDir 完整恢复所有 FGD 和 Qt5Core.dll 到 CS2 目录，并清理临时注入文件
    static bool RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);
};

