#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CS2WorkshopToolsLocalizerCN - 翻译字典查重检测脚本
用于检测 JSON / JSONC 翻译文件中是否存在重复的原文 (Key)，并精确定位行号。
"""

import sys
import os
import glob
import json
from collections import defaultdict

# 确保在 Windows 控制台或非 UTF-8 环境下输出正常
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

def extract_keys_with_lines(filepath):
    """
    解析 JSON / JSONC 文件并提取所有 Key 及其所在起始行号。
    支持 // 单行注释 和 /* ... */ 多行注释，仅在 JSON 对象 {...} 内部提取 Key。
    """
    with open(filepath, "r", encoding="utf-8") as f:
        text = f.read()

    i = 0
    n = len(text)
    line = 1
    depth = 0  # 跟踪对象深度 {...}
    keys = []
    
    while i < n:
        c = text[i]
        
        # 换行处理
        if c == "\n":
            line += 1
            i += 1
            continue
            
        # 单行注释 // ...
        if c == "/" and i + 1 < n and text[i+1] == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
            continue
            
        # 多行注释 /* ... */
        if c == "/" and i + 1 < n and text[i+1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i+1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2  # 跳过 */
            continue
            
        # 跟踪大括号深度
        if c == "{":
            depth += 1
            i += 1
            continue
        elif c == "}":
            if depth > 0:
                depth -= 1
            i += 1
            continue
            
        # 字符串（仅在 depth > 0 时可能为字典 Key）
        if c == '"':
            str_start_line = line
            i += 1
            chars = []
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    chars.append(text[i:i+2])
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
                
            if depth > 0:
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
                    elif text[j] == "/" and j + 1 < n and text[j+1] == "/":
                        j += 2
                        while j < n and text[j] != "\n":
                            j += 1
                    elif text[j] == "/" and j + 1 < n and text[j+1] == "*":
                        j += 2
                        while j + 1 < n and not (text[j] == "*" and text[j+1] == "/"):
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
                    keys.append((decoded_str, str_start_line))
            continue
            
        i += 1
        
    return keys

def check_file(filepath):
    """
    检查单个文件中的重复 Key
    返回: (has_error, error_count)
    """
    rel_path = os.path.relpath(filepath).replace("\\", "/")
    print(f"[CHECK] 正在检查: {rel_path} ...")
    
    try:
        keys_with_lines = extract_keys_with_lines(filepath)
    except Exception as e:
        print(f"[ERROR] 无法读取文件 {rel_path}: {e}")
        return True, 1

    key_map = defaultdict(list)
    for key, line_no in keys_with_lines:
        key_map[key].append(line_no)

    duplicates = {k: lines for k, lines in key_map.items() if len(lines) > 1}

    if not duplicates:
        print(f"[OK] {rel_path}: 查重通过，共 {len(keys_with_lines)} 条翻译，无重复原文。\n")
        return False, 0

    print(f"[ERROR] {rel_path}: 发现 {len(duplicates)} 处完全重复的原文翻译项！")
    for key, lines in duplicates.items():
        line_str = ", ".join([f"line {l}" for l in lines])
        # 输出 GitHub Actions Annotation 格式报错，直接在 PR/Commit 中精确定位
        print(f"::error file={rel_path},line={lines[-1]}::[原文查重失败] \"{key}\" 重复定义于 ({line_str})")
        print(f"   - 原文: \"{key}\"")
        print(f"     重复位置: ({line_str})\n")

    return True, len(duplicates)

def main():
    target_files = sys.argv[1:]
    if not target_files:
        # 默认检查根目录下所有 json 文件（排除 build 等目录）
        target_files = [
            f for f in glob.glob("*.json")
            if os.path.isfile(f)
        ]
        if not target_files:
            target_files = ["fgd_translations.json", "qt_translations.json"]

    total_errors = 0
    checked_files = 0

    print("==================================================")
    print("    CS2 Workshop Tools 翻译文本查重检测器")
    print("==================================================")

    for fpath in target_files:
        if not os.path.exists(fpath):
            continue
        has_error, err_count = check_file(fpath)
        checked_files += 1
        total_errors += err_count

    print("==================================================")
    if total_errors > 0:
        print(f"[FAILED] 查重检测失败！共检查 {checked_files} 个文件，发现 {total_errors} 处重复原文！")
        print("请检查上述报错行并删除或合并重复项后重试。")
        sys.exit(1)
    else:
        print(f"[SUCCESS] 全部查重通过！共检查 {checked_files} 个文件，未发现任何重复原文。")
        sys.exit(0)

if __name__ == "__main__":
    main()

