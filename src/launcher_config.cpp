#include "launcher_config.h"

#include <filesystem>

#include <QSettings>

namespace fs = std::filesystem;

namespace {

// config.ini 键名的唯一定义处
constexpr const char* kKeySelectedAddon   = "Launcher/SelectedAddon";
constexpr const char* kKeyLaunchArgs      = "Launcher/LaunchArgs";
constexpr const char* kKeyUseMachineTrans = "Launcher/UseMachineTrans";

constexpr const wchar_t* kConfigFileName = L"config.ini";

QString ConfigFilePath(const std::wstring& workingDir) {
    return QString::fromStdWString((fs::path(workingDir) / kConfigFileName).wstring());
}

}  // namespace

LauncherSettings LauncherConfig::Load(const std::wstring& workingDir) {
    QSettings settings(ConfigFilePath(workingDir), QSettings::IniFormat);

    LauncherSettings s;
    s.selectedAddon   = settings.value(kKeySelectedAddon, QString()).toString().trimmed();
    s.launchArgs      = settings.value(kKeyLaunchArgs, QString()).toString();
    s.useMachineTrans = settings.value(kKeyUseMachineTrans, true).toBool();
    return s;
}

void LauncherConfig::Save(const std::wstring& workingDir, const LauncherSettings& settings) {
    QSettings qs(ConfigFilePath(workingDir), QSettings::IniFormat);

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
