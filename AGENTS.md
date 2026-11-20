# AGENTS.md

## 核心约束

- **不写单元测试**：本项目不设单元测试目标（见 `CMakeLists.txt`），注入与 Hook 行为通过实机运行 Hammer 验证；词典改动通过 `python scripts/check_duplicates.py` 校验。
- **尊重架构设计**：严格保持 `UI (src/ui/)` → `Service (src/service/)` → `Core (src/core/)` 单向依赖。`Core` 不得反向依赖 `UI`，`Service` 不依赖 Qt 控件。
- **不重复造轮子**：复用现有工具与单点定义，例如路径常量与拼接统一走 `src/core/path_constants.h`，配置读写统一走 `src/core/launcher_config.*`，JSONC 原地编辑走 `src/core/dictionary_text_updater.*`。

## 构建与检查

- **构建命令**（Windows x64 + VS 2022 MSVC + CMake ≥ 3.20 + Ninja + Qt 6，需先初始化子模块 `third_party/minhook`）：
  ```powershell
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:Qt6_DIR"
  cmake --build build --config Release
  ```
- **产物目标**：`CS2WorkshopToolsLocalizerCN.exe`（Qt6 Widgets 启动器）与 `qtcore_qm.dll`（MinHook 注入 DLL，静态链接 `/MT` 运行库，不依赖 Qt）。
- **源码清单显式维护**：增删 `.cpp` 文件需同步更新 `CMakeLists.txt`；`MainWindow` 构造函数初始化列表顺序需与 `src/ui/mainwindow.h` 声明顺序一致（MSVC `/utf-8` + `C5038`）。

## 词典规范（`translations/*.jsonc`）

- 只编辑 `qt_translations.jsonc`、`fgd_translations.jsonc`、`fgd_override.jsonc`；`*_fallback.jsonc` 由脚本生成，不手工修改。
- 提交前执行 `python scripts/check_duplicates.py` 确保无重复 Key 且 JSONC 语法合法。
