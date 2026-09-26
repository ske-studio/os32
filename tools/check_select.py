#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_select.py — 変更したファイルから「変異込みで回す検査」を選ぶ (make check-changed)。

`make check` は全部の検査を変異込みで回すので約 10 分かかる (2026-09-26 実測)。
変異試験 (否定側) が意味を持つのは**その試験が見ているソースを変えたとき**だけ
なので、変更したファイルに関係する検査だけ変異込みで回し、残りは変異なしで回す。
対応表 (検査 → 入力のパスの glob) は tools/check_map.yaml の 1 か所に置く。

使い方:
    python3 tools/check_select.py --select [--base <ref>]   make check-changed の中
    python3 tools/check_select.py --select --files <パス>...  選び方の試し (git を見ない)
    python3 tools/check_select.py --lint                     make check-map の中
    python3 tools/check_select.py --inputs <検査名>          静的に拾えた入力の一覧
    python3 tools/check_select.py --suggest <検査名>...      対応表の下書き (yaml)

--select の規則 (安全側に倒す):
  * 変更 = `git diff --name-only <base>...HEAD` + 未コミット (staged / unstaged)
    + 追跡外 (gitignore を除く)。`ignore:` に当たるものは数えない。
  * 変更が無い                     → 全部を変異なし (= check-fast)
  * `full:` に当たる変更がある     → 全部を変異込み (= check)
  * どの検査の glob にも `docs_only:` にも当たらない変更がある
                                   → 全部を変異込み (= check)。表の漏れで
                                     否定側を落とさないため
  * 変更が全部 `docs_only:` に当たる → 当たった検査だけを回す (他は回さない)
  * それ以外                       → 当たった検査は変異込み、残りは変異なし
  出力は sh の代入 (CC_MODE / CC_STAGE1 / CC_MUT1 / CC_STAGE2)。説明は stderr。

--lint が見るもの (make check-map、check-fast / check の列に入っている):
  (a) build/sdk.mk の CHECK_PAR_TARGETS / CHECK_MUT_TARGETS と対応表の検査名が
      過不足なく一致すること
  (b) 対応表の各 glob が追跡されているファイルに 1 つ以上当たること (古い glob)
  (c) **漏れ**: 各検査の recipe から辿れる試験スクリプトが開く / #include する /
      `#[path]` で取り込むソースが、その検査の glob に入っていること。
      辿り方は静的 (文字列リテラルと #include "..." だけ) なので、
      os.walk で舐める検査器などは拾えない — そういう検査は glob を手で広く書く。
"""
import os
import re
import shlex
import subprocess
import sys

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_PATH = os.path.join(ROOT, "tools", "check_map.yaml")
MAKEFILES = ["Makefile"] + sorted(
    os.path.join("build", f) for f in os.listdir(os.path.join(ROOT, "build"))
    if f.endswith(".mk"))


# ---------------------------------------------------------------- 共通
def git(*args):
    p = subprocess.run(["git", "-C", ROOT] + list(args),
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("check_select: git %s に失敗: %s"
                         % (" ".join(args), p.stderr.strip()))
    return [l for l in p.stdout.splitlines() if l]


_TRACKED = None


def tracked():
    global _TRACKED
    if _TRACKED is None:
        # 未コミットの新しいファイルも入力になりうるので、追跡外 (gitignore を
        # 除く) も数える。
        _TRACKED = set(git("ls-files")) | set(
            git("ls-files", "--others", "--exclude-standard"))
    return _TRACKED


def glob_re(pat):
    """`**` はディレクトリを跨ぐ、`*` `?` は跨がない。`[...]` はそのまま。"""
    out, i = "", 0
    while i < len(pat):
        c = pat[i]
        if pat.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pat.startswith("**", i):
            out += ".*"
            i += 2
        elif c == "*":
            out += "[^/]*"
            i += 1
        elif c == "?":
            out += "[^/]"
            i += 1
        elif c == "[":
            j = pat.index("]", i)
            out += pat[i:j + 1]
            i = j + 1
        else:
            out += re.escape(c)
            i += 1
    return re.compile(out + r"\Z")


def compile_globs(globs):
    return [(g, glob_re(g)) for g in globs or []]


def matches(path, cglobs):
    return any(r.match(path) for _, r in cglobs)


def load_map():
    with open(MAP_PATH, encoding="utf-8") as f:
        m = yaml.safe_load(f)
    m.setdefault("ignore", [])
    m.setdefault("full", [])
    m.setdefault("docs_only", [])
    m.setdefault("checks", {})
    return m


# ---------------------------------------------------------------- Makefile
def read_makefiles():
    """{検査名: [recipe 行]} と {変数名: 値} を返す (check-* の規則だけ)。"""
    rules, vars_ = {}, {}
    for mf in MAKEFILES:
        with open(os.path.join(ROOT, mf), encoding="utf-8") as f:
            lines = f.read().split("\n")
        i, cur = 0, None
        while i < len(lines):
            line = lines[i]
            while line.endswith("\\") and i + 1 < len(lines):
                i += 1
                line = line[:-1] + " " + lines[i].strip()
            m = re.match(r"^([A-Z_][A-Z0-9_]*)\s*:?=\s*(.*)$", line)
            if m:
                vars_[m.group(1)] = m.group(2).split()
                cur = None
            elif re.match(r"^(check[\w-]*)\s*:(?!=)", line):
                cur = re.match(r"^(check[\w-]*)", line).group(1)
                rules.setdefault(cur, [])
            elif line.startswith("\t") and cur:
                rules[cur].append(line.strip())
            elif line.strip() and not line.startswith("#"):
                cur = None
            i += 1
    return rules, vars_


def check_lists(vars_):
    par = vars_.get("CHECK_PAR_TARGETS", [])
    mut = vars_.get("CHECK_MUT_TARGETS", [])
    if not par or not mut:
        raise SystemExit("check_select: build/sdk.mk に CHECK_PAR_TARGETS / "
                         "CHECK_MUT_TARGETS が見つからない")
    return par, mut


# ---------------------------------------------------------------- 入力の抽出
PY_STR = re.compile(r"""(?:[rbfRBF]{0,2})(['"])([^'"\n]{1,200})\1""")
PY_JOIN = re.compile(
    r"""(?:(?:['"][\w.+-][\w./+-]*['"])\s*(?:/|,)\s*)+['"][\w.+-][\w./+-]*['"]""")
C_INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
RS_PATH = re.compile(r'#\[path\s*=\s*"([^"]+)"\]')
PY_IMPORT = re.compile(r"^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w., ]+))",
                       re.M)
TOML_PATH = re.compile(r'path\s*=\s*"([^"]+)"')

SCAN_ROOTS = ("tools/", "userland/gshell/host/")   # ここの下は中身まで辿る


def norm(p):
    return os.path.normpath(p).replace(os.sep, "/")


def is_tracked_file(rel):
    return rel in tracked()


def tracked_under(d):
    d = d.rstrip("/") + "/"
    return [f for f in tracked() if f.startswith(d)]


class Extractor:
    def __init__(self):
        self.seen = set()
        self.found = set()

    def add(self, rel):
        rel = norm(rel)
        if rel.startswith("../") or not is_tracked_file(rel):
            return
        self.found.add(rel)
        if rel in self.seen:
            return
        self.seen.add(rel)
        if rel.startswith(SCAN_ROOTS) or rel.endswith("/Cargo.toml") \
                or "/host_tests/" in rel:
            self.scan(rel)

    def add_dir(self, rel):
        for f in tracked_under(norm(rel)):
            self.add(f)

    def resolve(self, cand, base_dir):
        """ROOT 相対・ファイルの場所相対の順に、追跡されているものを探す。"""
        for base in ("", base_dir):
            rel = norm(os.path.join(base, cand))
            if is_tracked_file(rel):
                return ("f", rel)
            if tracked_under(rel) and rel not in (".", ""):
                return ("d", rel)
        return None

    def scan(self, rel):
        path = os.path.join(ROOT, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            return
        d = os.path.dirname(rel)
        if rel.endswith(".py"):
            self.scan_py(text, d)
        elif rel.endswith((".c", ".h")):
            for inc in C_INC.findall(text):
                r = self.resolve(inc, d)
                if r and r[0] == "f":
                    self.add(r[1])
        elif rel.endswith(".rs"):
            for p in RS_PATH.findall(text):
                self.add(os.path.join(d, p))
        elif rel.endswith("Cargo.toml"):
            self.add_crate(d)
            for p in TOML_PATH.findall(text):
                self.add_crate(norm(os.path.join(d, p)))

    def add_crate(self, d):
        if ("crate", d) in self.seen:
            return
        self.seen.add(("crate", d))
        for f in tracked_under(d):
            if f.endswith((".rs", ".toml")):
                self.add(f)

    def scan_py(self, text, d):
        cands = set()
        for m in PY_JOIN.finditer(text):
            parts = re.findall(r"""['"]([^'"]+)['"]""", m.group(0))
            cands.add("/".join(parts))
        for _, s in PY_STR.findall(text):
            if "/" in s and " " not in s:
                cands.add(s)
        for c in cands:
            c = c.strip()
            if not c or c.startswith("/") or "*" in c or "$" in c:
                continue
            r = self.resolve(c, d)
            if not r:
                continue
            if r[0] == "f":
                self.add(r[1])
            elif r[1].startswith("tools/tests/") and r[1] != "tools/tests":
                self.add_dir(r[1])          # hostshim などの差し替えディレクトリ
        for a, b in PY_IMPORT.findall(text):
            for name in ([a] if a else [x.strip() for x in b.split(",")]):
                name = name.split(" as ")[0].strip()
                if not name:
                    continue
                mod = name.replace(".", "/")
                for base in (d, "tools", "tools/tests", ""):
                    r = norm(os.path.join(base, mod + ".py"))
                    if is_tracked_file(r):
                        self.add(r)
                        break

    def from_recipe(self, lines):
        for line in lines:
            line = re.sub(r"\$\([A-Z_]+\)", "", line).lstrip("@-")
            try:
                toks = shlex.split(line)
            except ValueError:
                toks = line.split()
            for i, t in enumerate(toks):
                if t == "-s" and i + 1 < len(toks) and "-p" in toks:
                    pat = toks[toks.index("-p") + 1]
                    rx = glob_re(norm(os.path.join(toks[i + 1], pat)))
                    for f in tracked():
                        if rx.match(f):
                            self.add(f)
                elif "=" in t and t.split("=", 1)[0].isupper():
                    continue
                elif is_tracked_file(norm(t)):
                    self.add(norm(t))


def extract(rules, target):
    ex = Extractor()
    ex.from_recipe(rules.get(target, []))
    return ex.found


# ---------------------------------------------------------------- --lint
def lint():
    m = load_map()
    rules, vars_ = read_makefiles()
    par, mut = check_lists(vars_)
    listed = set(par) | set(mut)
    mapped = set(m["checks"])
    errs = []
    for t in sorted(listed - mapped):
        errs.append("対応表に無い検査: %s (tools/check_map.yaml の checks: に足す)" % t)
    for t in sorted(mapped - listed):
        errs.append("列に無い検査が対応表にある: %s" % t)
    dup = set(par) & set(mut)
    for t in sorted(dup):
        errs.append("CHECK_PAR_TARGETS と CHECK_MUT_TARGETS の両方にある: %s" % t)
    trk = tracked()
    full = compile_globs(m["full"])
    ign = compile_globs(m["ignore"])
    leaks = 0
    for t in sorted(mapped & listed):
        globs = m["checks"][t] or []
        if not globs:
            errs.append("%s: glob が空" % t)
            continue
        cg = compile_globs(globs)
        for g, r in cg:
            if not any(r.match(f) for f in trk):
                errs.append("%s: glob %r が追跡されているどのファイルにも当たらない"
                            % (t, g))
        for f in sorted(extract(rules, t)):
            if not (matches(f, cg) or matches(f, full) or matches(f, ign)):
                errs.append("%s: 入力 %s が glob に入っていない (漏れ)" % (t, f))
                leaks += 1
    if errs:
        sys.stderr.write("check_select --lint: %d 件\n" % len(errs))
        for e in errs:
            sys.stderr.write("  " + e + "\n")
        return 1
    print("check-map: 検査 %d 本、対応表の漏れ 0 件" % len(listed))
    return 0


# ---------------------------------------------------------------- --select
def changed_files(base):
    files = set(git("diff", "--name-only", "--no-renames", "%s...HEAD" % base))
    files |= set(git("diff", "--name-only", "--no-renames", "HEAD"))
    files |= set(git("ls-files", "--others", "--exclude-standard"))
    return sorted(files)


def default_base():
    for ref in ("feat/gui", "origin/feat/gui", "main"):
        p = subprocess.run(["git", "-C", ROOT, "merge-base", ref, "HEAD"],
                           capture_output=True, text=True)
        if p.returncode == 0 and p.stdout.strip():
            return p.stdout.strip()
    return "HEAD"


def select(base, files=None):
    m = load_map()
    _, vars_ = read_makefiles()
    par, mut = check_lists(vars_)
    ign = compile_globs(m["ignore"])
    full = compile_globs(m["full"])
    docs = compile_globs(m["docs_only"])
    checks = {t: compile_globs(g) for t, g in m["checks"].items()}
    if files is None:
        files = changed_files(base)
    changed = [f for f in files if not matches(f, ign)]

    hit, unmatched, full_hits = set(), [], []
    for f in changed:
        ts = {t for t, cg in checks.items() if matches(f, cg)}
        hit |= ts
        if matches(f, full):
            full_hits.append(f)
        elif not ts and not matches(f, docs):
            unmatched.append(f)

    say = sys.stderr.write
    say("check-changed: 基点 %s、変更 %d 件\n" % (base[:12], len(changed)))
    if not changed:
        mode, s1, m1, s2 = "fast", par + mut, [], []
        say("  変更なし → 全部を変異なしで回す (= check-fast)\n")
    elif full_hits or unmatched:
        mode, s1, m1, s2 = "full", par, par, mut
        for f in full_hits[:10]:
            say("  full: に当たる: %s\n" % f)
        for f in unmatched[:10]:
            say("  対応表に無い: %s\n" % f)
        say("  → 安全側: 全部を変異込みで回す (= check)\n")
    elif all(matches(f, docs) for f in changed):
        mode = "docs"
        s1 = [t for t in par + mut if t in hit and t not in mut]
        m1 = list(s1)
        s2 = [t for t in mut if t in hit]
        say("  docs だけの変更 → 当たった %d 本だけ回す: %s\n"
            % (len(hit), " ".join(sorted(hit)) or "(なし)"))
    else:
        mode = "sel"
        s2 = [t for t in mut if t in hit]
        s1 = [t for t in par + mut if t not in s2]
        m1 = [t for t in par if t in hit]
        say("  変異込み %d 本: %s\n" % (len(hit), " ".join(sorted(hit))))
        say("  残り %d 本は変異なし\n" % (len(s1) - len(m1)))
    q = lambda xs: shlex.quote(" ".join(xs))
    print("CC_MODE=%s" % mode)
    print("CC_STAGE1=%s" % q(s1))
    print("CC_MUT1=%s" % q(m1))
    print("CC_STAGE2=%s" % q(s2))
    return 0


# ---------------------------------------------------------------- 下書き
def suggest(targets):
    rules, _ = read_makefiles()
    out = {}
    for t in targets:
        fs = sorted(extract(rules, t))
        out[t] = fs
    sys.stdout.write(yaml.safe_dump({"checks": out}, allow_unicode=True,
                                    sort_keys=False, default_flow_style=False))
    return 0


def main(argv):
    if "--lint" in argv:
        return lint()
    if "--select" in argv:
        base = argv[argv.index("--base") + 1] if "--base" in argv else ""
        files = None
        if "--files" in argv:                 # 試しに選ばせる (git を見ない)
            files = argv[argv.index("--files") + 1:]
            return select("(--files)", files)
        return select(base or default_base(), files)
    if "--inputs" in argv:
        rules, _ = read_makefiles()
        for f in sorted(extract(rules, argv[argv.index("--inputs") + 1])):
            print(f)
        return 0
    if "--suggest" in argv:
        return suggest(argv[argv.index("--suggest") + 1:])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
