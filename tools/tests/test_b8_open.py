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

票 B8 往復 5: **本物の `e2fsck -fn` を正解に加える** (抜き取り)。
試験バイナリが `@@E2FSCK <像> <media_ok> <ラベル>` を出して止まるので、ここで
e2fsck を当てて出力を「許容 (漏れ側)」「不整合」に分類し (tools/tests/b8_e2fsck.py)、
**自前の媒体検査 (media_ok) と食い違ったら失敗**にする。e2fsck が無い環境では
`E2FSCK SKIP` と明示して媒体検査だけで通す ([V4])。
"""
import collections
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import b8_e2fsck  # noqa: E402

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
               "fs/ext2_vfs.c", "fs/ext2_super.c", "fs/vfs.c", "fs/vfs_fd.c"]
TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                "-fno-pie", "-fno-stack-protector", "-nostdlib",
                "-mno-red-zone", "-fcommon", "-O2",
                "-Wall", "-Wextra", "-Werror",
                "-Wdeclaration-after-statement",
                "-Wno-sign-compare", "-Wno-unused-parameter",
                "-D__KERNEL_BUILD__", "-I.", "-Iinclude", "-Isdk/include",
                "-Isdk/include/os32", "-Ikernel", "-Idrivers", "-Inet",
                "-Ifs", "-Iexec", "-Igfx", "-Ilib", "-Ikapi"]


def find_e2fsck():
    for cand in ("/usr/sbin/e2fsck", "/sbin/e2fsck"):
        if os.access(cand, os.X_OK):
            return cand
    return shutil.which("e2fsck")


def run_with_e2fsck(exe, imgdir, e2fsck):
    """試験バイナリを動かし、@@E2FSCK の行ごとに e2fsck を当てる。"""
    stats = {"samples": 0, "clean": 0, "allowed": 0, "bad": 0, "mismatch": 0}
    cats_allowed = collections.OrderedDict()
    cats_bad = collections.OrderedDict()
    mismatches = []
    proc = subprocess.Popen([str(exe), str(imgdir)], cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    for raw in proc.stdout:
        line = raw.decode("utf-8", "replace")
        if not line.startswith("@@E2FSCK "):
            sys.stdout.write(line)
            continue
        _, path, media_ok, label = line.rstrip("\n").split(" ", 3)
        res = subprocess.run([e2fsck, "-fn", path], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=120)
        out = res.stdout.decode("utf-8", "replace")
        stats["samples"] += 1
        allowed, bad = b8_e2fsck.classify(out, path)
        # -n では壊れたディレクトリブロックを直せないので e2fsck は「Salvage? no」の後で
        # 打ち切る (rc 12)。不整合の診断が出ていればそれを判定に使い、診断の無い
        # rc >= 8 だけを道具の失敗とする。
        if res.returncode >= 8 and not (bad and "aborted" in out):
            mismatches.append((label, "e2fsck operational error rc=%d" % res.returncode, out))
            stats["mismatch"] += 1
        else:
            for cat, l in allowed:
                cats_allowed.setdefault(cat, [0, l, label])[0] += 1
            for cat, l in bad:
                cats_bad.setdefault(cat, [0, l, label])[0] += 1
            if bad:
                stats["bad"] += 1
            elif allowed:
                stats["allowed"] += 1
            else:
                stats["clean"] += 1
            e2f_ok = not bad
            if e2f_ok != (media_ok == "1"):
                stats["mismatch"] += 1
                mismatches.append((label, "media_ok=%s e2fsck=%s" % (
                    media_ok, "ok" if e2f_ok else "inconsistent"), out))
        proc.stdin.write(b"ok\n")
        proc.stdin.flush()
    proc.stdin.close()
    rc = proc.wait()

    print("E2FSCK samples=%d clean=%d allowed-only=%d inconsistent=%d mismatch=%d"
          % (stats["samples"], stats["clean"], stats["allowed"], stats["bad"],
             stats["mismatch"]), flush=True)
    print("E2FSCK 許容した分類:")
    for cat, (n, l, label) in cats_allowed.items():
        print("  %5d  %s\n         例: %s\n         (%s)" % (n, cat, l, label))
    print("E2FSCK 不整合の分類:")
    for cat, (n, l, label) in cats_bad.items():
        print("  %5d  %s\n         例: %s\n         (%s)" % (n, cat, l, label))
    for label, why, out in mismatches[:10]:
        print("E2FSCK MISMATCH: %s: %s\n%s" % (label, why, out))
    return rc, stats


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-b8-") as tmp:
        tmp = pathlib.Path(tmp)

        exe = tmp / "b8-open"
        subprocess.run(["gcc", *HOST_FLAGS, *INCLUDES, str(SRC), "-o", str(exe)],
                       cwd=ROOT, check=True)
        print("HOST GNU89 -Werror COMPILE PASS (real fs/ext2_*.c + vfs.c + vfs_fd.c)",
              flush=True)

        e2fsck = None if "--no-e2fsck" in sys.argv else find_e2fsck()
        mismatch = 0
        if e2fsck:
            rc, stats = run_with_e2fsck(exe, tmp, e2fsck)
            mismatch = stats["mismatch"]
            if stats["samples"] == 0:
                print("E2FSCK FAIL: no samples were produced", flush=True)
                mismatch = 1
        else:
            print("E2FSCK SKIP: e2fsck not found (or --no-e2fsck) — "
                  "media_check only, e2fsck cross-check NOT performed", flush=True)
            rc = subprocess.run([str(exe)], cwd=ROOT, timeout=900).returncode
        print("EXIT b8_open_host=%d" % rc, flush=True)

        if "--target" in sys.argv:
            for src in TARGET_SRCS:
                subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, "-c", src,
                                "-o", str(tmp / (pathlib.Path(src).stem + ".o"))],
                               cwd=ROOT, check=True)
            print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)

        sys.exit(1 if (rc != 0 or mismatch) else 0)
