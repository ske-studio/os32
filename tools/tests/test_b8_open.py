"""票 B8: 読み取り失敗を「不存在」として扱う経路を **vfs_open まで** 通して見る。

票:   docs/tasks/shell/TASK_FS_TYPE.md §2 (B8)
記録: tools/tests/b8_tdd.md

tools/tests/b8_open_host.c が

  * 実物の fs/ext2_super.c / ext2_inode.c / ext2_dir.c / ext2_file.c /
    ext2_fmt.c / ext2_vfs.c / vfs.c / vfs_fd.c をそのまま #include し、
  * 贋物は Device API と IDE だけ、
  * RAM 上の 8MB ディスクを実物の ext2_format() で作って、
  * **間接ブロックを使う大きなディレクトリ**の読み取りを一度だけ失敗させる

という形で、次を確かめる。

  - FD が発行されない
  - write_file が呼ばれない (合成ドライバ側で呼び出し回数を数える)
  - 既存の中身が残る (open を通さず ext2_read_file で読み直して確認)

`vfs_path_kind` の戻り値までしか見ない試験では同じ形の欠陥 (B7) を往復 4 で
取り逃しているので、**判定を消費する側**まで動かす。

  python3 -B tools/tests/test_b8_open.py [--target]

--target を付けると、触った fs/*.c が実ビルドと同じ i386-elf クロスコンパイラでも
通ることを確かめる。make・エミュレータ・実配備には一切触れない。
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

# u32 は unsigned long (include/types.h) なので **必ず ILP32 で組む**。
# ext2 のオンディスク配置を実物の構造体で扱うため、ここは譲れない。
HOST_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
              "-fno-stack-protector", "-nostdlib", "-static", "-O1",
              "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-parameter", "-Wno-sign-compare",
              "-Wdeclaration-after-statement", "-D__cdecl="]
INCLUDES = ["-I" + str(ROOT / p)
            for p in ("include", "fs", "lib", "kernel", "drivers",
                      "sdk/include/os32")]
SRC = ROOT / "tools/tests/b8_open_host.c"

# 票 B8 で触ったカーネル側。実ビルドと同じ素性で -Werror を通す ([C1])。
TARGET_SRCS = ["fs/ext2_inode.c", "fs/ext2_dir.c", "fs/ext2_file.c",
               "fs/ext2_vfs.c", "fs/vfs.c", "fs/vfs_fd.c"]
TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                "-fno-pie", "-fno-stack-protector", "-nostdlib",
                "-mno-red-zone", "-fcommon", "-O2",
                "-Wall", "-Wextra", "-Werror",
                "-Wdeclaration-after-statement",
                "-Wno-sign-compare", "-Wno-unused-parameter",
                "-D__KERNEL_BUILD__", "-I.", "-Iinclude", "-Isdk/include",
                "-Isdk/include/os32", "-Ikernel", "-Idrivers", "-Inet",
                "-Ifs", "-Iexec", "-Igfx", "-Ilib", "-Ikapi"]


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-b8-") as tmp:
        tmp = pathlib.Path(tmp)

        exe = tmp / "b8-open"
        subprocess.run(["gcc", *HOST_FLAGS, *INCLUDES, str(SRC), "-o", str(exe)],
                       cwd=ROOT, check=True)
        print("HOST GNU89 -Werror COMPILE PASS (real fs/ext2_*.c + vfs.c + vfs_fd.c)",
              flush=True)

        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=300).returncode
        print("EXIT b8_open_host=%d" % rc, flush=True)

        if "--target" in sys.argv:
            for src in TARGET_SRCS:
                subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                                "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                               cwd=ROOT, check=True)
            print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)

        sys.exit(rc)
