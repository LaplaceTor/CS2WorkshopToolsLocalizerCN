#include <QApplication>
#include <QMessageBox>
#include <QStyleFactory>
#include "ui/mainwindow.h"
#include "core/cs2_detector.h"

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

    // DetectCs2 内部已对每个候选路径做过 IsValidCs2Root 校验，这里不再重复判断
    if (!detected || cs2Root.empty()) {
        // 没检测到就终止程序并弹窗提示用户
        QMessageBox::critical(
            nullptr,
            "CS2 Workshop Tools Localizer CN",
            "未检测到 Counter-Strike 2 安装路径！\n\n"
            "请确认 CS2 已通过 Steam 正确安装，或 Steam 注册表处于正常状态后重试。"
        );
        return 1; // 终止程序；失败路径必须返回非零，否则脚本/CI 会误判为成功
    }

    // 2. 检测到 CS2，加载主窗口
    MainWindow mainWindow(cs2Root);
    mainWindow.show();

    // 检查是否带有 -debug 或 --debug 启动参数
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]).toLower();
        if (arg == "-debug" || arg == "--debug") {
            mainWindow.openDebugWindow();
            break;
        }
    }

    return app.exec();
}

