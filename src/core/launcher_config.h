#pragma once

#include <string>
#include <QString>

struct LauncherSettings {
    QString cs2Root;
    QString selectedAddon;
    QString launchArgs = "-gpuraytracing";
    bool    useMachineTrans = true;
};

class LauncherConfig {
public:
    static LauncherSettings Load(const std::wstring& workingDir);

    static void Save(const std::wstring& workingDir, const LauncherSettings& settings);

    static QString NormalizeAddonName(const QString& text);
};
