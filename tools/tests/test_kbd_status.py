"""キーボード 8251 のステータス判定 (drivers/kbd_status.c) のホスト試験。

記録: tools/tests/kbd_status_tdd.md
経緯: docs/POLICY_DEBUG.md §4-57 (実機 PC-9821Ra266 で打鍵が届かない)

実物の drivers/kbd_status.c を 1 行も写さずに #include して回す。I/O を
持たないので模型は要らない。

**ここで見る分岐のうち EMPTY と ERROR は NP21/W では踏めない** — NP21/W の
keyboard_i43 は IRQ1 の前に必ず RxRDY を立て、エラービットは自前の
バッファが溢れたときの OE しか出さない。

  python3 -B tools/tests/test_kbd_status.py            # ホストで全ケース
  python3 -B tools/tests/test_kbd_status.py --target   # + i386-elf で kbd.c も通す
  python3 -B tools/tests/test_kbd_status.py --mutate   # 否定側 (変異が RED になるか)
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/tests/kbd_status_host.c"
SRC = ROOT / "drivers/kbd_status.c"
HDR = ROOT / "drivers/kbd_status.h"

# カーネルと同じ i386-elf で通す実ソース。kbd.c まで入れるのは、
# IRQ1 ハンドラが判定を**実際に**呼ぶ形と KbdDiag の大きさ (STATIC_ASSERT)
# を固定するため。
TARGET_SRCS = [
    "drivers/kbd_status.c",
    "drivers/kbd.c",
]

CASES = ["empty", "error", "data"]

FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement"]

# 否定側。実装を 1 か所だけ壊して RED になることを見る。
# (対象ファイル, パターン, 置換, 説明)
MUTATIONS = [
    ("c", r"    if \(!\(st & KBD_STAT_RXRDY\)\) \{\n        return KBD_ST_EMPTY;\n    \}\n",
     "",
     "RxRDY を見ない (空 IRQ でも 0041h を読んで前回のバイトを打鍵にする)"),
    ("c", r"    if \(st & KBD_STAT_ERRORS\) \{\n        return KBD_ST_ERROR;\n    \}\n",
     "",
     "エラービットを見ない (化けたバイトを打鍵として配る)"),
    ("c", r"    if \(!\(st & KBD_STAT_RXRDY\)\) \{\n        return KBD_ST_EMPTY;\n    \}\n"
          r"    if \(st & KBD_STAT_ERRORS\) \{\n        return KBD_ST_ERROR;\n    \}\n",
     "    if (st & KBD_STAT_ERRORS) {\n        return KBD_ST_ERROR;\n    }\n"
     "    if (!(st & KBD_STAT_RXRDY)) {\n        return KBD_ST_EMPTY;\n    }\n",
     "エラーを RxRDY より先に見る (受信データが無いのに 0041h を読み捨てる)"),
    ("h", r"#define KBD_STAT_RXRDY   0x02",
     "#define KBD_STAT_RXRDY   0x04",
     "RxRDY を bit2 (TxEMP) と取り違える (NP21/W では常に立つので全部 DATA に化ける)"),
    ("h", r"#define KBD_STAT_ERRORS  \(KBD_STAT_PE \| KBD_STAT_OE \| KBD_STAT_FE\)",
     "#define KBD_STAT_ERRORS  (KBD_STAT_PE | KBD_STAT_FE)",
     "OE を落とす (オーバーランのバイトを使う)"),
    ("h", r"#define KBD_STAT_ERRORS  \(KBD_STAT_PE \| KBD_STAT_OE \| KBD_STAT_FE\)",
     "#define KBD_STAT_ERRORS  (KBD_STAT_PE | KBD_STAT_OE | KBD_STAT_FE | 0x80)",
     "DSR (bit7) をエラーに数える (NP21/W の `| 0x85` で打鍵が全部落ちる)"),
]


def host_build(tmp, c_text=None, h_text=None):
    """ハーネスをコンパイルして実行ファイルのパスを返す。"""
    src_dir = pathlib.Path(tmp)
    exe = src_dir / "kbd-status-host"
    if c_text is None and h_text is None:
        cmd = ["gcc", *FLAGS, str(HARNESS), "-o", str(exe)]
    else:
        mut = src_dir / "drivers"
        mut.mkdir(exist_ok=True)
        (mut / "kbd_status.c").write_text(
            c_text if c_text is not None else SRC.read_text(encoding="utf-8"),
            encoding="utf-8")
        (mut / "kbd_status.h").write_text(
            h_text if h_text is not None else HDR.read_text(encoding="utf-8"),
            encoding="utf-8")
        shim = src_dir / "harness.c"
        shim.write_text(
            HARNESS.read_text(encoding="utf-8").replace(
                '"../../drivers/kbd_status.c"', '"drivers/kbd_status.c"'),
            encoding="utf-8")
        cmd = ["gcc", *FLAGS, "-I" + str(src_dir), str(shim), "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return exe


def run_cases(exe, cases):
    failed = 0
    for case in cases:
        rc = subprocess.run([str(exe), case], cwd=ROOT).returncode
        print(f"EXIT {case}={rc}", flush=True)
        failed += rc != 0
    print(f"SUMMARY {len(cases) - failed}/{len(cases)} PASS", flush=True)
    return failed


def build_target(tmp):
    """カーネルと同じ i386-elf で drivers/kbd.c ごと通す。"""
    for rel in TARGET_SRCS:
        cmd = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386",
               "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-nostdlib",
               "-mno-red-zone", "-fcommon", "-fsigned-char", "-fno-short-enums",
               "-O2", "-Wall", "-Werror", "-Wdeclaration-after-statement",
               "-D__KERNEL_BUILD__",
               "-I" + str(ROOT), "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "arch/x86"), "-I" + str(ROOT / "platform/pc98"),
               "-I" + str(ROOT / "sdk/include/os32"), "-I" + str(ROOT / "kernel"),
               "-I" + str(ROOT / "drivers"), "-I" + str(ROOT / "lib"),
               "-c", str(ROOT / rel),
               "-o", str(pathlib.Path(tmp) / (rel.replace("/", "_") + ".o"))]
        subprocess.run(cmd, cwd=ROOT, check=True)
    print("TARGET i386-elf GNU89 -Werror PASS", flush=True)


def mutate(tmp):
    """実装を 1 か所ずつ壊して、どれも RED になることを見る。"""
    orig = {"c": SRC.read_text(encoding="utf-8"),
            "h": HDR.read_text(encoding="utf-8")}
    bad = 0
    for i, (which, pattern, repl, why) in enumerate(MUTATIONS, 1):
        mutated, n = re.subn(pattern, repl, orig[which], count=1)
        if n != 1:
            print(f"MUTATION {i} NOT APPLICABLE: {why}", flush=True)
            bad += 1
            continue
        try:
            if which == "c":
                exe = host_build(tmp, c_text=mutated)
            else:
                exe = host_build(tmp, h_text=mutated)
        except subprocess.CalledProcessError:
            print(f"MUTATION {i} RED (compile): {why}", flush=True)
            continue
        hits = sum(subprocess.run([str(exe), c], cwd=ROOT,
                                  stderr=subprocess.DEVNULL).returncode != 0
                   for c in CASES)
        status = "RED" if hits else "**GREEN (見逃し)**"
        print(f"MUTATION {i} {status} ({hits} 件): {why}", flush=True)
        bad += not hits
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="os32-kbd-status-") as tmp:
        exe = host_build(tmp)
        print("HOST GNU89 -Werror compile PASS (real drivers/kbd_status.c)",
              flush=True)
        if "--target" in args:
            build_target(tmp)
        rc = run_cases(exe, [a for a in args if not a.startswith("--")] or CASES)
        if "--mutate" in args:
            rc += mutate(tmp)
        sys.exit(bool(rc))
