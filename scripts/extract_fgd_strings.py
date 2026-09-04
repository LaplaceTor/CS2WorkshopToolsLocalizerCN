#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CS2WorkshopToolsLocalizerCN - FGD 实体定义描述文本提取工具
用于从 Valve Source 2 / CS2 的 FGD (Forge Game Data) 实体定义文件中提取所有已有的类说明、
属性显示名、悬停描述、输入输出说明、选项标签及工具元数据文本，生成待翻译的 JSON / JSONC 字典模板。

功能特性：
1. 完整解析 FGD 语法：包括 @PointClass, @SolidClass, @BaseClass, @KeyValues, choices, flags, inputs, outputs。
2. 提取 9 大类实体定义文本：
   - 实体类说明 (Class Description)
   - 属性显示名称 (Property Display Name)
   - 属性悬停描述 (Property Description)
   - 输入 / 输出说明 (Input/Output Description)
   - 选项与标记显示名 (Choice Display)
   - 选项附加说明 (Choice Description)
   - 实体工具元数据 (Entity Tool Metadata)
   - 实体分组 (Entity Group)
   - 按钮与组件说明 (Metadata Description)
3. 智能关联与增量比对：自动加载已有翻译字典，保留已翻译项，仅提取新增/未翻译条目。
4. 输出带清晰分类与来源注释的 JSONC 格式或标准 JSON 格式。
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import OrderedDict

# 确保在 Windows 控制台或非 UTF-8 环境下输出正常
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass


# ==============================================================================
# 纯文件路径与资源 URI 识别过滤模块
# ==============================================================================

# 常见文件扩展名集合（不区分大小写）
RESOURCE_EXTENSIONS = {
    # Source 2 / 游戏引擎资源与模型材质
    "vmdl", "vmat", "vpcf", "vsnd", "vmap", "vdata", "vcss", "vjs", "vxml", "vseq", "vpost",
    "vanim", "vmesh", "vphys", "vsmart", "vfont", "vblocks", "vcol", "vtex", "vwrld", "vts",
    "vcon", "vsub", "vcdlist", "vpk", "vcd", "vmt", "vtf", "mdl", "phy", "vtx", "vvd", "bsp",
    "nav", "ain", "nod", "dmx", "smd", "qc", "qci",
    # 代码、脚本与配置文件
    "dll", "exe", "so", "dylib", "pdb", "lib", "obj", "exp", "sys", "drv", "ocx",
    "cpp", "c", "cc", "cxx", "h", "hpp", "hxx", "inl", "py", "pyc", "pyw", "cs", "java", "rs",
    "go", "lua", "vscript", "js", "ts", "json", "jsonc", "xml", "fgd", "txt", "cfg", "ini",
    "bat", "cmd", "ps1", "sh", "inf", "manifest", "yaml", "yml", "toml", "log",
    # 图像、贴图与材质纹理
    "png", "jpg", "jpeg", "tga", "bmp", "svg", "dds", "hdr", "exr", "gif", "ico", "webp",
    "psd", "tif", "tiff",
    # 音频与媒体
    "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "mid", "midi", "avi", "mp4", "webm",
    # 着色器与 GPU 程序
    "hlsl", "glsl", "vfx", "shader", "fxc", "cg", "vert", "frag", "geom", "comp", "spv",
    # 字体
    "ttf", "otf", "woff", "woff2", "eot", "fon",
    # 压缩归档与文档
    "zip", "7z", "tar", "gz", "rar", "pdf", "doc", "docx"
}

# 常见引擎资源 / 系统根目录前缀（不区分大小写）
KNOWN_PATH_PREFIXES = (
    "models/", "materials/", "particles/", "sounds/", "soundevents/", "panorama/", "scripts/",
    "maps/", "resource/", "tools/", "content/", "game/", "core/", "csgo/", "dota/", "hlvr/",
    "steamaudio/", "editor/", "shaders/", "cfg/", "bin/", "src/", "fonts/", "images/", "icons/",
    "prefabs/", "vscripts/", "smartprops/", "surfacemap/", "expressions/", "addoninfo/",
    "import_scripts/", "postprocessing/", "vdata/",
    "models\\", "materials\\", "particles\\", "sounds\\", "soundevents\\", "panorama\\", "scripts\\",
    "maps\\", "resource\\", "tools\\", "content\\", "game\\", "core\\", "csgo\\", "dota\\", "hlvr\\",
    "steamaudio\\", "editor\\", "shaders\\", "cfg\\", "bin\\", "src\\", "fonts\\", "images\\", "icons\\",
    "prefabs\\", "vscripts\\", "smartprops\\", "surfacemap\\", "expressions\\", "addoninfo\\",
    "import_scripts\\", "postprocessing\\", "vdata\\"
)

KNOWN_URL_PREFIXES = (
    "http://", "https://", "ftp://", "file://", "file:///", "qrc:/", "res://", "game:", "tools:", "panorama:"
)

# 常见的 UI 菜单/选项/按钮复合词（保护不被误判为路径）
UI_SLASH_WHITELIST = {
    "import/export", "export/import", "cut/copy/paste", "copy/paste", "undo/redo", "redo/undo",
    "enable/disable", "disable/enable", "true/false", "false/true", "yes/no", "no/yes",
    "on/off", "off/on", "2d/3d", "3d/2d", "pass/fail", "fail/pass", "show/hide", "hide/show",
    "add/remove", "remove/add", "input/output", "inputs/outputs", "in/out", "left/right", "right/left",
    "up/down", "down/up", "min/max", "max/min", "width/height", "height/width", "pitch/yaw/roll",
    "x/y/z", "x/y", "u/v", "s/t", "r/g/b", "r/g/b/a", "ok/cancel", "read/write", "open/close",
    "start/stop", "play/pause", "load/save", "save/load", "lock/unlock", "expand/collapse",
    "all/none", "front/back", "top/bottom"
}


def is_pure_path_string(s: str) -> bool:
    """
    判断字符串是否为纯文件路径、资源 URI、文件名或着色器路径。
    过滤纯路径，同时严格保护诸如 'Import/Export', 'Enable/Disable', 'Undo/Redo' 等 UI 复合词。
    """
    if not s or len(s) < 2:
        return False

    s_clean = s.strip()
    s_lower = s_clean.lower()

    # 1. 保护 UI 斜杠白名单
    if s_lower in UI_SLASH_WHITELIST:
        return False

    # 2. 检查 URL / URI 协议头
    if s_lower.startswith(KNOWN_URL_PREFIXES):
        return True

    # 3. 检查常见引擎资源目录前缀 (如 models/, materials/, tools/ 等)
    if s_lower.startswith(KNOWN_PATH_PREFIXES):
        return True

    # 4. 检查绝对路径 (Windows 盘符如 C:\, D:/ 或 Linux 根路径 /usr/..., UNC路径 \\server\...)
    if re.match(r"^[a-zA-Z]:[/\\]", s_clean) or s_clean.startswith(("//", "\\\\")):
        return True
    if re.match(r"^/[a-zA-Z0-9_\-]+[/\\]", s_clean):
        return True
    if s_clean.startswith(("../", "..\\", "./", ".\\")):
        return True

    # 5. 检查通配符路径 (如 *.vmdl, */*.vmat, models/*)
    if re.match(r"^(?:\*|[a-zA-Z0-9_\-*?]+)[/\\]", s_clean) or re.match(r"^\*\.[a-zA-Z0-9_]+$", s_clean):
        return True

    # 6. 检查是否以已知资源/代码/文件扩展名结尾 (如 foo.vmdl, camera.vmat, helper.cpp)
    last_dot = s_clean.rfind(".")
    if last_dot != -1 and last_dot < len(s_clean) - 1:
        ext = s_clean[last_dot + 1:].lower()
        if ext in RESOURCE_EXTENSIONS:
            # 如果包含斜杠，或者不含空格且为纯文件名（如 camera.vmdl, Qt5Core.dll）
            if "/" in s_clean or "\\" in s_clean or " " not in s_clean:
                return True

    # 7. 检查多级无空格斜杠路径 (如 a/b/c 或 tools/images/...)
    if ("/" in s_clean or "\\" in s_clean) and " " not in s_clean:
        slash_count = s_clean.count("/") + s_clean.count("\\")
        if slash_count >= 2:
            return True
        # 仅 1 个斜杠时，若任一部分像文件/文件夹名 (以小写开头或包含扩展名或下划线)
        parts = re.split(r"[/\\]", s_clean)
        if len(parts) == 2:
            p1, p2 = parts[0], parts[1]
            if (p1 and (p1[0].islower() or "_" in p1 or "." in p1)) or (p2 and (p2[0].islower() or "_" in p2 or "." in p2)):
                return True

    return False


def is_compiler_section_or_symbol(s: str) -> bool:
    """
    判断字符串是否为 PE 节区名称、MSVC 编译器内部符号或重命名/重定位符号。
    - 常见 PE 节区：.text, .rdata, .data, .pdata, .reloc, .idata, .edata, .rsrc, .tls, .00cfg, .CRT, .bss 等
    - 包含 $ 的编译器分组节区：.text$mn, .CRT$XCA, .rdata$zzzdbg 等
    - MSVC C++ 符号修饰 (Mangled Names)：以 ? 或 ?? 开头
    - 纯 Hex 哈希 / GUID
    """
    if not s:
        return False
    s_clean = s.strip()

    # 1. MSVC C++ 符号修饰 (Mangled Symbol)
    if s_clean.startswith("?") or s_clean.startswith("__"):
        return True

    # 2. 包含 $ 且无空格的编译器内部节区/修饰名
    if "$" in s_clean and " " not in s_clean:
        return True

    # 3. 常见 PE 节区前缀/全名 (如 .text, .rdata, .data, .pdata, .00cfg, .CRT 等)
    if s_clean.startswith(".") and " " not in s_clean:
        known_sections = (
            ".text", ".rdata", ".data", ".pdata", ".reloc", ".idata", ".edata",
            ".rsrc", ".tls", ".00cfg", ".CRT", ".bss", ".gfids", ".giats",
            ".didat", ".xdata", ".cdata", ".stab", ".stabstr"
        )
        s_lower = s_clean.lower()
        if any(s_lower.startswith(sec) for sec in known_sections):
            return True

    # 4. GUID 格式字符串 (如 {12345678-ABCD-1234-ABCD-123456789ABC})
    if re.match(r"^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$", s_clean):
        return True

    # 5. 纯长十六进制 Hash (32+ 位)
    if len(s_clean) >= 32 and re.match(r"^[0-9a-fA-F]+$", s_clean):
        return True

    return False


def is_camel_case_identifier(s: str) -> bool:
    """
    判断单个无空格字符串是否为多驼峰命名的代码函数、类名、变量或接口版本标识符。
    例如：
    - InitializeCriticalSectionEx, GetSystemTimeAsFileTime, ResourceSystem013, SoundOpSystem
    - setToolTip, onButtonClicked, isEnabled, getItemData
    - CBaseEntity, QWidget, QAbstractItemModel, IVEngineClient2
    保护常规单个单词（如 "Translate", "Wireframe", "Workplane", "Normal", "Grid", "Vertex"）
    以及带空格的常规描述词。
    """
    if not s or " " in s:
        return False
    s_clean = s.strip()

    # 包含中文或空格的不属于代码标识符
    if re.search(r"[\u4e00-\u9fa5\s]", s_clean):
        return False

    # 1. 小驼峰命名 (camelCase): 开头小写字母，后续包含至少一个大写字母 (如 setToolTip, isRunning, onTrigger)
    if re.match(r"^[a-z]+[A-Z][a-zA-Z0-9]*$", s_clean):
        return True

    # 2. Qt / Valve 类名前缀: Q + 大写开头 (QWidget, QAction), C + 大写开头 (CBaseEntity), I + 大写开头 (IVEngineClient)
    if re.match(r"^[QCI][A-Z][a-z]+[a-zA-Z0-9]*$", s_clean):
        return True

    # 3. 大驼峰命名 (PascalCase) 包含 2 个及以上词根 (如 InitializeCriticalSection, ResourceSystem, SoundOpSystem)
    words = re.findall(r"[A-Z][a-z]+", s_clean)
    if len(words) >= 2:
        return True

    # 4. Valve 接口版本字符串 (如 VFileSystem017, Source2Engine002, HammerMapLoader001)
    if re.match(r"^[A-Z][a-zA-Z]*[0-9]{3,}$", s_clean):
        return True

    # 5. 全大写缩写后接驼峰/单词 (如 JSONParser, D3DDevice, GLTexture, XMLNode)
    if re.match(r"^[A-Z]{2,}[a-z]+", s_clean):
        return True

    return False


def is_code_identifier_or_constant(s: str) -> bool:
    """
    判断字符串是否为代码标识符、技术常量、枚举值、多驼峰函数名、纯数字或数字符号组合。
    - 过滤所有不包含字母或汉字的纯数字/纯符号/坐标向量（如 "16.0 16.0 0.15", "255 255 255", "[0, 0, 0]", "31", "-1"）
    - 过滤所有包含下划线且不含空格的单个标识符（如 HITGROUP_LEFT_LOWER_LEG, m_iszEntity, ACT_IDLE, cl_crosshaircolor）
    - 过滤多驼峰函数、类名、接口版本（如 InitializeCriticalSectionEx, ResourceSystem013, setToolTip, CBaseEntity）
    - 过滤 PE 节区与编译器内部符号（如 .text$mn, .CRT$XCA, .rdata, ?setToolTip）
    - 过滤分辨率与尺寸表达式（如 "1920x1080", "128x128", "16 x 16"）
    - 过滤带单位的纯数值（如 "100px", "60fps", "50ms", "0deg", "1.0f"）
    - 过滤纯数值范围与数值比例（如 "0..1", "0-255", "-1~1"）
    - 过滤纯版本号（如 "1.0.0.0"）
    """
    if not s:
        return False
    s_clean = s.strip()

    # 1. 必须包含至少一个英文字母或汉字字符（彻底过滤纯数字、纯标点、纯符号、3D向量坐标等）
    if not re.search(r"[a-zA-Z\u4e00-\u9fa5]", s_clean):
        return True

    # 2. 包含下划线且不含空格的单词（枚举常量、变量名、宏、函数符号等）
    if "_" in s_clean and " " not in s_clean:
        return True

    # 3. 编译器节区、符号修饰与内部符号
    if is_compiler_section_or_symbol(s_clean):
        return True

    # 4. 多驼峰函数名、类名、接口版本号标识符
    if is_camel_case_identifier(s_clean):
        return True

    # 5. 纯分辨率与尺寸乘积表达式 (如 1920x1080, 256x256, 16 x 16, 512*512)
    if re.match(r"^[0-9]+(?:\.[0-9]+)?\s*[xX*×]\s*[0-9]+(?:\.[0-9]+)?(?:\s*[xX*×]\s*[0-9]+(?:\.[0-9]+)?)?$", s_clean):
        return True

    # 6. 带单位的纯数值或字面量 (如 100px, 60fps, 50ms, 128MB, 0deg, 1.0f)
    if re.match(r"^[+-]?[0-9]+(?:\.[0-9]+)?\s*(?:px|pt|em|rem|ms|s|fps|hz|khz|mhz|ghz|kb|mb|gb|tb|deg|rad|f|d|u|l|ll|ull|ui)$", s_clean, re.IGNORECASE):
        return True

    # 7. 纯数值范围与比例表达式 (如 0..1, 0-255, -1~1, 1:1)
    if re.match(r"^[+-]?[0-9]+(?:\.[0-9]+)?\s*(?:-|~|\.\.|\/|:)\s*[+-]?[0-9]+(?:\.[0-9]+)?$", s_clean):
        return True

    # 8. 纯版本号 (如 1.0.0, 2.1.4.0)
    if re.match(r"^[0-9]+(?:\.[0-9]+){2,}$", s_clean):
        return True

    # 9. 包含 C++ 作用域解析运算符 (::) 的内部类、方法调用、模块日志或断言表达式 (如 TheCode::NoCode, FloatBitMap_t::WriteToBuffer, QAction::setText, std::vector)
    if "::" in s_clean:
        return True

    # 10. C/C++ 结构体指针箭头操作符 (如 pNode->GetParent)
    if "->" in s_clean and " " not in s_clean:
        return True

    return False


def parse_quoted_string_at(text: str, start_pos: int):
    """
    从 start_pos（指向开头的双引号 "）开始解析一个完整的双引号字符串，
    正确处理内部的转义字符如 \\", \\\\, \\n 等。
    返回 (decoded_str, end_pos)；若无法匹配则返回 (None, start_pos)。
    """
    n = len(text)
    if start_pos >= n or text[start_pos] != '"':
        return None, start_pos

    i = start_pos + 1
    chars = []
    while i < n:
        if text[i] == '\\' and i + 1 < n:
            chars.append(text[i:i + 2])
            i += 2
        elif text[i] == '"':
            i += 1
            raw = "".join(chars)
            try:
                decoded = json.loads(f'"{raw}"')
            except Exception:
                decoded = (
                    raw.replace('\\"', '"')
                    .replace('\\\\', '\\')
                    .replace('\\n', '\n')
                    .replace('\\t', '\t')
                    .replace("\\'", "'")
                )
            return decoded, i
        else:
            chars.append(text[i])
            i += 1
    return None, start_pos


def strip_line_comment(line: str) -> str:
    """
    移除行内的 // 注释，但保留引号内的 //。
    """
    in_q = False
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '\\' and in_q and i + 1 < n:
            i += 2
            continue
        elif c == '"':
            in_q = not in_q
            i += 1
        elif c == '/' and not in_q and i + 1 < n and line[i + 1] == '/':
            return line[:i]
        else:
            i += 1
    return line


def extract_property_header(code: str):
    """
    解析类似 prop_name(prop_type) [optional metadata] {optional metadata} 的头部。
    """
    i = 0
    n = len(code)
    while i < n and code[i] in ' \t':
        i += 1
    if i >= n or code[i] in '@/:':
        return None

    key_start = i
    while i < n and (code[i].isalnum() or code[i] in '_.-'):
        i += 1
    if i == key_start:
        return None
    prop_key = code[key_start:i]

    while i < n and code[i] in ' \t':
        i += 1
    if i >= n or code[i] != '(':
        return None
    i += 1
    type_start = i
    while i < n and code[i] != ')':
        i += 1
    if i >= n or code[i] != ')':
        return None
    prop_type = code[type_start:i]
    i += 1

    # bracket/brace parsing (KV3 metadata)
    while i < n:
        while i < n and code[i] in ' \t':
            i += 1
        if i >= n:
            break
        if code[i] == '[':
            depth = 1
            i += 1
            while i < n and depth > 0:
                if code[i] == '"':
                    _, next_i = parse_quoted_string_at(code, i)
                    i = next_i
                elif code[i] == '[':
                    depth += 1
                    i += 1
                elif code[i] == ']':
                    depth -= 1
                    i += 1
                else:
                    i += 1
            if depth != 0:
                return None
        elif code[i] == '{':
            depth = 1
            i += 1
            while i < n and depth > 0:
                if code[i] == '"':
                    _, next_i = parse_quoted_string_at(code, i)
                    i = next_i
                elif code[i] == '{':
                    depth += 1
                    i += 1
                elif code[i] == '}':
                    depth -= 1
                    i += 1
                else:
                    i += 1
            if depth != 0:
                return None
        else:
            break

    prop_head = code[:i]
    rest = code[i:]
    return prop_head, prop_key, prop_type, rest


def extract_from_fgd_file(filepath: str):
    """
    从单个 FGD 文件中提取所有描述文本。
    返回列表: [(text, category, source_info), ...]
    """
    entries = []
    base_name = os.path.basename(filepath)

    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"[-] 无法读取文件 {filepath}: {e}", file=sys.stderr)
        return entries

    full_text = "".join(lines)
    n = len(full_text)
    i = 0
    current_line = 1

    while i < n:
        c = full_text[i]

        if c == '\n':
            current_line += 1
            i += 1
            continue

        # 注释跳过
        if c == '/' and i + 1 < n and full_text[i + 1] == '/':
            i += 2
            while i < n and full_text[i] != '\n':
                i += 1
            continue

        # 匹配类定义指令 @ClassType
        if c == '@':
            dir_start = i
            i += 1
            while i < n and full_text[i].isalnum():
                i += 1
            directive = full_text[dir_start:i]

            # 跳过指令头部至等号（如 editormodel("models/...") 等辅助元数据不作为类说明）
            while i < n and full_text[i] != '=':
                if full_text[i] == '\n':
                    current_line += 1
                i += 1

            if i < n and full_text[i] == '=':
                i += 1
                # 寻找类说明与主体块 [...]
                while i < n and full_text[i] != '[':
                    if full_text[i] == '\n':
                        current_line += 1
                    elif full_text[i] == '"':
                        desc_text, next_i = parse_quoted_string_at(full_text, i)
                        if desc_text and desc_text.strip() and not is_pure_path_string(desc_text) and not is_code_identifier_or_constant(desc_text):
                            entries.append((desc_text, "Class Description", f"{base_name}:{current_line}"))
                        i = next_i
                        continue
                    i += 1

                if i < n and full_text[i] == '[':
                    # 进入类属性主体块
                    i += 1
                    depth = 1
                    while i < n and depth > 0:
                        ch = full_text[i]
                        if ch == '\n':
                            current_line += 1
                            i += 1
                            continue
                        elif ch == '/' and i + 1 < n and full_text[i + 1] == '/':
                            i += 2
                            while i < n and full_text[i] != '\n':
                                i += 1
                            continue
                        elif ch == '"':
                            desc_text, next_i = parse_quoted_string_at(full_text, i)
                            if desc_text and desc_text.strip() and not is_pure_path_string(desc_text) and not is_code_identifier_or_constant(desc_text):
                                entries.append((desc_text, "Property Description", f"{base_name}:{current_line}"))
                            i = next_i
                            continue
                        elif ch == '[':
                            depth += 1
                        elif ch == ']':
                            depth -= 1
                        i += 1
            continue

        # 常规引号文本扫描
        if c == '"':
            desc_text, next_i = parse_quoted_string_at(full_text, i)
            if desc_text and desc_text.strip() and not is_pure_path_string(desc_text) and not is_code_identifier_or_constant(desc_text):
                entries.append((desc_text, "Metadata Description", f"{base_name}:{current_line}"))
            i = next_i
            continue

        i += 1

    return entries


def collect_fgd_files(input_path: str):
    """递归搜集目标目录或文件下的所有 .fgd 文件。"""
    if os.path.isfile(input_path):
        return [os.path.abspath(input_path)]

    if os.path.isdir(input_path):
        matches = glob.glob(os.path.join(input_path, "**", "*.fgd"), recursive=True)
        return sorted([os.path.abspath(p) for p in matches])

    # 尝试在项目标准目录下检索
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    search_dirs = [
        os.path.join(project_root, "backup"),
        os.path.join(project_root, "backup", "game"),
        project_root,
        os.getcwd()
    ]

    for s_dir in search_dirs:
        if os.path.isdir(s_dir):
            matches = glob.glob(os.path.join(s_dir, "**", "*.fgd"), recursive=True)
            if matches:
                return sorted([os.path.abspath(p) for p in matches])

    return []


def strip_jsonc_comments(text: str) -> str:
    """去除 JSONC 中的 // 注释与 /* */ 注释，同时保留字符串字面量内容与换行。"""
    res = []
    i = 0
    n = len(text)
    in_str = False
    escape = False

    while i < n:
        c = text[i]
        if in_str:
            res.append(c)
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == '"':
                in_str = False
            i += 1
            continue

        if c == '"':
            in_str = True
            res.append(c)
            i += 1
            continue

        if c == '/' and i + 1 < n:
            next_c = text[i + 1]
            if next_c == '/':  # 单行注释
                i += 2
                while i < n and text[i] != '\n':
                    i += 1
                continue
            elif next_c == '*':  # 多行注释
                i += 2
                while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                    if text[i] == '\n':
                        res.append('\n')
                    i += 1
                i += 2  # 跳过 */
                continue

        res.append(c)
        i += 1

    cleaned = "".join(res)
    # 去除尾随逗号 (trailing commas)
    cleaned = re.sub(r',\s*([\}\]])', r'\1', cleaned)
    return cleaned


def load_existing_translations(dict_path: str) -> dict:
    """加载已存在的 JSONC / JSON 翻译字典。"""
    if not dict_path:
        return {}

    base, ext = os.path.splitext(dict_path)
    base_name = os.path.basename(base)

    candidates = [
        dict_path,
        base + ".jsonc",
        base + ".json",
        os.path.join("translations", os.path.basename(dict_path)),
        os.path.join("translations", base_name + ".jsonc"),
        os.path.join("translations", base_name + ".json"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "translations", os.path.basename(dict_path)),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "translations", base_name + ".jsonc"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", os.path.basename(dict_path))
    ]

    for p in candidates:
        if os.path.isfile(p):
            try:
                with open(p, 'r', encoding='utf-8') as f:
                    content = f.read()
                cleaned = strip_jsonc_comments(content)
                data = json.loads(cleaned)
                if isinstance(data, dict):
                    return data
            except Exception:
                pass
    return {}


def generate_jsonc_content(ordered_categories: OrderedDict, existing_translations: dict, include_sources: bool = False) -> str:
    """生成带分类与说明注释的 JSONC 字典文本。"""
    lines = []
    lines.append("{")
    lines.append("  // ==============================================================================")
    lines.append("  // CS2 Workshop Tools FGD 实体定义待翻译字典 (fgd_extracted.jsonc)")
    lines.append("  // 自动从游戏原版 FGD 文件中提取的描述与显示文本")
    lines.append("  // ==============================================================================")
    lines.append("  //")
    lines.append("  // 【译制提示】")
    lines.append("  // - 格式为标准键值对: \"英文原文\": \"中文译文\"")
    lines.append("  // - 未翻译项的值保持为空字符串 \"\"，翻译完成后可填入对应译文。")
    lines.append("  // - 翻译完成后可直接合并至 fgd_translations.jsonc 中使用。")
    lines.append("  // ==============================================================================")
    lines.append("")

    category_titles = {
        "Class Description": "1. 实体类说明 (Entity Class Descriptions)",
        "Property Display Name": "2. 属性显示名称 (Property Display Names)",
        "Property Description": "3. 属性悬停描述 (Property Hover Descriptions)",
        "Input/Output Description": "4. 输入 / 输出说明 (Input / Output Descriptions)",
        "Choice Display": "5. 选项与标记显示名 (Choices & Flags Labels)",
        "Choice Description": "6. 选项附加说明 (Choice Extra Descriptions)",
        "Entity Tool Metadata": "7. 实体工具元数据 (Tool Names & Tooltips)",
        "Entity Group": "8. 实体分组 (Entity Groups)",
        "Metadata Description": "9. 按钮与组件说明 (Button & Metadata Descriptions)"
    }

    for cat_name, items in ordered_categories.items():
        if not items:
            continue
        title = category_titles.get(cat_name, cat_name)
        lines.append(f"  // ------------------------------------------------------------------------------")
        lines.append(f"  // {title} (共 {len(items)} 条)")
        lines.append(f"  // ------------------------------------------------------------------------------")
        lines.append("")

        for text, meta in items:
            trans_val = existing_translations.get(text, "")
            key_json = json.dumps(text, ensure_ascii=False)
            val_json = json.dumps(trans_val, ensure_ascii=False)

            if include_sources and meta.get("sources"):
                src_summary = meta["sources"][0]
                if len(meta["sources"]) > 1:
                    src_summary += f" (+{len(meta['sources']) - 1} more)"
                lines.append(f"  // 来源: {src_summary}")

            lines.append(f"  {key_json}: {val_json},")
        lines.append("")

    # 去除最后一个有效键值对的逗号
    for idx in range(len(lines) - 1, -1, -1):
        if lines[idx].endswith(","):
            lines[idx] = lines[idx][:-1]
            break

    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="从 FGD 文件中提取已有描述类文本至 JSON/JSONC 文件以供本地化翻译",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例用法:
  python scripts/extract_fgd_strings.py
  python scripts/extract_fgd_strings.py -i backup/game -o fgd_extracted.jsonc -d translations/fgd_translations.jsonc
  python scripts/extract_fgd_strings.py --include-sources
"""
    )
    parser.add_argument("-i", "--input", default="backup", help="输入 FGD 文件路径或目录 (默认: backup)")
    parser.add_argument("-o", "--output", default="fgd_extracted.jsonc", help="输出 JSONC 文件路径 (默认: fgd_extracted.jsonc)")
    parser.add_argument("-d", "--dict", default="translations/fgd_translations.jsonc", help="已有翻译字典文件路径 (默认: translations/fgd_translations.jsonc)")
    parser.add_argument("--format", choices=["jsonc", "json", "detailed"], default="jsonc", help="输出格式: jsonc (带注释分类), json (标准JSON), detailed (带详细元数据)")
    parser.add_argument("--include-sources", action="store_true", help="在 JSONC 中为每条词条输出来源文件与行号注释")
    parser.add_argument("--check-dup", action="store_true", default=True, help="生成后自动调用查重检测器")

    args = parser.parse_args()

    print("==================================================")
    print("      CS2 FGD 实体定义描述文本提取器")
    print("==================================================")

    # 1. 搜集 FGD 文件
    fgd_files = collect_fgd_files(args.input)
    if not fgd_files:
        print(f"[ERROR] 未找到任何 FGD 文件，请检查输入路径: {args.input}")
        sys.exit(1)

    print(f"[*] 找到 {len(fgd_files)} 个 FGD 文件待解析:")
    for f in fgd_files:
        rel = os.path.relpath(f).replace("\\", "/")
        print(f"    - {rel}")
    print("--------------------------------------------------")

    # 2. 逐文件提取
    raw_entries = []
    for fpath in fgd_files:
        entries = extract_from_fgd_file(fpath)
        raw_entries.extend(entries)

    print(f"[+] 提取完成！共解析出 {len(raw_entries):,} 处文本项。")

    # 3. 按分类进行去重与聚合
    category_order = [
        "Class Description",
        "Property Display Name",
        "Property Description",
        "Input/Output Description",
        "Choice Display",
        "Choice Description",
        "Entity Tool Metadata",
        "Entity Group",
        "Metadata Description"
    ]

    seen_texts = set()
    ordered_categories = OrderedDict()
    for cat in category_order:
        ordered_categories[cat] = []

    text_meta_map = {}

    for text, cat, src in raw_entries:
        if text not in text_meta_map:
            text_meta_map[text] = {"primary_category": cat, "categories": set(), "sources": []}
        text_meta_map[text]["categories"].add(cat)
        text_meta_map[text]["sources"].append(src)

        if text not in seen_texts:
            seen_texts.add(text)
            if cat not in ordered_categories:
                ordered_categories[cat] = []
            ordered_categories[cat].append((text, text_meta_map[text]))

    total_unique = len(seen_texts)
    print(f"[+] 全局去重后获得 {total_unique:,} 条唯一定义文本。")
    print("各类别词条统计:")
    for cat in category_order:
        count = len(ordered_categories.get(cat, []))
        if count > 0:
            print(f"    - {cat:<26}: {count:>5} 条")
    print("--------------------------------------------------")

    # 4. 加载已有翻译
    existing_dict = load_existing_translations(args.dict)
    if existing_dict:
        matched_trans = sum(1 for t in seen_texts if t in existing_dict and isinstance(existing_dict[t], str) and existing_dict[t].strip())
        print(f"[*] 从已有字典 [{args.dict}] 中匹配到 {matched_trans} / {total_unique} 条已翻译文本。")
    else:
        print(f"[*] 未找到或未指定已有字典，所有词条将初始化为待翻译状态。")

    # 5. 生成输出文件
    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    if args.format == "jsonc":
        content = generate_jsonc_content(ordered_categories, existing_dict, include_sources=args.include_sources)
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(content)
    elif args.format == "json":
        flat_dict = OrderedDict()
        for cat, items in ordered_categories.items():
            for text, meta in items:
                flat_dict[text] = existing_dict.get(text, "")
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(flat_dict, f, ensure_ascii=False, indent=2)
    elif args.format == "detailed":
        detailed_dict = OrderedDict()
        for cat, items in ordered_categories.items():
            for text, meta in items:
                detailed_dict[text] = {
                    "translation": existing_dict.get(text, ""),
                    "primary_category": meta["primary_category"],
                    "categories": list(meta["categories"]),
                    "occurrences": len(meta["sources"]),
                    "sources": meta["sources"]
                }
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(detailed_dict, f, ensure_ascii=False, indent=2)

    rel_out = os.path.relpath(args.output).replace("\\", "/")
    print(f"[SUCCESS] 提取字典已成功输出至: {rel_out}")

    # 6. 查重自检
    if args.check_dup:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        dup_script = os.path.join(script_dir, "check_duplicates.py")
        if os.path.exists(dup_script):
            print("--------------------------------------------------")
            print("[*] 正在执行翻译查重自检...")
            res = subprocess.run(
                [sys.executable, dup_script, args.output],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace"
            )
            print(res.stdout)
            if res.returncode != 0:
                print(res.stderr)
                print("[!] 查重自检警告：发现潜在重复项，请检查。")
            else:
                print("[+] 查重自检通过！输出文件格式完全合规且无重复 Key。")

    print("==================================================")


if __name__ == "__main__":
    main()
