#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import glob
import json
import os
import platform
import re
import struct
import subprocess
import sys


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


COMMON_UI_WORDS = {
    "file", "edit", "view", "save", "open", "new", "close", "exit", "cancel", "apply", "browse", "ok", "yes", "no",
    "undo", "redo", "cut", "copy", "paste", "delete", "select", "all", "properties", "tools", "help", "window",
    "windows", "options", "settings", "about", "search", "filter", "name", "type", "size", "path", "status",
    "default", "enable", "disable", "lock", "unlock", "hide", "show", "clear", "reset", "revert", "add", "remove",
    "insert", "duplicate", "rename", "export", "import", "refresh", "reload", "publish", "build", "run", "test",
    "preview", "inspect", "width", "height", "depth", "color", "texture", "material", "position", "rotation",
    "scale", "radius", "visible", "hidden", "solid", "normal", "smooth", "grid", "snap", "origin", "angles",
    "target", "class", "entity", "model", "sound", "light", "camera", "particle", "physics", "collision",
    "layer", "group", "prefab", "compile", "bake", "lighting", "shading", "wireframe", "flat", "textured",
    "raytrace", "fog", "skybox", "environment", "zoom", "pan", "rotate", "translate", "center", "focus",
    "align", "distribute", "group", "ungroup", "attach", "detach", "parent", "unparent", "lock", "unlock",
    "freeze", "unfreeze", "isolate", "unisolate", "mask", "unmask", "mode", "toggle", "switch", "active",
    "inactive", "state", "value", "count", "index", "id", "tag", "category", "folder", "directory", "file",
    "project", "document", "session", "workspace", "layout", "reset", "defaults", "preferences", "custom",
    "user", "system", "general", "advanced", "details", "info", "warning", "error", "message", "dialog",
    "wizard", "assistant", "guide", "documentation", "feedback", "report", "bug", "issue", "version", "update",
    "check", "uncheck", "collapse", "expand", "up", "down", "left", "right", "top", "bottom", "front", "back",
    "perspective", "orthographic", "isometric", "viewport", "camera", "render", "display", "scene", "node",
    "item", "element", "object", "component", "asset", "resource", "package", "library", "browser", "editor"
}


def is_meaningful_string(s: str) -> bool:
    """
    精确识别在 src/qtcore_qm.cpp 中被 HOOK 拦截处理的 Qt / Hammer UI 字符串类型：
    1. QMetaObject::tr 翻译源文本
    2. QAction::setText / setToolTip / setStatusTip / setWhatsThis
    3. QAbstractButton::setText / QLabel::setText / QWidget::setWindowTitle
    4. QTreeWidgetItem::setText / QTableWidgetItem::setText / QListWidgetItem::setText / QComboBox::addItem
    5. QPainter::drawText 绘制的属性面板、表格视口文字 (如 "Clipping Tool [Shift+X]")
    6. Hammer RegisterAction 注册的动作描述与快捷键
    """
    s_stripped = s.strip()
    if len(s_stripped) < 2 or len(s_stripped) > 400:
        return False

    # 1. 必须包含有效英文字母或汉字 (排除 "...", "---", 等纯标点，但保留 "..." 或 "…")
    if not re.search(r"[a-zA-Z\u4e00-\u9fa5]", s_stripped):
        if s_stripped in ("...", "…"):
            return True
        return False

    # 2. 排除不平衡双引号或前后截断的双引号片段 (如 'Could not find type "', '" is specified...')
    if s_stripped.count('"') % 2 != 0:
        return False
    if re.match(r'^["\'][\s,;:.?\-]', s_stripped) or re.search(r'[\s,;:.?\-]["\']$', s_stripped):
        return False

    # 3. 剔除连续重复字符 (如 66666666, jjjjjjjj, aaaaaaaa)
    if re.search(r"(.)\1{3,}", s_stripped) and not s_stripped.startswith("..."):
        return False

    # 4. 剔除全字母表、十六进制表、Base64 字符表
    if s_stripped in (
        "0123456789ABCDEF",
        "0123456789abcdef",
        "0123456789abcdefABCDEF",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    ):
        return False

    # 5. 剔除 OpenSSL / 第三方底层库版权声明
    if re.search(
        r"part of OpenSSL|OpenSSL \d+\.\d+|Big Number part|RSA part|SHA1 part|MD5 part|AES part",
        s_stripped,
        re.IGNORECASE,
    ):
        return False

    # 6. 剔除 Protobuf / 内部序列化 / 反射 / 词法分析器 / 断言 / Descriptor 校验器
    if any(t.lower() in s_stripped.lower() for t in (
        "protobuf", "libprotobuf", "mapkey", "mapvalue", "reflection", "google::", "google.",
        "check failed", "protocol buffer", "descriptor at (", "non-repeated", "oneof", "tokenizer::",
        "cpptype_", "field_", "indent()", "outdent()", "do not parse", "lite message", "syntax error",
        "digit", "exponent", "floatliteral", "antlr", "lexer", "failed to allocate a lexer",
        "missing required fields", "fields may be stripped", "expected integer", "expected identifier",
        "expected string", "expected double", "invalid float number", "value of type", "invalid value for boolean",
        "unknown enumeration value", "reached an unintended state", "cannot skip field value",
        "is not defined or is not an extension", "ignoring extension", "has no field named",
        "recursion limit", "invalid key for map", "expected :", "actual   :", "swap()",
        "text-format", "protocol message", "unimplemented type", "proto type", "proto descriptor",
        "same descriptor", "input size too large", "can't print proto", "message type:", "field type:",
        "singular field", "repeated field", "message type", "reflection object", "type does not match",
        "invalid string position", "not compatible with", "hasfield", "fieldsize", "clearfield",
        "can't get here", "can't happen", "can't reach", "reached impossible case", "messageset",
        "repeatedfield", "rawrepeated", "stringreference", "enumvalue", "subtype mismatch",
        "invalid descriptor", "expect a decimal", "registered:", "generated pool", "out-of-bounds",
        "out of range", "invalid hash", "can't be packed", "cannot cross line", "escape sequence",
        "leading zero", "block comment", "end-of-file", "control characters", "non ascii codepoint",
        "tried to copy from a message", "should not reach here", "cannot get here", "fileoptions",
        "messageoptions", "fieldoptions", "enumoptions", "methodoptions", "oneofoptions",
        "servicemethods", "extensionrangeoptions", "serviceoptions", "bad cast", "bad locale", "bad alloc",
        "future_error", "regex_error", "ios_base::", "iostream", "unknown exception", "bad exception",
        "access violation - no rtti data!", "bad dynamic_cast!", "map/set too long", "vector too long",
        "string too long", "list too long", "truncated", "descriptorproto", "dependency index",
        "reserved range", "extension range", "uses reserved number", "field name \"$", "file recursively imports",
        "package name is too long", "already in the pool", "proto option data", "messages can't have default values",
        "comment started here", "need space between", "hex and octal", "string field", "unrecognized syntax",
        "exceeds maximum package depth", "missing name.", "is already defined", "proto3", "proto2",
        "lite_runtime", "generic_services", "field number", "extension number", "reserved number",
        "type_name", "jstype", "enum value \"$", "default value for an enum", "cannot extend a non-lite",
        "primitive type has", "map fields cannot be", "sint64", "fixed64", "sfixed64", "uint64",
        "int64", "float field", "double field", "map field", "is not a map", "the extension",
        "suggested field", "the first enum value", "groups are not supported", "explicit default values",
        "required fields are not allowed", "map entry type", "uninterpreted_option", "zerocopy",
        "backup()", "getprototype()", "descriptordatabase", "merge messages", "parsing attempt:",
        "could not create an instance of", "the global scope", "boolean default must be", "enums must contain",
        "invalid symbol name:", "invalid package name:", "already exists in database:"
    )):
        return False

    if re.match(r"^(?:Get|Set|Add|Clear|Has|Release|Mutable|GetRepeated|SetRepeated|AddRepeated)", s_stripped):
        return False

    if s_stripped in {"INFO", "WARNING", "ERROR", "FATAL", "True", "False", "Not supported.", "not found", "Unsupported", "No default value", "Entry", "Enum name"}:
        return False

    # 7. 剔除 POSIX errno 错误表
    if s_stripped.lower() in {
        "address family not supported", "address in use", "address not available", "already connected",
        "argument list too long", "argument out of domain", "bad address", "bad file descriptor",
        "bad message", "broken pipe", "connection aborted", "connection already in progress",
        "connection refused", "connection reset", "cross device link", "destination address required",
        "device or resource busy", "directory not empty", "executable format error", "file exists",
        "file too large", "filename too long", "function not supported", "host unreachable",
        "identifier removed", "illegal byte sequence", "inappropriate io control operation",
        "interrupted", "invalid argument", "invalid seek", "io error", "is a directory",
        "message size", "network down", "network reset", "network unreachable", "no buffer space",
        "no child process", "no link", "no lock available", "no message available", "no message",
        "no protocol option", "no space on device", "no stream resources", "no such device or address",
        "no such device", "no such file or directory", "no such process", "not a directory",
        "not a socket", "not a stream", "not connected", "not enough memory", "not supported",
        "operation canceled", "operation in progress", "operation not permitted", "operation not supported",
        "operation would block", "owner dead", "permission denied", "protocol error",
        "protocol not supported", "read only file system", "resource deadlock would occur",
        "resource unavailable try again", "result out of range", "state not recoverable",
        "stream timeout", "text file busy", "timed out", "too many files open in system",
        "too many files open", "too many links", "too many symbolic link levels", "value too large",
        "wrong protocol type", "unknown error"
    }:
        return False

    # 8. 剔除引擎接口版本名、路径、文件扩展名、Windows API-Set 与系统 DLL
    if re.match(r"^[A-Z][a-zA-Z0-9]+_\d{3}$|^V[A-Z][a-zA-Z0-9]+00\d$", s_stripped) and not s_stripped.startswith(("VConsole", "VProf")):
        return False
    if any(s_stripped.startswith(p) for p in ("../", "game:", "/", "\\", "api-ms-", "ext-ms-")):
        return False
    if any(s_stripped.endswith(e) for e in (".vdf", ".vpk", ".png", ".jpg", ".svg", ".dll", ".exe", ".txt", ".json", ".h", ".cpp", ".cc", ".inl")):
        return False
    if s_stripped.lower() in {
        "advapi32", "user32", "gdi32", "shell32", "ole32", "oleaut32", "ws2_32", "wsock32",
        "comctl32", "comdlg32", "ntdll", "mscoree", "msvcrt", "ucrtbase", "kernel32", "kernelbase",
        "version", "winmm", "imm32", "d3d9", "d3d11", "dxgi", "opengl32", "glu32"
    }:
        return False

    # 9. 剔除 MSVC C++ 符号修饰与虚表
    if "@@" in s_stripped:
        return False
    if re.search(r"\?[a-zA-Z0-9_]+@", s_stripped):
        return False
    if re.search(r"@[a-zA-Z0-9_]+@[A-Z]", s_stripped):
        return False
    if re.search(r"@[A-Z]{1,4}Z$", s_stripped):
        return False
    if re.search(r"@Q[A-Z][a-zA-Z0-9_]+", s_stripped) and " " not in s_stripped:
        return False
    if re.match(r"^\?{1,2}[a-zA-Z0-9_?@$]", s_stripped):
        return False
    if s_stripped.startswith(
        (".?AV", ".?AU", ".?AX", "._AV", "._AU", "._AX", "..@", "`", "restrict(")
    ) or s_stripped.endswith("'"):
        return False

    # 10. 剔除编译器内部关键字与修饰符
    if s_stripped in {
        "__int8", "__int16", "__int32", "__int64", "__int128", "__w64", "__ptr32", "__ptr64",
        "__sptr", "__uptr", "char8_t", "char16_t", "char32_t", "wchar_t", "decltype(auto)",
        "std::nullptr_t", "nullptr", "NULL", "noexcept", "volatile", "static", "virtual",
        "protected:", "private:", "public:", "extern", "inline", "constexpr", "consteval",
        "constinit", "coclass", "cointerface", "{flat}", "signed", "unsigned", "union",
        "class", "struct", "this", "lambda", "<ellipsis>", ",<ellipsis>", "(null)", "_is_double",
        "!_is_double", "INITY", "inity", "CorExitProcess", "en-US", "long", "const", "short",
        "int", "float", "double", "bool", "char", "auto", "void", "delete", "operator",
        "new[]", "delete[]", "typeid", "dynamic_cast", "static_cast", "reinterpret_cast", "const_cast",
        "sizeof", "typedef", "typename"
    } or s_stripped.startswith(('extern "', "operator")):
        return False

    # 11. 剔除国家语言代码与数学 CRT 函数
    if re.match(r"^LC_[A-Z]+$", s_stripped) or re.match(r"^[a-z]{2,3}-[A-Z]{2,3}(?:-[a-zA-Z0-9]+)?$", s_stripped):
        return False
    if s_stripped in {
        "log10", "sinh", "cosh", "tanh", "atan", "atan2", "asin", "acos", "ceil", "floor",
        "fabs", "modf", "ldexp", "fmod", "frexp", "_cabs", "_hypot", "_logb", "_nextafter",
        "copysign", "scalbn", "exp2", "log2", "hypot", "trunc", "round", "nearbyint"
    } or s_stripped.upper() in {"NAN(SNAN)", "NAN(IND)", "NAN", "SNAN", "QNAN", "IND"}:
        return False

    # 12. 剔除已知 Windows 底层导出 API
    if s_stripped in {
        "GetSystemTimePreciseAsFileTime", "GetTempPath2W", "GetCurrentPackageId", "AreFileApisANSI",
        "CompareStringEx", "CompareStringOrdinal", "InitializeSListHead", "IsDebuggerPresent",
        "IsProcessorFeaturePresent", "UnhandledExceptionFilter", "SetUnhandledExceptionFilter",
        "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetCurrentProcessId", "GetCurrentThreadId",
        "GetSystemTimeAsFileTime", "InitializeCriticalSectionAndSpinCount", "DeleteCriticalSection",
        "SetEvent", "ResetEvent", "WaitForSingleObjectEx", "CreateEventW", "GetModuleHandleW",
        "GetProcAddress", "MultiByteToWideChar", "WideCharToMultiByte", "EncodePointer", "DecodePointer",
        "RaiseException", "GetLastError", "SetLastError", "CloseHandle", "TlsAlloc", "TlsGetValue",
        "TlsSetValue", "TlsFree", "FlsAlloc", "FlsGetValue", "FlsSetValue", "FlsFree", "VirtualAlloc",
        "VirtualFree", "VirtualProtect", "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapSize",
        "GetProcessHeap", "GetStdHandle", "WriteFile", "GetModuleFileNameW", "ExitProcess",
        "GetModuleHandleExW", "FreeLibrary", "LoadLibraryExW", "CreateInterface", "InstallSchemaBindings",
        "ExtractModuleMetadata", "GetResourceManifestCount", "GetResourceManifests", "BinaryProperties_GetValue",
        "EnumSystemLocalesEx", "GetDateFormatEx", "GetLocaleInfoEx", "GetTimeFormatEx", "GetUserDefaultLocaleName",
        "IsValidLocaleName", "LCMapStringEx", "LCIDToLocaleName", "LocaleNameToLCID",
        "AppPolicyGetProcessTerminationMethod", "AppPolicyGetShowDeveloperDiagnostic",
        "AppPolicyGetWindowingModelPolicy", "AppPolicyGetThreadInitializationType"
    }:
        return False

    # 13. 剔除 GUID / Hash
    if re.match(
        r"^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$",
        s_stripped,
    ) or re.match(r"^[0-9a-fA-F]{32,}$", s_stripped):
        return False

    # 14. 正向 UI 模式匹配 (Hook 目标类型)
    # 类型 1: 带 Qt 快捷键加速符 '&' (如 "&File", "&Edit", "Save &As...")
    if s_stripped.startswith("&") or ("&" in s_stripped and not any(c in s_stripped for c in "<>;=")):
        return True

    # 类型 2: 带快捷键后缀 / 快捷键组合 (如 "Clipping Tool [Shift+X]", "Undo (Ctrl+Z)", "Save\tCtrl+S")
    if re.search(r"\[[A-Za-z0-9+ \-]+\]$|\([A-Za-z0-9+ \-]+\)$|\t[A-Za-z0-9+ \-]+$", s_stripped):
        return True

    # 类型 3: UI 标题、菜单项、按钮短语、省略号/冒号项 (如 "Save As...", "Texture Browser", "Light Intensity:", "...")
    if s_stripped.endswith(("...", "…", ":")):
        return True

    # 类型 4: 对话框问句、提示与富文本 HTML (如 "Are you sure you want to delete '%1'?", "<br>Inspect panel open...")
    if s_stripped.endswith(("?", "!")) or (s_stripped.startswith("<") and s_stripped.endswith(">")):
        return True

    # 类型 5: 多词英语短语/句子 (如 "Preview Baked Lighting", "Working Sets", "Reload From Disk", "%1 Assets Visible", "+ Untagged")
    if " " in s_stripped:
        words = s_stripped.split()
        first_word = words[0]
        if first_word[0].isupper() or first_word[0] in ("'", '"', '<', '%', '+', '-', '#') or first_word[0].isdigit():
            return True
        # 全小写多词 UI 短语 (如 "start time", "event type")
        if all(w.islower() and w.isalpha() for w in words):
            return True

    # 类型 6: 单个标准 UI 词汇 (如 File, Edit, View, OK, Cancel, Position, Mesh, Bone, Hammer, VConsole, Spray-Paint)
    else:
        if (s_stripped[0].isupper() or s_stripped.startswith("V")) and re.match(r"^[A-Za-z0-9_\-]+$", s_stripped):
            if s_stripped.lower() in COMMON_UI_WORDS:
                return True
            # 驼峰命名 (如 AssetBrowser, MapEditor, Viewport3D, SubTool)
            if re.match(r"^[A-Z][a-z0-9]+[A-Z][a-zA-Z0-9]*$", s_stripped):
                return True
            # 带连字符 (如 Un-parent, Spray-Paint)
            if "-" in s_stripped:
                return True
            # 普通首字母大写单词 (如 Transform, Visibility, Intensity)
            if re.match(r"^[A-Z][a-z0-9]{2,15}$", s_stripped):
                return True

    return False


def extract_strings_from_bytes(data: bytes, min_length: int = 4, base_offset: int = 0):
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
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)
            continue

        if any(char in item for char in ("*", "?", "[")):
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)
            continue

        if os.path.isfile(item):
            abs_p = os.path.abspath(item)
            if abs_p not in seen:
                seen.add(abs_p)
                resolved.append(item)
        else:
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)

    return resolved


def process_single_file(
    file_path: str,
    compiled_pattern: re.Pattern = None,
    min_length: int = 4,
    show_offset: bool = False,
    unique: bool = False,
    clean: bool = True,
    all_sections: bool = False,
):
    """提取单个文件中的字符串。自动进行节区过滤与智能清洗。"""
    sections = get_pe_data_sections(file_path, all_sections=all_sections)
    if sections is None:
        return None, 0, 0

    all_extracted = []
    for sec_offset, sec_name, sec_data in sections:
        sec_strings = extract_strings_from_bytes(
            sec_data, min_length=min_length, base_offset=sec_offset
        )
        all_extracted.extend(sec_strings)

    # 按文件内偏移量排序
    all_extracted.sort(key=lambda x: x[0])
    raw_count = len(all_extracted)

    matched_lines = []
    seen = set()

    for offset, enc, text in all_extracted:
        # 1. 正则规则过滤
        if compiled_pattern and not compiled_pattern.search(text):
            continue

        # 2. 智能清洗过滤无意义字符串
        if clean and not is_meaningful_string(text):
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
    """根据扩展名保存为 TXT 或 JSON 格式。"""
    is_json = output_file.lower().endswith(".json")

    if is_json:
        if json_format == "dict":
            # 翻译词典模板: {"原文": ""}
            data = {}
            for item in matched_items:
                text = item[2] if isinstance(item, tuple) else item
                data[text] = ""
        else:
            # 纯字符串列表: ["str1", "str2", ...]
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
        print(f"[+] 已为您打开文件: {abs_path}")
    except Exception as e:
        print(f"[-] 打开文件失败: {e}", file=sys.stderr)


def search_in_dll(
    file_path,
    pattern: str = "",
    output_file: str = None,
    output_dir: str = None,
    min_length: int = 4,
    ignore_case: bool = False,
    show_offset: bool = False,
    unique: bool = False,
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
        else "全部提取 (无过滤)"
    )

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # 单文件模式或导出为按 DLL 分别命名的文件
    if len(file_paths) == 1 or output_dir or export_json:
        total_extracted = 0
        total_raw = 0
        for idx, target_file in enumerate(file_paths, 1):
            base_name = os.path.splitext(os.path.basename(target_file))[0]
            ext = (
                ".json"
                if (export_json or (output_file and output_file.endswith(".json")))
                else ".txt"
            )

            if len(file_paths) == 1 and output_file and not output_dir:
                curr_output = output_file
            elif output_dir:
                curr_output = os.path.join(output_dir, f"{base_name}{ext}")
            else:
                curr_output = f"{base_name}{ext}"

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
                    f"    [+] 提取完成: 原始 {raw_cnt:,} 条 -> 清洗后 {clean_cnt:,} 条有效字符串，已保存至: {os.path.abspath(curr_output)}"
                )
                total_extracted += clean_cnt
                total_raw += raw_cnt
            except Exception as e:
                print(f"[-] 写入文件失败 ({curr_output}): {e}", file=sys.stderr)

        print("-" * 70)
        print(
            f"[*] 全部处理完成！共扫描 {len(file_paths)} 个 DLL，原始累计 {total_raw:,} 条 -> 最终清洗输出 {total_extracted:,} 条有效字符串。"
        )
        return 0

    # 多文件合并报告模式
    else:
        if not output_file:
            output_file = "batch_strings.txt"

        print(f"[*] 发现 {len(file_paths)} 个目标文件，开始批量扫描...")
        print(f"[*] 提取规则: {rule_desc}")
        print(f"[*] 智能清洗: {'已启用' if clean else '关闭'}")
        print(f"[*] 输出目标: {output_file}")
        print("=" * 70)

        file_results = []
        total_matches = 0

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

            if clean_cnt > 0:
                print(
                    f"[+] [{idx}/{len(file_paths)}] {os.path.basename(target_file)} -> 原始 {raw_cnt:,} 条 -> 清洗后 {clean_cnt:,} 条"
                )
                file_results.append((target_file, matched_lines))
                total_matches += clean_cnt
            else:
                print(
                    f"[-] [{idx}/{len(file_paths)}] {os.path.basename(target_file)} -> 无有效字符串"
                )

        try:
            with open(output_file, "w", encoding="utf-8") as out_f:
                out_f.write("# 批量字符串提取报告\n")
                out_f.write(f"# 提取规则: {rule_desc}\n")
                out_f.write(
                    f"# 扫描文件总数: {len(file_paths)} 个 (其中 {len(file_results)} 个文件包含有效字符串)\n"
                )
                out_f.write(f"# 有效字符串总数: {total_matches}\n")
                out_f.write("=" * 70 + "\n\n")

                if file_results:
                    for idx, (f_path, lines) in enumerate(file_results, 1):
                        out_f.write(
                            f"## [{idx}/{len(file_results)}] 文件: {os.path.abspath(f_path)} (共 {len(lines)} 条)\n"
                        )
                        out_f.write("-" * 70 + "\n")
                        for line in lines:
                            out_f.write(line + "\n")
                        out_f.write("\n\n")
                else:
                    out_f.write("# 未在任何文件中找到符合条件的字符串。\n")

            print("=" * 70)
            print(
                f"[*] 批量提取完成！共扫描 {len(file_paths)} 个文件，累计提取 {total_matches:,} 条有效字符串。"
            )
            print(f"[*] 结果已保存到: {os.path.abspath(output_file)}")
        except Exception as e:
            print(f"[-] 写入文件失败: {e}", file=sys.stderr)
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
        description="从 DLL/二进制文件中提取有意义的 UI/文本字符串，支持智能去噪清洗、节区过滤与批量导出"
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
        default=4,
        help="提取字符串的最小长度阈值 (默认: 4)",
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
        help="智能清洗无意义机器垃圾、汇编噪音与符号杂质 (默认开启)",
    )
    parser.add_argument(
        "--raw",
        "--no-clean",
        dest="clean",
        action="store_false",
        help="禁用智能清洗，导出未经处理的原始提取数据",
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

    # 合并位置参数和 -f 参数
    file_inputs = list(args.files_pos) + list(args.file)

    # 交互式提示输入
    if not file_inputs:
        print("=" * 60)
        print("     DLL / 二进制文件字符串智能提取与清洗工具")
        print("=" * 60)
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


