# CS2 Workshop Tools Localizer CN (CS2 创意工坊工具汉化启动器)

> 专为 **Counter-Strike 2 (CS2)** 创意工坊工具集 (Hammer 编辑器、材质编辑器等 Workshop Tools) 打造的一键中文汉化与启动工具。  
> 开箱即用、安全无痕，支持自由修改与扩充汉化词库！

---

## 🌟 软件特色

- **🇨🇳 深度汉化**：汉化覆盖主菜单、工具栏、实体属性面板、视口右键菜单、实体说明及输入输出等各个界面。
- **📦 模块级隔离词典**：采用单文件多子块架构，为 Hammer、ModelDoc、粒子编辑器等每个独立工具提供专属作用域，彻底避免“一词多义”冲突。
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

## ✍️ 如何参与完善汉化？（只需修改三个文本文件）

本项目的所有汉化词条与实体覆盖均保存在程序同目录下的三个 `.json` 文件中：

```text
CS2WorkshopToolsLocalizerCN/
├── qt_translations.json    # 1. 软件界面与菜单翻译字典 (支持模块子块与注释)
├── fgd_translations.json   # 2. 地图实体与属性已有字符串翻译字典 (精确匹配替换)
└── fgd_override.json       # 3. 地图实体键值描述补充与覆盖字典 (针对特定 Key 新增/覆盖说明)
```

> **⚠️ 译者提醒**：
> 1. 请尽量**不要**直接使用未经校对的 AI 翻译批量提交！地图工具中含有大量领域专有名词，需要人工结合上下文核对。
> 2. 所有字典文件均采用标准 JSON / JSONC 格式，支持 `//` 单行注释与 `/* */` 块注释，方便对词条做批注说明。

**当前主译者正在完善HAMMER以及ASSET BROWSER，如有贡献想法，请自行规避**

---

### 1️⃣ `qt_translations.json`（界面、菜单与工具栏多子块编写规范）

`qt_translations.json` 采用了**单文件多子块（Sectioned）**结构。每个子块对应一个工具 DLL 的名称（不含 `.dll` 扩展名）：

```jsonc
{
  // ==============================================================================
  // 1. 通用公共区（所有工具共享的回退词典，顶层基础 UI 项）
  // ==============================================================================
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

  // ==============================================================================
  // 2. Hammer 地图编辑器 (hammer.dll) 专属子块
  // ==============================================================================
  "hammer": {
    "Clipping Tool": "切割工具",
    "Translate Tool": "移动工具",
    "Rotate Tool": "旋转工具",
    "Selection Tool": "选择工具",
    "Undo": "撤销",
    "Redo": "重做",
    "Bone": "骨骼节点"      // 同词在 Hammer 中译为“骨骼节点”
  },

  // ==============================================================================
  // 3. ModelDoc 模型编辑器 (modeldoc_editor.dll) 专属子块
  // ==============================================================================
  "modeldoc_editor": {
    "Open ModelDoc File": "打开ModelDoc文件",
    "Bone": "骨骼"          // 同词在 ModelDoc 中译为“骨骼”
  },

  // ==============================================================================
  // 4. 其他工具或子工具（如 pet.dll, met.dll, sfm.dll, dashboard_subtool 等）
  // ==============================================================================
  "pet": {
    "Units": "单位"
  },
  "met": {
    "Linear": "线性"
  },
  "soundviewer_subtool": {
    "Resume History": "恢复历史",
    "Pause History": "暂停历史"
  }
}
```

#### 💡 翻译编写技巧：
- **同词不同译隔离**：不同工具如果有相同的英文单词但含义不同（如 `"Bone"`），分别写在各自的子块中即可精准区分。
- **自动回退机制**：若某词条在对应工具的专属子块中未找到，汉化引擎会自动回退到 `"common"` 公共区查找。
- **快捷键无需手动输入**：  
  如果界面上显示带快捷键的文本（例如 `"Clipping Tool [Shift+X]"`、`"Undo (Ctrl+Z)"` 或 `"Save\tCtrl+S"`），**你只需要翻译基础英文单词即可**：
  ```json
  "Clipping Tool": "切割工具",
  "Undo": "撤销"
  ```
  内置汉化引擎会在运行时**自动保留并拼接**后面的 `[Shift+X]`、`\tCtrl+S`、`...`、`:` 等后缀！

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

### 3️⃣ `fgd_override.json`（实体键值描述补充与覆盖字典）

用于针对 Valve 原版 FGD 实体定义中**缺失悬停描述**或**需要个性化说明**的特定属性 (Key)、实体类 (Class) 或输入输出 (I/O) 进行描述补充与覆盖。

- **与 `fgd_translations.json` 的区别**：
  - `fgd_translations.json`：根据已有英文原文进行精确匹配翻译（无法给原版无描述的属性补充中文）。
  - `fgd_override.json`：根据属性键名、类名或 I/O 名称直接**新增悬停描述**或**强制替换说明**。

- **格式与配置示例**：
  ```jsonc
  {
    // 1. 全局属性描述补充与覆盖 (按属性名匹配，如 disableshadows, bodygroups, vscripts)
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

    // 3. 实体类说明补充与类作用域专属属性配置
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

---

## 🔍 遇到修改汉化词条无效？如何获取完整原文字符串

有时候在 `qt_translations.json` 中添加了某个单词的翻译，但进入 Hammer 后发现界面依然显示英文。  
这通常是因为该界面的英文文本**并不是一个孤立的单词**，而是包含在一段更长的完整字符串、格式化占位符、特殊前缀（如 `&` 快捷键前缀、`...` 省略号、`%s` 占位符等）中。

本项目提供了辅助脚本 [`find_dll_strings.py`](./find_dll_strings.py)，用于直接从 CS2 的 DLL 库中扫描并提取匹配指定关键字或正则表达式的**完整原始字符串**。

### 🛠️ 辅助脚本使用指南 (`find_dll_strings.py`)

#### 1. 前提条件
- 确保系统已安装 **Python 3.6+**。

#### 2. 运行命令示例

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

#### 3. 参数说明
| 参数 | 说明 |
| :--- | :--- |
| `-f, --file` | 目标 DLL/二进制文件路径，支持通配符（如 `*.dll`、`tools/*.dll`）或文件夹路径 (**必填**) |
| `-p, --pattern` | 要搜索的关键字或正则表达式 (**必填**) |
| `-out, --output` | 输出文本文件路径 (单文件默认: `<DLL名>_matched_strings.txt`，批量默认: `batch_matched_strings.txt`) |
| `-i, --ignore-case`| 忽略大小写进行搜索匹配 |
| `-o, --show-offset`| 输出字符串在 DLL 文件中的 16 进制偏移量与字符编码 (ASCII / UTF-16LE) |
| `-u, --unique` | 去重输出，避免完全相同的重复字符串刷屏 |
| `--open` | 搜索完成后自动使用记事本/默认文本编辑器打开结果文件 |

#### 4. 填入词典生效
在输出的 `.txt` 文本文件中找到对应的完整英文原文后，将完整的原文本复制并填入 `qt_translations.json` 对应模块的子块中即可成功汉化。

---

### 💡 提交你的翻译与覆盖说明

如果你翻译或修正了新的词条，欢迎将修改后的 `qt_translations.json`、`fgd_translations.json` 或 `fgd_override.json` 提交 Pull Request，或者在 Issues 中分享，共同完善 CS2 中文地图与模组制作生态！
