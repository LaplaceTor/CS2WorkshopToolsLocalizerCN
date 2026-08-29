# CS2 Hammer 深度汉化启动器 (CS2HammerTranslateCN)

> 专为 **Counter-Strike 2 (CS2)** 地图创作者打造的 Hammer 编辑器一键中文汉化与启动工具。  
> 开箱即用、安全无痕，支持自由修改与扩充汉化词库！

---

## 🌟 软件特色

- **🇨🇳 深度汉化**：汉化覆盖主菜单、工具栏、实体属性面板、视口右键菜单、实体说明及输入输出等各个界面。
- **🛡️ 纯净安全**：启动时自动备份游戏原版文件，**关闭 Hammer 后自动还原所有文件并清理临时补丁**，完全不修改、不污染您的 CS2 游戏目录。
- **🎯 自动识别游戏**：启动时自动寻找您的 CS2 安装目录和已有的 Addon 模组，无需手动复杂配置。
- **⚡ 实时生效**：翻译词条直接保存在文本文件中，修改后无需任何编译，重新启动工具即可立刻看到中文效果！

---

## 🚀 快速使用指南

1. **运行启动器**：解压后双击打开 `CS2HammerTranslateCN.exe`。
2. **选择模组**：在下拉菜单中选择你要编辑的 Addon 模组。
3. **一键启动**：点击 **`🚀 启动 CS2 Hammer (汉化版)`** 按钮。
4. **尽情创作**：进入中文版 Hammer 编辑器进行地图创作；关闭 Hammer 后，启动器会自动帮您恢复所有原版文件。

---

## ✍️ 如何参与完善汉化？（只需修改两个文本文件）

本项目的所有汉化词条都保存在程序同目录下的两个 `.json` 文件中。  
任何人都可以使用 **记事本 (Notepad)**、**VS Code** 或任何文本编辑器打开它们，添加或修改翻译！

```text
CS2HammerTranslateCN/
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

### 💡 提交你的翻译

如果你翻译或修正了新的词条，欢迎将修改后的 `qt_translations.json` 或 `fgd_translations.json` 提交 Pull Request，或者在 Issues 中分享，共同完善 CS2 中文地图制作生态！
