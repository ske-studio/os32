"""切り詰め: シェルが入力を黙って切り詰める経路を実物のソースで押さえる。

票:   docs/tasks/shell/TASK_SH_TRUNCATION.md §5 の段 2 (T1 と §2-1)
記録: tools/tests/sh_truncation_tdd.md

  python3 -B tools/tests/test_sh_truncation.py [--mutate]

--mutate は**否定側**。この段の中心規則は

  (1) 切り詰めた値では比べない (T1)
  (2) スクリプト実行中に行を断ったら打ち切る (§2-1)
  (3) 対話 / rshell / 起動時の profile では打ち切らない

の 3 つなので、それぞれをわざと壊した版を作って**試験が落ちること**を見る。
GREEN のまま通ってしまう変異があれば、その規則を試験が見ていないということ。

test_sh_shell.py と同じ様式 — ホスト ILP32 GNU89 で走らせ、同じソースが外部
プログラムと同じ形の i386-elf-gcc でも通ることを別に見る ([C1] C89/GNU89)。
Make・エミュレータは使わない。

ホスト側は tools/tests/sh_truncation_host.c が userland/shell/main.c を
(したがって sh_exec.inc / sh_args.inc / sh_launch.inc / sh_pipe.inc も)
そのまま #include し、登録表と execute_command を**実物のまま**通す。
libc は使わない (-nostdlib) ので、shell.h が引く <string.h> と
<stdio.h> / <stdlib.h> だけ一時ディレクトリに薄いシムを置く。
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
        "-fno-stack-protector", "-Wall", "-Wdeclaration-after-statement",
        "-D__OS32_USERLAND__", "-DSHELL_AS_APP"]
INCLUDES = ["-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/include/os32"),
            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "userland/shell")]
HOST_SRC = ROOT / "tools/tests/sh_truncation_host.c"

STRING_SHIM = """/* テスト用の薄い <string.h>。実体は sh_truncation_host.c にある。 */
#ifndef OS32_TEST_STRING_H
#define OS32_TEST_STRING_H
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);
unsigned long strlen(const char *s);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
char *strncpy(char *d, const char *s, unsigned long n);
char *strncat(char *d, const char *s, unsigned long n);
char *strcat(char *d, const char *s);
#endif
"""

STDIO_SHIM = """/* テスト用の薄い <stdio.h>。main.c が使うのは setvbuf / fflush / printf。 */
#ifndef OS32_TEST_STDIO_H
#define OS32_TEST_STDIO_H
#define BUFSIZ 1024
#define _IOLBF 1
extern void *stdout_impl;
#define stdout (stdout_impl)
int printf(const char *fmt, ...);
int fflush(void *stream);
int setvbuf(void *stream, char *buf, int mode, unsigned long sz);
#endif
"""

STDLIB_SHIM = """/* テスト用の薄い <stdlib.h>。cmd_mnt.c が使うのは atoi だけ。 */
#ifndef OS32_TEST_STDLIB_H
#define OS32_TEST_STDLIB_H
int atoi(const char *s);
#endif
"""


def write_shims(tmp):
    (tmp / "string.h").write_text(STRING_SHIM)
    (tmp / "stdio.h").write_text(STDIO_SHIM)
    (tmp / "stdlib.h").write_text(STDLIB_SHIM)
    return ["-I" + str(tmp)]


def build_host(tmp, shim, name):
    exe = tmp / name
    subprocess.run(["gcc", *BASE, "-O0", *shim, *INCLUDES,
                    "-nostdlib", "-static", "-no-pie",
                    str(HOST_SRC), "-o", str(exe)], cwd=ROOT, check=True)
    return exe


# ---------------------------------------------------------------------------
#  変異 (否定側)。(ファイル, 置換前, 置換後) — 置換前は 1 か所だけに出ること。
# ---------------------------------------------------------------------------
MUTATIONS = [
    # 変異 1: 段 2 の前の姿。strip_quotes が黙って max-1 文字に切る。
    #         → 先頭 255 文字が同じ 2 つの値が「等しい」になる (T1 / U1 / U2)。
    ("compare_truncated", "userland/shell/cmd_script.c",
     "            if (di >= max - 1) return -1;",
     "            if (di >= max - 1) break;"),
    # 変異 2: 断っても**印を立てない**版 (§2-1 の否定側)。
    #         断り自体は出るので、スクリプトが後続行へ落ちるかどうかだけが変わる。
    ("no_mark", "userland/shell/main.c",
     '    g_api->kprintf(ATTR_RED, "%s too long (max %d)\\n", what, limit);\n'
     "    sh_refused_flag = 1;",
     '    g_api->kprintf(ATTR_RED, "%s too long (max %d)\\n", what, limit);'),
    # 変異 3: 印を**消し忘れる**版。前の行の断りが次の行に持ち越され、
    #         関係のないスクリプトが 1 行目で打ち切られる (誤発火)。
    ("no_clear", "userland/shell/main.c",
     "    if (g_exec_depth == 0) sh_refused_flag = 0;",
     "    if (0) sh_refused_flag = 0;"),
    # 変異 4: 入れ子でも印を消す版。断った段の後ろの段が `time ...` だと
    #         そこで印が消えて後続行へ落ちる (取りこぼし)。
    ("nested_clear", "userland/shell/main.c",
     "    if (g_exec_depth == 0) sh_refused_flag = 0;",
     "    sh_refused_flag = 0;"),
    # 変異 5: **対話でも打ち切る**版。断った行の次の行が動かなくなり、
    #         rshell も起動時の profile も道連れになる。
    ("abort_interactive", "userland/shell/main.c",
     "    if (g_exec_depth == 0) sh_refused_flag = 0;",
     "    if (g_exec_depth == 0 && sh_refused_flag) return;\n"
     "    if (g_exec_depth == 0) sh_refused_flag = 0;"),
    # 変異 6: 入れ子 source の断りを親へ伝えない版 (§2-1 の否定側)。
    ("source_not_propagated", "userland/shell/cmd_script.c",
     "    if (script_source_file(argv[1]) == SCRIPT_ERR_REFUSED) sh_refuse_mark();",
     "    (void)script_source_file(argv[1]);"),
    # 変異 7: パイプの段ループが印を**見ない**版 (= PM 決裁の前の姿)。
    #         断った段の後続の段が走り、`> file` が O_TRUNC で開かれる。
    ("pipe_no_peek", "userland/shell/main.c",
     "                if (sh_refused_peek()) break;",
     "                if (0) break;"),
    # 変異 8: 起動時の profile が印を立て直す版 (R2 の否定側)。
    #         profile の断りが起動後の 1 行目を巻き添えにする。
    ("profile_aborts_boot", "userland/shell/cmd_script.c",
     '        g_api->kprintf(ATTR_RED,\n'
     '                       "sh: %s aborted; continuing with defaults\\n", path);',
     '        g_api->kprintf(ATTR_RED,\n'
     '                       "sh: %s aborted; continuing with defaults\\n", path);\n'
     "        sh_refuse_mark();"),
]


def run_mutations(tmp, shim):
    bad = 0
    for name, rel, old, new in MUTATIONS:
        target = ROOT / rel
        original = target.read_text(encoding="utf-8")
        if original.count(old) != 1:
            print("MUTATE %-22s SKIP (目印が %d か所)"
                  % (name, original.count(old)), flush=True)
            bad += 1
            continue
        try:
            target.write_text(original.replace(old, new, 1), encoding="utf-8")
            try:
                exe = build_host(tmp, shim, "mut-" + name)
            except subprocess.CalledProcessError:
                print("MUTATE %-22s RED (コンパイルが通らない)" % name, flush=True)
                continue
            rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60,
                                capture_output=True).returncode
            if rc == 0:
                print("MUTATE %-22s **GREEN のまま = 試験が規則を見ていない**"
                      % name, flush=True)
                bad += 1
            else:
                print("MUTATE %-22s RED (期待どおり落ちた)" % name, flush=True)
        finally:
            target.write_text(original, encoding="utf-8")
    return bad


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-sh-trunc-") as tmp:
        tmp = pathlib.Path(tmp)
        shim = write_shims(tmp)
        failed = 0

        exe = build_host(tmp, shim, "sh_truncation")
        print("HOST ILP32 GNU89 COMPILE PASS", flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=30).returncode

        subprocess.run(["i386-elf-gcc", *BASE, "-O2", "-nostdlib",
                        "-mno-red-zone", "-fcommon", *shim, *INCLUDES,
                        "-c", str(HOST_SRC), "-o", str(tmp / "sh_truncation.o")],
                       cwd=ROOT, check=True)
        print("TARGET i386-elf GNU89 COMPILE PASS", flush=True)
        print("EXIT sh_truncation_host=%d" % rc, flush=True)
        failed += rc != 0

        if "--mutate" in sys.argv:
            failed += run_mutations(tmp, shim)

        sys.exit(1 if failed else 0)
