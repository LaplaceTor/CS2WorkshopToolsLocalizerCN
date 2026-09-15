#pragma once

// 启动器配置的持久化层。
//
// 职责：<工作目录>/config.ini 的读写，以及 config.ini 键名的唯一定义处。
// 不依赖任何 Qt 控件，便于 mainwindow 只做「取值 → 填控件」的编排。

#include <string>
#include <QString>

struct LauncherSettings {
    QString selectedAddon;          // 上次选择的 Addon 名
    QString launchArgs;             // 附加启动参数
    bool    useMachineTrans = true; // 是否启用机翻
};

class LauncherConfig {
public:
    // 读取 <workingDir>/config.ini；文件缺失或键缺失时使用默认值
    static LauncherSettings Load(const std::wstring& workingDir);

    // 写入 <workingDir>/config.ini 并立即 sync
    static void Save(const std::wstring& workingDir, const LauncherSettings& settings);

    // 下拉项常带附加说明（如 "my_addon (已汉化)"），仅持久化纯 Addon 名。
    // 规则：取首个空格之前的部分。
    static QString NormalizeAddonName(const QString& text);
};
