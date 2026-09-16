"""TASK_FS_TYPE §3: 種別が「分からない」とき cp / mv / rm が断ること (受け手の試験)。

票:   docs/tasks/shell/TASK_FS_TYPE.md §3 (fs_is_dir は「不明」を運べない)
記録: tools/tests/fs_kind_callers_tdd.md

tools/tests/fs_kind_callers_host.c が実物の userland/shell/cmd_fs_shared.c と
userland/shell/cmd_file.c を 1 行も写さずそのまま #include し、KernelAPI と
shell.c 側の 2 本 (shell_print_help / shell_register_cmds) だけを贋物にして回す。
贋 FS は fs_kind_host.c と共有する (tools/tests/fs_kind_fake.h)。

sys_stat と sys_ls の両方に OS32_ERR_IO を注入して「種別が分からない」を作り、
cmd_file.c の呼び出し元 5 箇所 (cp の宛先 / cp の入力 / mv の FS またぎ /
mv の宛先 / rm) が**断りを出し、open / mkdir / rename / unlink を呼ばない**
ことを見る。いちばん重い反例は

    cp -r /src /u  (/u は読めないディレクトリ)  ->  /u/a.txt を上書き

  python3 -B tools/tests/test_fs_kind_callers.py [--target]

--target を付けると、実機と同じ i386-elf クロスコンパイラでも
cmd_fs_shared.c / cmd_file.c が -Werror で通ることを確かめる ([C1] C89/GNU89)。
make・エミュレータ・実配備には一切触れない。
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


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-fs-kind-callers-") as tmp:
        tmp = pathlib.Path(tmp)

        exe = tmp / "fs-kind-callers"
        subprocess.run(["gcc", *HOST_FLAGS, *HOST_INC,
                        str(ROOT / "tools/tests/fs_kind_callers_host.c"),
                        "-o", str(exe)], cwd=ROOT, check=True)
        print("HOST GNU89 -Werror COMPILE PASS "
              "(real cmd_fs_shared.c + cmd_file.c)", flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60).returncode
        print("EXIT fs_kind_callers_host=%d" % rc, flush=True)

        if "--target" in sys.argv:
            for src in ("userland/shell/cmd_fs_shared.c",
                        "userland/shell/cmd_file.c"):
                subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                                "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                               cwd=ROOT, check=True)
                print("TARGET i386-elf -Werror COMPILE PASS (%s)" % src,
                      flush=True)

        sys.exit(rc)
