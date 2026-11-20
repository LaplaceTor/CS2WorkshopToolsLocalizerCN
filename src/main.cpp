#include <QApplication>
#include <QFileDialog>
#include <QFont>
#include <QMessageBox>
#include <QPushButton>
#include <QStyleFactory>
#include <filesystem>
#include "ui/mainwindow.h"
#include "core/cs2_detector.h"
#include "core/launcher_config.h"

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CS2WorkshopToolsLocalizerCN");
    app.setOrganizationName("CS2TranslationTools");

    QFont globalFont;
    globalFont.setFamilies({
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "Segoe UI",
        "Segoe UI Emoji",
        "Noto Color Emoji"
    });
    globalFont.setPointSize(9);
    app.setFont(globalFont);

    app.setStyle(QStyleFactory::create("Fusion"));

    std::wstring workingDir;
    const QString appDir = QApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        workingDir = appDir.toStdWString();
    } else {
        try {
            workingDir = fs::current_path().wstring();
        } catch (...) {
            workingDir.clear();
        }
    }

    LauncherSettings savedSettings = LauncherConfig::Load(workingDir);
    std::wstring cs2Root;
    bool detected = false;

    if (!savedSettings.cs2Root.isEmpty()) {
        const std::wstring candidate = Cs2Detector::NormalizeCs2RootCandidate(savedSettings.cs2Root.toStdWString());
        if (Cs2Detector::IsValidCs2Root(candidate)) {
            cs2Root = candidate;
            detected = true;
        }
    }

    if (!detected) {
        detected = Cs2Detector::DetectCs2(cs2Root);
    }

    while (!detected || cs2Root.empty()) {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setWindowTitle("CS2 Workshop Tools Localizer CN");
        msgBox.setText(
            "未检测到 Counter-Strike 2 安装路径！\n\n"
            "请确认 CS2 已通过 Steam 正确安装，或点击「手动选择目录」指定 CS2 根目录。"
        );
        QPushButton* pickBtn = msgBox.addButton("手动选择目录", QMessageBox::AcceptRole);
        QPushButton* exitBtn = msgBox.addButton("退出", QMessageBox::RejectRole);
        msgBox.setDefaultButton(pickBtn);
        msgBox.exec();

        if (msgBox.clickedButton() != pickBtn || msgBox.clickedButton() == exitBtn) {
            return 1;
        }

        const QString picked = QFileDialog::getExistingDirectory(
            nullptr,
            "选择 Counter-Strike Global Offensive 安装目录"
        );
        if (picked.isEmpty()) {
            continue;
        }
        const std::wstring normalized = Cs2Detector::NormalizeCs2RootCandidate(picked.toStdWString());
        if (Cs2Detector::IsValidCs2Root(normalized)) {
            cs2Root = normalized;
            detected = true;
        } else {
            QMessageBox::warning(
                nullptr,
                "目录无效",
                "所选目录中未找到有效的 game/bin/win64/cs2.exe 与 Qt5Core.dll，请重新选择。"
            );
        }
    }

    const QString resolvedCs2Root = QString::fromStdWString(cs2Root);
    if (!workingDir.empty() && savedSettings.cs2Root != resolvedCs2Root) {
        savedSettings.cs2Root = resolvedCs2Root;
        LauncherConfig::Save(workingDir, savedSettings);
    }

    MainWindow mainWindow(cs2Root);
    mainWindow.show();

    return app.exec();
}

