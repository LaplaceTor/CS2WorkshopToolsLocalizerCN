# 架构与核心链路速查

> 本文档面向 Agent 按需查阅，记录跨模块协同与不写进代码注释的隐性约定。

## 1. 双进程形态与安全边界

- **启动器（`CS2WorkshopToolsLocalizerCN.exe`）**：Qt6 Widgets 桌面程序，负责 CS2 探测、原版备份、PE 补丁部署、拉起 Hammer、进程生命周期监听、IPC 控制与词典编辑。
- **注入 DLL（`qtcore_qm.dll`）**：纯 Win32/C++17 + MinHook（静态 `/MT` 运行库，不链接 Qt），随 `Qt5Core.dll` 补丁入口被 CS2 进程加载。
- **宿主守卫（`ProcessGuard`）**：`qtcore_qm.dll` 初始化时通过 `ProcessGuard::IsCs2HostProcess()` 与 `HasToolsArgument()` 校验宿主与 `-tools` 参数，仅在 Workshop Tools 模式下激活 Hook，避免影响常规游戏进程。

## 2. 注入、备份与还原链路

1. **目录定位（`Cs2Detector` + `LauncherConfig`）**：`main.cpp` 优先读取并校验 `config.ini` 中的 `Launcher/Cs2Root`；缺失或失效时按「运行中 `cs2.exe` 进程 → `Uninstall\Steam App 730` → Steam `libraryfolders.vdf`（优先含 AppID `730` 的库）→ 全盘符常见路径」顺序探测。
2. **版本校验与备份（`BackupManager`）**：注入前校验 `cs2.exe` / `Qt5Core.dll` / `Qt5Widgets.dll` 的 SHA256 与版本号，在启动器工作目录的 `backup/` 下保存原版文件及 `backup_manifest.json`；并通过会话状态标记支持异常退出后的启动自恢复。
3. **PE 补丁重定向（`PePatcher`）**：`PePatcher::PatchQtCore()` 在 `Qt5Core.dll` 中注入 Code Cave 与 `PatchHeader` 标记，使进程启动时自动加载 `qtcore_qm.dll`。
4. **工作目录指针（`localizer_appdir.txt`）**：`LocalizationService::WriteAppDirPointer()` 在 `<cs2Root>/game/bin/win64/` 写入 UTF-8 指针文件，记录启动器工作目录与 `useMachineTrans` 开关；`qtcore_qm.dll` 通过 `qm::ReadAppDirPointer()` 读取该文件定位 `translations/`。
5. **自动还原**：`HammerService` 借助 `ProcessMonitor` 监测进程退出后触发 `BackupManager::RestoreAll()` 还原原版 `Qt5Core.dll` 并清理补丁。

## 3. 宿主内部双引擎（`src/core/qm/`）

- **Qt 界面汉化（`qt_hooks.*` + `translation_engine.*`）**：
  - 拦截 `QMetaObject::tr` 及 `Qt5Widgets` / `Qt5Gui` 渲染与控件文本接口。
  - 通过 `ResolveCallerModuleName()` 识别调用方 DLL（如 `hammer`、`modeldoc_editor`），在 `FindTranslationScoped()` 中按「当前 DLL 子块 → `common` 公共区」回退查表，同时保护可编辑列表/树形条目与文件路径/标识符不被误译。
- **FGD 实体纯内存汉化（`fgd_memory_hook.*` + `fgd_core.*`）**：
  - 挂钩 `FileSystem` / `Tier0` 读取链路，在内存中解析并替换实体、属性、I/O 描述（结合 `fgd_translations.jsonc`、`fgd_override.jsonc` 与可选 `fgd_fallback.jsonc`），不修改磁盘上的原版 `.fgd` 文件。
  - 实体底层 I/O 名称通过 `RegisterProtectedFgdIoNames()` / `IsProtectedFgdIoName()` 注册保护。

## 4. 跨进程 IPC 与未翻译动态捕获

- **控制指令（`HammerIpc`）**：`qtcore_qm.dll` 在宿主进程创建隐藏消息窗口（类名 `CS2_HAMMER_LOCALIZER_IPC`），接收启动器发送的窗口消息：
  - `kMsgToggleLang`（`WM_USER + 101`）：切换原文 / 翻译
  - `kMsgReloadDict`（`WM_USER + 102`）：免重启热重载磁盘词典
  - `kMsgQueryStatus`（`WM_USER + 103`）：查询状态
- **未翻译捕获（`untranslated_capture.h`）**：
  - 共享内存 `Local\CS2_Hammer_Localizer_Untranslated`（`UntranslatedSharedMemoryHeader` + 1024 槽位环形表，含 `generationId` 版本代次）。
  - `UntranslatedCaptureWriter` 在宿主侧过滤中文与无字母文本后记录 `{text, module, hitCount, lastTimestamp}`；`UntranslatedCaptureReader` 在启动器词典编辑器中拉取展示并支持一键清空递增 `generationId`。

## 5. 词典编译与原地编辑

- **编译加载（`DictionaryCompiler`）**：解析 JSONC 词典并按需合并 `*_fallback.jsonc`（精翻优先级高于机翻）。
- **无损编辑（`DictionaryTextUpdater`）**：词典编辑器新增/修改/删除词条时执行 JSONC 文本原地替换，保留原文件行内 `//` 注释与缩进格式。
