#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CS2WorkshopToolsLocalizerCN - 翻译字典查重检测工具
用于检测 JSON / JSONC 翻译文件中是否存在重复的英文原文 (Key)，支持嵌套作用域感知并精确定位行号。

功能特性：
1. 完整支持 JSONC 语法：安全忽略 // 单行注释与 /* ... */ 多行注释。
2. 作用域感知 (Scope-Aware)：按 JSON 对象的层级路径提取 Key，避免跨对象误判。
3. 行号精确定位：在终端直观显示重复项所在行，并兼容 GitHub Actions Annotation 报错格式。
4. 环境泛化：支持在任意工作目录执行，自动检索 translations/ 目录或指定文件。
"""

import argparse
import glob
import json
import os
import sys
from collections import defaultdict

# 确保在各平台控制台下标准输出为 UTF-8 编码
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass


def extract_keys_with_lines(filepath: str):
    """
    解析 JSON / JSONC 文件并提取所有 Key 及其所在起始行号。
    支持 // 单行注释 和 /* ... */ 多行注释，按对象作用域 (Scope Path) 提取 Key。
    """
    with open(filepath, "r", encoding="utf-8") as f:
        text = f.read()

    i = 0
    n = len(text)
    line = 1

    # 作用域栈：存储当前所处的对象键路径
    scope_stack = []
    # 记录当前已确定但尚未进入值的 key
    pending_key = None

    keys = []

    while i < n:
        c = text[i]

        # 换行处理
        if c == "\n":
            line += 1
            i += 1
            continue

        # 单行注释 // ...
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
            continue

        # 多行注释 /* ... */
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2  # 跳过 */
            continue

        # 进入新对象
        if c == "{":
            if pending_key is not None:
                scope_stack.append(pending_key)
                pending_key = None
            else:
                scope_stack.append("$root" if not scope_stack else "$obj")
            i += 1
            continue

        # 离开当前对象
        elif c == "}":
            if scope_stack:
                scope_stack.pop()
            pending_key = None
            i += 1
            continue

        # 逗号分隔同级元素
        elif c == ",":
            pending_key = None
            i += 1
            continue

        # 字符串
        if c == '"':
            str_start_line = line
            i += 1
            chars = []
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    chars.append(text[i:i + 2])
                    i += 2
                elif text[i] == '"':
                    i += 1
                    break
                else:
                    if text[i] == "\n":
                        line += 1
                    chars.append(text[i])
                    i += 1
            raw_str = "".join(chars)
            try:
                decoded_str = json.loads(f'"{raw_str}"')
            except Exception:
                decoded_str = raw_str

            if scope_stack:
                # 向前探测下一个有效非空/非注释字符是否为 ':'
                j = i
                temp_line = line
                is_key = False
                while j < n:
                    if text[j] == "\n":
                        temp_line += 1
                        j += 1
                    elif text[j].isspace():
                        j += 1
                    elif text[j] == "/" and j + 1 < n and text[j + 1] == "/":
                        j += 2
                        while j < n and text[j] != "\n":
                            j += 1
                    elif text[j] == "/" and j + 1 < n and text[j + 1] == "*":
                        j += 2
                        while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                            if text[j] == "\n":
                                temp_line += 1
                            j += 1
                        j += 2
                    elif text[j] == ":":
                        is_key = True
                        break
                    else:
                        break

                if is_key:
                    current_scope = tuple(scope_stack)
                    keys.append((current_scope, decoded_str, str_start_line))
                    pending_key = decoded_str
            continue

        i += 1

    return keys


def check_file(filepath: str):
    """
    检查单个文件中的重复 Key
    返回: (has_error, error_count)
    """
    rel_path = os.path.relpath(filepath).replace("\\", "/")
    print(f"[CHECK] 正在检查: {rel_path} ...")

    try:
        keys_with_lines = extract_keys_with_lines(filepath)
    except Exception as e:
        print(f"[ERROR] 无法读取或解析文件 {rel_path}: {e}")
        return True, 1

    key_map = defaultdict(list)
    for scope, key, line_no in keys_with_lines:
        scoped_id = (scope, key)
        key_map[scoped_id].append(line_no)

    duplicates = {k: lines for k, lines in key_map.items() if len(lines) > 1}

    if not duplicates:
        print(f"[OK] {rel_path}: 查重通过，共 {len(keys_with_lines):,} 条词条，无重复原文。\n")
        return False, 0

    print(f"[ERROR] {rel_path}: 发现 {len(duplicates)} 处完全重复的原文翻译项！")
    for (scope, key), lines in duplicates.items():
        line_str = ", ".join([f"第 {l} 行" for l in lines])
        scope_str = " -> ".join(scope)
        # 输出 GitHub Actions Annotation 格式报错，直接在 PR/Commit 中精确定位
        print(f"::error file={rel_path},line={lines[-1]}::[原文查重失败] [{scope_str}] \"{key}\" 重复定义于 ({line_str})")
        print(f"   - 作用域: {scope_str}")
        print(f"   - 原文: \"{key}\"")
        print(f"     重复位置: ({line_str})\n")

    return True, len(duplicates)


def resolve_default_files():
    """解析默认待检查的翻译文件列表。"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    search_dirs = [
        os.path.join(project_root, "translations"),
        os.path.join(os.getcwd(), "translations"),
        project_root,
        os.getcwd()
    ]

    found_files = []
    seen = set()

    for s_dir in search_dirs:
        if os.path.isdir(s_dir):
            matches = glob.glob(os.path.join(s_dir, "*.jsonc"))
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(abs_p):
                    seen.add(abs_p)
                    found_files.append(abs_p)

    return found_files


def main():
    parser = argparse.ArgumentParser(
        description="翻译字典查重检测工具：检测 JSONC 字典中是否存在重复的原文 Key",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例用法:
  python scripts/check_duplicates.py
  python scripts/check_duplicates.py translations/qt_translations.jsonc translations/fgd_translations.jsonc
  python scripts/check_duplicates.py path/to/my_dict.jsonc
"""
    )
    parser.add_argument(
        "files",
        nargs="*",
        default=[],
        help="待检查的 JSONC 文件路径（留空自动检索 translations/*.jsonc）"
    )

    args = parser.parse_args()

    target_files = args.files
    if not target_files:
        target_files = resolve_default_files()

    if not target_files:
        print("[-] 未找到任何待检测的 JSON / JSONC 翻译文件。请指定文件路径，例如: python scripts/check_duplicates.py <file.jsonc>")
        sys.exit(1)

    total_errors = 0
    checked_files = 0

    print("==================================================")
    print("    CS2 Workshop Tools 翻译文本查重检测器")
    print("==================================================")

    for fpath in target_files:
        if not os.path.exists(fpath):
            print(f"[!] 跳过不存在的文件: {fpath}")
            continue
        has_error, err_count = check_file(fpath)
        checked_files += 1
        total_errors += err_count

    print("==================================================")
    if total_errors > 0:
        print(f"[FAILED] 查重检测失败！共检查 {checked_files} 个文件，发现 {total_errors} 处重复原文！")
        print("请检查上述报错位置并删除或合并重复项后重试。")
        sys.exit(1)
    else:
        print(f"[SUCCESS] 全部查重通过！共检查 {checked_files} 个文件，未发现任何重复原文。")
        sys.exit(0)


if __name__ == "__main__":
    main()
