#include "service/localization_service.h"

#include <filesystem>
#include <fstream>

#include <QThread>
#include <QString>

#include "core/backup_manager.h"
#include "core/cs2_detector.h"
#include "core/dictionary_compiler.h"
#include "core/dictionary_paths.h"
#include "core/fgd_translator.h"
#include "core/path_constants.h"
#include "core/pe_patcher.h"

namespace fs = std::filesystem;
bool LocalizationService::Inject(const Context& ctx, bool useMachineTrans, const LogSink& log) {
    fs::path workPath(ctx.workingDir);

    fs::path backupDir =
        workPath / paths::kBackupDir;

    fs::path transDir =
        workPath / paths::kTranslationsDir;

    fs::path cs2Bin = paths::Win64Bin(ctx.cs2Root);

    fs::path fgdDictPath = ResolveDictionaryPath(ctx.workingDir, L"fgd_translations.jsonc");
    fs::path fgdFallbackPath = ResolveDictionaryPath(ctx.workingDir, L"fgd_fallback.jsonc");
    fs::path fgdOverridePath = ResolveDictionaryPath(ctx.workingDir, L"fgd_override.jsonc");
    fs::path qtDictPath = ResolveDictionaryPath(ctx.workingDir, L"qt_translations.jsonc");
    fs::path qtFallbackPath = ResolveDictionaryPath(ctx.workingDir, L"qt_fallback.jsonc");

    fs::path qmDllSrc =
        workPath / paths::kInjectDll;
    if (!fs::exists(qmDllSrc)) {
        if (fs::exists(fs::current_path() / paths::kInjectDll)) {
            qmDllSrc = fs::current_path() / paths::kInjectDll;
        }
    }

    std::wstring notice;

    if (FgdTranslator::EnsureFgdDictionaryExists(
            fgdDictPath.wstring(),
            L"",
            notice)) {

        log(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    if (FgdTranslator::EnsureFgdOverrideDictionaryExists(
            fgdOverridePath.wstring(),
            L"",
            notice)) {

        log(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    if (FgdTranslator::EnsureQtDictionaryExists(
            qtDictPath.wstring(),
            L"",
            notice)) {

        log(
            "[i] " + QString::fromStdWString(notice),
            "#66d9ef"
        );
    }

    // ==========================================
    // STEP 1: 原版备份
    // ==========================================
    log(
        "[1/3] 正在校验游戏版本并准备原版备份...",
        "#e6db74"
    );

    auto matchResult =
        BackupManager::BackupMatchesCurrentGame(
            ctx.cs2Root,
            backupDir.wstring()
        );

    bool forceRecreate = false;

    if (matchResult.status ==
        BackupMatchStatus::GameUpdated) {

        log(
            QString(
                "[!] 检测到 CS2 游戏版本发生变化: %1"
            )
                .arg(
                    QString::fromStdWString(
                        matchResult.reason
                    )
                ),
            "#fd971f"
        );

        // 二次哈希确认：Steam 更新进行中时文件哈希会不稳定，
        // 避免把半更新状态的游戏文件定格为"纯净原版备份"（此时运行于 worker 线程，可安全等待）
        log(
            "[*] 疑似游戏更新，2 秒后进行二次哈希确认...",
            "#66d9ef"
        );

        QThread::msleep(2000);

        auto reconfirm =
            BackupManager::BackupMatchesCurrentGame(
                ctx.cs2Root,
                backupDir.wstring()
            );

        if (reconfirm.status == BackupMatchStatus::Matches) {
            log(
                "[+] 二次校验显示备份与当前版本一致（此前可能正处于 Steam 更新过程中），继续使用现有备份。",
                "#a6e22e"
            );
        } else if (reconfirm.status == BackupMatchStatus::GameUpdated) {
            log(
                "[*] 二次校验仍检测到版本变化，确认游戏已更新，旧备份已失效，将重新建立当前版本原版备份...",
                "#66d9ef"
            );

            forceRecreate = true;
        } else {
            log(
                QString(
                    "[-] 二次版本校验失败，已中止注入: %1"
                )
                    .arg(
                        QString::fromStdWString(
                            reconfirm.reason
                        )
                    ),
                "#f92672"
            );

            return false;
        }
    }

    std::vector<std::wstring> backedFgd;
    std::wstring err;

    if (!BackupManager::CreateOrUpdateBackup(
            ctx.cs2Root,
            backupDir.wstring(),
            backedFgd,
            err,
            forceRecreate)) {

        log(
            QString(
                "[-] 备份原版文件失败: %1"
            )
                .arg(
                    QString::fromStdWString(err)
                ),
            "#f92672"
        );

        return false;
    }

    log(
        "[+] 成功捕获并绑定官方原版 Qt5Core.dll（FGD 采用纯内存引擎，免磁盘备份）",
        "#a6e22e"
    );

    // ==========================================
    // STEP 2: FGD 汉化引擎准备 (纯内存模式)
    // ==========================================
    log(
        "[2/3] 正在准备 FGD 纯内存汉化引擎...",
        "#e6db74"
    );

    if (useMachineTrans) {
        log(
            "[*] 已启用机翻模式：自动加载 fgd_fallback.jsonc 与 qt_fallback.jsonc 作为兜底词典",
            "#66d9ef"
        );
    }

    // 写入应用程序目录与机翻状态指针文件，供注入模块读取词典
    WriteAppDirPointer(ctx.cs2Root, ctx.workingDir, useMachineTrans);

    // 检查并恢复可能存在的旧版本残留磁盘翻译 FGD 文件，确保 CS2 磁盘目录 100% 保持官方原版纯净
    size_t restoredOldFgd = 0;
    try {
        if (fs::exists(backupDir)) {
            for (const auto& entry : fs::recursive_directory_iterator(backupDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".fgd") {
                    fs::path relPath = fs::relative(entry.path(), backupDir);
                    fs::path gameFgdPath = fs::path(ctx.cs2Root) / relPath;
                    if (fs::exists(gameFgdPath)) {
                        if (fs::file_size(entry.path()) != fs::file_size(gameFgdPath)) {
                            BackupManager::SafeCopyFileWithRetry(entry.path(), gameFgdPath);
                            restoredOldFgd++;
                        }
                    }
                }
            }
        }
    } catch (...) {}

    if (restoredOldFgd > 0) {
        log(
            QString("[*] 已检测并清理 %1 个旧版残留的磁盘 FGD 翻译文件，还原为官方原版纯净状态").arg(restoredOldFgd),
            "#66d9ef"
        );
    }

    log(
        "[+] FGD 实体汉化引擎就绪：采用纯内存动态指向模式（0 磁盘写入，零文件污染）",
        "#a6e22e"
    );

    // ==========================================
    // STEP 3: Qt Patch
    // ==========================================
    log(
        "[3/3] 正在部署 Qt 汉化模块并修补 Qt5Core.dll...",
        "#e6db74"
    );

    try {
        fs::path destQtJson =
            cs2Bin / paths::kQtDictFile;

        fs::path destQmDll =
            cs2Bin / L"qtcore_qm.dll";

        if (!fs::exists(qtDictPath)) {
            log(
                "[-] 找不到 qt_translations.jsonc",
                "#f92672"
            );

            Restore(ctx, false, log);
            return false;
        }

        if (!fs::exists(qmDllSrc)) {
            log(
                "[-] 找不到 qtcore_qm.dll",
                "#f92672"
            );

            Restore(ctx, false, log);
            return false;
        }

        if (!BackupManager::SafeCopyFileWithRetry(qmDllSrc, destQmDll)) {
            log(
                "[-] 部署 qtcore_qm.dll 失败 (目标被占用或无写权限)",
                "#f92672"
            );

            Restore(ctx, false, log);
            return false;
        }

        std::wstring qtFallbackParam = (useMachineTrans && fs::exists(qtFallbackPath)) ? qtFallbackPath.wstring() : L"";

        // 优先合并主词典与机翻兜底词典；合并失败或未启用机翻时直接部署主词典
        bool qtJsonMerged = false;
        if (!qtFallbackParam.empty()) {
            std::wstring mergeErr;
            qtJsonMerged = DictionaryCompiler::MergeJsonFiles(
                    qtDictPath.wstring(),
                    qtFallbackParam,
                    destQtJson.wstring(),
                    mergeErr);
        }

        if (!qtJsonMerged) {
            if (!BackupManager::SafeCopyFileWithRetry(qtDictPath, destQtJson)) {
                log(
                    "[-] 部署 qt_translations.jsonc 失败 (目标被占用或无写权限)",
                    "#f92672"
                );

                Restore(ctx, false, log);
                return false;
            }
        }

        // 部署 FGD 词典副本到游戏 bin 目录作为可靠本地兜底
        if (fs::exists(fgdDictPath)) {
            BackupManager::SafeCopyFileWithRetry(fgdDictPath, cs2Bin / L"fgd_translations.jsonc");
        }
        if (fs::exists(fgdOverridePath)) {
            BackupManager::SafeCopyFileWithRetry(fgdOverridePath, cs2Bin / L"fgd_override.jsonc");
        }
        if (useMachineTrans && fs::exists(fgdFallbackPath)) {
            BackupManager::SafeCopyFileWithRetry(fgdFallbackPath, cs2Bin / L"fgd_fallback.jsonc");
        }

        // 修补 Qt5Core.dll
        fs::path backupQtCore = paths::Qt5Core(backupDir);

        fs::path targetQtCore =
            cs2Bin / paths::kQt5CoreDll;

        if (!PePatcher::PatchQtCore(
                backupQtCore.wstring(),
                targetQtCore.wstring(),
                err)) {

            log(
                QString(
                    "[-] 修补 Qt5Core.dll 失败: %1"
                )
                    .arg(
                        QString::fromStdWString(err)
                    ),
                "#f92672"
            );

            Restore(ctx, false, log);
            return false;
        }

        log(
            "[+] Qt5Core.dll PE Code Cave 注入与重定向修补成功",
            "#a6e22e"
        );

    } catch (const std::exception& e) {

        log(
            QString(
                "[-] 部署补丁异常: %1"
            )
                .arg(e.what()),
            "#f92672"
        );

        Restore(ctx, false, log);
        return false;
    }

    /*
     * 注入成功后记录 session_state。
     *
     * 这样即使用户之后直接关闭启动器，
     * 下次启动依然可以知道当前目录没有被还原。
     */
    if (!BackupManager::SaveSessionState(
            ctx.workingDir,
            true
        )) {
        log(
            "[-] 会话状态写入失败 (session_state.json)，异常退出后的自动恢复可能失效",
            "#f92672"
        );
    }

    log(
        "[SUCCESS] 汉化补丁注入完成，当前处于“已注入”状态。",
        "#a6e22e"
    );

    return true;
}

// ---------------------------------------------------------------------------

bool LocalizationService::Restore(const Context& ctx, bool showLog, const LogSink& log) {
    fs::path workPath(ctx.workingDir);

    fs::path backupDir =
        workPath / paths::kBackupDir;

    if (!BackupManager::HasBackup(
            backupDir.wstring())) {

        return true;
    }

    if (showLog) {
        log(
            "[*] 正在还原原版 FGD 实体定义及核心二进制...",
            "#66d9ef"
        );
    }

    std::wstring err;

    if (!BackupManager::RestoreAll(
            ctx.cs2Root,
            backupDir.wstring(),
            err)) {

        if (showLog) {
            log(
                QString(
                    "[-] 还原备份失败: %1"
                )
                    .arg(
                        QString::fromStdWString(err)
                    ),
                "#f92672"
            );
        }

        return false;
    }

    if (showLog) {
        log(
            "[SUCCESS] 还原操作完成！所有原版 FGD 实体定义及 Qt5Core.dll 已恢复原样。",
            "#a6e22e"
        );
    }

    return true;
}

// ---------------------------------------------------------------------------

RestoreCheck LocalizationService::CheckRestore(const Context& ctx, QString* reason) {
    const auto validation = BackupManager::BackupMatchesCurrentGame(
        ctx.cs2Root,
        (fs::path(ctx.workingDir) / paths::kBackupDir).wstring()
    );

    if (validation.status == BackupMatchStatus::Matches) {
        return RestoreCheck::Allowed;
    }

    if (reason) {
        *reason = QString::fromStdWString(validation.reason);
    }

    return (validation.status == BackupMatchStatus::GameUpdated)
               ? RestoreCheck::RejectedGameUpdated
               : RestoreCheck::RejectedOther;
}

bool LocalizationService::HasPendingRecovery(const Context& ctx) {
    const fs::path backupDir = fs::path(ctx.workingDir) / paths::kBackupDir;

    return BackupManager::HasUnrestoredSession(ctx.workingDir) ||
           (BackupManager::HasBackup(backupDir.wstring()) &&
            BackupManager::IsPatchDeployed(ctx.cs2Root));
}

bool LocalizationService::ClearSessionState(const std::wstring& workingDir) {
    return BackupManager::ClearSessionState(workingDir);
}

bool LocalizationService::IsCs2Running() {
    return Cs2Detector::IsCs2ProcessRunning();
}

bool LocalizationService::HasBackup(const Context& ctx) {
    return BackupManager::HasBackup((fs::path(ctx.workingDir) / paths::kBackupDir).wstring());
}

bool LocalizationService::IsPatchDeployed(const Context& ctx) {
    // 没有备份，不认为当前处于有效的已注入状态
    if (!HasBackup(ctx)) {
        return false;
    }

    // session_state 是否标记为已注入
    const bool sessionPatched = BackupManager::HasUnrestoredSession(ctx.workingDir);

    // CS2 目录中是否存在实际补丁文件
    const bool patchFilesPresent = BackupManager::IsPatchDeployed(ctx.cs2Root);

    return sessionPatched || patchFilesPresent;
}

bool LocalizationService::IsBackupMatching(const Context& ctx) {
    const auto validation = BackupManager::BackupMatchesCurrentGame(
        ctx.cs2Root,
        (fs::path(ctx.workingDir) / paths::kBackupDir).wstring()
    );
    return validation.status == BackupMatchStatus::Matches;
}

void LocalizationService::SyncDictionariesFromParent(const std::wstring& workingDir) {
    const fs::path transDir       = fs::path(workingDir) / paths::kTranslationsDir;
    const fs::path parentTransDir = fs::path(workingDir) / L".." / paths::kTranslationsDir;

    if (!fs::exists(parentTransDir)) {
        return;
    }

    for (const auto& name : {
             L"qt_translations.jsonc",
             L"qt_fallback.jsonc",
             L"fgd_translations.jsonc",
             L"fgd_override.jsonc",
             L"fgd_fallback.jsonc"
         }) {
        const fs::path pSrc = parentTransDir / name;
        const fs::path pDst = transDir / name;

        if (!fs::exists(pSrc)) {
            continue;
        }

        try {
            if (!fs::exists(pDst) || fs::last_write_time(pSrc) > fs::last_write_time(pDst)) {
                fs::copy_file(pSrc, pDst, fs::copy_options::overwrite_existing);
            }
        } catch (...) {}
    }
}

void LocalizationService::WriteAppDirPointer(const std::wstring& cs2Root,
                                             const std::wstring& workingDir,
                                             bool useMachineTrans) {
    if (cs2Root.empty()) return;

    const fs::path cs2Bin = paths::Win64Bin(cs2Root);
    if (!fs::exists(cs2Bin)) return;

    const fs::path pointerFile = cs2Bin / L"localizer_appdir.txt";

    try {
        std::wofstream ofs(pointerFile, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << workingDir << L"\n";
            ofs << L"use_machine_trans=" << (useMachineTrans ? 1 : 0) << L"\n";
        }
    } catch (...) {}
}

