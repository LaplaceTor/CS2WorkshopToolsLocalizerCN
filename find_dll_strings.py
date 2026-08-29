#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import glob
import os
import platform
import re
import subprocess
import sys


def extract_strings_from_bytes(data: bytes, min_length: int = 4):
    """从二进制数据中提取 ASCII 和 UTF-16LE 字符串及其在文件中的偏移量。"""
    results = []

    # 1. 提取 ASCII / UTF-8 格式的连续可打印字符
    # 可打印字符范围：0x20 - 0x7E 以及常见控制字符（\t, \r, \n）
    ascii_regex = re.compile(
        rb"[\x20-\x7e\t\r\n]{" + str(min_length).encode("ascii") + rb",}"
    )
    for match in ascii_regex.finditer(data):
        raw_bytes = match.group()
        try:
            text = raw_bytes.decode("utf-8", errors="ignore").strip()
            if len(text) >= min_length:
                results.append((match.start(), "ASCII/UTF-8", text))
        except Exception:
            continue

    # 2. 提取 UTF-16LE 宽字符格式字符串 (Windows DLL 常用 wchar_t)
    utf16_regex = re.compile(
        rb"(?:[\x20-\x7e\t\r\n]\x00){"
        + str(min_length).encode("ascii")
        + rb",}"
    )
    for match in utf16_regex.finditer(data):
        raw_bytes = match.group()
        try:
            text = raw_bytes.decode("utf-16le", errors="ignore").strip()
            if len(text) >= min_length:
                results.append((match.start(), "UTF-16LE", text))
        except Exception:
            continue

    # 按文件偏移量排序
    results.sort(key=lambda x: x[0])
    return results


def open_file_in_viewer(file_path: str):
    """在操作系统默认的文本编辑器/关联程序中打开文件。"""
    try:
        abs_path = os.path.abspath(file_path)
        system = platform.system()
        if system == "Windows":
            os.startfile(abs_path)
        elif system == "Darwin":  # macOS
            subprocess.run(["open", abs_path], check=True)
        else:  # Linux / Unix
            subprocess.run(["xdg-open", abs_path], check=True)
        print(f"[+] 已为您打开文件: {abs_path}")
    except Exception as e:
        print(f"[-] 自动打开文件失败: {e}，请手动打开: {file_path}", file=sys.stderr)


def resolve_file_paths(file_inputs: list) -> list:
    """根据输入的路径列表解析文件，支持通配符 (如 *.dll)、目录路径或具体文件。"""
    resolved = []
    seen = set()

    for item in file_inputs:
        if not item:
            continue

        # 1. 如果是现有的目录，则搜索该目录下所有的 .dll 文件
        if os.path.isdir(item):
            pattern = os.path.join(item, "*.dll")
            matches = glob.glob(pattern)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)
            continue

        # 2. 如果包含通配符 (*, ?, [)，使用 glob 匹配
        if any(char in item for char in ("*", "?", "[")):
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)
            continue

        # 3. 具体文件路径
        if os.path.isfile(item):
            abs_p = os.path.abspath(item)
            if abs_p not in seen:
                seen.add(abs_p)
                resolved.append(item)
        else:
            # 尝试通过 glob 解析
            matches = glob.glob(item, recursive=True)
            for p in sorted(matches):
                abs_p = os.path.abspath(p)
                if abs_p not in seen and os.path.isfile(p):
                    seen.add(abs_p)
                    resolved.append(p)

    return resolved


def process_single_file(
    file_path: str,
    compiled_pattern: re.Pattern,
    min_length: int = 4,
    show_offset: bool = False,
    unique: bool = False,
):
    """提取单个文件中的匹配字符串。"""
    try:
        with open(file_path, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"[-] 读取文件失败 ({file_path}): {e}", file=sys.stderr)
        return None

    extracted_strings = extract_strings_from_bytes(data, min_length=min_length)
    matched_lines = []
    seen = set()

    for offset, enc, text in extracted_strings:
        if compiled_pattern.search(text):
            if unique and text in seen:
                continue
            seen.add(text)

            if show_offset:
                line = f"[0x{offset:08X}] [{enc:<10}] {text}"
            else:
                line = text

            matched_lines.append(line)

    return matched_lines


def search_in_dll(
    file_path,
    pattern: str,
    output_file: str = None,
    min_length: int = 4,
    ignore_case: bool = False,
    show_offset: bool = False,
    unique: bool = False,
    auto_open: bool = None,
):
    """在 DLL 文件或文件列表中根据正则表达式搜索匹配的完整字符串并输出。"""
    if isinstance(file_path, str):
        file_inputs = [file_path]
    elif isinstance(file_path, (list, tuple)):
        file_inputs = list(file_path)
    else:
        file_inputs = [str(file_path)]

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

    # 单文件模式
    if len(file_paths) == 1:
        target_file = file_paths[0]
        if not output_file:
            base_name = os.path.splitext(os.path.basename(target_file))[0]
            output_file = f"{base_name}_matched_strings.txt"

        print(f"[*] 正在分析文件: {target_file}")
        print(f"[*] 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})")
        print(f"[*] 输出目标: {output_file}")
        print("-" * 70)

        matched_lines = process_single_file(
            target_file, compiled_pattern, min_length, show_offset, unique
        )
        if matched_lines is None:
            return 1

        for line in matched_lines[:20]:
            print(line)
        if len(matched_lines) > 20:
            print(f"... 其余 {len(matched_lines) - 20} 条结果已省略控制台显示，详情请查看输出文件 ...")

        try:
            with open(output_file, "w", encoding="utf-8") as out_f:
                out_f.write(f"# 分析文件: {os.path.abspath(target_file)}\n")
                out_f.write(f"# 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})\n")
                out_f.write(f"# 匹配总数: {len(matched_lines)}\n")
                out_f.write("=" * 70 + "\n\n")
                for line in matched_lines:
                    out_f.write(line + "\n")
            print("-" * 70)
            print(f"[*] 匹配完成，共找到 {len(matched_lines)} 条字符串，已保存到: {os.path.abspath(output_file)}")
        except Exception as e:
            print(f"[-] 写入文件失败: {e}", file=sys.stderr)
            return 1

    # 多文件批量模式
    else:
        if not output_file:
            output_file = "batch_matched_strings.txt"

        print(f"[*] 发现 {len(file_paths)} 个目标文件，开始批量扫描...")
        print(f"[*] 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})")
        print(f"[*] 输出目标: {output_file}")
        print("=" * 70)

        file_results = []
        total_matches = 0

        for idx, target_file in enumerate(file_paths, 1):
            matched_lines = process_single_file(
                target_file, compiled_pattern, min_length, show_offset, unique
            )
            if matched_lines is None:
                continue

            count = len(matched_lines)
            if count > 0:
                print(f"[+] [{idx}/{len(file_paths)}] {os.path.basename(target_file)} -> 匹配到 {count} 条")
                file_results.append((target_file, matched_lines))
                total_matches += count
            else:
                print(f"[-] [{idx}/{len(file_paths)}] {os.path.basename(target_file)} -> 无匹配项")

        # 写入合并报告文件
        try:
            with open(output_file, "w", encoding="utf-8") as out_f:
                out_f.write("# 批量扫描分析报告\n")
                out_f.write(f"# 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})\n")
                out_f.write(f"# 扫描文件总数: {len(file_paths)} 个 (其中 {len(file_results)} 个文件包含匹配项)\n")
                out_f.write(f"# 匹配字符串总数: {total_matches} 条\n")
                out_f.write("=" * 70 + "\n\n")

                if file_results:
                    for idx, (f_path, lines) in enumerate(file_results, 1):
                        out_f.write(f"## [{idx}/{len(file_results)}] 文件: {os.path.abspath(f_path)} (共 {len(lines)} 条匹配)\n")
                        out_f.write("-" * 70 + "\n")
                        for line in lines:
                            out_f.write(line + "\n")
                        out_f.write("\n\n")
                else:
                    out_f.write("# 未在任何文件中找到符合条件的匹配项。\n")

            print("=" * 70)
            print(f"[*] 批量搜索完成！共扫描 {len(file_paths)} 个文件，其中 {len(file_results)} 个文件匹配到 {total_matches} 条字符串。")
            print(f"[*] 结果已保存到: {os.path.abspath(output_file)}")
        except Exception as e:
            print(f"[-] 写入文件失败: {e}", file=sys.stderr)
            return 1

    # 提示用户是否打开文件
    if auto_open is True:
        open_file_in_viewer(output_file)
    elif auto_open is False:
        pass
    else:
        # 交互式询问用户
        try:
            choice = input("\n[?] 是否立即打开文本文件进行浏览? [Y/n]: ").strip().lower()
            if choice in ("", "y", "yes"):
                open_file_in_viewer(output_file)
        except (EOFError, KeyboardInterrupt):
            pass

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="从 DLL/二进制文件中提取并搜索包含指定正则/关键字的完整字符串，支持通配符与文件夹批量扫描，并输出到文本文件"
    )
    parser.add_argument(
        "-f",
        "--file",
        nargs="+",
        required=True,
        help="目标 DLL/二进制文件路径，支持通配符（如 *.dll、tools/*.dll）或文件夹路径",
    )
    parser.add_argument(
        "-p", "--pattern", required=True, help="搜索的字符串或正则表达式"
    )
    parser.add_argument(
        "-out",
        "--output",
        default=None,
        help="输出文本文件路径 (单文件默认: <dll名>_matched_strings.txt，批量默认: batch_matched_strings.txt)",
    )
    parser.add_argument(
        "-m",
        "--min-len",
        type=int,
        default=4,
        help="提取字符串的最小长度阈值 (默认: 4)",
    )
    parser.add_argument(
        "-i", "--ignore-case", action="store_true", help="忽略大小写匹配"
    )
    parser.add_argument(
        "-o", "--show-offset", action="store_true", help="显示字符串在文件中的 16 进制偏移量和编码"
    )
    parser.add_argument(
        "-u", "--unique", action="store_true", help="去除重复出现的字符串"
    )
    parser.add_argument(
        "--open", dest="auto_open", action="store_true", default=None, help="完成后直接自动打开文本文件"
    )
    parser.add_argument(
        "--no-open", dest="auto_open", action="store_false", help="完成后不提示打开文件"
    )

    args = parser.parse_args()

    sys.exit(
        search_in_dll(
            file_path=args.file,
            pattern=args.pattern,
            output_file=args.output,
            min_length=args.min_len,
            ignore_case=args.ignore_case,
            show_offset=args.show_offset,
            unique=args.unique,
            auto_open=args.auto_open,
        )
    )


if __name__ == "__main__":
    main()


