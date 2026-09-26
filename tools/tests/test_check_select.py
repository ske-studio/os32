"""make check-changed の選び方 (tools/check_select.py + tools/check_map.yaml) の試験。

代行レビュー (2026-09-26、P2-1〜P2-3) の筋書きを固定する:
  * 実装の .c が #include する .inc を変えたら、その .c を見る検査が変異込みになる
    (userland/system/hsync_protect.inc、userland/shell/sh_pipe.inc)
  * 裸のファイル名で文書を読む検査器 (README.md → check-kapi-version、
    CLAUDE.md → check-manifests) が docs だけの変更でも回る
  * docs だけの変更では文書を読む検査 (docs_always:) を常に回す
  * 走査型 (broad:) の `**` glob にしか当たらない変更は安全側 (全部変異込み)
  * サブモジュールの bump は check-manifests / check-packages-host だけ変異込み
  * feat/gui の上でコミットした後は HEAD~1 を基点にする (merge-base == HEAD で
    「変更なし」に退化しない)
  * 実物の対応表に漏れが無い (--lint = 0)

  python3 -B tools/tests/test_check_select.py            # 筋書き
  python3 -B tools/tests/test_check_select.py --mutate   # 否定側 (選び方を壊して RED か)

変異は check_select.py の**写しの文字列**に当てて exec するので、実物は書き換えない
(check-par で回せる)。
"""
import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import types

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "tools/check_select.py"


def load(text=None):
    mod = types.ModuleType("check_select_under_test")
    mod.__file__ = str(SRC)
    code = compile(text if text is not None else SRC.read_text(encoding="utf-8"),
                   str(SRC), "exec")
    exec(code, mod.__dict__)
    return mod


def run_plan(cs, files):
    mode, s1, m1, s2, _ = cs.plan(files)
    return mode, set(s1), set(m1), set(s2)


def git(d, *args):
    subprocess.run(["git", "-C", d] + list(args), check=True,
                   capture_output=True, text=True)


# ------------------------------------------------------------------ 筋書き
def case_inc_extract(cs):
    rules, _ = cs.read_makefiles()
    for t in ("check-hsync-h2-host", "check-settings-protect-host"):
        assert "userland/system/hsync_protect.inc" in cs.extract(rules, t), t
    assert "userland/shell/sh_pipe.inc" in cs.extract(rules, "check-sh-status-host")


def case_hsync_protect(cs):
    mode, s1, m1, s2 = run_plan(cs, ["userland/system/hsync_protect.inc"])
    assert mode == "sel", mode
    for t in ("check-hsync-h2-host", "check-hsync-h3-host", "check-h4-manifest-host"):
        assert t in s2, (t, s2)                  # check-mut 側 → 2 段目で変異込み
    assert "check-settings-protect-host" in m1, m1


def case_sh_pipe(cs):
    mode, s1, m1, s2 = run_plan(cs, ["userland/shell/sh_pipe.inc"])
    assert mode == "sel", mode
    assert "check-sh-status-host" in s2, s2
    assert "check-sh-truncation-host" in m1, m1
    assert "check-sh-status-host" not in s1, "変異込みの 2 段目と 1 段目の両方に出た"


def case_bare_extract(cs):
    rules, _ = cs.read_makefiles()
    assert "README.md" in cs.extract(rules, "check-kapi-version")
    assert "CLAUDE.md" in cs.extract(rules, "check-manifests")


def case_readme(cs):
    mode, s1, m1, s2 = run_plan(cs, ["README.md"])
    assert mode == "docs", mode
    assert "check-kapi-version" in s1, s1
    assert not s2, s2
    assert "check-serialfs-host" not in s1, "docs だけなのに重い検査を回した"


def case_claude(cs):
    mode, s1, m1, s2 = run_plan(cs, ["CLAUDE.md"])
    assert mode == "docs", mode
    assert {"check-manifests", "check-constraints"} <= s1, s1


def case_docs_always(cs):
    mode, s1, m1, s2 = run_plan(cs, ["docs/08_build.md"])
    assert mode == "docs", mode
    for t in ("check-constraints", "check-kapi-version", "check-manifests",
              "check-packages-host", "check-tests-inventory"):
        assert t in s1, (t, s1)


def case_broad_only(cs):
    # drivers/dev.c は走査型 (arch-asm / le-access) の ** にしか当たらない。
    # 文字列を割ってあるのは、check-map の抽出がこの試験の入力と数えないため
    # (数えるとこの試験自身の glob に当たって筋書きが崩れる)。
    mode, s1, m1, s2 = run_plan(cs, ["drivers/" + "dev.c"])
    assert mode == "full", mode


def case_submodule(cs):
    mode, s1, m1, s2 = run_plan(cs, ["apps", "game"])
    assert mode == "sel", mode
    assert m1 == {"check-manifests", "check-packages-host"}, m1


def case_nothing(cs):
    mode, s1, m1, s2 = run_plan(cs, [])
    assert mode == "fast" and not m1 and not s2, mode


def case_featgui_commit(cs):
    with tempfile.TemporaryDirectory(prefix="os32-cksel-") as d:
        git(d, "init", "-q", "-b", "feat/gui")
        git(d, "config", "user.email", "t@example.invalid")
        git(d, "config", "user.name", "t")
        pathlib.Path(d, "a.txt").write_text("a\n")
        git(d, "add", "a.txt")
        git(d, "commit", "-q", "-m", "a")
        first = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        pathlib.Path(d, "b.txt").write_text("b\n")
        git(d, "add", "b.txt")
        git(d, "commit", "-q", "-m", "b")
        old_root, old_trk = cs.ROOT, cs._TRACKED
        cs.ROOT, cs._TRACKED = d, None
        try:
            base, note = cs.default_base()
            assert base == first, (base, note)
            assert "HEAD~1" in note, note
            committed, work = cs.changed_files(base)
            assert committed == ["b.txt"], committed
            # 枝の上なら merge-base のまま
            git(d, "checkout", "-q", "-b", "wt/x")
            pathlib.Path(d, "c.txt").write_text("c\n")
            git(d, "add", "c.txt")
            git(d, "commit", "-q", "-m", "c")
            base2, note2 = cs.default_base()
            assert "merge-base" in note2 and "HEAD~1" not in note2, note2
            assert cs.changed_files(base2)[0] == ["c.txt"]
        finally:
            cs.ROOT, cs._TRACKED = old_root, old_trk


def case_lint_real(cs):
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        rc = cs.lint()
    assert rc == 0, err.getvalue()[:2000]


CASES = [case_inc_extract, case_hsync_protect, case_sh_pipe, case_bare_extract,
         case_readme, case_claude, case_docs_always, case_broad_only,
         case_submodule, case_nothing, case_featgui_commit, case_lint_real]


def run_cases(cs, quiet=False):
    failed = []
    for c in CASES:
        try:
            c(cs)
            ok = True
        except Exception as e:           # noqa: BLE001 — 変異で何が起きても RED
            ok = False
            if not quiet:
                print("FAIL %s: %r" % (c.__name__, e), flush=True)
        if not ok:
            failed.append(c.__name__)
            if quiet:
                return failed            # 変異は最初に落ちたケースで打ち切る
        elif not quiet:
            print("ok   %s" % c.__name__, flush=True)
    return failed


# ------------------------------------------------------------------ 否定側
MUTATIONS = [
    ('or rel.endswith(C_EXTS):', ':',
     "実装の .c / .inc の #include を辿らない (P2-1: .inc が表から落ちる)"),
    ('if "/" in s or s.endswith(BARE_EXTS):', 'if "/" in s:',
     "裸のファイル名 (README.md / CLAUDE.md) を候補にしない (P2-2)"),
    ('if not (t in broad and "**" in g)]', ']',
     "走査型の ** glob を「表に載っている」に数える (P2-1 の保険が外れる)"),
    ('run = hit | (set(m["docs_always"]) & (set(par) | set(mut)))', 'run = hit',
     "docs だけの変更で文書を読む検査を常には回さない (P2-2)"),
    ('        if mb != head:\n', '        if True:\n',
     "feat/gui の上でコミットした後も merge-base (== HEAD) を基点にする (P2-3)"),
    ('    elif full_hits or unmatched:\n', '    elif full_hits:\n',
     "表に無い変更でも安全側 (全部変異込み) に倒さない"),
    ('        s2 = [t for t in mut if t in hit]\n        s1 = [t for t in par + mut if t not in s2]',
     '        s2 = []\n        s1 = [t for t in par + mut if t not in s2]',
     "check-mut 側の当たった検査を変異込みで回さない"),
    ('    for f in changed:\n        hit |=', '    for f in changed[:0]:\n        hit |=',
     "変更を検査に突き合わせない"),
]


def mutate():
    original = SRC.read_text(encoding="utf-8")
    bad = 0
    for i, (old, new, why) in enumerate(MUTATIONS, 1):
        if original.count(old) != 1:
            print("MUTATION %d NOT APPLICABLE: %s" % (i, why), flush=True)
            bad += 1
            continue
        try:
            cs = load(original.replace(old, new))
            failed = run_cases(cs, quiet=True)
        except Exception as e:           # noqa: BLE001
            failed = ["load: %r" % e]
        if failed:
            print("MUTATION %d RED (%s): %s" % (i, failed[0], why), flush=True)
        else:
            print("MUTATION %d **GREEN (見逃し)**: %s" % (i, why), flush=True)
            bad += 1
    return bad


if __name__ == "__main__":
    os.chdir(ROOT)
    failed = run_cases(load())
    print("SUMMARY %d/%d PASS" % (len(CASES) - len(failed), len(CASES)), flush=True)
    rc = len(failed)
    if "--mutate" in sys.argv[1:]:
        rc += mutate()
    sys.exit(bool(rc))
