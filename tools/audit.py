#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
audit.py —— 注入链静态自检

为什么需要它：这个项目没法在本地编译（Windows、无 theos/clang），
每次验证都要走 CI + 真机，周期很长。所以凡是"能在文本层面查出来"的问题，
必须在提交前查掉，不能让设备去承担。

检查项分四类：
  A. 语法结构   —— 花括号/方括号是否配平
  B. 平台禁例   —— iOS SDK 里会被 #error 掉的头文件、不可用 API
  C. 逻辑不变量 —— 注入流程里必须存在/必须不存在的代码形态
  D. 文案一致性 —— 日志里声明的行为与实际代码是否一致
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL = []
WARN = []


def read(rel):
    p = os.path.join(ROOT, rel)
    if not os.path.exists(p):
        FAIL.append(f"[缺失] {rel} 不存在")
        return ""
    with open(p, encoding="utf-8") as f:
        return f.read()


def strip_strings_and_comments(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        if c == "'":
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            out.append("''")
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def check_balance(rel):
    src = read(rel)
    if not src:
        return
    clean = strip_strings_and_comments(src)
    for open_ch, close_ch, name in (('{', '}', '花括号'), ('(', ')', '圆括号'), ('[', ']', '方括号')):
        # [] 在 C++ 里也用于 lambda capture 与 attributes，配平即可
        if clean.count(open_ch) != clean.count(close_ch):
            FAIL.append(f"[结构] {rel}: {name}不配平 {clean.count(open_ch)} vs {clean.count(close_ch)}")
    depth = 0
    for ch in clean:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth < 0:
                FAIL.append(f"[结构] {rel}: 出现多余的右花括号")
                break
    if depth != 0:
        FAIL.append(f"[结构] {rel}: 文件结束时花括号未闭合，depth={depth}")


FILES = [
    "src/Inject/ManualLoad.h",
    "src/Inject/ThreadHijack.h",
    "src/Inject/ContainerCopy.h",
    "src/Inject/BundleInfo.h",
    "src/Inject/RemoteLoad.h",
    "src/Game/Ue4Remote.h",
    "daemon/main.mm",
    "host/main.mm",
    "payload_manual/main.mm",
]

BANNED = [
    ("#include <mach/mach_vm.h>", "iOS SDK 里 mach_vm.h 会 #error，必须用 vm_* 系列"),
    ("mach_vm_", "同上：mach_vm_* 在 iOS SDK 不可用"),
    ("printf(\"", None),  # 占位，下面单独处理
]

for rel in FILES:
    src = read(rel)
    if not src:
        continue
    check_balance(rel)

    for bad, why in BANNED:
        if why and bad in src:
            FAIL.append(f"[平台] {rel}: 出现 {bad}（{why}）")

    # iOS 上不存在 libproc.h 的便捷封装，必须自己声明
    # 注释里提到这个头是允许的，所以检查代码本体（去掉注释/字符串后）
    if "libproc.h" in strip_strings_and_comments(src):
        FAIL.append(f"[平台] {rel}: 真的 include 了 libproc.h，iOS SDK 无此头")

    # ObjC 方法体里 self 是既有标识符，声明同名局部变量必然编译失败
    if rel.endswith(".mm") and re.search(r'^\s*\w+\s*\*?\s*self\s*=', src, re.M):
        FAIL.append(f"[语法] {rel}: 用 self 当局部变量名（ObjC 里 self 已被占用）")

    # 日志里不能有未转义的裸换行出现在字符串内部（之前踩过）
    if re.search(r'printf\("[^"]*$', src, re.M):
        WARN.append(f"[文案] {rel}: 可能有被换行截断的 printf 字符串")

# ---------------- C. 逻辑不变量 ----------------
# 日志必须无缓冲：闪退时块缓冲会丢掉全部现场
host_src = read("host/main.mm")
if host_src and "setvbuf" not in host_src:
    FAIL.append("[诊断] host 未设置无缓冲 stdout，闪退时日志会丢失")
dmn_src = read("daemon/main.mm")
if dmn_src and "setvbuf" not in dmn_src:
    FAIL.append("[诊断] daemon 未设置无缓冲 stdout")

ml = read("src/Inject/ManualLoad.h")
if ml:
    if "target_is_running" not in ml:
        FAIL.append("[逻辑] ManualLoad 缺少挂起门禁 target_is_running")
    if "suspend_count" not in ml:
        FAIL.append("[逻辑] ManualLoad 未检查 suspend_count，会在冻结进程上白干")
    if "THREAD_BASIC_INFO" in ml and "mach/thread_info.h" not in ml and "thread_info.h" not in ml:
        FAIL.append("[平台] ManualLoad 用了 THREAD_BASIC_INFO 但没 include <mach/thread_info.h>")
    if "N_SECT" not in ml:
        FAIL.append("[逻辑] resolve_sym 未过滤未定义符号（会把全局变量解析成偏移 0）")
    if "hijack_quick" not in ml:
        FAIL.append("[逻辑] 入口未使用短窗口劫持")
    if re.search(r'call_addr\(task,\s*entry', ml) and "execMode == 1" not in ml:
        FAIL.append("[逻辑] 入口默认路径在用 thread_create（已证明无法执行）；只允许出现在自测分支")
    if "入口未执行" not in ml:
        FAIL.append("[逻辑] 入口未执行时必须返回 0，否则 daemon 会误判成功并停止重试")
    if "read_remote_slot" not in ml:
        FAIL.append("[逻辑] 缺少结果槽回读，无法判断是否执行")

th = read("src/Inject/ThreadHijack.h")
if th:
    if "THREAD_BASIC_INFO" in th and "mach/thread_info.h" not in th:
        FAIL.append("[平台] ThreadHijack 用了 THREAD_BASIC_INFO 但没 include <mach/thread_info.h>")
    if "hijack_quick" not in th:
        FAIL.append("[逻辑] ThreadHijack 缺少 hijack_quick")
    # 短劫持里必须释放借用栈，否则每次注入泄漏 32KB
    q = th[th.find("hijack_quick"):] if "hijack_quick" in th else ""
    freed = ("(vm_address_t)stack" in q and "vm_deallocate" in q)
    if q and not freed:
        WARN.append("[资源] hijack_quick 未释放借用的栈内存（每次泄漏 32KB）")
    # 还原失败必须报出来
    if q and "还原->" not in q:
        WARN.append("[逻辑] hijack_quick 未记录还原结果")

dm = read("daemon/main.mm")
if dm:
    if "本次注入未成功" in dm and "sleep(" not in dm.split("本次注入未成功")[1][:400]:
        WARN.append("[逻辑] daemon 注入失败分支可能没有限速，会形成紧凑重试循环")
    if "arp_probe" not in dm:
        FAIL.append("[逻辑] daemon 未调用 arp_probe 入口")

pl = read("payload_manual/main.mm")
if pl:
    for sym in ("arp_probe", "arp_spin"):
        if f"void {sym}" not in pl:
            FAIL.append(f"[逻辑] payload 缺少导出入口 {sym}")
    if "svc #0x80" in strip_strings_and_comments(pl):
        WARN.append("[风险] payload 仍含裸 svc 调用（G37 曾因此被系统杀）")
    if "g_slot" in strip_strings_and_comments(pl):
        WARN.append("[风险] payload 仍使用全局变量（依赖 rebase，未修正时会写坏）")

# ---------------- 输出 ----------------
print("=" * 60)
print(f"审查文件 {len(FILES)} 个")
if FAIL:
    print(f"\n必须修复 {len(FAIL)} 项：")
    for x in FAIL:
        print("  ✗", x)
if WARN:
    print(f"\n需要注意 {len(WARN)} 项：")
    for x in WARN:
        print("  !", x)
if not FAIL and not WARN:
    print("\n全部检查通过")
print("=" * 60)
sys.exit(1 if FAIL else 0)
