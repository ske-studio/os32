"""シリアルの速度判定 (drivers/serial_plan.c) のホスト試験。

記録: tools/tests/serial_vfast_tdd.md
票  : docs/tasks/realhw/TASK_SERIAL_VFAST.md (実機を 115200bps まで上げる)

実物の drivers/serial_plan.c を 1 行も写さずに #include して回す。判定は
I/O もタイマも触らないので模型は要らない。

**ここはエミュレータでは踏めない。** NP21/W は通信速度を模擬しないので
(np21w-src/src/io/serial.c)、分周が合っているかは実機でしか分からない。
だからホストで表そのものを押さえる。

  python3 -B tools/tests/test_serial_vfast.py            # ホストで全ケース
  python3 -B tools/tests/test_serial_vfast.py --target   # + i386-elf で serial.c も通す
  python3 -B tools/tests/test_serial_vfast.py --mutate   # 否定側 (変異が RED になるか)
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/tests/serial_vfast_host.c"
SRC = ROOT / "drivers/serial_plan.c"
# カーネルと同じ i386-elf で通す実ソース。serial.c は判定を使う側。
TARGET_SRCS = [
    ("drivers/serial_plan.c", []),
    ("drivers/serial.c", []),
]

CASES = ["vfast_table", "compat_exact", "compat_inexact", "mode_choice",
         "tx_budget", "status_bits", "fifo_detect", "real_hw_story"]

FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement", "-D__cdecl="]
INCLUDES = ["-I" + str(ROOT / p) for p in ("include", "drivers")]

# 否定側。実装を 1 か所だけ壊して RED になることを見る。
# (パターン, 置換, 説明)
MUTATIONS = [
    (r"\{ 115200UL, SER_VFAST_DIV_115200 \},",
     "{ 115200UL, SER_VFAST_DIV_57600  },",
     "V･FAST の表を 1 行ずらす (115200 のつもりで 57600 が出る)"),
    (r"\{   9600UL, SER_VFAST_DIV_9600   \}",
     "{   4800UL, SER_VFAST_DIV_9600   }",
     "表に無い速度を足す (9600 の戻しが V･FAST に入らなくなる)"),
    (r"return \(u8\)\(\(mode == SER_MODE_VFAST\) \? SER_FSTS_RXRDY : STS_RXRDY\);",
     "return (u8)((mode == SER_MODE_VFAST) ? STS_RXRDY : STS_RXRDY);",
     "FIFO の RxRDY を互換のビット位置 (bit1) で見る"),
    (r"return \(u8\)\(\(mode == SER_MODE_VFAST\) \? SER_FSTS_TXRDY : STS_TXRDY\);",
     "return (u8)((mode == SER_MODE_VFAST) ? SER_FSTS_TXEMP : STS_TXRDY);",
     "FIFO の TxRDY を TxEMP (bit0) と取り違える"),
    (r"if \(us < SER_TX_BUDGET_MIN_US\) \{\n        us = SER_TX_BUDGET_MIN_US;\n    \}",
     "",
     "予算の下限を外す (速い速度で 0µs になり、直す前の hlt 待ちに戻る)"),
    (r"if \(want_vfast && has_fifo\) \{", "if (want_vfast || has_fifo) {",
     "FIFO があれば明示指定なしでも V･FAST に入る (起動時の既定 9600 が"
     "互換で上がらなくなる / FIFO 非搭載機でも 013Ah を叩く)"),
    (r"out->exact = \(u8\)\(\(out->actual == baud\) \? 1 : 0\);",
     "out->exact = 1;",
     "割り切れない分周を「ちょうど出る」と答える (38400 → 41600 を見逃す)"),
]


def host_build(tmp, source_text=None):
    """ハーネスをコンパイルして実行ファイルのパスを返す。"""
    src_dir = pathlib.Path(tmp)
    exe = src_dir / "serial-vfast-host"
    cmd = ["gcc", *FLAGS, *INCLUDES, str(HARNESS), "-o", str(exe)]
    if source_text is not None:
        # 変異させた serial_plan.c を一時の drivers/ に置いて、そちらを先に引かせる
        # (実物のソースは書き換えない = make check-par で並列に回せる)。
        mut = src_dir / "drivers"
        mut.mkdir(exist_ok=True)
        (mut / "serial_plan.c").write_text(source_text, encoding="utf-8")
        for name in ("serial_plan.h", "serial.h"):
            (mut / name).write_text(
                (ROOT / "drivers" / name).read_text(encoding="utf-8"),
                encoding="utf-8")
        shim = src_dir / "harness.c"
        shim.write_text(
            HARNESS.read_text(encoding="utf-8").replace(
                '"../../drivers/serial_plan.c"', '"drivers/serial_plan.c"'),
            encoding="utf-8")
        cmd = ["gcc", *FLAGS, "-I" + str(ROOT / "include"), "-I" + str(src_dir),
               "-I" + str(mut), str(shim), "-o", str(exe)]
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
    """カーネルと同じ i386-elf で drivers/serial.c ごと通す。"""
    for rel, extra in TARGET_SRCS:
        cmd = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386",
               "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-nostdlib",
               "-mno-red-zone", "-fcommon", "-fsigned-char", "-fno-short-enums",
               "-O2", "-Wall", "-Werror", "-Wdeclaration-after-statement",
               "-D__KERNEL_BUILD__",
               "-I" + str(ROOT), "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "arch/x86"), "-I" + str(ROOT / "platform/pc98"),
               "-I" + str(ROOT / "sdk/include/os32"), "-I" + str(ROOT / "drivers"),
               "-I" + str(ROOT / "kernel"), "-I" + str(ROOT / "lib"),
               *extra, "-c", str(ROOT / rel),
               "-o", str(pathlib.Path(tmp) / (rel.replace("/", "_") + ".o"))]
        subprocess.run(cmd, cwd=ROOT, check=True)
    print("TARGET i386-elf GNU89 -Werror PASS", flush=True)


def mutate(tmp):
    """実装を 1 か所ずつ壊して、どれも RED になることを見る。"""
    original = SRC.read_text(encoding="utf-8")
    bad = 0
    for i, (pattern, repl, why) in enumerate(MUTATIONS, 1):
        mutated, n = re.subn(pattern, repl, original, count=1)
        if n != 1:
            print(f"MUTATION {i} NOT APPLICABLE: {why}", flush=True)
            bad += 1
            continue
        try:
            exe = host_build(tmp, mutated)
        except subprocess.CalledProcessError:
            # コンパイルが通らないのも RED (見逃しではない)。
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
    with tempfile.TemporaryDirectory(prefix="os32-serial-vfast-") as tmp:
        exe = host_build(tmp)
        print("HOST GNU89 -Werror compile PASS (real drivers/serial_plan.c)",
              flush=True)
        if "--target" in args:
            build_target(tmp)
        rc = run_cases(exe, [a for a in args if not a.startswith("--")] or CASES)
        if "--mutate" in args:
            rc += mutate(tmp)
        sys.exit(bool(rc))
