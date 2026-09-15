#pragma once

// 汉化补丁的注入与还原（原 MainWindow::injectLocalizationCore / doRestore）。
//
// 设计要点：本模块**不依赖任何 Qt 控件**，可安全地在 worker 线程执行。
// 日志通过 LogSink 回调外抛，由调用方决定输出到 UI 还是别处。

#include <functional>
#include <string>

#include <QString>

// 还原前的备份版本校验结论
enum class RestoreCheck {
    Allowed,              // 备份与当前游戏版本一致，允许还原
    RejectedGameUpdated,  // 游戏已更新，还原旧备份会破坏新版，拒绝
    RejectedOther         // 无清单 / 清单读取失败等，无法安全还原
};

class LocalizationService {
public:
    // 日志回调：(消息, 颜色)。实现需保证线程安全——本服务会在 worker 线程中调用它。
    using LogSink = std::function<void(const QString& message, const QString& color)>;

    struct Context {
        std::wstring cs2Root;     // CS2 安装根目录
        std::wstring workingDir;  // 本工具工作目录（backup/ 与 translations/ 的父目录）
    };

    // 完整注入流程：版本校验与原版备份 → FGD 汉化 → Qt 补丁部署。
    // 任一步失败都会自动尝试还原，避免游戏停留在「半注入」状态。
    static bool Inject(const Context& ctx, bool useMachineTrans, const LogSink& log);

    // 还原原版 FGD 与 Qt5Core.dll。
    // showLog=false 用于注入失败后的自动回滚（静默），true 用于用户手动点击还原。
    static bool Restore(const Context& ctx, bool showLog, const LogSink& log);

    // 还原前的版本校验：确认 backup 与当前游戏版本一致。
    // reason 用于接收被拒绝的具体原因，便于上层提示用户。
    static RestoreCheck CheckRestore(const Context& ctx, QString* reason);

    // 是否存在「上次异常退出遗留的未还原补丁」，供启动时的自动恢复流程判断
    static bool HasPendingRecovery(const Context& ctx);

    // -----------------------------------------------------------------------
    // 只读状态查询
    //
    // 原先 MainWindow 直接调用 Cs2Detector / BackupManager 拼装这几个判断
    // （"当前是否已注入"、"能不能还原"、"CS2 是否在运行"），
    // 导致 UI 层既要懂业务流程又要懂底层文件布局。
    // 这里统一收口：UI 只问「现在是什么状态」，不再关心由谁判定、怎么判定。
    // -----------------------------------------------------------------------

    // CS2 / Hammer 进程是否正在运行（内部会遍历进程快照，属于较重操作）
    static bool IsCs2Running();

    // 工作目录下是否存在可用的原版备份
    static bool HasBackup(const Context& ctx);

    // 当前是否处于「已注入」状态（轻量判断，不涉及校验和）。
    // 判定规则：存在备份，且「会话标记已注入」或「CS2 目录中实际存在补丁文件」——
    // 异常关闭后 session_state 可能被破坏，此时只要补丁文件仍在就仍算已注入。
    static bool IsPatchDeployed(const Context& ctx);

    // 备份内容是否与当前游戏版本一致。
    // 内含多次全文件 SHA256，属于重 IO，应在后台线程调用。
    static bool IsBackupMatching(const Context& ctx);

    // 把工作目录与机翻开关写入游戏目录的 localizer_appdir.txt，
    // 供注入模块直读定位词典文件
    static void WriteAppDirPointer(const std::wstring& cs2Root,
                                   const std::wstring& workingDir,
                                   bool useMachineTrans);

    // 清除「已注入」会话标记（还原成功后调用）
    static bool ClearSessionState(const std::wstring& workingDir);

    // 若在源码/开发目录（工作目录的上一级 translations/）中编辑了词典，
    // 按修改时间同步覆盖到当前程序运行目录
    static void SyncDictionariesFromParent(const std::wstring& workingDir);
};
