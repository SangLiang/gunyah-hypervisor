#!/usr/bin/env python3
# 一键预览所有事件的 trigger 声明 + 定义，不需要 LLVM / Ninja。
#
# 用法（在仓库根目录）：
#   python -m venv venv
#   source venv/Scripts/activate     # Windows git bash
#   pip install -r tools/requirements.txt
#   python gen_events_preview.py            # 生成所有 interface
#   python gen_events_preview.py boot        # 只生成 boot
#
# 产物在 build_preview/events/<module>.h 和 <module>.c
#
# 注意：仓库的 tools/grammars/events_dsl.lark 不支持 .ev 里用的
# `require_preempt_disabled` 语法（grammar 与 .ev 不同步）。本脚本把
# .ev 复制到临时目录、过滤掉这些行后再喂给 event_gen.py，不改源码。
# 这只影响生成的约束标注，不影响 handler 调用顺序（优先级）。

import os
import re
import sys
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EVENT_GEN = ROOT / "tools" / "events" / "event_gen.py"
TMPL_DIR = ROOT / "tools" / "events" / "templates"
TRIGGERS_TMPL = TMPL_DIR / "triggers.h.tmpl"
C_TMPL = TMPL_DIR / "c.tmpl"
OUT_DIR = ROOT / "build_preview" / "events"
TMP_DIR = ROOT / "build_preview" / "_ev_tmp"

# 子进程 launcher：解决 Python 3.8 内置 parser 模块遮蔽 tools/events/parser.py，
# 以及 event_gen.py 的相对 import 依赖 __package__ is None 的问题。
LAUNCHER = r"""
import sys, os, importlib.util
cwd = os.getcwd()
sys.path.insert(0, os.path.join(cwd, 'tools', 'events'))
sys.path.insert(0, os.path.join(cwd, 'tools'))
# Python 3.8 自带一个内置 C 模块叫 parser，会遮蔽 tools/events/parser.py。
# 手动加载 tools/events/parser.py 并塞进 sys.modules['parser']，让
# event_gen.py 的 `from parser import TransformToIR` 拿到正确的那个。
parser_path = os.path.join(cwd, 'tools', 'events', 'parser.py')
spec = importlib.util.spec_from_file_location('parser', parser_path)
parser_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser_mod)
sys.modules['parser'] = parser_mod
event_gen = os.path.join(cwd, 'tools', 'events', 'event_gen.py')
# 让 event_gen.py 的 argparse 拿到干净的 argv：prog + 真正的参数
sys.argv = ['event_gen.py'] + sys.argv[1:]
with open(event_gen, encoding='utf-8') as f:
    src = f.read()
# 让 event_gen 在遇到 unknown event 等错误时不退出，继续渲染模板。
# object_create_thread 等事件是构建时从模板生成的，本预览没跑那步，
# 会导致 "subscribed to unknown event" 错误；但目标接口（如 boot）的
# trigger 只依赖订阅该接口的 handler，不受这些错误影响。
import re as _re
src = _re.sub(
    r'logger\.error\("Found %d errors, exiting\.\.\.", errors\)\s*\n\s*sys\.exit\(1\)',
    'logger.error("Found %d errors (ignored for preview)", errors)\n        pass',
    src)
g = {'__name__': '__main__', '__file__': event_gen, '__package__': None,
     '__builtins__': __builtins__}
exec(compile(src, event_gen, 'exec'), g)
"""

# 扫描所有 .ev，排除测试输入
ev_files = []
for ev in ROOT.rglob("*.ev"):
    rel = ev.relative_to(ROOT).as_posix()
    if rel.startswith("tools/codegen_tests/"):
        continue
    ev_files.append(ev)

# 找出所有 interface 模块名（每个 interface X 对应一个 trigger 文件）
interface_re = re.compile(r"^interface\s+(\w+)\s*$", re.MULTILINE)
interfaces = {}
for ev in ev_files:
    text = ev.read_text(encoding="utf-8")
    for m in interface_re.finditer(text):
        name = m.group(1)
        interfaces.setdefault(name, []).append(ev)

# 找出每个 interface 需要喂哪些 .ev：该接口的 .ev + 所有订阅该接口事件的模块的 .ev。
# 这样能避开无关模块的语法问题（仓库 grammar 与部分 .ev 不同步）。
subscribe_re = re.compile(r"^\s*subscribe\s+(?:optional\s+)?(\w+)", re.MULTILINE)
def evs_for_interface(iface):
    # 只喂该接口相关的 .ev：所有接口定义（让事件都已知）+ 订阅该接口事件的模块，
    # 跳过需要真正 cpp 预处理的文件（#define/#include）和非 qemu 平台（避免冲突）。
    needed = set()
    for ev in ev_files:
        text = ev.read_text(encoding="utf-8")
        if "#define" in text or "#include" in text:
            continue
        rel = ev.relative_to(ROOT).as_posix()
        # 跳过非 qemu 平台目录，避免多平台 handler 优先级冲突
        if "/soc_fvp/" in rel or rel.startswith("hyp/platform/soc_fvp/"):
            continue
        # 接口定义一律喂；模块则只喂订阅该接口事件的
        is_interface = bool(interface_re.search(text))
        if is_interface:
            needed.add(ev)
            continue
        for m in subscribe_re.finditer(text):
            evname = m.group(1)
            if evname == iface or evname.startswith(iface + "_"):
                needed.add(ev)
                break
    return sorted(needed)

# 命令行可指定只生成某个 interface
wanted = sys.argv[1:] if len(sys.argv) > 1 else sorted(interfaces.keys())

OUT_DIR.mkdir(parents=True, exist_ok=True)
TMP_DIR.mkdir(parents=True, exist_ok=True)

# 把 .ev 复制到临时目录，过滤掉 grammar 不支持的约束标注行，
# 并做简陋的 C 预处理（#if/#ifdef/#else/#endif），所有宏当作未定义。
# 仓库的 .ev 依赖构建系统定义的宏，这里不求值当 false，#else 块保留。
# 这只为预览 trigger 调用顺序，不保证和真实构建完全一致。
# grammar 只支持 acquire_/release_/require_/exclude_ 后跟 lock|read；
# 其他如 require_preempt_disabled、require_rcu_read 等一律过滤掉。
# 为简单起见，所有这类约束标注行都过滤（预览 trigger 调用顺序足够）。
unsupported_re = re.compile(
    r"^\s*(?:require|exclude|acquire|release)_[A-Za-z0-9_]+\s*$",
    re.MULTILINE)

def preprocess_ev(text):
    # 去掉 grammar 不支持的约束标注
    text = unsupported_re.sub("", text)
    # 简陋 C 预处理：逐行处理 #if/#ifdef/#ifndef/#elif/#else/#endif
    out = []
    # 栈元素: (active, any_branch_taken) —— 当前块是否活跃，本层是否已有分支被取
    stack = [(True, True)]
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            # 去掉行内注释，取指令
            directive = stripped[1:].strip()
            head = directive.split(None, 1)[0] if directive else ""
            cond = directive[len(head):].strip() if head else ""
            parent_active = stack[-1][0]
            if head in ("if", "ifdef", "ifndef"):
                if not parent_active:
                    stack.append((False, False))
                    continue
                if head == "ifdef":
                    val = False  # 所有宏未定义
                elif head == "ifndef":
                    val = True
                else:  # if
                    val = eval_cpp_cond(cond)
                stack.append((val, val))
                continue
            elif head == "elif":
                if len(stack) == 1:
                    continue
                active, taken = stack[-1]
                parent_active = stack[-2][0] if len(stack) > 1 else True
                if not parent_active:
                    stack[-1] = (False, taken)
                    continue
                if taken:
                    stack[-1] = (False, taken)
                else:
                    val = eval_cpp_cond(cond)
                    stack[-1] = (val, val)
                continue
            elif head == "else":
                if len(stack) == 1:
                    continue
                active, taken = stack[-1]
                parent_active = stack[-2][0] if len(stack) > 1 else True
                if not parent_active:
                    stack[-1] = (False, taken)
                    continue
                stack[-1] = (not taken, True)
                continue
            elif head == "endif":
                if len(stack) > 1:
                    stack.pop()
                continue
            elif head in ("error", "warning", "define", "undef", "pragma",
                         "include", "line"):
                # 跳过这些指令（.ev 的 include 用 grammar 自己的语法，不是 #include）
                continue
            else:
                # 未知指令，跳过
                continue
        if stack[-1][0]:
            out.append(line)
    return "\n".join(out) + "\n"

def eval_cpp_cond(cond):
    """简陋求值 #if 条件：所有 defined(X) 为 False，标识符当 0，再 eval。"""
    # 把 defined(X) 替换成 False，defined X 替换成 False
    cond = re.sub(r"defined\s*\(\s*\w+\s*\)", "False", cond)
    cond = re.sub(r"defined\s+\w+", "False", cond)
    # 续行已在 splitlines 前处理（\ 结尾），这里 cond 是单行
    # 把剩余的标识符（宏）替换成 0
    cond = re.sub(r"[A-Za-z_][A-Za-z0-9_]*", "0", cond)
    cond = cond.replace("&&", "and").replace("||", "or").replace("!", "not ")
    try:
        return bool(eval(cond, {"__builtins__": {}}, {}))
    except Exception:
        return False

tmp_ev_args = []
for ev in ev_files:
    rel = ev.relative_to(ROOT)
    tmp = TMP_DIR / rel
    tmp.parent.mkdir(parents=True, exist_ok=True)
    text = ev.read_text(encoding="utf-8")
    # 处理行尾续行（\ 结尾）合并到下一行，避免 #if \ 条件被截断
    text = re.sub(r"\\\n", " ", text)
    text = preprocess_ev(text)
    tmp.write_text(text, encoding="utf-8")
    tmp_ev_args.append(str(tmp))

# 临时文件路径 -> 原文件的映射，用于按接口筛选
tmp_of = {}
for ev in ev_files:
    rel = ev.relative_to(ROOT)
    tmp_of[ev] = str(TMP_DIR / rel)

failures = []
for name in wanted:
    if name not in interfaces:
        print(f"[skip] interface '{name}' 不存在", file=sys.stderr)
        continue

    # 只喂该接口相关的 .ev（接口定义 + 订阅该接口事件的模块）
    needed = evs_for_interface(name)
    inputs = [tmp_of[e] for e in needed]

    for tmpl, suffix in ((TRIGGERS_TMPL, "h"), (C_TMPL, "c")):
        out = OUT_DIR / f"{name}.{suffix}"
        argv = [
            "-t", str(tmpl),
            "-m", name,
            "-o", str(out),
        ] + inputs
        print(f"[gen] {name}.{suffix}  ({len(inputs)} 个 .ev)")
        ret = subprocess.run(
            [sys.executable, "-c", LAUNCHER] + argv,
            cwd=str(ROOT),
        )
        if ret.returncode != 0:
            failures.append(f"{name}.{suffix}")

print()
print(f"输出目录: {OUT_DIR}")
if failures:
    print(f"失败: {failures}", file=sys.stderr)
    sys.exit(1)
print("完成。先看 boot.c 里的 trigger_boot_cold_init_event() ——")
print("那就是第 2 课讲的'按优先级调用所有订阅者'的真身。")
