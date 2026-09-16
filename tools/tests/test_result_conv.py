"""試験プログラムの「合否の出し方」を固定する (票 docs/tasks/test/TASK_TEST_RESULT.md §2)。

記録: tools/tests/result_conv_tdd.md

ゲストの試験の合否を人が画面を読まずに判定できるようにするには、

  * 終了コードが 0 (全部合格) / 1 (1 件以上不合格) / 2 (実行しなかった) のどれかで、
    シェルの予約値 126 / 127 / 130 / 139 を**返さない**こと、
  * 最終行に `<名前>: PASS <n>/<m>` / `FAIL <n>/<m>` / `SKIP <理由>` が 1 行出ること、
  * **その 2 つが必ず一致する**こと (片方だけ直すとランナーが 2 つの答えを持つ)

の 3 つが要る。この試験はそれを 2 段で見る。

  (A) 実行時 — tools/tests/test_result_conv_host.c が実物の
      userland/lib/rt/testresult.h と、userland/tests/ の**実物のプログラム 5 本**
      (stat_t / restest / test2 / klibc_test / font_load_test) を贋物の KernelAPI で
      走らせ、出た文字列と `main` の返り値の両方を観測する。合格側・不合格側・
      SKIP 側の 3 通りと、argv[0] を変えても集計行が動かないことを踏む。
      **grep では一致は確かめられない** — 「PASS と出して 1 を返す」版も
      grep はどちらの行も通してしまう。

  (B) 静的 — 第 1 陣 (票 §4) と、`rt/testresult.h` を取り込んだ試験すべてについて
      `void main` が残っていないこと、集計行の名前が固定文字列であること
      (argv[0] 由来にしない)、`main` の return が必ず集計の答えを返していること、
      予約値をそのまま返す経路が無いことを見る。

      Rust の alloc_demo は C のヘッダを使えないので、書式と終了コードが
      testresult.h と同じであることをここで突き合わせる。

使い方:

    python3 -B tools/tests/test_result_conv.py [--target] [--mutate]

--target を付けると、適合させた試験が実機と同じ i386-elf クロスコンパイラでも
-Werror で通ることを確かめる ([C1] C89/GNU89)。

--mutate は**否定側**。集計行と終了コードを食い違わせる / 予約値を返す /
`void main` に戻す / 集計行の名前を argv[0] 由来にする / 約束事の本体を壊す、を
それぞれ作り、この試験がちゃんと RED になることを見る。**どれもコンパイルは
通る** — コンパイルエラーで落ちるだけなら試験の目が働いたことにならない。

make・エミュレータ・実配備には一切触れない。
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

HARNESS = ROOT / "tools/tests/test_result_conv_host.c"

# 実物のプログラムを取り込む翻訳単位。**列挙で拾わず名前で持つ** — ここが
# docs/TESTS.md の「対象ソース」の出どころにもなる (tools/gen_tests_inventory.py
# が SHIMS の中身を読んで #include 先を辿る)。
SHIMS = [
    "tools/tests/result_conv/run_stat_t.c",
    "tools/tests/result_conv/run_restest.c",
    "tools/tests/result_conv/run_test2.c",
    "tools/tests/result_conv/run_klibc_test.c",
    "tools/tests/result_conv/run_font_load_test.c",
]
CONV_HEADER = ROOT / "userland/lib/rt/testresult.h"
RUST_DEMO = ROOT / "userland/rust/alloc_demo/src/lib.rs"
TESTS_DIR = ROOT / "userland/tests"

# 票 §4 の第 1 陣。**ここが唯一の管理元** — 増やすのはこの表だけ。
# これ以外にも rt/testresult.h を取り込んだ試験は自動で検査の対象になる。
WAVE1 = [
    "klibc_test", "db_test", "math_test", "mgx_test", "save_test", "restest",
    "stat_t", "asset_test", "gui_call_test", "test2", "e2test", "ecs_test",
    "db_v50_test", "host_test", "input_test", "font_load_test",
]

HOST_FLAGS = ["-std=gnu89", "-Wall", "-Wextra",
              "-Wdeclaration-after-statement",
              "-D__cdecl=", "-D__OS32_USERLAND__"]
HOST_INC = ["-I" + str(ROOT / p) for p in
            (".", "include", "sdk/include", "sdk/include/os32", "userland/lib")]

CROSS_DIR = pathlib.Path(os.environ.get("CROSS_DIR", "/usr/local/cross"))
if not CROSS_DIR.exists():
    alt = pathlib.Path.home() / "opt/cross"
    if alt.exists():
        CROSS_DIR = alt

TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                "-fno-pie", "-fno-stack-protector", "-nostdlib",
                "-mno-red-zone", "-fcommon", "-fsigned-char",
                "-fno-short-enums", "-O2",
                "-Wall", "-Wextra", "-Werror",
                "-Wdeclaration-after-statement",
                "-D__OS32_USERLAND__", "-I.", "-Iinclude", "-Isdk/include",
                "-Isdk/include/os32", "-Iuserland/lib", "-Iuserland/lib/math",
                "-Iuserland/lib/ecs", "-Iuserland/lib/input",
                "-Iuserland/lib/save", "-Iuserland/lib/db",
                "-Iuserland/lib/asset", "-Iuserland/lib/mgx", "-Ilib/zlib",
                "-I" + str(CROSS_DIR / "i386-elf/include")]


# --------------------------------------------------------------------------
#  ビルド
# --------------------------------------------------------------------------

def build_host(tmp, name):
    """ハーネス + 取り込み用の翻訳単位を 1 本の実行ファイルにする。

    取り込み側 (run_*.c) は**実機 32 ビット向けのソースをホストの 64 ビットで
    コンパイルする**ので警告を止める (ポインタを u32 へ落とす箇所など)。
    実機と同じ -Werror は --target が i386-elf-gcc で見る。"""
    objs = []
    exe = tmp / name
    subprocess.run(["gcc", *HOST_FLAGS, "-Werror", *HOST_INC,
                    "-c", str(HARNESS), "-o", str(tmp / (name + "-h.o"))],
                   cwd=ROOT, check=True)
    objs.append(str(tmp / (name + "-h.o")))
    for rel in SHIMS:
        src = ROOT / rel
        obj = tmp / (name + "-" + src.stem + ".o")
        subprocess.run(["gcc", *HOST_FLAGS, "-w", *HOST_INC,
                        "-c", str(src), "-o", str(obj)], cwd=ROOT, check=True)
        objs.append(str(obj))
    subprocess.run(["gcc", *objs, "-o", str(exe)], cwd=ROOT, check=True)
    return exe


# --------------------------------------------------------------------------
#  静的検査 — ソースを読んで約束事に照らす
# --------------------------------------------------------------------------

def strip_c(text):
    """コメントと文字列の中身を空白に潰す。波括弧の対応を取るため。

    もとの長さと改行の位置は保つ (行番号を壊さない)。"""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\':
                    j += 1
                j += 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        else:
            i += 1
    return "".join(out)


MAIN_RE = re.compile(
    r"^(?P<ret>void|int)\s+(?:__cdecl\s+)?main\s*\((?P<args>[^)]*)\)\s*$",
    re.M)


def main_span(text):
    """`main` の宣言と本体の範囲を返す。見つからなければ None。"""
    blank = strip_c(text)
    m = MAIN_RE.search(blank)
    if not m:
        # `int main(...) {` のように同じ行に `{` が来る書き方も拾う。
        m = re.search(r"^(?P<ret>void|int)\s+(?:__cdecl\s+)?main\s*"
                      r"\((?P<args>[^)]*)\)\s*\{", blank, re.M)
        if not m:
            return None
    start = blank.find("{", m.end() - 1)
    if start < 0:
        return None
    depth = 0
    i = start
    while i < len(blank):
        if blank[i] == '{':
            depth += 1
        elif blank[i] == '}':
            depth -= 1
            if depth == 0:
                return m.group("ret"), m.group("args"), start, i + 1
        i += 1
    return None


NAME_ARG = re.compile(r"os32_test_summary(?:_skip)?\s*\(\s*[^,]+,\s*[^,]+,\s*"
                      r"(?P<name>[^,]+),")
ASSIGN = re.compile(r"(?P<var>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
                    r"os32_test_summary(?:_skip)?\s*\(")
RETURN = re.compile(r"\breturn\s+(?P<what>[^;]*);")
RESERVED = (126, 127, 130, 139)


def scan_source(path, report):
    """試験 1 本を約束事に照らす。違反を report(msg) へ流す。"""
    stem = path.stem
    text = path.read_text(encoding="utf-8")
    span = main_span(text)
    if span is None:
        report("%s: main() の宣言が見つからない" % stem)
        return
    ret, args, body_start, body_end = span

    # (1) `void main` は約束違反 (票 §2-1)。crt0_c.c は int として呼ぶので、
    #     終了コードが eax の残骸になり再現性が無い。
    if ret != "int":
        report("%s: `%s main` — 終了コードが eax の残骸になる (票 §2-1)"
               % (stem, ret))

    # (2) 署名は int main(int, char **, KernelAPI *)。
    if "KernelAPI" not in args:
        report("%s: main が KernelAPI * を受け取っていない (票 §2-1): (%s)"
               % (stem, " ".join(args.split())))

    # (3) 約束事の管理元を取り込んでいる。
    if 'rt/testresult.h' not in text:
        report("%s: rt/testresult.h を取り込んでいない "
               "(集計行と終了コードを手書きしない)" % stem)
        return

    body = text[body_start:body_end]
    blank_body = strip_c(text)[body_start:body_end]

    # (4) 集計行の名前は固定文字列で、プログラム名と同じ。
    names = NAME_ARG.findall(text)
    if not names:
        report("%s: os32_test_summary / _skip を呼んでいない" % stem)
    for raw in names:
        arg = raw.strip()
        if not (arg.startswith('"') and arg.endswith('"')):
            report("%s: 集計行の名前が固定文字列でない (%s) — argv[0] 由来だと"
                   "リダイレクトや呼び方で行が変わる" % (stem, arg))
        elif arg.strip('"') != stem:
            report("%s: 集計行の名前が \"%s\" でプログラム名と違う"
                   % (stem, arg.strip('"')))

    # (5) main の return は必ず集計の答えを返す。
    #     「FAIL を出しながら 0 を返す」「PASS を出しながら 1 を返す」を止める。
    vars_ = set(m.group("var") for m in ASSIGN.finditer(body))
    if not vars_:
        report("%s: main の中で os32_test_summary の答えを受けていない" % stem)
    for m in RETURN.finditer(blank_body):
        what = body[m.start("what"):m.end("what")].strip()
        if what in vars_:
            continue
        if what.isdigit() or (what.startswith("-") and what[1:].isdigit()):
            n = int(what)
            if n in RESERVED:
                report("%s: main が予約値 %d をそのまま返している (票 §2-1)"
                       % (stem, n))
            else:
                report("%s: main が `return %s;` — 集計行と食い違いうる "
                       "(答えは os32_test_summary から受ける)" % (stem, what))
        else:
            report("%s: main が `return %s;` — 集計の答えではない"
                   % (stem, what))

    # (6) 予約値を書いている経路が無い。
    for n in RESERVED:
        if re.search(r"\breturn\s+%d\s*;" % n, blank_body):
            report("%s: 予約値 %d を返す経路がある" % (stem, n))


def conforming_sources():
    """検査の対象 — 第 1 陣 + rt/testresult.h を取り込んだ試験。"""
    out = []
    for name in WAVE1:
        p = TESTS_DIR / (name + ".c")
        if not p.exists():
            raise SystemExit("第 1 陣の %s が無い" % p)
        out.append(p)
    for p in sorted(TESTS_DIR.glob("*.c")):
        if p in out:
            continue
        if 'rt/testresult.h' in p.read_text(encoding="utf-8"):
            out.append(p)
    return out


def check_sources():
    bad = []
    srcs = conforming_sources()
    for p in srcs:
        scan_source(p, bad.append)
    for msg in bad:
        print("  FAIL %s" % msg, flush=True)
    print("STATIC %d sources, %d violations" % (len(srcs), len(bad)),
          flush=True)
    return len(bad)


# --------------------------------------------------------------------------
#  Rust 側 (alloc_demo) を C の管理元と突き合わせる
# --------------------------------------------------------------------------

def header_int(name):
    text = CONV_HEADER.read_text(encoding="utf-8")
    m = re.search(r"#define\s+%s\s+(-?\d+)" % re.escape(name), text)
    if not m:
        raise SystemExit("%s が rt/testresult.h に無い" % name)
    return int(m.group(1))


def check_rust():
    """alloc_demo は no_std Rust で C のヘッダを使えない。書式と終了コードが
    testresult.h と同じであることをここで押さえる (票 §5-1)。"""
    bad = []
    text = RUST_DEMO.read_text(encoding="utf-8")

    if '"{}: {} {}/{}\\n' not in text:
        bad.append("alloc_demo: 集計行の書式が `<名前>: <動詞> <n>/<m>` でない")
    for verb in ('"PASS"', '"FAIL"'):
        if verb not in text:
            bad.append("alloc_demo: %s を出さない" % verb)
    for const, want in (("EXIT_PASS", header_int("OS32_TEST_EXIT_PASS")),
                        ("EXIT_FAIL", header_int("OS32_TEST_EXIT_FAIL"))):
        m = re.search(r"const\s+%s\s*:\s*i32\s*=\s*(-?\d+)\s*;" % const, text)
        if not m:
            bad.append("alloc_demo: %s を定義していない" % const)
        elif int(m.group(1)) != want:
            bad.append("alloc_demo: %s が %s (rt/testresult.h は %d)"
                       % (const, m.group(1), want))
    if not re.search(r'const\s+TEST_NAME\s*:\s*&str\s*=\s*"alloc_demo"', text):
        bad.append("alloc_demo: 名前が固定文字列 \"alloc_demo\" でない")
    # 「無条件に合格と印字して 0 を返す」旧版に戻っていないこと。
    # 見るのは**印字する側の字面** (b"...") だけ。経緯を書いた冒頭のコメントに
    # 同じ語が出るので、素の部分一致だと自分のコメントで落ちる。
    if 'b"All tests passed!' in text:
        bad.append("alloc_demo: 無条件の \"All tests passed!\" を印字している")
    if "sum == 285" not in text:
        bad.append("alloc_demo: sum=285 を比較していない (コメントだけになっている)")
    if not re.search(r"\bcheck\(", text):
        bad.append("alloc_demo: 検査を 1 つもしていない")

    for msg in bad:
        print("  FAIL %s" % msg, flush=True)
    print("RUST alloc_demo: %d violations" % len(bad), flush=True)
    return len(bad)


# --------------------------------------------------------------------------
#  否定側
# --------------------------------------------------------------------------

# (相対パス, 目印, 置き換え) — どれも**コンパイルは通る**。
MUTATIONS = [
    # 変異 1: 集計行は FAIL と言うのに 0 を返す (その逆も同じ穴)。
    ("userland/tests/stat_t.c",
     "    rc = os32_test_summary(line, sizeof(line), \"stat_t\", g_passed, g_total);\n"
     "    api->kprintf(rc ? ATTR_RED : ATTR_GREEN, \"%s\", line);\n"
     "    return rc;",
     "    rc = os32_test_summary(line, sizeof(line), \"stat_t\", g_passed, g_total);\n"
     "    api->kprintf(rc ? ATTR_RED : ATTR_GREEN, \"%s\", line);\n"
     "    (void)rc;\n"
     "    return 0;"),
    # 変異 2: 予約値 (127 = 実行ファイルが見つからない) を返す。
    ("userland/tests/font_load_test.c",
     "        rc = os32_test_summary_skip(line, sizeof(line), \"font_load_test\",\n"
     "                                    \"font file not found\");\n"
     "        printf(\"%s\", line);\n"
     "        return rc;",
     "        rc = os32_test_summary_skip(line, sizeof(line), \"font_load_test\",\n"
     "                                    \"font file not found\");\n"
     "        printf(\"%s\", line);\n"
     "        (void)rc;\n"
     "        return 127;"),
    # 変異 3: `void main` に戻す (終了コードが eax の残骸になる)。
    ("userland/tests/asset_test.c",
     "int main(int argc, char **argv, KernelAPI *sys_api)",
     "void main(int argc, char **argv, KernelAPI *sys_api)"),
    # 変異 4: 集計行の名前を argv[0] 由来にする。
    ("userland/tests/restest.c",
     "    rc = os32_test_summary(line, sizeof(line), \"restest\", g_passed, g_total);",
     "    rc = os32_test_summary(line, sizeof(line), argv[0], g_passed, g_total);"),
    # 変異 5: 約束事の本体を壊す — 総数 0 を合格にする (集計変数が初期値の
    #         まま早期 return した試験が緑になる)。
    ("userland/lib/rt/testresult.h",
     "    ok = (total > 0) && (pass == total);",
     "    ok = (pass == total);"),
    # 変異 6: 約束事の本体を壊す — 終了コードだけ 0 に固定する
    #         (集計行は FAIL のまま = 2 つの答えを持つ)。
    ("userland/lib/rt/testresult.h",
     "    return ok ? OS32_TEST_EXIT_PASS : OS32_TEST_EXIT_FAIL;",
     "    return OS32_TEST_EXIT_PASS;"),
    # 変異 7: SKIP を不合格と同じ 1 にする (前提の欠如と不合格が混ざる)。
    ("userland/lib/rt/testresult.h",
     "    return OS32_TEST_EXIT_SKIP;\n}",
     "    return OS32_TEST_EXIT_FAIL;\n}"),
    # 変異 8: alloc_demo を「何も検査しない」旧版へ戻す。
    ("userland/rust/alloc_demo/src/lib.rs",
     "        check(b\"sum of squares 0..9 == 285\\0\", sum == 285);",
     "        /* sum は 285 のはず (比較しない) */"),
]


def run_once(tmp, tag):
    """今のソースで一巡する。返り値: (ビルドできたか, 落ちた本数)"""
    fails = 0
    try:
        exe = build_host(tmp, tag)
    except subprocess.CalledProcessError:
        return False, 0
    out = subprocess.run([str(exe)], cwd=ROOT, timeout=300,
                         capture_output=True)
    if out.returncode != 0:
        fails += out.stdout.decode("utf-8", "replace").count("FAIL ")
        fails = max(fails, 1)
    fails += check_sources_quiet()
    fails += check_rust_quiet()
    return True, fails


def _quiet(fn):
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        n = fn()
    return n


def check_sources_quiet():
    return _quiet(check_sources)


def check_rust_quiet():
    return _quiet(check_rust)


def run_mutations(tmp):
    bad = 0
    for i, (relpath, old, new) in enumerate(MUTATIONS, 1):
        target = ROOT / relpath
        original = target.read_text(encoding="utf-8")
        label = "%d %s" % (i, pathlib.Path(relpath).name)
        if old not in original:
            print("MUTATE %-28s SKIP (目印が見つからない)" % label, flush=True)
            bad += 1
            continue
        try:
            target.write_text(original.replace(old, new, 1), encoding="utf-8")
            built, fails = run_once(tmp, "mut%d" % i)
            if not built:
                print("MUTATE %-28s **コンパイルが通らない = 目が働いていない**"
                      % label, flush=True)
                bad += 1
            elif fails == 0:
                print("MUTATE %-28s **GREEN のまま = 試験が規則を見ていない**"
                      % label, flush=True)
                bad += 1
            else:
                print("MUTATE %-28s RED (期待どおり落ちた: %d 件)"
                      % (label, fails), flush=True)
        finally:
            target.write_text(original, encoding="utf-8")
    return bad


# --------------------------------------------------------------------------

def run_target(tmp):
    """実機と同じ i386-elf クロスコンパイラで -Werror を通す ([C1])。"""
    for src in conforming_sources():
        rel = src.relative_to(ROOT)
        subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", str(rel),
                        "-o", str(tmp / (src.stem + ".o"))],
                       cwd=ROOT, check=True)
    print("TARGET i386-elf -Werror COMPILE PASS (%d sources)"
          % len(conforming_sources()), flush=True)
    for src in ("userland/tests/ring3_hello.c", "userland/tests/ring3_fault.c",
                "userland/tests/ring3_guard.c"):
        subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                        "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                       cwd=ROOT, check=True)
        print("TARGET i386-elf -Werror COMPILE PASS (%s)" % src, flush=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-result-conv-") as tmp:
        tmp = pathlib.Path(tmp)
        failed = 0

        exe = build_host(tmp, "result-conv")
        print("HOST GNU89 -Werror COMPILE PASS "
              "(real rt/testresult.h + 5 real test programs)", flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=300).returncode
        print("EXIT test_result_conv_host=%d" % rc, flush=True)
        failed += rc != 0

        failed += 1 if check_sources() else 0
        failed += 1 if check_rust() else 0

        if "--target" in sys.argv:
            run_target(tmp)

        if "--mutate" in sys.argv:
            failed += run_mutations(tmp)

        sys.exit(1 if failed else 0)
