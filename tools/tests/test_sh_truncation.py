"""切り詰め: シェルが入力を黙って切り詰める経路を実物のソースで押さえる。

票:   docs/tasks/shell/TASK_SH_TRUNCATION.md §5 の段 1「足場」
記録: tools/tests/sh_truncation_tdd.md

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


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-sh-trunc-") as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "string.h").write_text(STRING_SHIM)
        (tmp / "stdio.h").write_text(STDIO_SHIM)
        (tmp / "stdlib.h").write_text(STDLIB_SHIM)
        shim = ["-I" + str(tmp)]
        exe = tmp / "sh_truncation"
        subprocess.run(["gcc", *BASE, "-O0", *shim, *INCLUDES,
                        "-nostdlib", "-static", "-no-pie",
                        str(HOST_SRC), "-o", str(exe)], cwd=ROOT, check=True)
        print("HOST ILP32 GNU89 COMPILE PASS", flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=30).returncode

        subprocess.run(["i386-elf-gcc", *BASE, "-O2", "-nostdlib",
                        "-mno-red-zone", "-fcommon", *shim, *INCLUDES,
                        "-c", str(HOST_SRC), "-o", str(tmp / "sh_truncation.o")],
                       cwd=ROOT, check=True)
        print("TARGET i386-elf GNU89 COMPILE PASS", flush=True)
        print("EXIT sh_truncation_host=%d" % rc, flush=True)
        sys.exit(rc)
