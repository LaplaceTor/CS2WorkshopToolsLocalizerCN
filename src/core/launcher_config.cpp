#include "core/launcher_config.h"

#include <filesystem>

#include <QSettings>

namespace fs = std::filesystem;

namespace {

constexpr const char* kKeyCs2Root         = "Launcher/Cs2Root";
constexpr const char* kKeySelectedAddon   = "Launcher/SelectedAddon";
constexpr const char* kKeyLaunchArgs      = "Launcher/LaunchArgs";
constexpr const char* kKeyUseMachineTrans = "Launcher/UseMachineTrans";

constexpr const wchar_t* kConfigFileName = L"config.ini";

QString ConfigFilePath(const std::wstring& workingDir) {
    return QString::fromStdWString((fs::path(workingDir) / kConfigFileName).wstring());
}

}

LauncherSettings LauncherConfig::Load(const std::wstring& workingDir) {
    QSettings settings(ConfigFilePath(workingDir), QSettings::IniFormat);

    LauncherSettings s;
    s.cs2Root         = settings.value(kKeyCs2Root, QString()).toString().trimmed();
    s.selectedAddon   = settings.value(kKeySelectedAddon, QString()).toString().trimmed();
    QString args      = settings.value(kKeyLaunchArgs, "-gpuraytracing").toString().trimmed();
    if (args.isEmpty()) {
        args = "-gpuraytracing";
    }
    s.launchArgs      = args;
    s.useMachineTrans = settings.value(kKeyUseMachineTrans, true).toBool();
    return s;
}

void LauncherConfig::Save(const std::wstring& workingDir, const LauncherSettings& settings) {
    QSettings qs(ConfigFilePath(workingDir), QSettings::IniFormat);

    qs.setValue(kKeyCs2Root, settings.cs2Root);
    qs.setValue(kKeySelectedAddon, settings.selectedAddon);
    qs.setValue(kKeyLaunchArgs, settings.launchArgs);
    qs.setValue(kKeyUseMachineTrans, settings.useMachineTrans);
    qs.sync();
}

QString LauncherConfig::NormalizeAddonName(const QString& text) {
    const QString trimmed = text.trimmed();
    const int spacePos = trimmed.indexOf(' ');
    return (spacePos < 0) ? trimmed : trimmed.left(spacePos);
}
