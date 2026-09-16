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

6 章は別の欠陥 (継承バグ台帳): **`cp -r` が失敗時に空のディレクトリを
残す**。do_copy_recursive_impl は sys_mkdir(dst) を件数の確認より前に呼び、
しかも戻り値を見ていなかった。収集してから mkdir する / 戻り値を見る /
既に在る**ディレクトリ**への上書きコピーだけは通す、の 3 つを見る。

  python3 -B tools/tests/test_fs_kind_callers.py [--target] [--mutate]

--target を付けると、実機と同じ i386-elf クロスコンパイラでも
cmd_fs_shared.c / cmd_file.c が -Werror で通ることを確かめる ([C1] C89/GNU89)。
--mutate は**否定側**。mkdir を収集の前へ戻した版 / 戻り値を見ない版 /
EXIST を型を見ずに通す版 / 列挙の失敗を無視する版を作り、この試験が
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


# 否定側。cmd_file.c を一時的に書き換えて、この試験が RED になることを見る。
MKDIR_BLOCK = r"""    rc = g_api->sys_mkdir(dst);
    if (rc != 0 && !(rc == OS32_ERR_EXIST && fs_path_kind(dst) == FS_KIND_DIR)) {
        g_api->kprintf(ATTR_RED, "cp -r: cannot create directory '%s': %s\n",
                       dst, fs_strerror(rc));
        g_api->mem_free(local_entries);
        return;
    }
"""

LS_BLOCK = r"""    rc = g_api->sys_ls(src, collect_entries_cb, (void *)0);
    if (rc < 0) {
        g_api->kprintf(ATTR_RED, "cp -r: cannot read directory '%s': %s\n",
                       src, fs_strerror(rc));
        g_api->mem_free(local_entries);
        return;
    }
"""

MUTATIONS = [
    # 変異 1 = 欠陥そのもの: mkdir を収集の**前**へ戻す。
    # 件数超過や列挙の失敗で引き返す経路が空のディレクトリを残す。
    ("mkdir_before_collect",
     "    /* **\u53ce\u96c6\u304c\u5148\u3001mkdir \u306f\u5f8c**\u3002",
     "    g_api->sys_mkdir(dst);\n    /* **\u53ce\u96c6\u304c\u5148\u3001mkdir \u306f\u5f8c**\u3002"),
    # 変異 2 = mkdir の戻り値を見ない版 (作れていないのに中へ進む)。
    ("mkdir_ret_ignored", MKDIR_BLOCK, "    g_api->sys_mkdir(dst);\n"),
    # 変異 3 = EXIST を型を見ずに通す版 (同名のファイルへ展開する)。
    ("exist_type_not_checked", MKDIR_BLOCK,
     MKDIR_BLOCK.replace(
         "if (rc != 0 && !(rc == OS32_ERR_EXIST && fs_path_kind(dst) == FS_KIND_DIR)) {",
         "if (rc != 0 && rc != OS32_ERR_EXIST) {")),
    # 変異 4 = 列挙の失敗を無視する版 (空の宛先を作って終わる)。
    ("ls_err_ignored", LS_BLOCK,
     "    rc = g_api->sys_ls(src, collect_entries_cb, (void *)0);\n"
     "    (void)rc;\n"),
]


def build_host(tmp, name):
    exe = tmp / name
    subprocess.run(["gcc", *HOST_FLAGS, *HOST_INC,
                    str(ROOT / "tools/tests/fs_kind_callers_host.c"),
                    "-o", str(exe)], cwd=ROOT, check=True)
    return exe


def run_mutations(tmp):
    target = ROOT / "userland/shell/cmd_file.c"
    bad = 0
    for name, old, new in MUTATIONS:
        original = target.read_text(encoding="utf-8")
        if old not in original:
            print("MUTATE %-24s SKIP (\u76ee\u5370\u304c\u898b\u3064\u304b\u3089\u306a\u3044)" % name, flush=True)
            bad += 1
            continue
        try:
            target.write_text(original.replace(old, new, 1), encoding="utf-8")
            try:
                exe = build_host(tmp, "mut-" + name)
            except subprocess.CalledProcessError:
                print("MUTATE %-24s RED (\u30b3\u30f3\u30d1\u30a4\u30eb\u304c\u901a\u3089\u306a\u3044)" % name,
                      flush=True)
                continue
            out = subprocess.run([str(exe)], cwd=ROOT, timeout=120,
                                 capture_output=True)
            if out.returncode == 0:
                print("MUTATE %-24s **GREEN \u306e\u307e\u307e = \u8a66\u9a13\u304c\u898f\u5247\u3092\u898b\u3066\u3044\u306a\u3044**"
                      % name, flush=True)
                bad += 1
            else:
                fails = out.stdout.decode("utf-8", "replace").count("FAIL ")
                print("MUTATE %-24s RED (\u671f\u5f85\u3069\u304a\u308a\u306b\u843d\u3061\u305f: FAIL %d \u4ef6)"
                      % (name, fails), flush=True)
        finally:
            target.write_text(original, encoding="utf-8")
    return bad


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

        if "--mutate" in sys.argv:
            rc += run_mutations(tmp)

        sys.exit(1 if rc else 0)
