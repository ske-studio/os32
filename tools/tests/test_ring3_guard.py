"""WM の文脈では KAPI の出力検査を効かせない — 実物の exec/ring3_str.c と kernel/gui.c で。

票:   docs/tasks/memory/TASK_KAPI_OUTPUT_GUARD.md (追補 2026-09-26)
記録: tools/tests/ring3_guard_tdd.md

GUI で filer.bin を起動すると窓が出ずに消え、fault_kill_count が +1 した。
WM (gshell、CPL=0) はアプリの syscall の中で走るので ring3_in_syscall = 1 の
まま、WM 自身のスタックの MouseInfo が「アプリの出力先」として検査され、
シェル帯には USER が無いので拒否 → アプリが kill。直しは「カーネルが WM の
コードへ入っている深さ」を gui_call / ポンプ / owner_exit で数え、判定を
ring3_guard_active(in_syscall, wm_depth) に寄せること。

見るもの:
  (1) 判定表 — in_syscall × wm_depth (負は安全側)
  (2) gui_call / gui_owner_exit の前後で深さが対になる (入れ子も)

test_ring3_str.py と同じ様式 — ホスト ILP32 GNU89 で走らせたあと、同じ
ソースが i386-elf-gcc -Werror でも通ることを見る ([C1])。libc は使わない。

  python3 -B tools/tests/test_ring3_guard.py            # ホスト
  python3 -B tools/tests/test_ring3_guard.py --target   # + i386-elf の -Werror
  python3 -B tools/tests/test_ring3_guard.py --mutate   # 否定側 (写しの上で変異)
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
         "-fno-stack-protector", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement"]
INC_DIRS = ("include", "kernel", "lib", "exec", "sdk/include/os32")
HOST_SRC = ROOT / "tools/tests/ring3_guard_host.c"
KERNEL_SRCS = [ROOT / "exec/ring3_str.c", ROOT / "kernel/gui.c"]

# 否定側: 実物の 1 行を壊すと RED になることを見る (写しの上で。ソースは触らない)。
MUTATIONS = [
    ("exec/ring3_str.c",
     "    if (wm_depth > 0) return 0;\n",
     "    (void)wm_depth;\n",
     "WM の文脈を見ない (= 直す前の判定)"),
    ("kernel/gui.c",
     "    ring3_wm_enter();\n    r = g_gui_handler(op, arg, res_owner_get());\n",
     "    r = g_gui_handler(op, arg, res_owner_get());\n",
     "gui_call が印を立てない"),
    ("kernel/gui.c",
     "    r = g_gui_handler(op, arg, res_owner_get());\n    ring3_wm_leave();\n",
     "    r = g_gui_handler(op, arg, res_owner_get());\n",
     "gui_call が印を下ろさない"),
    ("kernel/gui.c",
     "        ring3_wm_enter();\n        g_gui_handler(GUI_OP_OWNER_EXIT, 0, owner);\n",
     "        g_gui_handler(GUI_OP_OWNER_EXIT, 0, owner);\n",
     "owner_exit が印を立てない"),
]


def includes(root):
    return ["-I" + str(root / p) for p in INC_DIRS]


def run_host(root, tmp, quiet=False):
    exe = tmp / "ring3-guard"
    p = subprocess.run(["gcc", *FLAGS, "-O0", "-D__KERNEL_BUILD__", *includes(root),
                        "-nostdlib", "-static", "-no-pie",
                        str(root / "tools/tests/ring3_guard_host.c"), "-o", str(exe)],
                       cwd=root, capture_output=True, text=True)
    if p.returncode != 0:
        if not quiet:
            sys.stdout.write(p.stdout + p.stderr)
        return p.returncode
    return subprocess.run([str(exe)], cwd=root, timeout=30,
                          stdout=subprocess.DEVNULL if quiet else None).returncode


def mutate():
    """実物の写し (一時ディレクトリ) に変異を当てて、ホスト試験が落ちることを見る。"""
    red = 0
    with tempfile.TemporaryDirectory(prefix="os32-ring3-guard-mut-") as tmp:
        tmp = pathlib.Path(tmp)
        for i, (rel, old, new, why) in enumerate(MUTATIONS, 1):
            copy = tmp / ("m%d" % i)
            for d in INC_DIRS + ("tools/tests",):
                shutil.copytree(ROOT / d, copy / d, dirs_exist_ok=True)
            src = copy / rel
            text = src.read_text(encoding="utf-8")
            if text.count(old) != 1:
                print("MUTATION %d SKIP (pattern not found once): %s" % (i, why))
                return 1
            src.write_text(text.replace(old, new), encoding="utf-8")
            rc = run_host(copy, copy, quiet=True)
            state = "RED" if rc != 0 else "GREEN (bad)"
            print("MUTATION %d %s: %s" % (i, state, why), flush=True)
            if rc != 0:
                red += 1
    print("MUTATIONS %d/%d RED" % (red, len(MUTATIONS)))
    return 0 if red == len(MUTATIONS) else 1


if __name__ == "__main__":
    if "--mutate" in sys.argv:
        sys.exit(mutate())
    with tempfile.TemporaryDirectory(prefix="os32-ring3-guard-") as tmp:
        tmp = pathlib.Path(tmp)
        rc = run_host(ROOT, tmp)
        print("HOST ILP32 GNU89 EXIT ring3_guard_host=%d" % rc, flush=True)
        if rc == 0 and "--target" in sys.argv:
            for src in KERNEL_SRCS:
                subprocess.run(["i386-elf-gcc", *FLAGS, "-D__KERNEL_BUILD__",
                                *includes(ROOT), "-O2", "-c", str(src),
                                "-o", str(tmp / (src.stem + ".o"))],
                               cwd=ROOT, check=True)
            print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)
        sys.exit(rc)
