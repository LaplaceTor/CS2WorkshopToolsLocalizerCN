#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CS2WorkshopToolsLocalizerCN - 二进制 / DLL 字符串智能提取与去噪工具
用于从 CS2 / Hammer 及各类 Windows PE (DLL/EXE) 二进制与资源文件中提取待汉化字符串，生成 JSON / TXT 词典模板。

设计理念与特性：
1. 宽进严出，保障完整性：由于汉化翻译已交由具备大上下文和专业术语理解能力的大模型处理，
   本脚本不使用狭隘的高风险白名单，全面保留短语、单手势缩写、状态提示、报错与选项文本。
2. 基础噪音与汇编杂质清洗：
   - 过滤不可打印控制字符与二进制乱码片段。
   - 过滤纯标点、纯符号与无文字的分隔线（保留菜单省略号 "..." 与 "…"）。
   - 过滤 MSVC 编译器修饰符、虚表符号（`vftable' 等）、RTTI 描述符（.?AV 等）与 ABI 关键字（__stdcall 等）。
   - 过滤 PE 导入表系统前缀（api-ms-win-...、ext-ms-win-...）。
   - 过滤内存填充对齐字节（如 AAAAAA 等连续重复字符）、纯 16 进制内存指针与纯 GUID。
3. 全局 / 单文件有序去重：保持首次发现顺序，输出标准的 {"原文": ""} 字典格式。
4. 环境泛化：支持直接传参、拖拽路径、通配符（*.dll）批量扫描与目录递归。
"""

import argparse
import glob
import json
import os
import platform
import re
import struct
import subprocess
import sys

# 确保在各平台控制台下标准输出为 UTF-8 编码
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass


def clean_path_input(path_str: str) -> str:
    """清理用户输入或拖拽产生的引号与空白。"""
    if not path_str:
        return ""
    path_str = path_str.strip()
    if (path_str.startswith('"') and path_str.endswith('"')) or (
        path_str.startswith("'") and path_str.endswith("'")
    ):
        path_str = path_str[1:-1].strip()
    return path_str


def get_pe_data_sections(filepath: str, all_sections: bool = False):
    """
    解析 PE (DLL/EXE) 文件节区。
    默认只提取只读数据与资源节区（.rdata, .data, .rsrc 等），
    自动跳过 .text 代码节区（消除汇编指令假象 ASCII）和 .pdata/.reloc 表。
    如果不是 PE 文件或指定 all_sections=True，则返回整文件原始数据。
    """
    try:
        with open(filepath, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"[-] 读取文件失败 ({filepath}): {e}", file=sys.stderr)
        return None

    if all_sections or len(data) < 64 or data[:2] != b"MZ":
        return [(0, "FILE", data)]

    try:
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if (
            e_lfanew + 24 > len(data)
            or data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00"
        ):
            return [(0, "FILE", data)]

        num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
        opt_header_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
        section_offset = e_lfanew + 24 + opt_header_size

        sections = []
        for i in range(num_sections):
            s_off = section_offset + i * 40
            if s_off + 40 > len(data):
                break
            name = (
                data[s_off : s_off + 8]
                .rstrip(b"\x00")
                .decode("latin1", errors="ignore")
            )
            raw_size, raw_ptr = struct.unpack_from("<II", data, s_off + 16)
            characteristics = struct.unpack_from("<I", data, s_off + 36)[0]

            is_executable = (characteristics & 0x20000000) != 0
            is_code = (characteristics & 0x00000020) != 0
            is_metadata_only = name.lower() in (
                ".pdata",
                ".reloc",
                ".tls",
                ".gfids",
            )

            # 仅保留非可执行、非代码的数据节区
            if (
                not is_executable
                and not is_code
                and not is_metadata_only
                and raw_size > 0
            ):
                if raw_ptr + raw_size <= len(data):
                    sec_chunk = data[raw_ptr : raw_ptr + raw_size]
                    sections.append((raw_ptr, name, sec_chunk))

        if sections:
            return sections
    except Exception:
        pass

    return [(0, "FILE", data)]


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


# ==============================================================================
# 基础去噪过滤模块（清理机器垃圾、汇编噪音、纯符号杂质与纯文件路径）
# ==============================================================================

# 常见合法短词与技术/UI 缩写词库 (长度 <= 4)
KNOWN_SHORT_WORDS = {
    # 2 字符
    "ok", "no", "up", "to", "in", "on", "by", "at", "or", "2d", "3d", "id", "uv", "ui", "ai", "ik",
    "fx", "vr", "ar", "go", "if", "is", "it", "me", "my", "we", "he", "so", "do", "as", "an", "am",
    # 3 字符
    "add", "all", "and", "any", "arc", "arm", "art", "ask", "bad", "bag", "bar", "bat", "bed", "beg",
    "bet", "bid", "big", "bin", "bit", "box", "boy", "bug", "bus", "but", "buy", "cam", "can", "cap",
    "car", "cat", "cfg", "cli", "cmd", "cog", "col", "con", "cop", "cpu", "cue", "cup", "cut", "day",
    "dds", "dec", "del", "dev", "die", "dig", "dim", "dir", "dmx", "doc", "dot", "dry", "due", "duo",
    "dye", "ear", "eat", "end", "era", "err", "eye", "fan", "far", "fat", "fax", "fee", "few", "fig",
    "fit", "fix", "fly", "fog", "for", "fov", "fps", "fun", "gap", "gas", "gem", "geo", "get", "gif",
    "giz", "gls", "gpu", "gui", "gum", "gun", "gut", "guy", "gym", "had", "has", "hat", "hdr", "hex",
    "hid", "hip", "hit", "hot", "how", "hsv", "hub", "hud", "hue", "hug", "hut", "ice", "ico", "ill",
    "ini", "ink", "inn", "ion", "jam", "jar", "jaw", "jay", "jet", "job", "jog", "joy", "key", "kid",
    "kin", "kit", "lab", "lap", "law", "lay", "led", "leg", "let", "lid", "lie", "lip", "lit", "lod",
    "log", "lot", "low", "lua", "mac", "mad", "man", "map", "mat", "max", "may", "mid", "min", "mix",
    "mob", "mod", "mop", "mp3", "mp4", "mud", "mug", "nav", "net", "new", "nil", "nod", "non", "nor",
    "not", "now", "num", "nut", "oak", "oar", "odd", "off", "oil", "old", "one", "opt", "orb", "ore",
    "our", "out", "owl", "own", "pad", "pan", "par", "pas", "pat", "paw", "pay", "pea", "peg", "pen",
    "per", "pet", "phy", "pie", "pig", "pin", "pit", "ply", "png", "pod", "pop", "pos", "pot", "pre",
    "pro", "pub", "pun", "pup", "put", "rad", "rag", "ram", "ran", "raw", "ray", "red", "ref", "res",
    "rgb", "rib", "rid", "rig", "rim", "rip", "rob", "rod", "rot", "row", "rub", "rug", "run", "sad",
    "sap", "sat", "saw", "say", "sdk", "sea", "see", "set", "sew", "sin", "sip", "sir", "sit", "six",
    "sky", "smd", "son", "spy", "sub", "sum", "sun", "svg", "tab", "tag", "tan", "tap", "tar", "tax",
    "tea", "ten", "tex", "tga", "the", "tic", "tie", "tin", "tip", "toe", "ton", "too", "top", "toy",
    "try", "tub", "two", "url", "uri", "use", "val", "van", "var", "vcd", "vec", "vex", "vfx", "via",
    "vis", "vmd", "vmt", "vpk", "vtf", "vox", "war", "was", "wav", "wax", "way", "web", "wet", "who",
    "why", "wig", "win", "xml", "xyz", "yam", "yap", "yaw", "yes", "yet", "zip", "zoo",
    # 4 字符
    "edit", "file", "view", "help", "save", "open", "load", "copy", "paste", "undo", "redo", "find",
    "next", "prev", "back", "quit", "exit", "show", "hide", "lock", "move", "zoom", "flip", "snap",
    "clip", "face", "edge", "vert", "mesh", "node", "root", "path", "bone", "skin", "pose", "anim",
    "step", "play", "stop", "loop", "mute", "solo", "size", "font", "type", "name", "date", "time",
    "mode", "tool", "info", "warn", "fail", "pass", "send", "post", "sync", "push", "pull", "code",
    "data", "text", "line", "tree", "list", "grid", "cube", "cone", "arch", "quad", "disk", "ring",
    "tube", "pipe", "star", "null", "none", "auto", "both", "left", "right", "high", "rgba", "srgb",
    "jpeg", "true", "false", "done", "apply", "reset", "clear", "about", "group", "layer", "scale",
    "pivot", "alpha", "color", "sound", "light", "model", "shape", "world", "space", "local", "align",
    "order", "index", "value", "state", "level", "speed", "delay", "range", "limit", "count", "total",
    "start", "pause", "track", "audio", "video", "image", "brush", "solid", "point", "curve", "patch",
    "joint", "input", "event", "param", "blend", "paint", "sculpt", "deform", "smooth", "weight",
    "normal", "tangent", "radius", "offset", "center", "target", "source", "parent", "child", "select",
    "filter", "search", "browse", "create", "delete", "remove", "insert", "attach", "detach", "import",
    "export", "render", "bake", "build", "compile", "verify", "update", "modify", "change", "enable",
    "toggle", "switch", "invert", "mirror", "bridge", "bevel", "extrude", "slice", "weld", "split",
    "stitch", "relax", "thicken", "flatten", "subdiv", "hollow", "carve", "lathe", "revolve", "sweep",
    "loft", "boolean", "union", "intersect", "subtract", "mask", "poly", "hull", "room", "hall",
    "door", "gate", "wall", "roof", "step", "stair", "ramp", "desk", "prop", "seat", "lamp", "glow",
    "beam", "drop", "rain", "snow", "wind", "fire", "heat", "cold", "warm", "dark", "deep", "soft",
    "hard", "fast", "slow", "wide", "tall", "thin", "flat", "bold", "fade", "blur", "glow", "cast",
    "fill", "draw", "plot", "test", "demo", "game", "play", "user", "host", "peer", "port", "sock",
    "byte", "word", "long", "real", "bool", "char", "utf8", "guid", "uuid", "dump", "hook", "link"
}


def is_clean_extracted_string(s: str, min_length: int = 3) -> bool:
    """
    轻量去噪判断：过滤无意义机器垃圾、汇编噪音、编译器杂质、纯符号、纯文件路径、短乱码与下划线标识符。
    保留所有合法 UI、实体属性、选项值、报错信息及技术字符串。
    """
    s_stripped = s.strip()
    if len(s_stripped) < min_length or len(s_stripped) > 1000:
        return False

    # 1. 允许标准菜单省略号与特定操作符
    if s_stripped in ("...", "…", "+/-"):
        return True

    # 2. 必须包含至少一个英文字母或汉字字符（剔除纯符号、纯标点、纯数字杂质）
    if not re.search(r"[a-zA-Z\u4e00-\u9fa5]", s_stripped):
        return False

    # 3. 剔除控制字符与不可打印乱码字节
    if re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", s_stripped):
        return False

    # 4. 如果包含控制字符 (\t, \r, \n) 但总长度 <= 15 或无空格，必定是二进制对齐噪音
    if re.search(r"[\t\r\n]", s_stripped):
        if len(s_stripped) <= 15 or " " not in s_stripped:
            return False

    # 5. 严格过滤 2-4 字符的短二进制碎片 (如 ts[, 8\tZ, D!Z, VOZ, *s[, 4uZ, j2 等)
    if len(s_stripped) <= 4:
        s_lower = s_stripped.lower()
        if s_lower not in KNOWN_SHORT_WORDS:
            # 如果包含非字母数字或空格/中划线以外的任何符号，必定是机器碎片
            if re.search(r"[^a-zA-Z0-9\s\-]", s_stripped):
                return False
            # 如果混合了数字与字母 (如 8BZ, Z1Z, 4uZ, 2e)
            if re.search(r"[0-9]", s_stripped) and re.search(r"[a-zA-Z]", s_stripped):
                return False
            # 必须包含元音字母 (剔除 XFZ, LMZ, BLZ, PKZ, BZZ, JYZ, HXZ, XWZ, JUZ 等纯辅音垃圾)
            if not re.search(r"[aeiouy\u4e00-\u9fa5]", s_lower):
                return False
            # 异常大小写组合 (如 NyZ, HaZ, bZ)
            if re.match(r"^[A-Z][a-z][A-Z]$", s_stripped) or re.match(r"^[a-z][A-Z]$", s_stripped):
                return False
            # 3 字符未在常用词典中的组合直接过滤
            if len(s_stripped) <= 3:
                return False
            # 4 字符不满足标准英文单词结构 (首字母可大写+3小写) 且不在词典中的直接过滤
            if len(s_stripped) == 4 and not re.match(r"^[A-Z]?[a-z]{3}$", s_stripped):
                return False

    # 6. 剔除纯占位符/格式化串且不含实质英文字词 (如 "%d, %d", "%.3f, %.3f", "%s = %s", "(%d,%d)", "%lld")
    no_fmt = re.sub(r"%[0-9.*#+\-]*[a-zA-Z]", "", s_stripped)
    no_fmt = re.sub(r"\{[0-9a-zA-Z_:]*\}", "", no_fmt)
    no_fmt = re.sub(r"0x[0-9a-fA-F]+", "", no_fmt)
    no_fmt = re.sub(r"[^a-zA-Z\u4e00-\u9fa5]", "", no_fmt)
    if len(no_fmt) < 2 and s_stripped not in ("...", "…", "+/-"):
        return False

    # 7. 剔除首尾出现不合常理机器符号的短字符串 (如以 * ~ ^ \ | > [ ] { } 开头或结尾且无空格)
    if re.match(r"^[\*~^\\\|><\[\]\{\}@#\$%&+=;:,`\'].{0,5}$", s_stripped) or re.match(r"^.{0,5}[\*~^\\\|><\[\]\{\}@#\$%&+=;:,`\']$", s_stripped):
        if " " not in s_stripped and len(s_stripped) <= 8 and s_stripped not in ("...", "…", "+/-"):
            return False

    # 8. 剔除代码断言头与内部调试前缀 (如 CHECK failed:, Assertion failed:, ASSERT:)
    if s_stripped.startswith(("CHECK failed:", "Assertion failed:", "ASSERT:", "ASSERT failed:")):
        return False

    # 9. 剔除 Qt moc 内部信号与槽签名 (如 1OnToggleOrientationAction(), 2triggered())
    if re.match(r"^[0-3][A-Za-z0-9_]+\s*\(.*\)$", s_stripped):
        return False

    # 10. 剔除 MSVC / C++ 编译器内部修饰符、虚表、RTTI 与 ABI 关键字
    if s_stripped.startswith((".?AV", ".?AU", ".?AX", "._AV", "._AU", "._AX", "..@", "`")):
        return False
    if "@@" in s_stripped or re.search(r"\?[a-zA-Z0-9_]+@[a-zA-Z0-9_@$]+", s_stripped):
        return False
    if s_stripped.startswith((
        "__cdecl", "__stdcall", "__thiscall", "__fastcall", "__vectorcall", "__clrcall",
        "__ptr64", "__restrict", "__unaligned", "__based(", "__pascal", "__eabi", "__swift_"
    )):
        return False
    if "<class " in s_stripped or "<struct " in s_stripped or "<typename " in s_stripped:
        return False

    # 11. 剔除 PE 导入表系统前缀与底层 Windows 系统 DLL 纯文件名
    s_lower = s_stripped.lower()
    if s_lower.startswith(("api-ms-win-", "ext-ms-win-")):
        return False
    if s_lower in {
        "advapi32.dll", "kernel32.dll", "user32.dll", "gdi32.dll", "ntdll.dll", "msvcrt.dll",
        "ucrtbase.dll", "ole32.dll", "shell32.dll", "ws2_32.dll", "version.dll", "comctl32.dll"
    }:
        return False

    # 12. 剔除连续重复无意义填充字符 (如 aaaaaaaa, XXXXXXXX, 00000000)
    if re.search(r"(.)\1{4,}", s_stripped) and not s_stripped.startswith("..."):
        return False

    # 13. 剔除纯十六进制内存地址与纯 GUID 串
    if re.match(r"^(?:0x[0-9a-fA-F]{8,16}|[0-9a-fA-F]{16,64})$", s_stripped):
        return False
    if re.match(r"^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$", s_stripped):
        return False

    # 14. 剔除纯文件路径、资源 URI、文件名及着色器路径 (如 models/..., materials/..., foo.vmdl, C:\...)
    if is_pure_path_string(s_stripped):
        return False

    # 15. 剔除包含下划线的单个代码标识符、枚举常量、多驼峰函数名、PE节区符号与纯数字
    if is_code_identifier_or_constant(s_stripped):
        return False

    return True


def extract_strings_from_bytes(data: bytes, min_length: int = 3, base_offset: int = 0):
    """从二进制数据中提取以 NULL 结尾的标准 C 字符串 (ASCII/UTF-8 与 UTF-16LE)。"""
    results = []

    # 1. 提取以 NULL 结尾的 ASCII / UTF-8 连续可打印字符
    ascii_regex = re.compile(
        rb"(?:(?<=\x00)|^)([\x20-\x7e\t\r\n]{"
        + str(min_length).encode("ascii")
        + rb",})(?=\x00)"
    )
    for match in ascii_regex.finditer(data):
        raw_bytes = match.group(1)
        try:
            text = raw_bytes.decode("utf-8", errors="ignore").strip()
            if len(text) >= min_length:
                results.append((base_offset + match.start(1), "ASCII/UTF-8", text))
        except Exception:
            continue

    # 2. 提取以 NULL 结尾的 UTF-16LE 宽字符格式字符串
    utf16_regex = re.compile(
        rb"(?:(?<=\x00\x00)|^)((?:[\x20-\x7e\t\r\n]\x00){"
        + str(min_length).encode("ascii")
        + rb",})(?=\x00\x00)"
    )
    for match in utf16_regex.finditer(data):
        raw_bytes = match.group(1)
        try:
            text = raw_bytes.decode("utf-16le", errors="ignore").strip()
            if len(text) >= min_length:
                results.append((base_offset + match.start(1), "UTF-16LE", text))
        except Exception:
            continue

    results.sort(key=lambda x: x[0])
    return results


def resolve_file_paths(file_inputs: list) -> list:
    """解析输入的文件路径列表，支持通配符与文件夹。"""
    resolved = []
    seen = set()

    for item in file_inputs:
        item = clean_path_input(item)
        if not item:
            continue

        if os.path.isdir(item):
            pattern = os.path.join(item, "*.dll")
            matches = glob.glob(pattern)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(abs_p):
                    seen.add(abs_p)
                    resolved.append(abs_p)
            continue

        if any(char in item for char in ("*", "?", "[")):
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(abs_p):
                    seen.add(abs_p)
                    resolved.append(abs_p)
            continue

        if os.path.isfile(item):
            abs_p = os.path.abspath(item)
            if abs_p not in seen:
                seen.add(abs_p)
                resolved.append(abs_p)
        else:
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(abs_p):
                    seen.add(abs_p)
                    resolved.append(abs_p)

    return resolved


def process_single_file(
    file_path: str,
    compiled_pattern: re.Pattern = None,
    min_length: int = 3,
    show_offset: bool = False,
    unique: bool = True,
    clean: bool = True,
    all_sections: bool = False,
):
    """提取单个文件中的字符串。自动进行节区过滤、去噪与去重。"""
    sections = get_pe_data_sections(file_path, all_sections=all_sections)
    if sections is None:
        return None, 0, 0

    all_extracted = []
    for sec_offset, sec_name, sec_data in sections:
        sec_strings = extract_strings_from_bytes(
            sec_data, min_length=min_length, base_offset=sec_offset
        )
        all_extracted.extend(sec_strings)

    all_extracted.sort(key=lambda x: x[0])
    raw_count = len(all_extracted)

    matched_lines = []
    seen = set()

    for offset, enc, text in all_extracted:
        # 1. 正则过滤
        if compiled_pattern and not compiled_pattern.search(text):
            continue

        # 2. 去噪清洗
        if clean and not is_clean_extracted_string(text, min_length=min_length):
            continue

        # 3. 去重
        if unique and text in seen:
            continue
        seen.add(text)

        if show_offset:
            line = f"[0x{offset:08X}] [{enc:<10}] {text}"
        else:
            line = text

        matched_lines.append(line)

    return matched_lines, raw_count, len(matched_lines)


def save_extracted_strings(
    output_file: str,
    target_file: str,
    matched_items: list,
    rule_desc: str,
    json_format: str = "dict",
):
    """根据扩展名保存为 TXT 或 JSON/JSONC 格式。"""
    is_json = output_file.lower().endswith(".jsonc") or output_file.lower().endswith(".json")

    out_dir = os.path.dirname(os.path.abspath(output_file))
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    if is_json:
        if json_format == "dict":
            data = {}
            for item in matched_items:
                text = item[2] if isinstance(item, tuple) else item
                data[text] = ""
        else:
            data = [
                item[2] if isinstance(item, tuple) else item
                for item in matched_items
            ]

        with open(output_file, "w", encoding="utf-8") as out_f:
            json.dump(data, out_f, ensure_ascii=False, indent=2)
    else:
        with open(output_file, "w", encoding="utf-8") as out_f:
            out_f.write(f"# 分析文件: {os.path.abspath(target_file)}\n")
            out_f.write(f"# 提取规则: {rule_desc}\n")
            out_f.write(f"# 字符串总数: {len(matched_items)}\n")
            out_f.write("=" * 70 + "\n\n")
            for item in matched_items:
                line = item if isinstance(item, str) else item[2]
                out_f.write(line + "\n")


def open_file_in_viewer(file_path: str):
    """打开文件浏览。"""
    try:
        abs_path = os.path.abspath(file_path)
        system = platform.system()
        if system == "Windows":
            os.startfile(abs_path)
        elif system == "Darwin":
            subprocess.run(["open", abs_path], check=True)
        else:
            subprocess.run(["xdg-open", abs_path], check=True)
        print(f"[+] 已打开文件: {abs_path}")
    except Exception as e:
        print(f"[-] 打开文件失败: {e}", file=sys.stderr)


def search_in_dll(
    file_path,
    pattern: str = "",
    output_file: str = None,
    output_dir: str = None,
    min_length: int = 3,
    ignore_case: bool = False,
    show_offset: bool = False,
    unique: bool = True,
    clean: bool = True,
    all_sections: bool = False,
    auto_open: bool = None,
    json_format: str = "dict",
    export_json: bool = False,
):
    """在 DLL 文件或列表中提取字符串并输出。"""
    if isinstance(file_path, str):
        file_inputs = [file_path]
    elif isinstance(file_path, (list, tuple)):
        file_inputs = list(file_path)
    else:
        file_inputs = [str(file_path)]

    compiled_pattern = None
    if pattern:
        try:
            flags = re.IGNORECASE if ignore_case else 0
            compiled_pattern = re.compile(pattern, flags)
        except re.error as e:
            print(f"[-] 正则表达式语法错误: {e}", file=sys.stderr)
            return 1

    file_paths = resolve_file_paths(file_inputs)
    if not file_paths:
        print(f"[-] 错误: 未找到任何匹配的文件 -> {file_inputs}", file=sys.stderr)
        return 1

    rule_desc = (
        f"/{pattern}/ (区分大小写: {not ignore_case})"
        if pattern
        else "全部提取 (基础去噪)"
    )

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # 模式 1: 指定了 output_dir，按 DLL 文件分别输出独立文件
    if output_dir:
        total_extracted = 0
        total_raw = 0
        for idx, target_file in enumerate(file_paths, 1):
            base_name = os.path.splitext(os.path.basename(target_file))[0]
            ext = (
                ".jsonc"
                if (export_json or (output_file and (output_file.endswith(".jsonc") or output_file.endswith(".json"))))
                else ".txt"
            )
            curr_output = os.path.join(output_dir, f"{base_name}{ext}")

            print(f"[*] [{idx}/{len(file_paths)}] 正在分析: {target_file}")
            matched_lines, raw_cnt, clean_cnt = process_single_file(
                target_file,
                compiled_pattern,
                min_length,
                show_offset,
                unique,
                clean,
                all_sections,
            )
            if matched_lines is None:
                continue

            try:
                save_extracted_strings(
                    curr_output,
                    target_file,
                    matched_lines,
                    rule_desc,
                    json_format,
                )
                print(
                    f"    [+] 提取完成: 原始 {raw_cnt:,} 条 -> 去噪后 {clean_cnt:,} 条有效字符串，已保存至: {os.path.abspath(curr_output)}"
                )
                total_extracted += clean_cnt
                total_raw += raw_cnt
            except Exception as e:
                print(f"[-] 写入文件失败 ({curr_output}): {e}", file=sys.stderr)

        print("-" * 70)
        print(
            f"[*] 全部处理完成！共扫描 {len(file_paths)} 个 DLL，原始累计 {total_raw:,} 条 -> 最终去噪输出 {total_extracted:,} 条有效字符串。"
        )
        return 0

    # 模式 2: 单个目标文件
    elif len(file_paths) == 1:
        target_file = file_paths[0]
        base_name = os.path.splitext(os.path.basename(target_file))[0]
        is_json = export_json or (output_file and (output_file.endswith(".jsonc") or output_file.endswith(".json")))
        ext = ".jsonc" if is_json else ".txt"

        if not output_file:
            output_file = f"{base_name}_all_strings{ext}"

        print(f"[*] 正在分析单个文件: {target_file}")
        matched_lines, raw_cnt, clean_cnt = process_single_file(
            target_file,
            compiled_pattern,
            min_length,
            show_offset,
            unique,
            clean,
            all_sections,
        )
        if matched_lines is None:
            return 1

        try:
            save_extracted_strings(
                output_file,
                target_file,
                matched_lines,
                rule_desc,
                json_format,
            )
            print("=" * 70)
            print(
                f"[*] 提取完成: 原始 {raw_cnt:,} 条 -> 去噪输出 {clean_cnt:,} 条有效字符串。"
            )
            print(f"[*] 结果已保存至: {os.path.abspath(output_file)}")
        except Exception as e:
            print(f"[-] 写入文件失败 ({output_file}): {e}", file=sys.stderr)
            return 1

    # 模式 3: 多个文件批量处理
    else:
        if output_file:
            is_json = output_file.lower().endswith(".jsonc") or output_file.lower().endswith(".json") or export_json
            print(f"[*] 发现 {len(file_paths)} 个目标文件，检测到指定输出文件: {output_file}")
            print("[*] 正在执行多文件全局去重整合...")
            print(f"[*] 提取规则: {rule_desc}")
            print(f"[*] 基础去噪: {'已启用' if clean else '关闭'}")
            print(f"[*] 全局去重: {'已启用' if unique else '关闭'}")
            print(f"[*] 输出目标: {output_file}")
            print("=" * 70)

            global_seen = set()
            merged_lines = []
            file_summary = []
            total_raw = 0
            total_cleaned_sum = 0

            for idx, target_file in enumerate(file_paths, 1):
                matched_lines, raw_cnt, clean_cnt = process_single_file(
                    target_file,
                    compiled_pattern,
                    min_length,
                    show_offset,
                    unique=True,
                    clean=clean,
                    all_sections=all_sections,
                )
                if matched_lines is None:
                    continue

                total_raw += raw_cnt
                total_cleaned_sum += clean_cnt

                file_unique_added = 0
                for line in matched_lines:
                    if unique:
                        if line not in global_seen:
                            global_seen.add(line)
                            merged_lines.append(line)
                            file_unique_added += 1
                    else:
                        merged_lines.append(line)
                        file_unique_added += 1

                file_summary.append((target_file, raw_cnt, clean_cnt, file_unique_added))
                print(
                    f"[+] [{idx}/{len(file_paths)}] {os.path.basename(target_file):<25} -> 原始 {raw_cnt:>6,} 条 -> 单文件有效 {clean_cnt:>5,} 条 (贡献新词 {file_unique_added:>5,} 条)"
                )

            try:
                if is_json:
                    if json_format == "dict":
                        data = {line: "" for line in merged_lines}
                    else:
                        data = merged_lines

                    with open(output_file, "w", encoding="utf-8") as out_f:
                        json.dump(data, out_f, ensure_ascii=False, indent=2)
                else:
                    with open(output_file, "w", encoding="utf-8") as out_f:
                        out_f.write("# 批量字符串提取报告 (多文件去重整合)\n")
                        out_f.write(f"# 提取规则: {rule_desc}\n")
                        out_f.write(f"# 基础去噪: {'已启用' if clean else '关闭'}\n")
                        out_f.write(f"# 全局去重: {'已启用' if unique else '关闭'}\n")
                        out_f.write(f"# 扫描文件总数: {len(file_paths)} 个\n")
                        out_f.write("# ----------------------------------------------------------------------\n")
                        out_f.write("# 扫描文件详情列表:\n")
                        for f_path, r_cnt, c_cnt, u_cnt in file_summary:
                            out_f.write(
                                f"#   - {os.path.basename(f_path):<25}: 原始 {r_cnt:>6,} 条 | 有效 {c_cnt:>5,} 条 | 贡献新词 {u_cnt:>5,} 条 ({os.path.abspath(f_path)})\n"
                            )
                        out_f.write("# ----------------------------------------------------------------------\n")
                        out_f.write(f"# 累计原始提取项: {total_raw:,}\n")
                        out_f.write(f"# 单文件有效项累计: {total_cleaned_sum:,}\n")
                        out_f.write(f"# 全局去重整合后有效字符串总数: {len(merged_lines):,}\n")
                        out_f.write("=" * 70 + "\n\n")

                        if merged_lines:
                            for line in merged_lines:
                                out_f.write(line + "\n")
                        else:
                            out_f.write("# 未在任何文件中找到符合条件的字符串。\n")

                print("=" * 70)
                print(
                    f"[*] 批量去重整合完成！共扫描 {len(file_paths)} 个文件，累计原始 {total_raw:,} 条 -> 单文件有效累计 {total_cleaned_sum:,} 条 -> 全局去重输出 {len(merged_lines):,} 条有效字符串。"
                )
                print(f"[*] 结果已保存到: {os.path.abspath(output_file)}")
            except Exception as e:
                print(f"[-] 写入文件失败 ({output_file}): {e}", file=sys.stderr)
                return 1

        else:
            is_json = export_json
            output_file = "batch_strings.jsonc" if is_json else "batch_strings.txt"

            print(f"[*] 发现 {len(file_paths)} 个目标文件，开始批量扫描 (分文件独立报告模式)...")
            print(f"[*] 提取规则: {rule_desc}")
            print(f"[*] 基础去噪: {'已启用' if clean else '关闭'}")
            print(f"[*] 输出目标: {output_file}")
            print("=" * 70)

            file_results = []
            total_matches = 0
            total_raw = 0

            for idx, target_file in enumerate(file_paths, 1):
                matched_lines, raw_cnt, clean_cnt = process_single_file(
                    target_file,
                    compiled_pattern,
                    min_length,
                    show_offset,
                    unique,
                    clean,
                    all_sections,
                )
                if matched_lines is None:
                    continue

                total_raw += raw_cnt
                if clean_cnt > 0:
                    print(
                        f"[+] [{idx}/{len(file_paths)}] {os.path.basename(target_file):<25} -> 原始 {raw_cnt:>6,} 条 -> 去噪后 {clean_cnt:>5,} 条"
                    )
                    file_results.append((target_file, raw_cnt, matched_lines))
                    total_matches += clean_cnt
                else:
                    print(
                        f"[-] [{idx}/{len(file_paths)}] {os.path.basename(target_file):<25} -> 无有效字符串"
                    )

            try:
                if is_json:
                    if json_format == "dict":
                        data = {
                            os.path.basename(f_path): {line: "" for line in lines}
                            for f_path, _, lines in file_results
                        }
                    else:
                        data = {
                            os.path.basename(f_path): lines
                            for f_path, _, lines in file_results
                        }

                    with open(output_file, "w", encoding="utf-8") as out_f:
                        json.dump(data, out_f, ensure_ascii=False, indent=2)
                else:
                    with open(output_file, "w", encoding="utf-8") as out_f:
                        out_f.write("# 批量字符串提取报告 (按文件分节)\n")
                        out_f.write(f"# 提取规则: {rule_desc}\n")
                        out_f.write(f"# 基础去噪: {'已启用' if clean else '关闭'}\n")
                        out_f.write(
                            f"# 扫描文件总数: {len(file_paths)} 个 (其中 {len(file_results)} 个文件包含有效字符串)\n"
                        )
                        out_f.write(f"# 累计有效字符串总数: {total_matches:,}\n")
                        out_f.write("=" * 70 + "\n\n")

                        if file_results:
                            for idx, (f_path, r_cnt, lines) in enumerate(file_results, 1):
                                out_f.write(
                                    f"## [{idx}/{len(file_results)}] 文件: {os.path.abspath(f_path)} (共 {len(lines):,} 条)\n"
                                )
                                out_f.write("-" * 70 + "\n")
                                for line in lines:
                                    out_f.write(line + "\n")
                                out_f.write("\n\n")
                        else:
                            out_f.write("# 未在任何文件中找到符合条件的字符串。\n")

                print("=" * 70)
                print(
                    f"[*] 批量分节提取完成！共扫描 {len(file_paths)} 个文件，累计提取 {total_matches:,} 条有效字符串。"
                )
                print(f"[*] 结果已保存到: {os.path.abspath(output_file)}")
            except Exception as e:
                print(f"[-] 写入文件失败 ({output_file}): {e}", file=sys.stderr)
                return 1

    if auto_open is True:
        open_file_in_viewer(output_file)
    elif auto_open is False:
        pass
    else:
        try:
            choice = (
                input("\n[?] 是否立即打开输出文件进行浏览? [Y/n]: ")
                .strip()
                .lower()
            )
            if choice in ("", "y", "yes"):
                open_file_in_viewer(output_file)
        except (EOFError, KeyboardInterrupt):
            pass

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="从 DLL/二进制文件中提取待翻译字符串，支持基础去噪清洗、节区过滤与批量导出",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例用法:
  # 从单个 DLL 提取字符串并保存为 JSONC 词典模板
  python scripts/find_dll_strings.py -f subtools/convarhelper_subtool.dll -j -out translations/convar_extracted.jsonc

  # 批量扫描 subtools 目录下所有 DLL 并全局去重合并为一个文件
  python scripts/find_dll_strings.py -f subtools/*.dll -j -out translations/subtools_merged.jsonc

  # 按正则搜索包含 "Material" 的字符串并显示内存偏移
  python scripts/find_dll_strings.py -f hammer.dll -p "Material" -o
"""
    )
    parser.add_argument(
        "files_pos",
        nargs="*",
        default=[],
        help="目标 DLL/二进制文件路径（位置参数，支持直接传参或拖拽）",
    )
    parser.add_argument(
        "-f",
        "--file",
        nargs="+",
        default=[],
        help="目标 DLL/二进制文件路径，支持通配符（如 *.dll、tools/*.dll）或文件夹路径",
    )
    parser.add_argument(
        "-p", "--pattern", default="", help="可选：搜索过滤关键字或正则表达式（留空提取全部）"
    )
    parser.add_argument(
        "-out",
        "--output",
        default=None,
        help="输出文件路径 (单文件默认: <dll名>_all_strings.txt/json，批量默认: batch_strings.txt)",
    )
    parser.add_argument(
        "-out-dir",
        "--output-dir",
        default=None,
        help="指定输出目录（批量处理时将按 DLL 文件名分别保存）",
    )
    parser.add_argument(
        "-m",
        "--min-len",
        type=int,
        default=3,
        help="提取字符串的最小长度阈值 (默认: 3)",
    )
    parser.add_argument(
        "-i", "--ignore-case", action="store_true", help="忽略大小写匹配（仅在指定 -p/--pattern 时生效）"
    )
    parser.add_argument(
        "-o", "--show-offset", action="store_true", help="显示字符串在文件中的 16 进制偏移量和编码"
    )
    parser.add_argument(
        "-u", "--unique", action="store_true", default=True, help="去除重复出现的字符串 (默认开启)"
    )
    parser.add_argument(
        "--no-unique", dest="unique", action="store_false", help="保留重复字符串"
    )
    parser.add_argument(
        "--clean",
        dest="clean",
        action="store_true",
        default=True,
        help="清洗无意义机器垃圾、汇编噪音与符号杂质 (默认开启)",
    )
    parser.add_argument(
        "--raw",
        "--no-clean",
        dest="clean",
        action="store_false",
        help="禁用去噪清洗，导出未经处理的原始提取数据",
    )
    parser.add_argument(
        "--all-sections",
        action="store_true",
        default=False,
        help="扫描包含 .text 代码段在内的所有节区 (默认仅扫描只读数据节区)",
    )
    parser.add_argument(
        "-j",
        "--json",
        dest="export_json",
        action="store_true",
        help="导出为 JSON 格式文件",
    )
    parser.add_argument(
        "--json-format",
        choices=["dict", "list"],
        default="dict",
        help="JSON 导出格式：dict 为翻译字典模板 {\"原文\": \"\"}，list 为纯字符串列表 [\"str\", ...]",
    )
    parser.add_argument(
        "--open", dest="auto_open", action="store_true", default=None, help="完成后直接自动打开文件"
    )
    parser.add_argument(
        "--no-open", dest="auto_open", action="store_false", help="完成后不提示打开文件"
    )

    args = parser.parse_args()

    file_inputs = list(args.files_pos) + list(args.file)

    if not file_inputs:
        print("==================================================")
        print("     DLL / 二进制文件字符串提取与去噪工具")
        print("==================================================")
        try:
            user_input = input("[*] 请输入目标 DLL 文件路径（或直接将 DLL 拖入本窗口）: ").strip()
        except (EOFError, KeyboardInterrupt):
            return 0
        user_input = clean_path_input(user_input)
        if not user_input:
            print("[-] 未输入任何有效路径，已退出。")
            return 1
        file_inputs = [user_input]

        try:
            pat_input = input("[*] 请输入过滤关键词/正则表达式 (直接按回车提取全部): ").strip()
            if pat_input:
                args.pattern = pat_input
        except (EOFError, KeyboardInterrupt):
            pass

    sys.exit(
        search_in_dll(
            file_path=file_inputs,
            pattern=args.pattern,
            output_file=args.output,
            output_dir=args.output_dir,
            min_length=args.min_len,
            ignore_case=args.ignore_case,
            show_offset=args.show_offset,
            unique=args.unique,
            clean=args.clean,
            all_sections=args.all_sections,
            auto_open=args.auto_open,
            json_format=args.json_format,
            export_json=args.export_json,
        )
    )


if __name__ == "__main__":
    main()
