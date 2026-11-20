# AGENTS.md

## 核心约束

- **验证方式**：跳过单元测试；注入与 Hook 改动通过实机运行 Hammer 验证，词典改动执行 `python scripts/check_duplicates.py` 校验。
- **架构分层**：保持 `src/ui/` → `src/service/` → `src/core/` 单向依赖；`Service` 保持无 Qt 控件，`Core` 仅依赖底层实现。
- **优先复用**：新增逻辑前先检索既有实现，不重复造轮子。

## 模块速查索引（一跳定位）

| 目录 / 文件 | 职责 |
| :--- | :--- |
| `src/main.cpp` | 启动入口：读取/探测/持久化 `Cs2Root` 并拉起主窗口 |
| `src/ui/mainwindow.*` | 主界面：配置读写、注入/启动/还原/热重载按钮与状态刷新 |
| `src/ui/dictionary_editor_window.*` | 词典编辑器与运行时未翻译捕获池界面 |
| `src/ui/debug_window.*` | 调试监控窗口（日志流与模块诊断） |
| `src/service/localization_service.*` | 备份 → 目录指针 → PE 补丁 → 还原业务编排 |
| `src/service/hammer_service.*` | Hammer 拉起、进程存活性监控、IPC 与热重载编排 |
| `src/service/dictionary_service.*` / `dictionary_editor_service.*` | 在线词典更新、模板生成、词典检索与捕获拉取 |
| `src/service/update_service.*` | 启动时程序版本与词典更新检查 |
| `src/core/path_constants.h` | CS2 目录/文件名常量与路径拼接单点定义 |
| `src/core/launcher_config.*` | `config.ini` 读写（`Cs2Root` / `SelectedAddon` / `LaunchArgs` / `UseMachineTrans`） |
| `src/core/cs2_detector.*` | CS2 根目录探测（运行进程 / 注册表 / `libraryfolders.vdf` / 盘符）与 Addon 扫描 |
| `src/core/backup_manager.*` | 原版文件 SHA256 校验、备份与异常退出恢复 |
| `src/core/pe_patcher.*` | `Qt5Core.dll` PE Code Cave 注入与补丁标记解析 |
| `src/core/hammer_ipc.*` | 启动器与宿主隐藏窗口之间的 `WM_USER` 消息通信 |
| `src/core/untranslated_capture.h` | 跨进程未翻译文本共享内存协议（`Writer` / `Reader`） |
| `src/core/dictionary_compiler.*` / `dictionary_text_updater.*` | JSONC 词典解析合并 / 保留注释的原地文本更新 |
| `src/core/qtcore_qm.cpp` & `src/core/qm/` | 注入 DLL 入口、Qt Hook（`qt_hooks`）、FGD 内存 Hook（`fgd_memory_hook`）与翻译引擎（`translation_engine`） |

## 按场景读取与检查

- **修改注入、Hook、备份、IPC 或捕获协议前**：先阅读 [ARCHITECTURE.md](ARCHITECTURE.md) 了解双进程协同与不变量。
- **构建与产物**：以 `CMakeLists.txt` 和 `.github/workflows/pr-ci.yml` 为准（需先初始化 `third_party/minhook` 子模块）；增删 `.cpp` 文件需同步更新 `CMakeLists.txt`。
- **修改 `MainWindow` 成员**：保持 `src/ui/mainwindow.cpp` 构造函数初始化列表与 `src/ui/mainwindow.h` 声明顺序一致（MSVC `C5038`）。
- **修改词典（`translations/`）**：仅编辑 `qt_translations.jsonc`、`fgd_translations.jsonc`、`fgd_override.jsonc`（`*_fallback.jsonc` 由脚本生成），提交前运行 `python scripts/check_duplicates.py`。
