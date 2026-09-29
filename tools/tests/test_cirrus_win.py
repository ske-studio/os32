"""Cirrus の窓の可否は RAM の上端ではなく物理地図で決める (gfx/backend_cirrus.c)。

記録: tools/tests/cirrus_win_tdd.md / 教訓 docs/POLICY_DEBUG.md §4-34

tools/tests/cirrus_win_host.c が実物の gfx/backend_cirrus.c を 1 行も写さずに
#include し、pgalloc_range_has_ram (贋の物理地図) と sys_get_mem_kb (上端) と
ボードグルーだけを贋物にして ILP32 で回す (test_pgalloc_range.py と同じ形)。

  python3 -B tools/tests/test_cirrus_win.py [--mutate]

--mutate は否定側。判定を壊した版 (上端で見る旧判定に戻す / 物理地図を
見ない / 窓の末尾ページを問い合わせから落とす / 窓の前のページまで広げる /
ページングの守備範囲を見ない) を写しの木で組み、この試験が RED になることを見る。
make・エミュレータ・配備には触れない。
"""
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutpar                                                   # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
         "-fno-stack-protector", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement"]
# build/config.mk の INC_GFX と同じ探索先 (INC_COMMON + gfx drivers fs lib kernel)。
INCLUDES = ["-I" + str(ROOT / p)
            for p in ("include", "arch/x86", "platform/pc98",
                      "sdk/include/os32",
                      "gfx", "drivers", "fs", "lib", "kernel")]
SRC = ROOT / "tools/tests/cirrus_win_host.c"
MUT_TARGET = "gfx/backend_cirrus.c"


def host_cmd(exe):
    return ["gcc", *FLAGS, "-Wno-unused-function", "-O0", "-nostdlib",
            "-static", "-no-pie", *INCLUDES, str(SRC), "-o", str(exe)]


def target_cmd(obj):
    return ["i386-elf-gcc", *FLAGS, "-O2", *INCLUDES,
            "-c", str(ROOT / MUT_TARGET), "-o", str(obj)]


MUTATIONS = [
    # 1 = 本件の欠陥そのもの: RAM の上端 (sys_get_mem_kb) で窓を決める旧判定。
    ("top_of_ram",
     "    return !pgalloc_range_has_ram(base / PAGE_SIZE,\n"
     "                                  (base + size + PAGE_SIZE - 1) / PAGE_SIZE);",
     "    return !(sys_get_mem_kb() > base / 1024UL);"),
    # 2 = 物理地図を見ない (窓は常に空いている扱い)。
    ("no_map",
     "    return !pgalloc_range_has_ram(base / PAGE_SIZE,\n"
     "                                  (base + size + PAGE_SIZE - 1) / PAGE_SIZE);",
     "    return 1;"),
    # 3 = 窓の末尾ページを問い合わせから落とす (部分一致の見落とし)。
    ("end_short",
     "(base + size + PAGE_SIZE - 1) / PAGE_SIZE);",
     "(base + size - 1) / PAGE_SIZE);"),
    # 4 = 窓の前のページまで問い合わせる (隣の RAM で窓を塞ぐ)。
    ("first_early",
     "    return !pgalloc_range_has_ram(base / PAGE_SIZE,",
     "    return !pgalloc_range_has_ram(base / PAGE_SIZE - 1,"),
    # 5 = ページングの守備範囲の末尾を見ない。
    ("no_paging_tail",
     "    if (size > PAGING_MAP_SIZE - base) return 0;\n",
     ""),
]


def one_mutation(item):
    name, old, new = item
    original = (ROOT / MUT_TARGET).read_text(encoding="utf-8")
    if old not in original:
        return "MUTATE %-16s SKIP (目印が見つからない)" % name, 1
    with tempfile.TemporaryDirectory(prefix="os32-cirrus-win-mut-") as td:
        exe = pathlib.Path(td) / ("mut-" + name)
        try:
            tree = mutpar.build_in_tree(
                ROOT, td, {MUT_TARGET: original.replace(old, new, 1)},
                [host_cmd(exe)], capture_output=True)
        except subprocess.CalledProcessError:
            return "MUTATE %-16s RED (コンパイルが通らない)" % name, 0
        out = subprocess.run([str(exe)], cwd=str(tree), timeout=60,
                             capture_output=True)
    if out.returncode == 0:
        return ("MUTATE %-16s **GREEN のまま = 試験が規則を見ていない**"
                % name, 1)
    last = out.stdout.decode("utf-8", "replace").strip().splitlines()[-1:]
    return "MUTATE %-16s RED (期待どおり落ちた: %s)" % (
        name, last[0] if last else "rc=%d" % out.returncode), 0


if __name__ == "__main__":
    failed = 0
    with tempfile.TemporaryDirectory(prefix="os32-cirrus-win-") as tmp:
        tmp = pathlib.Path(tmp)
        exe = tmp / "cirrus-win"
        subprocess.run(host_cmd(exe), cwd=ROOT, check=True)
        print("HOST ILP32 GNU89 COMPILE PASS (real gfx/backend_cirrus.c)",
              flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60).returncode
        print("EXIT cirrus_win_host=%d" % rc, flush=True)
        failed += rc != 0
        subprocess.run(target_cmd(tmp / "backend_cirrus.o"), cwd=ROOT,
                       check=True)
        print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)
        if "--mutate" in sys.argv:
            failed += mutpar.run_with_control(one_mutation, MUTATIONS,
                                              ("control", "", ""))
    sys.exit(1 if failed else 0)
