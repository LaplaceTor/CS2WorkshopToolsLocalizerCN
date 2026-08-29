#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
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


def search_in_dll(
    file_path: str,
    pattern: str,
    output_file: str = None,
    min_length: int = 4,
    ignore_case: bool = False,
    show_offset: bool = False,
    unique: bool = False,
    auto_open: bool = None,
):
    """在 DLL 文件中根据正则表达式搜索匹配的完整字符串并写入文本文件。"""
    if not os.path.isfile(file_path):
        print(f"[-] 错误: 文件不存在 -> {file_path}", file=sys.stderr)
        return

    try:
        flags = re.IGNORECASE if ignore_case else 0
        compiled_pattern = re.compile(pattern, flags)
    except re.error as e:
        print(f"[-] 正则表达式语法错误: {e}", file=sys.stderr)
        return

    try:
        with open(file_path, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"[-] 读取文件失败: {e}", file=sys.stderr)
        return

    # 若未指定输出文件名，则根据输入文件名自动生成
    if not output_file:
        base_name = os.path.splitext(os.path.basename(file_path))[0]
        output_file = f"{base_name}_matched_strings.txt"

    print(f"[*] 正在分析文件: {file_path}")
    print(f"[*] 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})")
    print(f"[*] 输出目标: {output_file}")
    print("-" * 70)

    extracted_strings = extract_strings_from_bytes(data, min_length=min_length)
    matched_lines = []
    seen = set()

    for offset, enc, text in extracted_strings:
        # 执行正则搜索匹配
        if compiled_pattern.search(text):
            if unique and text in seen:
                continue
            seen.add(text)

            if show_offset:
                line = f"[0x{offset:08X}] [{enc:<10}] {text}"
            else:
                line = text

            matched_lines.append(line)
            # 在控制台实时展示前 20 条，避免控制台刷屏过多
            if len(matched_lines) <= 20:
                print(line)

    if len(matched_lines) > 20:
        print(f"... 其余 {len(matched_lines) - 20} 条结果已省略控制台显示，详情请查看输出文件 ...")

    # 写入结果到文本文件
    try:
        with open(output_file, "w", encoding="utf-8") as out_f:
            out_f.write(f"# 分析文件: {os.path.abspath(file_path)}\n")
            out_f.write(f"# 匹配规则: /{pattern}/ (区分大小写: {not ignore_case})\n")
            out_f.write(f"# 匹配总数: {len(matched_lines)}\n")
            out_f.write("=" * 70 + "\n\n")
            for line in matched_lines:
                out_f.write(line + "\n")
        print("-" * 70)
        print(f"[*] 匹配完成，共找到 {len(matched_lines)} 条字符串，已保存到: {os.path.abspath(output_file)}")
    except Exception as e:
        print(f"[-] 写入文件失败: {e}", file=sys.stderr)
        return

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


def main():
    parser = argparse.ArgumentParser(
        description="从 DLL/二进制文件中提取并搜索包含指定正则/关键字的完整字符串，并输出到文本文件"
    )
    parser.add_argument(
        "-f", "--file", required=True, help="目标 DLL 或二进制文件路径"
    )
    parser.add_argument(
        "-p", "--pattern", required=True, help="搜索的字符串或正则表达式"
    )
    parser.add_argument(
        "-out", "--output", default=None, help="输出文本文件路径 (默认: <dll名>_matched_strings.txt)"
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


if __name__ == "__main__":
    main()

