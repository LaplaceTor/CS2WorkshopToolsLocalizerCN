#include <QApplication>
#include <QMessageBox>
#include <QStyleFactory>
#include "mainwindow.h"
#include "cs2_detector.h"

int main(int argc, char *argv[]) {
    // 启用高 DPI 缩放支持
    QApplication app(argc, argv);
    app.setApplicationName("CS2WorkshopToolsLocalizerCN");
    app.setOrganizationName("CS2TranslationTools");

    // 设置全局现代暗色调样式
    app.setStyle(QStyleFactory::create("Fusion"));

    // 1. 启动时检测 CS2
    std::wstring cs2Root;
    bool detected = Cs2Detector::DetectCs2(cs2Root);

    if (!detected || cs2Root.empty() || !Cs2Detector::IsValidCs2Root(cs2Root)) {
        // 没检测到就终止程序并弹窗提示用户
        QMessageBox::critical(
            nullptr,
            "CS2 Workshop Tools Localizer CN",
            "未检测到 Counter-Strike 2 安装路径！\n\n"
            "请确认 CS2 已通过 Steam 正确安装，或 Steam 注册表处于正常状态后重试。"
        );
        return 0; // 终止程序
    }

    // 2. 检测到 CS2，加载主窗口
    MainWindow mainWindow(cs2Root);
    mainWindow.show();

    return app.exec();
}

