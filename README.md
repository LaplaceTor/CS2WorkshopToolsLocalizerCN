# CS2 Workshop Tools Localizer CN (CS2 创意工坊工具汉化启动器)

> 专为 **Counter-Strike 2 (CS2)** 创意工坊工具集 (Hammer 编辑器、材质编辑器等 Workshop Tools) 打造的一键中文汉化与启动工具。  
> 开箱即用、安全无痕，支持自由修改与扩充汉化词库！

---

## 🌟 软件特色

- **🇨🇳 深度汉化**：汉化覆盖主菜单、工具栏、实体属性面板、视口右键菜单、实体说明及输入输出等各个界面。
- **🌐 在线词典更新**：支持一键从 GitHub 官方仓库拉取最新汉化词条，保持与社区最新汉化同步（纯手动触发，防误触安全确认）。
- **🛡️ 纯净安全**：启动时自动备份游戏原版文件，**关闭工具后自动还原所有文件并清理临时补丁**，完全不修改、不污染您的 CS2 游戏目录。
- **🎯 自动识别游戏**：启动时自动寻找您的 CS2 安装目录和已有的 Addon 模组，无需手动复杂配置。
- **⚡ 实时生效**：翻译词条直接保存在文本文件中，修改后无需任何编译，重新启动工具即可立刻看到中文效果！

---

## 🚀 快速使用指南

1. **运行启动器**：解压后双击打开 `CS2WorkshopToolsLocalizerCN.exe`。
2. **更新词典 (可选)**：可点击 **`🌐 更新在线翻译`** 获取云端最新翻译词典。
3. **选择模组**：在下拉菜单中选择你要编辑的 Addon 模组。
4. **一键启动**：点击 **`🚀 启动 CS2 Workshop Tools (汉化版)`** 按钮。
5. **尽情创作**：进入中文版 CS2 Workshop Tools 进行地图与模组创作；关闭工具后，启动器会自动帮您恢复所有原版文件。

---

## ✍️ 如何参与完善汉化？（只需修改两个文本文件）

本项目的所有汉化词条都保存在程序同目录下的两个 `.json` 文件中。  
任何人都可以使用 **记事本 (Notepad)**、**VS Code** 或任何文本编辑器打开它们，添加或修改翻译！

**提醒：请尽量不要使用AI翻译完就提交合并请求！有大量应用层面的专有名词是AI无法正确翻译的，需要人工翻译调整**

```text
CS2WorkshopToolsLocalizerCN/
├── qt_translations.json    # 1. 软件界面与菜单翻译字典
└── fgd_translations.json   # 2. 地图实体与属性描述翻译字典
```

### 📝 翻译格式非常简单：
每一行都是标准的 `"英文原词": "中文翻译"` 格式：

```json
{
  "File": "文件",
  "Edit": "编辑",
  "Clipping Tool": "剪切工具",
  "Transform Locked": "变换锁定"
}
```

---

### 1️⃣ `qt_translations.json`（界面、菜单与工具栏）

用于翻译 Hammer 的主界面、顶部菜单、左侧工具栏、右侧属性栏等。

- **💡 快捷键无需手动输入**：  
  如果界面上显示带快捷键的文本（例如 `"Clipping Tool [Shift+X]"` 或 `"Undo (Ctrl+Z)"`），**你只需要翻译基础英文单词即可**：
  ```json
  "Clipping Tool": "剪切工具",
  "Undo": "撤销"
  ```
  启动器内置的汉化引擎会**自动保留并拼接**后面的 `[Shift+X]`、`\tCtrl+S`、`...`、`:` 等后缀，完全不用担心快捷键丢失！

  > 主译者(DramaCa)目前正在处理：Hammer 与 Asset Browser。自行避免重复工作。
---

### 2️⃣ `fgd_translations.json`（实体与属性说明）

用于翻译地图中各类实体、灯光、物理属性、触发器、输入输出 (I/O) 的显示名称与悬停中文说明。

- **常见可翻译内容示例**：
  ```json
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

- **⚠️ 注意事项**：  
  实体的底层代码英文标识符（例如 `targetname`、`angles` 等）工具会自动保护，无需翻译。**请只翻译双引号内部的展示文本与描述说明即可**。
---

## 🔍 遇到修改汉化词条无效？如何获取完整原文字符串

有时候在 `qt_translations.json` 中添加了某个单词的翻译，但进入 Hammer 后发现界面依然显示英文。  
这通常是因为该界面的英文文本**并不是一个孤立的单词**，而是包含在一段更长的完整字符串、格式化占位符、特殊前缀（如 `&` 快捷键前缀、`...` 省略号、`%s` 占位符等）中。如果字典中的英文原文与 DLL 内部存储的原文字符串不完全匹配，翻译就不会生效。

为此，本项目提供了辅助脚本 [`find_dll_strings.py`](./find_dll_strings.py)，用于直接从 CS2 的 DLL 库中扫描并提取匹配指定关键字或正则表达式的**完整原始字符串**。

### 🛠️ 辅助脚本使用指南 (`find_dll_strings.py`)

#### 1. 前提条件
- 确保系统已安装 **Python 3.6+**。

#### 2. 定位需要扫描的 DLL 文件
Hammer 相关的界面与功能字符串通常分布在 CS2 安装目录下的以下二进制文件中：
- `game/bin/win64/tools/*.dll`
- `game/bin/win64/subtools/*.dll`

#### 3. 运行命令示例

在终端 / PowerShell / CMD 中运行如下命令：

- **文件夹批量扫描 / 通配符匹配（支持 `*.dll` 或目录路径）：**
  ```bash
  # 扫描 tools 目录下所有 DLL 文件
  python find_dll_strings.py -f "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\tools\*.dll" -p "Transform"

  # 或者直接传入文件夹路径
  python find_dll_strings.py -f "D:\...\game\bin\win64\tools" -p "Transform"
  ```

- **单文件精确搜索（包含关键字）：**
  ```bash
  python find_dll_strings.py -f "D:\...\tools\hammer.dll" -p "Transform"
  ```

- **忽略大小写 + 正则表达式匹配：**
  ```bash
  # 搜索以 Open 或 Create 开头的界面与操作字符串
  python find_dll_strings.py -f "D:\...\tools\*.dll" -p "^(Open|Create).*" -i
  ```

- **搜索后自动打开文本文件浏览：**
  ```bash
  python find_dll_strings.py -f "D:\...\tools\*.dll" -p "Grid" --open
  ```

#### 4. 参数说明
| 参数 | 说明 |
| :--- | :--- |
| `-f, --file` | 目标 DLL/二进制文件路径，支持通配符（如 `*.dll`、`tools/*.dll`）或文件夹路径 (**必填**) |
| `-p, --pattern` | 要搜索的关键字或正则表达式 (**必填**) |
| `-out, --output` | 输出文本文件路径 (单文件默认: `<DLL名>_matched_strings.txt`，批量默认: `batch_matched_strings.txt`) |
| `-i, --ignore-case`| 忽略大小写进行搜索匹配 |
| `-o, --show-offset`| 输出字符串在 DLL 文件中的 16 进制偏移量与字符编码 (ASCII / UTF-16LE) |
| `-u, --unique` | 去重输出，避免完全相同的重复字符串刷屏 |
| `--open` | 搜索完成后自动使用记事本/默认文本编辑器打开结果文件 |

#### 5. 填入词典生效
在输出的 `.txt` 文本文件中找到对应的完整英文原文后，将完整的原文本复制并填入 `qt_translations.json` 即可成功汉化：
```json
{
  "完整的英文原文字符串": "对应的中文翻译"
}
```

---

### 💡 提交你的翻译

如果你翻译或修正了新的词条，欢迎将修改后的 `qt_translations.json` 或 `fgd_translations.json` 提交 Pull Request，或者在 Issues 中分享，共同完善 CS2 中文地图制作生态！

