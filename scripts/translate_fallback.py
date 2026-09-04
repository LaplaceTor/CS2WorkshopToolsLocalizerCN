 #!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CS2WorkshopToolsLocalizerCN - 大模型汉化翻译工具 (OpenAI 兼容 / 本地 / 云端通用版)
面向 Valve Source 2 / CS2 创意工坊开发工具集（Hammer 地图编辑器、Material Editor 材质编辑器、
ModelDoc 3D模型编辑器、Asset Browser 资产浏览器、AnimGraph 等）的专业汉化翻译引擎。

功能特性与设计理念：
1. 通用 API 兼容：支持任何兼容 OpenAI 接口规范的本地或云端大模型服务
   - 本地推理后端：llama.cpp, Ollama, vLLM, LM Studio, Text-Generation-WebUI 等。
   - 云端推理接口：OpenAI, DeepSeek, Qwen, GLM 等。
   - 支持通过命令行参数或环境变量 (TRANSLATION_API_URL, OPENAI_API_BASE, OPENAI_API_KEY, TRANSLATION_MODEL) 配置。
2. 3D 建模与关卡设计前置提示词：
   - 注入 Source 2 核心术语库（Translate -> 平移, Normal -> 法线, Bake -> 烘焙, Mutator -> 修改器 等）。
   - 智能自判技术标识符（保持 rsa-xxx, gl_Position, RGBA8888, KeyValues3 等原样不翻译）。
3. 闭环防积压多线程批处理架构：
   - 默认按批打包（10 行/批）输入模型，大幅提升上下文理解与响应速度（~0.8s~1.2s/行）。
   - 各线程严格“取 1 个批次 -> 请求模型 -> 清洗格式 -> 写入硬盘 -> 再取下 1 批”，随时 Ctrl+C 仅需数秒即可平稳落盘保存，不向模型后端积压多余请求队列。
4. 占位符与标点符号智能防护：
   - 自动检测并剔除模型编造的不存在占位符。
   - 纠正全角变形（％s -> %s，% s -> %s，{ 0 } -> {0}）。
   - 严格保留冒号、省略号、问号、感叹号、快捷键后缀（如 [Shift+X], (Ctrl+Z)）与首尾排版空格。
5. 完美保留 JSONC 注释：行级精确解析与替换，100% 保持 fgd_fallback.jsonc 中的 // 注释与版块排版。
"""

import argparse
import json
import os
import re
import signal
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import OrderedDict

# Windows 控制台 UTF-8 编码兼容
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

# 全局中断控制标记
_STOP_REQUESTED = False


def signal_handler(signum, frame):
    """捕获中断信号，设置退出标记"""
    global _STOP_REQUESTED
    if not _STOP_REQUESTED:
        print("\n\n[!] 接收到中断信号 (Ctrl+C)，正在等待当前小批次完成并保存进度，请稍候...")
        _STOP_REQUESTED = True
    else:
        print("\n[!] 强制中断中...")
        sys.exit(130)


signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)


# ==============================================================================
# 专业领域系统提示词与术语表
# ==============================================================================

SYSTEM_PROMPT = (
    "你是一个专业的游戏开发与 3D 工具本地化翻译引擎，专门针对 Valve Source 2 引擎与 CS2 创意工坊开发工具集"
    "（包括 Hammer 3D 地图编辑器、Material Editor 材质编辑器、ModelDoc 3D 模型编辑器、Asset Browser 资产浏览器、FGD 实体定义、AnimGraph 动画图等）。\n"
    "请将给定的英文 UI 界面文本、编辑器工具文本和实体定义描述文本准确翻译为简体中文（zh-CN）。\n\n"
    "【核心翻译原则与无需翻译项判定】\n"
    "1. 智能判定无需翻译项（极重要：技术标识符必须 100% 保持英文原文，切勿生硬翻译）：\n"
    "   - 加密、哈希、算法与协议名称（如 'rsa-sha2-256', 'aes256-gcm', 'diffie-hellman-xxx', 'hmac-sha1', 'sha256', 'md5', 'base64'）\n"
    "   - 代码标识符、API 符号、着色器变量与宏定义（如 'gl_Position', 'D3D11_USAGE_xxx', 'nullptr', 'size_t', 'int32'）\n"
    "   - 文件名、文件扩展名、可执行程序名、URL 网址与文件路径（如 'vconsole2.exe', 'hammer.dll', '.vmdl', '.vmat', 'http://...'）\n"
    "   - 贴图纹理格式与图形标准（如 'RGBA8888', 'BC7', 'DXT5', 'sRGB', 'Vulkan', 'Direct3D', 'OpenGL'）\n"
    "   - 引擎专有格式与技术品牌名（如 'KeyValues3', 'KV3', 'Qt', 'JSON', 'XML'）\n"
    "   - 标准字体名称（如 'Times New Roman', 'Arial'）\n"
    "   - 内部底层技术键名、UUID、哈希值或非 UI 代码标识\n"
    "   - 若某个条目无需翻译或属于代码常量，请直接输出其英文原文（无需翻译时保持原文）\n\n"
    "2. 3D 编辑器与创意工坊专业术语标准：\n"
    "   - 3D 变换：'Translate' / 'Translation' -> '平移' / '位移'（极重要：在 3D 工具语境中 Translate 表示平移操作，严禁翻译为“翻译”！）；'Rotate' / 'Rotation' -> '旋转'；'Scale' -> '缩放'；'Transform' -> '变换'\n"
    "   - 3D 几何与建模：'Normal'（3D 语境）-> '法线'（严禁翻译为“正常”）；'Normal Map' -> '法线贴图'；'Bake' / 'Baking' -> '烘焙'；'Snap' / 'Grid Snap' -> '吸附' / '网格吸附'；'Extrude' -> '挤出'；'Bevel' -> '倒角'；'Inset' -> '内嵌'；'Bridge' -> '桥接'；'Weld' -> '焊接'；'Edge Loop' -> '循环边'\n"
    "   - 材质与贴图：'Material' -> '材质'（严禁翻译为“材料”或“素材”）；'Texture' -> '纹理' 或 '贴图'；'Mesh' -> '网格'；'Pivot' -> '轴点'；'Cordon' -> '屏蔽区'；'Workplane' -> '工作平面'；'Selection Set' -> '可选组'；'Vis Contributor' -> 'Vis贡献者'；'World Layer' -> '世界层'\n"
    "   - 实体与资产：'Prop' / 'Props' -> '模型' / 'Prop'；'Prefab' -> 'Prefab'；'Instance' -> '实例'；'Steam Audio' -> 'Steam音频'；'Reverb' -> '混响'；'Pathing' -> '寻路'\n"
    "   - KV3 / 材质语境：'Mutator' / 'Property Mutators' -> '修改器' / '属性修改器'（严禁翻译为“变异器”或“变异体”）\n\n"
    "3. 格式与排版要求：\n"
    "   - 严格原样保留所有格式占位符（%s, %d, {0}, \\n, \\t 等）以及快捷键后缀（如 [Shift+X], (Ctrl+Z), \\tCtrl+S）\n"
    "   - UI 按钮、菜单项和标签标题结尾切勿擅自添加句号\n"
    "   - 严格以 JSON 字符串数组格式返回：[\"翻译1\", \"翻译2\", ...]，数组元素数量必须与输入数组严格完全一致。仅输出该 JSON 数组，严禁包含任何 Markdown 标记或多余解释。"
)


# ==============================================================================
# 占位符与标点符号清洗/校验模块
# ==============================================================================

PLACEHOLDER_REGEX = re.compile(
    r'(?:'
    r'%(\d+\$)?[#0\- +\'I]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|ll|[hlLzjtI64I32])?[diouxXeEfFgGaAcpsn%]'
    r'|%[0-9]+(?:![^!\r\n]+!)?'
    r'|\{[0-9a-zA-Z_]+\}'
    r'|\\n|\\t|\\r'
    r')'
)

HOTKEY_SUFFIX_REGEX = re.compile(r'(\s*(?:\[[^\]]+\]|\([A-Za-z0-9+ \-_]+\)|\t[A-Za-z0-9+ \-_]+))\s*$')


def normalize_fullwidth_placeholders(s: str) -> str:
    """归一化模型误生成的全角占位符与多余空格"""
    s = re.sub(r'％\s*([0-9]*[a-zA-Z%])', r'%\1', s)
    s = re.sub(r'%\s+([0-9]*[a-zA-Z%])', r'%\1', s)
    s = re.sub(r'\{\s*([0-9a-zA-Z_]+)\s*\}', r'{\1}', s)
    return s


def extract_placeholders(s: str) -> list:
    """提取字符串中所有的格式占位符"""
    return [m.group(0) for m in PLACEHOLDER_REGEX.finditer(s)]


def clean_and_align_translation(src: str, trans: str) -> str:
    """对模型生成的翻译结果进行结构对齐、占位符修复与标点清洗"""
    if not trans:
        return src

    trans = normalize_fullwidth_placeholders(trans)

    # 剥离模型输出的包裹性 Markdown 代码块或 JSON 结构
    trans = re.sub(r'^```(?:json)?\s*', '', trans, flags=re.IGNORECASE)
    trans = re.sub(r'\s*```$', '', trans)
    trans = trans.strip()

    # 剥离残留的 JSON 字典/数组格式
    if trans.startswith('{"') and trans.endswith('"}'):
        m = re.match(r'^\{"[^"]*"\s*:\s*"((?:[^"\\]|\\.)*)"\}$', trans)
        if m:
            trans = m.group(1)
    elif trans.startswith('["') and trans.endswith('"]'):
        m = re.match(r'^\["((?:[^"\\]|\\.)*)"\]$', trans)
        if m:
            trans = m.group(1)

    src_leading_spaces = len(src) - len(src.lstrip(' '))
    src_trailing_spaces = len(src) - len(src.rstrip(' '))

    src_core = src.strip()
    trans_core = trans.strip()

    # 快捷键后缀处理
    src_hotkey_match = HOTKEY_SUFFIX_REGEX.search(src_core)
    src_hotkey = src_hotkey_match.group(1) if src_hotkey_match else ""
    if src_hotkey:
        src_without_hotkey = src_core[:-len(src_hotkey)].rstrip()
    else:
        src_without_hotkey = src_core

    trans_hotkey_match = HOTKEY_SUFFIX_REGEX.search(trans_core)
    if trans_hotkey_match:
        trans_without_hotkey = trans_core[:-len(trans_hotkey_match.group(1))].rstrip()
    else:
        trans_without_hotkey = trans_core

    # 占位符校验与清洗
    src_phs = extract_placeholders(src_without_hotkey)
    trans_phs = extract_placeholders(trans_without_hotkey)

    if not src_phs and trans_phs:
        trans_without_hotkey = PLACEHOLDER_REGEX.sub('', trans_without_hotkey)
        trans_without_hotkey = re.sub(r'\s+([.。!！?？:：])', r'\1', trans_without_hotkey)
        trans_without_hotkey = re.sub(r'\s{2,}', ' ', trans_without_hotkey).strip()

    # 保证前导占位符对齐（例如原文以 %s 开头但译文漏掉）
    if src_phs and src_without_hotkey.startswith(src_phs[0]) and not trans_without_hotkey.startswith(src_phs[0]):
        trans_without_hotkey = src_phs[0] + trans_without_hotkey

    # 标点符号对齐
    if src_without_hotkey.endswith(('...', '…')):
        if not trans_without_hotkey.endswith(('...', '…')):
            trans_without_hotkey = re.sub(r'[.。…]+$', '', trans_without_hotkey).rstrip() + '...'
    elif src_without_hotkey.endswith((':', '：')):
        trans_without_hotkey = re.sub(r'[:：.。]+$', '', trans_without_hotkey).rstrip() + '：'
    elif src_without_hotkey.endswith('?'):
        if not trans_without_hotkey.endswith(('?', '？')):
            trans_without_hotkey = re.sub(r'[.。!！?？]+$', '', trans_without_hotkey).rstrip() + '？'
    elif src_without_hotkey.endswith('!'):
        if not trans_without_hotkey.endswith(('!', '！')):
            trans_without_hotkey = re.sub(r'[.。!！?？]+$', '', trans_without_hotkey).rstrip() + '！'
    elif src_without_hotkey.endswith('.'):
        if not trans_without_hotkey.endswith(('.', '。')):
            trans_without_hotkey = trans_without_hotkey.rstrip() + '。'
    else:
        # 原文末尾无句号时，剔除模型多余添加的句号与空白
        trans_without_hotkey = re.sub(r'[.。]+$', '', trans_without_hotkey).rstrip()

    # 组装快捷键
    if src_hotkey:
        final_core = trans_without_hotkey + src_hotkey
    else:
        final_core = trans_without_hotkey

    final_result = (' ' * src_leading_spaces) + final_core + (' ' * src_trailing_spaces)
    return final_result


# ==============================================================================
# 模型 API 通信与批处理解析
# ==============================================================================

def normalize_api_url(url: str) -> str:
    """智能归一化 API 地址为 /v1/chat/completions 端点"""
    url = url.strip().rstrip('/')
    if not url:
        return "http://127.0.0.1:8080/v1/chat/completions"
    if url.endswith("/v1/chat/completions"):
        return url
    if url.endswith("/chat/completions"):
        return url
    if url.endswith("/v1"):
        return f"{url}/chat/completions"
    return f"{url}/v1/chat/completions"


def parse_batch_json_response(raw_text: str, expected_len: int):
    """解析模型返回的 JSON 数组文本"""
    if not raw_text:
        return None

    cleaned = raw_text.strip()
    cleaned = re.sub(r'^```(?:json)?\s*', '', cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r'\s*```$', '', cleaned).strip()

    def try_parse(json_str):
        try:
            val = json.loads(json_str)
            if isinstance(val, list):
                flattened = []
                for item in val:
                    if isinstance(item, str):
                        flattened.append(item)
                    elif isinstance(item, dict):
                        for v in item.values():
                            if isinstance(v, str):
                                flattened.append(v)
                                break
                    else:
                        flattened.append(str(item))
                if len(flattened) == expected_len:
                    return flattened
            elif isinstance(val, dict):
                for k in ["translations", "result", "output", "data"]:
                    if k in val and isinstance(val[k], list) and len(val[k]) == expected_len:
                        return [str(x) for x in val[k]]
        except Exception:
            pass
        return None

    res = try_parse(cleaned)
    if res is not None:
        return res

    start_idx = cleaned.find('[')
    end_idx = cleaned.rfind(']')
    if start_idx != -1 and end_idx != -1 and end_idx > start_idx:
        res = try_parse(cleaned[start_idx:end_idx + 1])
        if res is not None:
            return res

    return None


def call_translation_api_batch(
    texts: list,
    api_url: str,
    api_key: str = None,
    model: str = None,
    timeout: float = 60.0,
    temperature: float = 0.7,
    top_p: float = 1.0,
    top_k: int = -1,
    repetition_penalty: float = 1.0,
    max_tokens: int = 4096
) -> list:
    """批量调用 OpenAI 兼容大模型 API"""
    if not texts:
        return []

    if len(texts) == 1:
        single_res = call_translation_api_single(
            texts[0], api_url, api_key=api_key, model=model, timeout=timeout,
            temperature=temperature, top_p=top_p, top_k=top_k,
            repetition_penalty=repetition_penalty, max_tokens=max_tokens
        )
        return [single_res]

    payload = {
        "messages": [
            {
                "role": "system",
                "content": SYSTEM_PROMPT
            },
            {
                "role": "user",
                "content": json.dumps(texts, ensure_ascii=False)
            }
        ],
        "temperature": temperature,
        "top_p": top_p,
        "top_k": top_k,
        "repetition_penalty": repetition_penalty,
        "max_tokens": max_tokens,
        "stream": False
    }

    if model:
        payload["model"] = model

    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req_data = json.dumps(payload).encode('utf-8')
    req = urllib.request.Request(api_url, data=req_data, headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            res_body = response.read().decode('utf-8')
            res_json = json.loads(res_body)

            raw_content = ""
            if "choices" in res_json and len(res_json["choices"]) > 0:
                choice = res_json["choices"][0]
                if "message" in choice and "content" in choice["message"]:
                    raw_content = choice["message"]["content"]
                elif "text" in choice:
                    raw_content = choice["text"]
            elif "content" in res_json:
                raw_content = res_json["content"]

            parsed_list = parse_batch_json_response(raw_content, len(texts))
            if parsed_list is not None:
                return parsed_list

    except Exception:
        pass

    # 若批处理返回异常，自动回退逐条处理
    fallback_results = []
    for t in texts:
        if _STOP_REQUESTED:
            break
        r = call_translation_api_single(
            t, api_url, api_key=api_key, model=model, timeout=30.0,
            temperature=temperature, top_p=top_p, top_k=top_k,
            repetition_penalty=repetition_penalty, max_tokens=max_tokens
        )
        fallback_results.append(r)
    return fallback_results


def call_translation_api_single(
    text: str,
    api_url: str,
    api_key: str = None,
    model: str = None,
    timeout: float = 30.0,
    temperature: float = 0.7,
    top_p: float = 1.0,
    top_k: int = -1,
    repetition_penalty: float = 1.0,
    max_tokens: int = 4096
) -> str:
    """单条调用翻译 API"""
    if not text or not text.strip():
        return text

    payload = {
        "messages": [
            {
                "role": "system",
                "content": SYSTEM_PROMPT
            },
            {
                "role": "user",
                "content": f"请将以下文本翻译为简体中文：{text}"
            }
        ],
        "temperature": temperature,
        "top_p": top_p,
        "top_k": top_k,
        "repetition_penalty": repetition_penalty,
        "max_tokens": max_tokens,
        "stream": False
    }

    if model:
        payload["model"] = model

    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req_data = json.dumps(payload).encode('utf-8')
    req = urllib.request.Request(api_url, data=req_data, headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            res_body = response.read().decode('utf-8')
            res_json = json.loads(res_body)

            if "choices" in res_json and len(res_json["choices"]) > 0:
                choice = res_json["choices"][0]
                if "message" in choice and "content" in choice["message"]:
                    return choice["message"]["content"].strip()
                elif "text" in choice:
                    return choice["text"].strip()
    except Exception:
        pass

    return text


# ==============================================================================
# 文件多线程闭环翻译调度
# ==============================================================================

def chunk_list(lst: list, chunk_size: int) -> list:
    """将列表切分为固定大小的批次"""
    return [lst[i:i + chunk_size] for i in range(0, len(lst), chunk_size)]


def find_file_path(filename: str) -> str:
    """查找 JSONC 文件的绝对路径"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    base, ext = os.path.splitext(filename)
    target_name = base + ".jsonc" if ext.lower() != ".jsonc" else filename

    candidates = [
        target_name,
        os.path.join("translations", target_name),
        os.path.join(project_root, "translations", target_name),
        os.path.join(project_root, target_name),
        os.path.join(script_dir, target_name),
    ]

    for p in candidates:
        if os.path.exists(p):
            return os.path.abspath(p)

    return os.path.abspath(os.path.join(project_root, "translations", target_name))


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
    cleaned = re.sub(r',\s*([\}\]])', r'\1', cleaned)
    return cleaned


def translate_fgd_fallback(
    filepath: str,
    api_url: str,
    api_key: str = None,
    model: str = None,
    num_threads: int = 3,
    batch_size: int = 10,
    force: bool = False,
    max_items: int = 0,
    gen_params: dict = None
):
    """翻译 fgd_fallback.jsonc (完美保留 JSONC 注释与版块排版，当不翻译时自动剔除对应行)"""
    gen_params = gen_params or {}
    print(f"\n[*] 开始处理 FGD 实体定义文件 (JSONC 模式): {filepath}")

    if not os.path.exists(filepath):
        print(f"[!] 错误: 文件不存在: {filepath}")
        return

    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    entry_pattern = re.compile(r'^(\s*)"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"(\s*,?\s*(?://.*)?)$')

    tasks = []
    for line_idx, line in enumerate(lines):
        line_clean = line.rstrip('\r\n')
        m = entry_pattern.match(line_clean)
        if m:
            raw_k = m.group(2)
            raw_v = m.group(3)
            try:
                k = json.loads(f'"{raw_k}"')
            except Exception:
                k = raw_k
            try:
                v = json.loads(f'"{raw_v}"')
            except Exception:
                v = raw_v

            if force or not v.strip():
                tasks.append({
                    "line_idx": line_idx,
                    "indent": m.group(1),
                    "key": k,
                    "raw_key": raw_k,
                    "suffix": m.group(4)
                })

    total_tasks = len(tasks)
    print(f"[-] 总行数: {len(lines)} | 待翻译词条数: {total_tasks}")

    if total_tasks == 0:
        print("[+] 所有词条均已翻译完成，无需处理。")
        return

    batches = chunk_list(tasks, batch_size)
    batch_iter = iter(enumerate(batches, 1))
    batch_lock = threading.Lock()
    file_lock = threading.Lock()

    translated_count = 0
    deleted_count = 0

    def atomic_save():
        with file_lock:
            temp_file = filepath + ".tmp"
            active_lines = [l for l in lines if l is not None]
            with open(temp_file, 'w', encoding='utf-8') as f:
                f.writelines(active_lines)
            os.replace(temp_file, filepath)

    def worker_loop(worker_id: int):
        nonlocal translated_count, deleted_count
        while not _STOP_REQUESTED:
            with batch_lock:
                if max_items > 0 and (translated_count + deleted_count) >= max_items:
                    break
                try:
                    b_idx, batch_tasks = next(batch_iter)
                except StopIteration:
                    break

            keys = [t["key"] for t in batch_tasks]
            start_t = time.time()
            try:
                raw_translations = call_translation_api_batch(
                    keys, api_url, api_key=api_key, model=model, **gen_params
                )
            except Exception as e:
                print(f"\n[!] [Worker-{worker_id}] 批次请求出错: {e}")
                break

            cost_t = time.time() - start_t
            cleaned_translations = []
            for k, raw_trans in zip(keys, raw_translations):
                cleaned = clean_and_align_translation(k, raw_trans)
                cleaned_translations.append(cleaned)

            with file_lock:
                for t_item, cleaned_trans in zip(batch_tasks, cleaned_translations):
                    line_idx = t_item["line_idx"]
                    raw_k = t_item["raw_key"]
                    k = t_item["key"]

                    if cleaned_trans.strip() == k.strip():
                        # 模型输出等于输入（不翻译），从 JSONC 中删除对应行
                        lines[line_idx] = None
                        # 如果前一行是来源注释，也一并删除
                        if line_idx > 0 and lines[line_idx - 1] is not None:
                            prev_line = lines[line_idx - 1].strip()
                            if prev_line.startswith("// 来源:") or prev_line.startswith("// Source:"):
                                lines[line_idx - 1] = None
                        deleted_count += 1
                    else:
                        indent = t_item["indent"]
                        suffix = t_item["suffix"]
                        esc_val = json.dumps(cleaned_trans, ensure_ascii=False)[1:-1]
                        lines[line_idx] = f'{indent}"{raw_k}": "{esc_val}"{suffix}\n'
                        translated_count += 1

                temp_file = filepath + ".tmp"
                active_lines = [l for l in lines if l is not None]
                with open(temp_file, 'w', encoding='utf-8') as f:
                    f.writelines(active_lines)
                os.replace(temp_file, filepath)

                sample_k = keys[0][:20]
                sample_t = cleaned_translations[0][:20]
                processed_total = translated_count + deleted_count
                progress_pct = (processed_total / total_tasks) * 100
                avg_speed = cost_t / len(keys) if keys else 0
                is_del = (sample_t.strip() == sample_k.strip())
                print(f"[{processed_total}/{total_tasks}] ({progress_pct:.1f}%) "
                      f"[T{worker_id} | 批次{len(keys)}条 | {cost_t:.2f}s (均{avg_speed:.2f}s/条)] "
                      f"{repr(sample_k)} -> {repr(sample_t)}"
                      f"{' (未翻译已剔除)' if is_del else ''}")

    threads = []
    actual_threads = min(num_threads, len(batches))
    for t_id in range(actual_threads):
        t = threading.Thread(target=worker_loop, args=(t_id + 1,), daemon=False)
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    atomic_save()
    print(f"[+] FGD 处理完成/中断保存: 本次成功翻译 {translated_count} 条，剔除不翻译词条 {deleted_count} 条，文件已安全写入硬盘。\n")


def translate_qt_fallback(
    filepath: str,
    api_url: str,
    api_key: str = None,
    model: str = None,
    num_threads: int = 3,
    batch_size: int = 10,
    force: bool = False,
    max_items: int = 0,
    gen_params: dict = None
):
    """翻译 qt_fallback.jsonc (JSONC 字典，当不翻译时自动从字典中删除对应 Key)"""
    gen_params = gen_params or {}
    print(f"\n[*] 开始处理 Qt 界面词条文件 (JSON 模式): {filepath}")

    if not os.path.exists(filepath):
        print(f"[!] 错误: 文件不存在: {filepath}")
        return

    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    cleaned_json = strip_jsonc_comments(content)
    try:
        data = json.loads(cleaned_json, object_pairs_hook=OrderedDict)
    except Exception as e:
        print(f"[!] 解析 JSON 失败: {e}")
        return

    total_entries = len(data)
    pending_keys = [k for k, v in data.items() if force or not (isinstance(v, str) and v.strip())]
    already_done = total_entries - len(pending_keys)

    print(f"[-] 总条目数: {total_entries} | 已翻译: {already_done} | 待翻译: {len(pending_keys)}")

    if not pending_keys:
        print("[+] 所有条目均已翻译完成，无需处理。")
        return

    batches = chunk_list(pending_keys, batch_size)
    batch_iter = iter(enumerate(batches, 1))
    batch_lock = threading.Lock()
    file_lock = threading.Lock()

    translated_count = 0
    deleted_count = 0

    def atomic_save():
        with file_lock:
            temp_file = filepath + ".tmp"
            with open(temp_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
                f.write('\n')
            os.replace(temp_file, filepath)

    def worker_loop(worker_id: int):
        nonlocal translated_count, deleted_count
        while not _STOP_REQUESTED:
            with batch_lock:
                if max_items > 0 and (translated_count + deleted_count) >= max_items:
                    break
                try:
                    b_idx, keys = next(batch_iter)
                except StopIteration:
                    break

            start_t = time.time()
            try:
                raw_translations = call_translation_api_batch(
                    keys, api_url, api_key=api_key, model=model, **gen_params
                )
            except Exception as e:
                print(f"\n[!] [Worker-{worker_id}] 批次请求出错: {e}")
                break

            cost_t = time.time() - start_t
            cleaned_pairs = []
            for k, raw_trans in zip(keys, raw_translations):
                cleaned = clean_and_align_translation(k, raw_trans)
                cleaned_pairs.append((k, cleaned))

            with file_lock:
                for k, cleaned_trans in cleaned_pairs:
                    if cleaned_trans.strip() == k.strip():
                        # 模型输出等于输入（不翻译），从字典中删除对应 Key
                        data.pop(k, None)
                        deleted_count += 1
                    else:
                        data[k] = cleaned_trans
                        translated_count += 1

                temp_file = filepath + ".tmp"
                with open(temp_file, 'w', encoding='utf-8') as f:
                    json.dump(data, f, ensure_ascii=False, indent=2)
                    f.write('\n')
                os.replace(temp_file, filepath)

                sample_k = keys[0][:20]
                sample_t = cleaned_pairs[0][1][:20]
                processed_total = translated_count + deleted_count
                progress_pct = ((already_done + processed_total) / total_entries) * 100
                avg_speed = cost_t / len(keys) if keys else 0
                is_del = (sample_t.strip() == sample_k.strip())
                print(f"[{processed_total}/{len(pending_keys)}] ({progress_pct:.1f}%) "
                      f"[T{worker_id} | 批次{len(keys)}条 | {cost_t:.2f}s (均{avg_speed:.2f}s/条)] "
                      f"{repr(sample_k)} -> {repr(sample_t)}"
                      f"{' (未翻译已剔除)' if is_del else ''}")

    threads = []
    actual_threads = min(num_threads, len(batches))
    for t_id in range(actual_threads):
        t = threading.Thread(target=worker_loop, args=(t_id + 1,), daemon=False)
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    atomic_save()
    print(f"[+] Qt 处理完成/中断保存: 本次成功翻译 {translated_count} 条，剔除不翻译词条 {deleted_count} 条，文件已安全写入硬盘。\n")


def run_unit_tests():
    """单元测试：验证各类占位符与标点符号清洗规则"""
    print("[*] 正在运行清洗规则单元测试...")
    test_cases = [
        ("Option must have a name.", "选项必须有一个名称。", "选项必须有一个名称。"),
        ("Mutator", "变异器。", "变异器"),
        ("Target must have Attr: '%s'", "目标必须包含属性：“%s”", "目标必须包含属性：“%s”"),
        ("Pre-Generated Output Material", "预生成的输出材质。", "预生成的输出材质"),
        ("Save As...", "另存为...", "另存为..."),
        ("Name:", "名称：", "名称："),
        ("Clipping Tool [Shift+X]", "裁剪工具", "裁剪工具 [Shift+X]"),
        ("Undo (Ctrl+Z)", "撤销 (Ctrl+Z)", "撤销 (Ctrl+Z)"),
        ("No placeholders here", "这里没有占位符 %s。", "这里没有占位符"),
        ("Test %s value", "测试 ％ s 数值", "测试 %s 数值"),
        (" move ", " 移动 ", " 移动 "),
        ("%sCopy Property %s:'%s' to '%s'", "复制属性 %s:'%s' to '%s'", "%s复制属性 %s:'%s' to '%s'"),
    ]

    passed = 0
    for src, trans, expected in test_cases:
        res = clean_and_align_translation(src, trans)
        if res == expected:
            passed += 1
            print(f" [PASS] {repr(src)} -> {repr(res)}")
        else:
            print(f" [FAIL] {repr(src)}\n        Got:      {repr(res)}\n        Expected: {repr(expected)}")

    print(f"\n[*] 测试完成: {passed}/{len(test_cases)} 通过。")


def main():
    default_api_url = (
        os.environ.get("TRANSLATION_API_URL")
        or os.environ.get("OPENAI_API_BASE")
        or "http://127.0.0.1:8080/v1/chat/completions"
    )
    default_api_key = os.environ.get("OPENAI_API_KEY", "")
    default_model = os.environ.get("TRANSLATION_MODEL") or os.environ.get("OPENAI_MODEL", "")

    parser = argparse.ArgumentParser(
        description="大模型汉化翻译工具：支持本地 (llama.cpp/Ollama/vLLM) 与云端 (OpenAI/DeepSeek) 接口批量翻译 CS2 字典",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例用法:
  # 1. 使用默认本地模型 (127.0.0.1:8080) 翻译所有字典 (Qt + FGD)
  python scripts/translate_fallback.py

  # 2. 指定并发线程数与单批次行数
  python scripts/translate_fallback.py --file qt --threads 3 --batch-size 10

  # 3. 使用第三方 / 云端 OpenAI 兼容接口 (如 DeepSeek 或 Ollama)
  python scripts/translate_fallback.py --api-url https://api.deepseek.com/v1/chat/completions --api-key sk-xxxx --model deepseek-chat

  # 4. 强制重新翻译已有内容的条目
  python scripts/translate_fallback.py --file fgd --force
"""
    )
    parser.add_argument(
        "--file", "-f",
        choices=["all", "fgd", "qt"],
        default="all",
        help="指定要翻译的目标文件 (默认: all，依次处理 fgd 和 qt)"
    )
    parser.add_argument(
        "--threads", "-j",
        type=int,
        default=3,
        help="并发线程数 (默认: 3，建议 2~4 线程以兼顾吞吐与交互响应)"
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=10,
        help="单次向模型发送的词条行数 (默认: 10，提供充足上下文且保持高速响应)"
    )
    parser.add_argument(
        "--fgd-path",
        default=None,
        help="fgd_fallback.jsonc 路径 (默认自动查找 translations/fgd_fallback.jsonc)"
    )
    parser.add_argument(
        "--qt-path",
        default=None,
        help="qt_fallback.jsonc 路径 (默认自动查找 translations/qt_fallback.jsonc)"
    )
    parser.add_argument(
        "--api-url",
        default=default_api_url,
        help=f"OpenAI 兼容 API 接口地址 (默认: {default_api_url}，或读取 TRANSLATION_API_URL / OPENAI_API_BASE)"
    )
    parser.add_argument(
        "--api-key",
        default=default_api_key,
        help="API 访问密钥 (可选，支持读取 OPENAI_API_KEY)"
    )
    parser.add_argument(
        "--model",
        default=default_model,
        help="模型名称 (可选，如 deepseek-chat / gpt-4o-mini / qwen-turbo，支持读取 TRANSLATION_MODEL)"
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="强制重新翻译已有内容的条目"
    )
    parser.add_argument(
        "--max-items",
        type=int,
        default=0,
        help="最多翻译条目数量 (0 表示不限制)"
    )
    parser.add_argument("--temperature", type=float, default=0.7, help="采样温度 (默认: 0.7)")
    parser.add_argument("--top-p", type=float, default=1.0, help="Top-p 采样 (默认: 1.0)")
    parser.add_argument("--top-k", type=int, default=-1, help="Top-k 采样 (默认: -1)")
    parser.add_argument("--repetition-penalty", type=float, default=1.0, help="重复惩罚 (默认: 1.0)")
    parser.add_argument("--max-tokens", type=int, default=4096, help="单次最大生成 token (默认: 4096)")
    parser.add_argument(
        "--test-rules",
        action="store_true",
        help="运行内置占位符与标点规则单元测试"
    )

    args = parser.parse_args()

    if args.test_rules:
        run_unit_tests()
        return

    api_url = normalize_api_url(args.api_url)
    fgd_path = args.fgd_path or find_file_path("fgd_fallback.jsonc")
    qt_path = args.qt_path or find_file_path("qt_fallback.jsonc")

    gen_params = {
        "temperature": args.temperature,
        "top_p": args.top_p,
        "top_k": args.top_k,
        "repetition_penalty": args.repetition_penalty,
        "max_tokens": args.max_tokens,
    }

    print("==================================================")
    print(" CS2 汉化字典大模型专业翻译工具 (OpenAI 兼容版)")
    print(f" 模型接口: {api_url}")
    if args.model:
        print(f" 模型名称: {args.model}")
    print(f" 并发架构: {args.threads} 线程 × 每批 {args.batch_size} 行 (闭环防积压)")
    print(f" 采样参数: temp={args.temperature}, top_p={args.top_p}, rep_p={args.repetition_penalty}")
    print("==================================================")

    if args.file in ["all", "fgd"]:
        if not _STOP_REQUESTED:
            translate_fgd_fallback(
                fgd_path,
                api_url,
                api_key=args.api_key,
                model=args.model,
                num_threads=args.threads,
                batch_size=args.batch_size,
                force=args.force,
                max_items=args.max_items,
                gen_params=gen_params
            )

    if args.file in ["all", "qt"]:
        if not _STOP_REQUESTED:
            translate_qt_fallback(
                qt_path,
                api_url,
                api_key=args.api_key,
                model=args.model,
                num_threads=args.threads,
                batch_size=args.batch_size,
                force=args.force,
                max_items=args.max_items,
                gen_params=gen_params
            )

    print("[*] 全部任务执行完毕。")


if __name__ == "__main__":
    main()
