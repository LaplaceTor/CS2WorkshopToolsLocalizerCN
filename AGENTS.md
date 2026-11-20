# AGENTS.md

## 核心约束

- **验证方式**：跳过单元测试；注入与 Hook 改动通过实机运行 Hammer 验证，词典改动执行 `python scripts/check_duplicates.py` 校验。
- **架构分层**：保持 `src/ui/` → `src/service/` → `src/core/` 单向依赖；`Service` 保持无 Qt 控件，`Core` 仅依赖底层实现。
- **优先复用**：新增逻辑前先检索既有单点实现：路径常量见 `src/core/path_constants.h`，配置持久化见 `src/core/launcher_config.*`，JSONC 原地编辑见 `src/core/dictionary_text_updater.*`。

## 按场景检查

- **构建与产物**：构建步骤与目标定义以 `CMakeLists.txt` 和 `.github/workflows/pr-ci.yml` 为准（需先初始化 `third_party/minhook` 子模块）。
- **增删源码文件**：同步更新 `CMakeLists.txt` 中的显式文件清单。
- **修改 `MainWindow` 成员**：保持 `src/ui/mainwindow.cpp` 构造函数初始化列表与 `src/ui/mainwindow.h` 声明顺序一致（MSVC `C5038`）。
- **修改词典（`translations/`）**：仅编辑 `qt_translations.jsonc`、`fgd_translations.jsonc`、`fgd_override.jsonc`（`*_fallback.jsonc` 由脚本生成），提交前运行 `python scripts/check_duplicates.py`。
