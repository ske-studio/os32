"""`cat -n` の行番号は行の先頭でだけ出る (userland/shell/cmd_file.c)。

記録: tools/tests/cat_linenum_tdd.md

tools/tests/cat_linenum_host.c が実物の userland/shell/cmd_fs_shared.c と
userland/shell/cmd_file.c を 1 行も写さずそのまま #include し、KernelAPI と
shell.c 側の 2 本 (shell_print_help / shell_register_cmds) だけを贋物にして回す。
判定は sys_write(1, ...) に出た全バイトを、試験側が別に書いた素朴な参照実装と
1 バイトずつ突き合わせる。

直した欠陥は 2 つ:

  (a) `for (i = 0; i <= len; i++)` が i == len でも行番号を出すので、**改行で
      終わるファイル**では最後の改行の後ろに中身のない行がもう 1 行出ていた
      (`printf 'a\\nb\\n' | cat -n` が 3 行)。
  (b) cmd_cat は sys_read 1 回ごとに cat_with_linenum を呼ぶ。行頭かどうかの
      状態を読み取りをまたいで持たないので、**IO_BUF_SIZE (65536) を超える
      ファイル**では切れ目ごとに (a) と同じことが起きる。行の途中で切れても
      そこで 1 行終わったものとして扱われていた。

  python3 -B tools/tests/test_cat_linenum.py [--target] [--mutate]

--target を付けると、実機と同じ i386-elf クロスコンパイラでも
cmd_fs_shared.c / cmd_file.c が -Werror で通ることを確かめる ([C1] C89/GNU89)。
--mutate は**否定側**。(a) と (b) をそれぞれ元に戻した版を作り、この試験が
ちゃんと RED になることを見る。make・エミュレータ・実配備には一切触れない。
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

HOST_FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
              "-Wdeclaration-after-statement",
              "-D__cdecl=", "-D__OS32_USERLAND__"]
HOST_INC = ["-I" + str(ROOT / p) for p in
            (".", "include", "sdk/include", "sdk/include/os32",
             "userland/lib", "userland/shell")]

CROSS_DIR = pathlib.Path(os.environ.get("CROSS_DIR", "/usr/local/cross"))
if not CROSS_DIR.exists():
    alt = pathlib.Path.home() / "opt/cross"
    if alt.exists():
        CROSS_DIR = alt

TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                "-fno-pie", "-fno-stack-protector", "-nostdlib",
                "-mno-red-zone", "-fcommon", "-O2",
                "-Wall", "-Wextra", "-Werror",
                "-Wdeclaration-after-statement",
                "-D__OS32_USERLAND__", "-I.", "-Iinclude", "-Isdk/include",
                "-Isdk/include/os32", "-Iuserland/lib", "-Iuserland/shell",
                "-I" + str(CROSS_DIR / "i386-elf/include")]

SRC = ROOT / "tools/tests/cat_linenum_host.c"


def build_host(tmp, name):
    exe = tmp / name
    subprocess.run(["gcc", *HOST_FLAGS, *HOST_INC, str(SRC), "-o", str(exe)],
                   cwd=ROOT, check=True)
    return exe


# 否定側。cmd_file.c を一時的に書き換えて、この試験が RED になることを見る。
MUTATIONS = [
    # 変異 1 = 欠陥 (a): 元の `for (i = 0; i <= len; i++)` は i == len でも
    # 1 行終わったことにして行番号を出していた。バッファが改行で終わっている
    # (= *at_bol が立っている) ときの「中身のない行」がそれ。
    ("a_extra_number_at_end",
     "        if (i > start) {\n"
     "            g_api->sys_write(1, &data[start], i - start);\n"
     "        }\n"
     "    }\n"
     "}",
     "        if (i > start) {\n"
     "            g_api->sys_write(1, &data[start], i - start);\n"
     "        }\n"
     "    }\n"
     "    if (*at_bol) {\n"
     "        int n = *line_num;\n"
     "        char tmp[12];\n"
     "        int ti = 0;\n"
     "        nlen = 0;\n"
     "        if (n == 0) tmp[ti++] = '0';\n"
     "        while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }\n"
     "        for (j = 0; j < 6 - ti; j++) num_buf[nlen++] = ' ';\n"
     "        while (ti > 0) num_buf[nlen++] = tmp[--ti];\n"
     "        num_buf[nlen++] = ' ';\n"
     "        num_buf[nlen++] = ' ';\n"
     "        g_api->sys_write(1, num_buf, nlen);\n"
     "        g_api->sys_write(1, \"\\n\", 1);\n"
     "        (*line_num)++;\n"
     "    }\n"
     "}"),
    # 変異 2 = 欠陥 (b): 行頭の状態を読み取りごとに捨てる版に戻す。
    # IO_BUF_SIZE の切れ目ごとに行番号が 1 つ余分に増える。
    ("b_state_not_carried",
     "            int at_bol = 1;\n"
     "            while (1) {",
     "            int at_bol = 1;\n"
     "            while (1) {\n"
     "                at_bol = 1;"),
]


def run_mutations(tmp):
    target = ROOT / "userland/shell/cmd_file.c"
    bad = 0
    for name, old, new in MUTATIONS:
        original = target.read_text(encoding="utf-8")
        if old not in original:
            print("MUTATE %-24s SKIP (目印が見つからない)" % name, flush=True)
            bad += 1
            continue
        try:
            target.write_text(original.replace(old, new, 1), encoding="utf-8")
            try:
                exe = build_host(tmp, "mut-" + name)
            except subprocess.CalledProcessError:
                print("MUTATE %-24s RED (コンパイルが通らない)" % name,
                      flush=True)
                continue
            out = subprocess.run([str(exe)], cwd=ROOT, timeout=120,
                                 capture_output=True)
            if out.returncode == 0:
                print("MUTATE %-24s **GREEN のまま = 試験が規則を見ていない**"
                      % name, flush=True)
                bad += 1
            else:
                fails = out.stdout.decode("utf-8", "replace").count("FAIL ")
                print("MUTATE %-24s RED (期待どおり落ちた: FAIL %d 件)"
                      % (name, fails), flush=True)
        finally:
            target.write_text(original, encoding="utf-8")
    return bad


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-cat-linenum-") as tmp:
        tmp = pathlib.Path(tmp)
        failed = 0

        exe = build_host(tmp, "cat-linenum")
        print("HOST GNU89 -Werror COMPILE PASS "
              "(real cmd_fs_shared.c + cmd_file.c)", flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=120).returncode
        print("EXIT cat_linenum_host=%d" % rc, flush=True)
        failed += rc != 0

        if "--target" in sys.argv:
            for src in ("userland/shell/cmd_fs_shared.c",
                        "userland/shell/cmd_file.c"):
                subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                                "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                               cwd=ROOT, check=True)
                print("TARGET i386-elf -Werror COMPILE PASS (%s)" % src,
                      flush=True)

        if "--mutate" in sys.argv:
            failed += run_mutations(tmp)

        sys.exit(1 if failed else 0)
