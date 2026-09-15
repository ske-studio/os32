#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_arch_asm.py — CPU 原始命令の直書き検査 (移植性の準備、順序 2)

カーネル側の C ソースが `hlt` / `cli` / `sti` をインライン asm で直接書くと、
別アーキテクチャへ移すときに「どこを差し替えればよいか」がソース全体に
散らばってしまう。これらは include/io.h の原始命令
(`_halt` / `_idle` / `_stop` / `_enable` / `_disable` / `irq_save` /
`irq_restore`) 経由で使い、io.h だけを arch 差し替えの境界にする。

検査するもの:
  対象ディレクトリの *.c / *.h にあるインライン asm 文の **文字列リテラル**
  に `hlt` / `cli` / `sti` がニーモニックとして現れないこと。

許可するもの:
  1. include/io.h            — 原始命令の定義元 (差し替えの境界そのもの)
  2. ARCH-ASM-OK の印が付いた asm 文
     asm の直前 (ALLOW_LOOKBACK 行以内) に `ARCH-ASM-OK` と書いてあれば
     見逃す。命令列の一部としてしか意味を持たず、原始命令に切り出すと
     壊れる箇所のための逃げ道。印を付けるときは **なぜ切り出せないか** を
     同じコメントに書くこと。

*.asm (NASM) は元から arch 固有なので対象外。userland は CPL=3 でこれらの
命令自体が使えないので tools/check_privileged.py の担当。
"""

import os
import re
import sys

PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 検査対象ディレクトリ (カーネル空間で走る C ソースと、それが引くヘッダ)
SCAN_DIRS = ["kernel", "drivers", "exec", "fs", "kapi", "lib", "net", "gfx",
             "include"]

# 検査から外すパス (PROJ_DIR からの相対、前方一致)
SKIP_PREFIXES = [
    os.path.join("lib", "sqlite3"),   # 第三者コード
]

# 原始命令の定義元。ここだけは直書きしてよい。
ALLOW_FILES = [os.path.join("include", "io.h")]

ALLOW_MARK = "ARCH-ASM-OK"
ALLOW_LOOKBACK = 16       # 印を探す行数 (asm 文の直前のコメント)

# インライン asm 文の開始 (開き括弧まで)。volatile の有無は問わない。
ASM_RE = re.compile(r"\b(?:__asm__|asm)\s*(?:__volatile__|volatile)?\s*\(")

# 命令ニーモニックとしての出現だけを拾う ("sticky" / "client" は拾わない)。
# 区切りは 行頭/行末・空白・';'・アセンブラ側の改行やタブ (\n \t)。
MNEMONIC_RE = re.compile(r"(?:\A|[;\s]|\\[nt])(hlt|cli|sti)(?:\Z|[;\s]|\\[nt])")


def asm_span(text, open_paren):
    """開き括弧の位置から、対応する閉じ括弧の位置を返す (文字列リテラルを考慮)。"""
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    break
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def scan_file(path):
    """返り値: [(行番号, ニーモニック, その行), ...]"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    lines = text.split("\n")
    hits = []

    pos = 0
    while True:
        m = ASM_RE.search(text, pos)
        if not m:
            break
        open_paren = m.end() - 1
        close_paren = asm_span(text, open_paren)
        stmt = text[m.start():close_paren + 1]
        start_line = text.count("\n", 0, m.start())      # 0 起点

        # 直前 ALLOW_LOOKBACK 行に印があれば見逃す。
        back = "\n".join(lines[max(0, start_line - ALLOW_LOOKBACK):start_line + 1])
        if ALLOW_MARK not in back:
            for sm in STR_RE.finditer(stmt):
                mn = MNEMONIC_RE.search(sm.group(1))
                if mn:
                    ln = start_line + stmt.count("\n", 0, sm.start())
                    hits.append((ln + 1, mn.group(1), lines[ln].strip()))
        pos = close_paren + 1
    return hits


def main():
    problems = []
    scanned = 0

    for d in SCAN_DIRS:
        root_dir = os.path.join(PROJ_DIR, d)
        if not os.path.isdir(root_dir):
            continue
        for root, dirs, files in os.walk(root_dir):
            dirs[:] = [x for x in dirs if x not in ("target", ".git")]
            for name in sorted(files):
                if not (name.endswith(".c") or name.endswith(".h")):
                    continue
                path = os.path.join(root, name)
                rel = os.path.relpath(path, PROJ_DIR)
                if any(rel.startswith(p) for p in SKIP_PREFIXES):
                    continue
                if rel in ALLOW_FILES:
                    continue
                scanned += 1
                for (ln, mn, line) in scan_file(path):
                    problems.append("%s:%d: インライン asm に `%s` の直書き\n"
                                    "         %s" % (rel, ln, mn, line))

    if problems:
        print("=== 原始命令の直書き検査: NG (%d 件) ===" % len(problems))
        for p in problems:
            print("  [NG] %s" % p)
        print("")
        print("  hlt / cli / sti は include/io.h の原始命令を使うこと:")
        print("    _halt()     割り込み許可済みの文脈で、次の割り込みまで眠る")
        print("    _idle()     割り込みを許可して眠る (不可分。分けてはいけない)")
        print("    _stop()     割り込みを禁じて止まる (for (;;) で囲む)")
        print("    _enable() / _disable() / irq_save() / irq_restore()")
        print("")
        print("  命令列の一部としてしか意味を持たない箇所は、asm の直前の")
        print("  コメントに ARCH-ASM-OK と、切り出せない理由を書く。")
        return 1

    print("=== 原始命令の直書き検査: OK (%d ファイル、直書きなし) ===" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
