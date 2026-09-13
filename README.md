# CS2 Workshop Tools Localizer CN（CS2 创意工坊工具汉化启动器）

> 专为 **Counter-Strike 2 (CS2)** 创意工坊工具集（Hammer 地图编辑器、ModelDoc、材质编辑器等 Workshop Tools）打造的一键中文汉化与启动工具。
> 开箱即用、安全无痕，支持自由修改与扩充汉化词库！

---

## 📑 目录

- [软件特色](#-软件特色)
- [安装](#-安装)
  - [方式一：下载发行版（推荐）](#方式一下载发行版推荐)
  - [方式二：从源码构建](#方式二从源码构建)
- [快速使用指南](#-快速使用指南)
- [使用示例](#-使用示例)
- [命令行辅助脚本](#-命令行辅助脚本)
- [参与完善汉化：词条编写规范](#-参与完善汉化词条编写规范)
- [项目结构与工作原理](#-项目结构与工作原理)
- [贡献指南](#-贡献指南)
- [常见问题 FAQ](#-常见问题-faq)
- [许可证与免责声明](#-许可证与免责声明)

---

## 🌟 软件特色

- **🇨🇳 深度汉化**：覆盖主菜单、工具栏、实体属性面板、视口右键菜单、实体说明及输入输出等各个界面。
- **📦 模块级隔离词典**：采用单文件多子块架构，为 Hammer、ModelDoc、粒子编辑器等每个独立工具提供专属作用域，彻底避免"一词多义"冲突。
- **🌐 在线词典更新**：支持一键从 GitHub 官方仓库拉取最新汉化词条，保持与社区最新汉化同步（纯手动触发，防误触安全确认）。
- **🛡️ 纯净安全**：启动时自动备份游戏原版文件，**关闭工具后自动还原所有文件并清理临时补丁**，完全不修改、不污染您的 CS2 游戏目录。
- **🎯 自动识别游戏**：启动时自动寻找 CS2 安装目录（Steam 注册表 / 库文件夹 / 常见盘符）和已有的 Addon 模组，无需手动配置。
- **⚡ 实时生效**：翻译词条直接保存在文本文件中，改完无需编译——点击"热重载词典"即可在运行中的 Hammer 里立刻看到中文效果。
- **🤖 可选机翻兜底**：勾选"使用机翻"后自动引入 `*_fallback.jsonc` 兜底词典，未精翻的词条显示机翻结果，精翻词条始终保持最高优先级。
- **🐞 调试监控**：内置调试窗口（`-debug` 启动），可查看实时日志流、内存诊断与崩溃事件。

---

## 📥 安装

### 系统要求

| 项目 | 要求 |
| :--- | :--- |
| 操作系统 | Windows 10 / 11（64 位） |
| 游戏 | 已通过 Steam 安装 **Counter-Strike 2**，并已安装 **Counter-Strike 2 Workshop Tools**（Steam 库 → CS2 → 属性 → DLC） |
| 运行时 | Microsoft Visual C++ 运行库（x64，一般随系统/游戏已安装） |
| 权限 | 建议**以普通权限运行**即可；若游戏安装在 `Program Files` 等受保护目录且写入失败，请右键"以管理员身份运行" |
| 磁盘 | 约 60 MB（含 Qt 运行时与全部词典） |

> ⚠️ **注意**：请**不要**把启动器放在 CS2 游戏目录内运行，放在任意独立文件夹即可。

### 方式一：下载发行版（推荐）

1. 前往 [Releases](https://github.com/LaplaceTor/CS2WorkshopToolsLocalizerCN/releases) 页面下载最新的 `CS2WorkshopToolsLocalizerCN-<版本号>.zip`。
2. 将压缩包**完整解压**到任意文件夹（保持目录结构，不要只单独拷出 exe）。
3. 双击 `CS2WorkshopToolsLocalizerCN.exe` 即可启动。

解压后会得到如下内容（**以下二进制均为发行包产物，不存放于 Git 仓库；仓库中只有源码、脚本与 `translations/` 词典**）：

```text
CS2WorkshopToolsLocalizerCN/
├── CS2WorkshopToolsLocalizerCN.exe   # 启动器主程序
├── qtcore_qm.dll                     # 注入到工具的 Qt 汉化模块
├── Qt6Core.dll / Qt6Gui.dll ...      # Qt 运行时（由 windeployqt 收集）
├── translations/                     # 汉化词典目录（可自由修改）
│   ├── qt_translations.jsonc
│   ├── qt_fallback.jsonc
│   ├── fgd_translations.jsonc
│   ├── fgd_fallback.jsonc
│   └── fgd_override.jsonc
├── README.md
└── LICENSE
```

> **升级**：直接下载新版本解压覆盖即可，词典可先备份后覆盖，或用启动器的"🌐 更新在线翻译"单独更新词条。

### 方式二：从源码构建

适用于想参与开发或自行定制的开发者。

**前置依赖**

| 依赖 | 版本 | 说明 |
| :--- | :--- | :--- |
| Visual Studio | 2022（含"使用 C++ 的桌面开发"工作负载） | 提供 MSVC 编译器 |
| CMake | ≥ 3.20 | 构建系统（VS 自带版本亦可） |
| Ninja | 任意新版 | 默认生成器，需加入 `PATH` |
| Qt | 6.x（推荐 **6.8.0 msvc2022_64**） | 需要 `Core / Gui / Widgets / Network / Concurrent` 组件 |
| Python | ≥ 3.8 | 仅使用辅助脚本时必需（可选） |
| Git | 任意新版 | 用于拉取子模块 minhook |

**构建步骤**

仓库只提供 `CMakeLists.txt`，直接用标准 CMake 流程构建即可（`Qt6_DIR` / `QTDIR` / `Qt6_ROOT` 任一环境变量均可让 CMake 定位 Qt，未设置时需显式传 `CMAKE_PREFIX_PATH`）：

```bash
# 1. 克隆仓库（含子模块）
git clone --recursive https://github.com/LaplaceTor/CS2WorkshopToolsLocalizerCN.git
cd CS2WorkshopToolsLocalizerCN

# 若已克隆但缺少子模块：
# git submodule update --init --recursive

# 2. 配置（在 "x64 Native Tools Command Prompt for VS 2022" 中执行）
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\msvc2022_64" -DAPP_VERSION=v1.0.0

# 3. 编译
cmake --build build --config Release
```

构建完成后，你指定的构建目录（上例为 `build/`，本地生成、不入库）中会产出：
- `CS2WorkshopToolsLocalizerCN.exe`（启动器主程序）
- `qtcore_qm.dll`（Qt 汉化注入模块）
- `translations/`（由 CMake 自动从仓库复制的词典目录）

调试时可直接运行构建目录中的 exe；若要生成可分发的绿色包，可自行执行 `windeployqt --release --no-translations --no-opengl-sw --no-system-dxc-compiler --no-compiler-runtime <exe>` 收集 Qt 运行时并打包（见 `.github/workflows/release.yml` 中的官方打包步骤）。

**运行测试**

```bash
cmake --build build --config Release --target test_components
```

随后运行构建目录中生成的 `test_components` 可执行文件即可。

---

## 🚀 快速使用指南

1. **启动**：双击 `CS2WorkshopToolsLocalizerCN.exe`（首次建议先完全退出 Steam 中的 CS2 / Hammer）。
2. **更新词典（可选）**：点击 **`🌐 更新在线翻译`** 获取云端最新词条。
3. **选择模组**：在"目标 Addon 模组"下拉菜单中选择要编辑的 Addon（默认为 `addon_template`）。
4. **一键启动**：点击 **`启动 HAMMER 汉化版`**，程序会自动完成备份 → 汉化 → 打补丁并拉起 Hammer。
5. **尽情创作**：关闭 Hammer 后，启动器会自动还原所有原版文件；也可随时手动点击 **`还原`**。

### 界面功能说明

| 区域 / 按钮 | 作用 |
| :--- | :--- |
| **目标 Addon 模组** | 选择要编辑的 Addon，列表来自 `content/csgo_addons/` |
| **附加启动参数** | 传给 Workshop Tools 的命令行参数，例如 `-gpuraytracing` |
| **使用机翻** | 启用 `fgd_fallback.jsonc` / `qt_fallback.jsonc` 兜底词典 |
| **仅注入** | 只部署汉化补丁，不启动 Hammer |
| **启动 HAMMER 汉化版** | 注入（如未注入）+ 启动 Hammer |
| **还原** | 恢复原版文件并清理补丁 |
| **🌐 更新在线翻译** | 从 GitHub / jsDelivr 拉取最新词典（Hammer 运行时不可用） |
| **🔀 切换原文 / 翻译** | 运行中一键切换原文与中文，便于对照 |
| **⚡ 热重载词典** | 运行中重新从磁盘读取词典，免重启生效（保存词典文件也会自动触发） |
| **🐞 调试监控** | 打开调试窗口，查看日志流与诊断信息 |
| **📖 字典指南** | 打开词典编写说明 |

---

## 💡 使用示例

### 示例 1：第一次使用并启动汉化版 Hammer

1. 确认 CS2 与 Workshop Tools 已安装、Steam 处于正常状态，且 **CS2 未在运行**。
2. 运行 `CS2WorkshopToolsLocalizerCN.exe`。若提示"未检测到 Counter-Strike 2 安装路径"，说明 Steam 注册表异常，请修复 Steam 安装或重装一次游戏后再试。
3. 下拉选择你的 Addon，点击 **`启动 HAMMER 汉化版`**，日志区会依次出现：

```text
[1/3] 正在校验游戏版本并准备原版备份...
[+] 成功捕获并绑定 N 个原版 FGD 与 Qt5Core.dll
[2/3] 正在部署 FGD 汉化...
[+] 成功汉化并部署 N 个 FGD 文件
[3/3] 正在部署 Qt 汉化模块并修补 Qt5Core.dll...
[+] Qt5Core.dll PE Code Cave 注入与重定向修补成功
[SUCCESS] 汉化补丁注入完成，当前处于"已注入"状态。
```

4. Hammer 启动后即为中文界面。关掉 Hammer，启动器自动还原；下次启动会重新注入。

### 示例 2：改了词典，马上看效果（免重启）

1. 在 `translations/qt_translations.jsonc` 的 `hammer` 子块中加入一条：
   ```jsonc
   "Clipping Tool": "切割工具",
   ```
2. 保存文件 → 启动器监测到词典变化，自动热重载；或手动点击 **`⚡ 热重载词典`**。
3. Hammer 中对应菜单立即变为中文，无需重启。

### 示例 3：界面还有英文？开启机翻兜底

勾选 **`使用机翻`** 后重新注入，未精翻的词条会使用 `qt_fallback.jsonc` / `fgd_fallback.jsonc` 中的机翻结果；已精翻词条优先级更高，不会被覆盖。

### 示例 4：给 Hammer 传启动参数

在"附加启动参数"中填写后启动，例如：

```text
-gpuraytracing
```

### 示例 5：以调试模式排查问题

```bat
CS2WorkshopToolsLocalizerCN.exe --debug
```

启动后自动打开调试监控窗口，可查看实时日志流、内存诊断与崩溃事件。

### 示例 6：手动还原（程序异常退出后）

正常关闭时会自动还原；若曾强制结束进程，重新启动启动器时会执行异常退出检测并尝试自动恢复。若仍有残留，手动点击 **`还原`** 即可。注入时备份的原版文件保存在启动器所在目录（**运行时生成的本地目录，不在 Git 仓库中**）。

---

## 🧰 命令行辅助脚本

所有脚本位于 `scripts/` 目录，需 **Python 3.8+**（Windows 下建议用 `python`，输出已做 UTF-8 兼容处理）。

> 提示：下面示例中的路径全部指向**你本机 CS2 的安装目录**（如 `game\bin\win64\tools\`），与仓库目录无关；输出文件请自行指定到仓库外的临时位置。

### `find_dll_strings.py` — 从 DLL 中查找界面原文

当在 `qt_translations.jsonc` 里加了词却不生效时，用它在 CS2 的 DLL 中搜出**完整的原始字符串**。

```bash
# 扫描 tools 目录下所有 DLL，查找包含 Transform 的字符串
python scripts/find_dll_strings.py -f "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\tools\*.dll" -p "Transform"

# 单文件精确搜索
python scripts/find_dll_strings.py -f "D:\...\tools\hammer.dll" -p "Transform"

# 正则 + 忽略大小写
python scripts/find_dll_strings.py -f "D:\...\tools\*.dll" -p "^(Open|Create).*" -i

# 导出为可直接使用的词典模板 {"原文": ""}（输入为 CS2 安装目录下的工具 DLL）
python scripts/find_dll_strings.py -f "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\tools\hammer.dll" -j -out hammer_extracted.jsonc

# 搜索完自动用记事本打开结果
python scripts/find_dll_strings.py -f "D:\...\tools\*.dll" -p "Grid" --open
```

常用参数：`-f/--file`（目标文件，支持通配符/目录）、`-p/--pattern`（关键字或正则）、`-out`、`-out-dir`、`-m/--min-len`、`-i`、`-o/--show-offset`、`-u/--unique`、`--raw`、`--all-sections`、`-j/--json`、`--open`。

### `extract_fgd_strings.py` — 从 FGD 提取待翻译文本

输入为 CS2 安装目录下的 `game` 目录（内含 `core/`、`csgo/`、`csgo_core/` 等 FGD 文件），默认输出 `fgd_extracted.jsonc`，并会与现有词典增量比对、只提取未翻译条目。

```bash
# 从 CS2 安装目录提取待翻译文本
python scripts/extract_fgd_strings.py -i "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game"

# 指定输出文件，并附带来源文件与行号注释
python scripts/extract_fgd_strings.py -i "D:\...\game" -o fgd_extracted.jsonc --include-sources

# 生成后自动查重
python scripts/extract_fgd_strings.py -i "D:\...\game" -o fgd_extracted.jsonc --check-dup
```

### `translate_fallback.py` — 大模型批量生成机翻兜底词典

兼容任何 OpenAI 接口规范的服务（llama.cpp / Ollama / vLLM / DeepSeek / Qwen …）。

```bash
# 使用本地默认服务 http://127.0.0.1:8080 翻译全部字典
python scripts/translate_fallback.py

# 只翻译 Qt 词典，3 线程、每批 10 行
python scripts/translate_fallback.py --file qt --threads 3 --batch-size 10

# 使用云端接口
python scripts/translate_fallback.py --api-url https://api.deepseek.com/v1/chat/completions --api-key sk-xxxx --model deepseek-chat
```

支持环境变量：`TRANSLATION_API_URL` / `OPENAI_API_BASE` / `OPENAI_API_KEY` / `TRANSLATION_MODEL`。

### `check_duplicates.py` — 词典查重（提交前必跑，CI 同款）

```bash
python scripts/check_duplicates.py
python scripts/check_duplicates.py translations/qt_translations.jsonc translations/fgd_translations.jsonc
```

---

## ✍️ 参与完善汉化：词条编写规范

所有汉化词条保存在 `translations/` 目录下的 `.jsonc` 文件中：

```text
translations/
├── qt_translations.jsonc   # 1. 软件界面与菜单精翻字典（支持模块子块与注释）
├── qt_fallback.jsonc       # 2. 软件界面机翻兜底字典（勾选"使用机翻"时生效，脚本生成）
├── fgd_translations.jsonc  # 3. 地图实体与属性精翻字典（精确匹配替换）
├── fgd_fallback.jsonc      # 4. 地图实体与属性机翻兜底字典（勾选"使用机翻"时生效，脚本生成）
└── fgd_override.jsonc      # 5. 实体键值描述补充与覆盖字典
```

> **⚠️ 译者提醒**
> 1. 请尽量**不要**直接提交未经人工校对的 AI 批量翻译！地图工具中大量领域专有名词需结合上下文核对。
> 2. 所有字典文件均为标准 JSONC 格式，支持 `//` 单行注释与 `/* */` 块注释，方便批注。
> 3. `*_fallback.jsonc` 由脚本生成，**请勿手工编辑**（会被覆盖）。
>
> **当前主译者正在完善 HAMMER 以及 ASSET BROWSER，如有贡献想法，请自行规避。**

### 1️⃣ `qt_translations.jsonc`（界面、菜单与工具栏多子块规范）

采用**单文件多子块（Sectioned）**结构，每个子块对应一个工具 DLL 的名称（不含 `.dll` 扩展名）：

```jsonc
{
  // 1. 通用公共区（所有工具共享的回退词典）
  "common": {
    "File": "文件",
    "Edit": "编辑",
    "View": "视图",
    "Tools": "工具",
    "Help": "帮助",
    "Exit": "退出",
    "Asset Browser": "资产浏览器",
    "All Assets": "所有资产"
  },

  // 2. Hammer 地图编辑器 (hammer.dll) 专属子块
  "hammer": {
    "Clipping Tool": "切割工具",
    "Translate Tool": "移动工具",
    "Rotate Tool": "旋转工具",
    "Selection Tool": "选择工具",
    "Undo": "撤销",
    "Redo": "重做",
    "Bone": "骨骼节点"      // 同词在 Hammer 中译为"骨骼节点"
  },

  // 3. ModelDoc 模型编辑器 (modeldoc_editor.dll) 专属子块
  "modeldoc_editor": {
    "Open ModelDoc File": "打开ModelDoc文件",
    "Bone": "骨骼"          // 同词在 ModelDoc 中译为"骨骼"
  },

  // 4. 其他工具（pet.dll, met.dll, sfm.dll, 各类 subtool 等）
  "pet": { "Units": "单位" },
  "met": { "Linear": "线性" },
  "soundviewer_subtool": {
    "Resume History": "恢复历史",
    "Pause History": "暂停历史"
  }
}
```

**翻译编写技巧**

- **同词不同译隔离**：相同的英文单词（如 `Bone`）写在各自子块即可精准区分。
- **自动回退机制**：子块中查不到时，引擎自动回退到 `"common"` 公共区。
- **快捷键无需手输**：界面显示带快捷键的文本（如 `"Clipping Tool [Shift+X]"`、`"Save\tCtrl+S"`）时，只翻译基础单词即可，引擎会**自动保留并拼接** `[Shift+X]`、`\tCtrl+S`、`...`、`:` 等后缀。

### 2️⃣ `fgd_translations.jsonc`（实体与属性说明）

用于翻译地图实体、灯光、物理属性、触发器、输入输出 (I/O) 的显示名称与悬停说明。

```jsonc
{
  "Omnidirectional point light": "全向点光源",
  "Light Source": "光源",
  "Name": "名称",
  "The name that other entities use to refer to this entity.": "其他实体用于引用此实体的名称。",
  "Removes this entity from the world.": "从世界中移除此实体。",
  "Enabled": "已启用",
  "Disabled": "已禁用"
}
```

> ⚠️ 实体底层的英文标识符（如 `targetname`、`angles`）会被工具自动保护，**只翻译展示文本与描述说明**即可。

### 3️⃣ `fgd_override.jsonc`（实体键值描述补充与覆盖）

用于给 Valve 原版 FGD 中**缺失悬停描述**或**需要个性化说明**的属性 (Key)、实体类 (Class)、I/O 补充中文。

- `fgd_translations.jsonc`：按已有英文原文精确匹配翻译（无法给原版无描述的属性补说明）。
- `fgd_override.jsonc`：按键名 / 类名 / I/O 名直接**新增**或**强制替换**说明。

```jsonc
{
  // 1. 全局属性描述补充与覆盖（按属性名匹配）
  "properties": {
    "bodygroups": "设置模型的子部件与可选身体部件网格组合。",
    "vscripts": "实体生成后自动加载并执行的 VScript 脚本文件列表。",
    "clientSideEntity": "是否仅在客户端创建并运行此实体（不向服务器同步）。",
    "TeamNum": "所属队伍编号（0: 任意/无队伍, 2: T 阵营, 3: CT 阵营）。",
    "box_mins": "包围盒/光照探针体积的最小边界坐标 (X Y Z)。",
    "flood_fill": "忽略玩家不可达的空间，加快光照烘焙速度并节省显存。"
  },

  // 2. 输入 / 输出 (I/O) 说明补充
  "io": {
    "ClearParent": "解除与父级实体的挂载绑定关系，使其独立运动。",
    "FollowEntity": "骨骼合并 (Bone Merge) 附加到目标实体。",
    "Kill": "从世界中移除此实体并释放资源。",
    "SetHealth": "设置该实体的当前生命值。"
  },

  // 3. 实体类说明补充与类作用域专属属性
  "classes": {
    "info_node": "AI 地面导航节点，供 NPC 寻路与路径规划计算使用。",
    "csm_fov_override": "级联阴影贴图 (CSM) 视场角覆盖控制器。",
    "env_cubemap": {
      "description": "用于采样环境间接镜面反射的高动态范围立方体贴图实体。",
      "properties": {
        "influenceradius": "当前立方体贴图的生效影响半径（单位：英寸）。"
      }
    }
  }
}
```

### 🔍 改了词条却没生效？

这通常是因为该界面的英文文本**不是孤立的单词**，而是包含在更长的完整字符串、格式化占位符或特殊前缀（`&` 快捷键、`...`、`%s` 占位符等）中。
用 [`find_dll_strings.py`](#-命令行辅助脚本) 从 DLL 中扫出完整原文，复制填入对应子块即可。

---

## 🏗️ 项目结构与工作原理

```text
CS2WorkshopToolsLocalizerCN/
├── src/                    # C++ 源码
│   ├── main.cpp            # 程序入口（支持 --debug）
│   ├── mainwindow.*        # 主界面与全部业务流程
│   ├── debug_window.*      # 调试监控窗口
│   ├── cs2_detector.*      # CS2 / Addon 目录检测（注册表 + 库文件夹 + 常见路径）
│   ├── backup_manager.*    # 原版文件备份与一致性校验（SHA256）
│   ├── fgd_translator.*    # FGD 实体定义汉化
│   ├── pe_patcher.*        # Qt5Core.dll 的 PE Code Cave 注入与重定向
│   ├── hook_manager.*      # MinHook 封装
│   ├── dictionary_compiler.*# JSONC 词典解析、模块子块与 fallback 合并
│   └── qtcore_qm.cpp       # 注入到 Workshop Tools 进程的 Qt 汉化模块
├── third_party/minhook/    # Git 子模块
├── translations/           # 汉化词典（社区贡献主战场）
├── scripts/                # Python 辅助脚本
├── tests/                  # 组件测试
└── .github/workflows/      # CI：词典查重、Release 打包
```

**注入流程**：`① 校验游戏版本并备份原版文件（FGD + Qt5Core.dll）` → `② 汉化并部署 FGD` → `③ 部署 qtcore_qm.dll 并对 Qt5Core.dll 做 PE 补丁`。
Hammer 退出或点击"还原"时，从启动时建立的本地备份中恢复全部原版文件并清理补丁，游戏目录保持纯净（备份目录仅存在于本地，不纳入版本库）。

---

## 🤝 贡献指南

欢迎任何形式的贡献：补充/修正词条、改进汉化引擎、完善脚本、修复 Bug、改进文档。

### 贡献汉化词条（无需编程）

1. **Fork** 本仓库，基于 `main` 建分支：`git checkout -b trans/hammer-toolbar`。
2. 修改 `translations/` 下的**精翻字典**（`qt_translations.jsonc`、`fgd_translations.jsonc`、`fgd_override.jsonc`）。
3. 本地自查（务必执行，CI 会跑同款检查）：
   ```bash
   python scripts/check_duplicates.py
   ```
4. 运行启动器验证效果（可用 `⚡ 热重载词典` 免重启预览）。
5. 提交并发起 **Pull Request**。

**词条规范要求**

- ✅ 只翻译**展示文本**，不要动代码标识符、文件路径、控制台命令、`%s`/`{0}` 等占位符。
- ✅ 保持原文中的快捷键后缀、冒号、省略号与首尾空格（引擎会自动拼接，但你不应手动改写原文结构）。
- ✅ 一个英文原文在同一作用域内**只能出现一次**（查重脚本会拦截重复 Key）。
- ✅ 术语保持一致：`Translate → 平移`、`Normal → 法线`、`Bake → 烘焙`、`Mutator → 修改器`、`Entity → 实体`、`Brush → 笔刷` 等。
- ❌ 不要提交整批未经人工校对的机翻结果。
- ❌ 不要手工编辑 `qt_fallback.jsonc` / `fgd_fallback.jsonc`（由 `translate_fallback.py` 生成）。
- ❌ 不要改动与本次翻译无关的内容，避免大范围格式化造成冲突。

### 贡献代码

- **开发环境**：Visual Studio 2022（MSVC）+ CMake ≥ 3.20 + Ninja + Qt 6.8.0 msvc2022_64，参见[从源码构建](#方式二从源码构建)。
- **编码规范**：源码与注释统一 UTF-8（CMake 已为 MSVC 开启 `/utf-8`）；新增源文件请同步更新 `CMakeLists.txt`。
- **提交信息**：建议遵循 [Conventional Commits](https://www.conventionalcommits.org/)，如 `feat: 支持词典热重载`、`fix: 修复还原失败`、`trans: 补充 Hammer 工具栏词条`。
- **测试**：改动 `cs2_detector` / `pe_patcher` / `fgd_translator` / `backup_manager` / `dictionary_compiler` 后，请构建并运行 `test_components`。
- **PR 说明**：请描述改动动机、验证方式（游戏版本 + 复现步骤），必要时附截图。

### CI 工作流

| 工作流 | 触发条件 | 作用 |
| :--- | :--- | :--- |
| `check-translations.yml` | 推送/PR 中改动 `*.json` 或 `scripts/check_duplicates.py` | 检测词典重复 Key，失败会阻断合并 |
| `release.yml` | 手动 `workflow_dispatch`，输入版本号（如 `v1.0.0`） | 构建、windeployqt、打 ZIP 并发布 GitHub Release |

---

## ❓ 常见问题 FAQ

**Q：启动就弹窗"未检测到 Counter-Strike 2 安装路径"？**
A：程序通过 Steam 注册表、Steam 库文件夹与常见盘符定位 CS2。请确认 CS2 已正确安装且 Steam 注册表正常；修复方式：Steam → 设置 → 下载 → "Steam 库文件夹"刷新，或重装一次游戏。

**Q：注入时提示文件被占用 / 写入失败？**
A：请先**完全退出 CS2 与 Hammer**，再执行注入；若仍失败，右键"以管理员身份运行"。

**Q：游戏更新后汉化失效？**
A：启动器会校验游戏版本，检测到更新会自动重建原版备份；如中途 Steam 正在更新，会在 2 秒后进行二次哈希确认。等待更新完成后重新注入即可。

**Q：程序被强杀，游戏文件会不会被改坏？**
A：不会。注入前已对原版 FGD 与 Qt5Core.dll 做完整备份（保存在启动器所在目录，仅本地生成、不入库），重新启动启动器会检测异常退出并自动恢复；也可手动点击 **`还原`**。

**Q：会不会被 VAC 封号？**
A：本工具仅修改本地 Workshop Tools 的界面文本，不注入、不修改 CS2 游戏主程序逻辑，也不联网参与对局。请自行判断风险，作者不对使用后果负责。

**Q：为什么有些界面仍是英文？**
A：该文本可能未收录进词典，或原文是带占位符/快捷键的完整长字符串。用 `find_dll_strings.py` 找到完整原文后加入词典，或勾选"使用机翻"启用兜底。

---

## 📄 许可证与免责声明

- 本项目基于 **GNU General Public License v3.0 (GPL-3.0)** 开源，详见 [LICENSE](./LICENSE)。二次分发请遵循相同协议。
- 第三方组件：[MinHook](https://github.com/TsudaKageyu/minhook)（BSD-2-Clause）、Qt 6（LGPL/GPL/商业授权）。
- 本项目为社区非官方工具，与 Valve Corporation 无任何关联。Counter-Strike 2 及相关商标归 Valve 所有。
- 使用本工具造成的任何损失，作者不承担责任；请在了解风险的前提下使用。

---

<div align="center">

如果这个项目帮到了你，欢迎点个 ⭐ Star，也可以把你的翻译与修正通过 PR / Issues 分享出来，一起完善 CS2 中文地图与模组制作生态！

</div>
