#pragma once

#include <string>
#include <vector>

class BackupManager {
public:
    // 检查 backupDir 目录是否存在且包含有效的备份文件
    static bool HasBackup(const std::wstring& backupDir);

    // 递归备份 CS2 目录下的所有 FGD 文件到 backupDir（按相对路径）
    static bool BackupFgdFiles(const std::wstring& cs2Root, const std::wstring& backupDir, std::vector<std::wstring>& outBackedUpFiles, std::wstring& outError);

    // 备份 Qt5Core.dll 到 backupDir/game/bin/win64/Qt5Core.dll
    static bool BackupQtCore(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);

    // 从 backupDir 完整恢复所有 FGD 和 Qt5Core.dll 到 CS2 目录，并清理临时注入文件
    static bool RestoreAll(const std::wstring& cs2Root, const std::wstring& backupDir, std::wstring& outError);

    // 检查 CS2 目录中是否存在未还原的补丁文件（如 qtcore_qm.dll / qt_translations.json）
    static bool IsPatchDeployed(const std::wstring& cs2Root);

    // 检查工作目录下是否存在未正常结束的会话状态记录
    static bool HasUnrestoredSession(const std::wstring& workingDir);

    // 保存运行会话状态 (isPatched = true 表示补丁已部署且未还原)
    static void SaveSessionState(const std::wstring& workingDir, bool isPatched);

    // 清除运行会话状态记录
    static void ClearSessionState(const std::wstring& workingDir);
};

