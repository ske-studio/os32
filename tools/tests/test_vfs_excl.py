"""X1 / X2: 排他的作成 O_EXCL (票 H2 §2-1、KAPI v53)。

票:   docs/tasks/shell/TASK_H2.md §2-1 / §4-1 の X1 X2
記録: tools/tests/vfs_excl_tdd.md

tools/tests/vfs_excl_host.c が実物の fs/vfs.c と fs/vfs_fd.c をそのまま
#include し、境界 (kstring / kmalloc / コンソール) だけを同義の C で置く。
FS ドライバは合成した VfsOps で、create_excl を**持つ / 持たない**両方と、
create_excl / get_file_size / write_file の戻り値と呼び出し回数を指定できる。

押さえる規則:
  X1  O_CREAT|O_EXCL: 既存ファイル / 既存ディレクトリ -> どちらも EXIST、
      判定不能 (I/O) -> **その負値**。いずれも作らない・切り詰めない。
  X2  O_EXCL 単独 -> INVAL、create_excl を持たない FS -> NOSYS。
      **非対応の判定は種別検査より先**なので get_file_size / list_dir を
      1 度も呼ばない (票 §6 往復 2 の指摘)。

  python3 -B tools/tests/test_vfs_excl.py [--target] [--mutate]

--target は実機と同じ i386-elf クロスコンパイラでも fs/vfs.c / fs/vfs_fd.c /
fs/ext2_vfs.c が -Werror で通ることの確認 ([C1] C89/GNU89)。

--mutate は**否定側**。この票の中心規則は「読めなかったを無いと読まない」と
「非対応を先に断る」なので、それを崩した版で試験が確かに落ちることを見る。

make・エミュレータ・実配備には一切触れない。
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

HOST_FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-parameter", "-Wno-sign-compare",
              "-Wdeclaration-after-statement", "-D__cdecl="]
HOST_INC = ["-I" + str(ROOT / p)
            for p in ("include", "fs", "lib", "kernel", "drivers",
                      "sdk/include/os32")]

TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                "-fno-pie", "-fno-stack-protector", "-nostdlib",
                "-mno-red-zone", "-fcommon", "-O2",
                "-Wall", "-Wextra", "-Werror",
                "-Wdeclaration-after-statement",
                "-Wno-sign-compare", "-Wno-unused-parameter",
                "-D__KERNEL_BUILD__", "-I.", "-Iinclude",
                "-Iarch/x86", "-Iplatform/pc98", "-Isdk/include",
                "-Isdk/include/os32", "-Ikernel", "-Idrivers", "-Inet",
                "-Ifs", "-Iexec", "-Igfx", "-Ilib", "-Ikapi"]


def check_flag_constant():
    """[ABI1]/[ABI3]: KAPI_O_EXCL の値と KAPI_VERSION の同期。

    O_EXCL は kapi.json の関数表ではなく手書きの定数なので、版数の同期は
    check_kapi_version.py が見るが、**フラグの値そのもの**はここで見る。
    既存のフラグと重なると、古いバイナリの O_TRUNC が O_EXCL に化ける。
    """
    import json

    shared = (ROOT / "sdk/include/os32/os32_kapi_shared.h").read_text(
        encoding="utf-8")
    flags = dict((m.group(1), int(m.group(2), 0)) for m in re.finditer(
        r"#define\s+KAPI_(O_[A-Z]+)\s+(0x[0-9A-Fa-f]+|\d+)", shared))
    if flags.get("O_EXCL") != 0x0400:
        raise SystemExit("KAPI_O_EXCL が 0x0400 ではない: %r" % flags.get("O_EXCL"))
    for name, val in flags.items():
        if name == "O_EXCL":
            continue
        if val & 0x0400:
            raise SystemExit("KAPI_O_EXCL のビットが KAPI_%s と重なる" % name)

    kapi = json.loads((ROOT / "sdk/kapi.json").read_text(encoding="utf-8"))
    m = re.search(r"#define\s+KAPI_VERSION\s+(\d+)", shared)
    if not m or int(m.group(1)) != int(kapi["version"]):
        raise SystemExit("KAPI_VERSION が kapi.json の version と違う")
    if int(kapi["version"]) < 53:
        raise SystemExit("O_EXCL を足したら KAPI は v53 以上 ([ABI3])")

    # スロットは 1 本も増えていない (O_EXCL はフラグだけ、[ABI2])
    gen = (ROOT / "sdk/include/os32/os32_kapi_generated.h").read_text(
        encoding="utf-8")
    m = re.search(r"#define\s+KAPI_FUNC_COUNT\s+(\d+)", gen)
    if not m or int(m.group(1)) != len(kapi["api"]):
        raise SystemExit("生成ヘッダの KAPI_FUNC_COUNT が kapi.json と違う")
    if kapi["api"][-1]["name"] != "sys_set_mtime":
        raise SystemExit("H2 でスロットを足してはいけない "
                         "(末尾は v52 の sys_set_mtime のまま、[ABI2])")

    print("KAPI FLAG PASS (KAPI_O_EXCL=0x400, v%s, スロット %d 本のまま)"
          % (kapi["version"], len(kapi["api"])), flush=True)


def build_host(tmp, name, src="tools/tests/vfs_excl_host.c"):
    exe = tmp / name
    subprocess.run(["gcc", *HOST_FLAGS, *HOST_INC, str(ROOT / src),
                    "-o", str(exe)], cwd=ROOT, check=True)
    print("HOST GNU89 -Werror COMPILE PASS (real fs/vfs.c + fs/vfs_fd.c)",
          flush=True)
    return exe


MUTATIONS = [
    # 変異 1: 非対応 FS の判定を種別検査より後ろへ動かした版 (往復 2 の指摘)。
    ("nosys_after_kind",
     "    if (mode & O_EXCL) {\n"
     "        if (!(mode & O_CREAT)) return VFS_ERR_INVAL;\n"
     "        if (!ops->create_excl) return VFS_ERR_NOSYS;\n",
     "    if (mode & O_EXCL) {\n"
     "        if (!(mode & O_CREAT)) return VFS_ERR_INVAL;\n"
     "        if (!ops->create_excl) {\n"
     "            int k = vfs_path_kind(resolved);\n"
     "            if (k == VFS_KIND_DIR) return VFS_ERR_ISDIR;\n"
     "            if (k < 0 && k != VFS_ERR_NOTFOUND) return k;\n"
     "            return VFS_ERR_NOSYS;\n"
     "        }\n"),
    # 変異 2: 判定不能を「無い」に読み替えて作ってしまう版 (票 B8 の否定側)。
    ("excl_error_is_absent",
     "        rc = ops->create_excl(fs_ctx, rel_path);\n"
     "        if (rc != VFS_OK) return rc;",
     "        rc = ops->create_excl(fs_ctx, rel_path);\n"
     "        if (rc != VFS_OK && rc != VFS_ERR_EXIST) rc = VFS_OK;\n"
     "        if (rc != VFS_OK) return rc;"),
    # 変異 3: O_EXCL 単独を素通しする版。
    ("excl_alone_ok",
     "        if (!(mode & O_CREAT)) return VFS_ERR_INVAL;",
     "        if (!(mode & O_CREAT)) mode |= O_CREAT;"),
]


def run_mutations(tmp):
    target = ROOT / "fs/vfs_fd.c"
    bad = 0
    for name, old, new in MUTATIONS:
        original = target.read_text(encoding="utf-8")
        if old not in original:
            print("MUTATE %-22s SKIP (目印が見つからない)" % name, flush=True)
            bad += 1
            continue
        try:
            target.write_text(original.replace(old, new, 1), encoding="utf-8")
            try:
                exe = build_host(tmp, "mut-" + name)
            except subprocess.CalledProcessError:
                print("MUTATE %-22s RED (コンパイルが通らない)" % name, flush=True)
                continue
            rc = subprocess.run([str(exe)], cwd=ROOT, timeout=120,
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
    with tempfile.TemporaryDirectory(prefix="os32-vfs-excl-") as tmp:
        tmp = pathlib.Path(tmp)
        failed = 0

        check_flag_constant()

        exe = build_host(tmp, "vfs-excl")
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60).returncode
        print("EXIT vfs_excl_host=%d" % rc, flush=True)
        failed += rc != 0

        if "--target" in sys.argv:
            for src in ("fs/vfs.c", "fs/vfs_fd.c", "fs/ext2_vfs.c",
                        "fs/ext2_dir.c", "fs/ext2_file.c", "fs/ext2_super.c"):
                subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                                "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                               cwd=ROOT, check=True)
                print("TARGET i386-elf -Werror COMPILE PASS (%s)" % src,
                      flush=True)

        if "--mutate" in sys.argv:
            failed += run_mutations(tmp)

        sys.exit(1 if failed else 0)
