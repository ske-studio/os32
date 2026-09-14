"""H3: mtime の取得・保存と、日時を前置フィルタにした同一判定。

票:   docs/tasks/shell/TASK_H3.md §6 / §8
      docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md §5 / §7.2 / §9 の A03 A04 A16
記録: tools/tests/h3_tdd.md

tools/tests/hsync_h3_host.c が userland/system/hsync.c を 1 行も写さず
そのまま #include し、KernelAPI だけをオンメモリの贋ファイルシステムへ
差し替えて回す (模型ではない)。贋 FS は**ノードごとに mtime を持ち**、
sys_set_mtime の成功 / NOSYS / I/O 失敗を注入できる。sys_read の呼び出し
回数も数えるので「サイズも日時も同じなら 1 バイトも読まない」を直に見られる。

fs/hostdrv_stat_rules.inc の FILETIME 変換 (hdrv_filetime_to_unix /
hdrv_stat_mtime) も同じ翻訳単位で直接叩く (A16)。

  python3 -B tools/tests/test_hsync_h3.py [--target] [--mutate]

--target を付けると、実機と同じ i386-elf クロスコンパイラでも
hsync.c / fs/hostdrvfs.c / fs/ext2_vfs.c / kapi/kapi_sys.c が -Werror で
通ることを確かめる ([C1] C89/GNU89)。

--mutate は**否定側**。この票の中心規則は
「証拠が無いことを同一の根拠にしない」なので、日時が不明 (0) のときに
スキップしてしまう版へ差し替え、試験が確かに落ちることを確かめる。
落ちなければ試験が規則を見ていない。

make・エミュレータ・実配備には一切触れない。
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

HOST_FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
              "-Wdeclaration-after-statement",
              "-D__cdecl=", "-D__OS32_USERLAND__"]
HOST_INC = ["-I" + str(ROOT / p) for p in
            (".", "include", "sdk/include", "sdk/include/os32",
             "userland/lib", "userland/system", "lib")]

CROSS_DIR = pathlib.Path(os.environ.get("CROSS_DIR", "/usr/local/cross"))
if not CROSS_DIR.exists():
    alt = pathlib.Path.home() / "opt/cross"
    if alt.exists():
        CROSS_DIR = alt

TARGET_COMMON = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
                 "-fno-pie", "-fno-stack-protector", "-nostdlib",
                 "-mno-red-zone", "-fcommon", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-Wdeclaration-after-statement"]
TARGET_USER = TARGET_COMMON + [
    "-D__OS32_USERLAND__", "-I.", "-Iinclude", "-Isdk/include",
    "-Isdk/include/os32", "-Iuserland/lib",
    "-I" + str(CROSS_DIR / "i386-elf/include")]
TARGET_KERNEL = TARGET_COMMON + [
    "-D__KERNEL_BUILD__", "-I.", "-Iinclude", "-Isdk/include",
    "-Isdk/include/os32", "-Ikernel", "-Idrivers", "-Inet", "-Ifs",
    "-Iexec", "-Igfx", "-Ilib", "-Ikapi"]


def check_kapi_slot():
    """[ABI1]/[ABI2]: sys_set_mtime が kapi.json の**末尾**にあり、版が揃う。

    末尾追記でなくなった瞬間に配備済みバイナリの ABI が壊れる。生成物を
    手で触っていないことも、生成ヘッダの KAPI_FUNC_COUNT で見る。
    """
    import json

    kapi = json.loads((ROOT / "sdk/kapi.json").read_text(encoding="utf-8"))
    api = kapi["api"]
    if api[-1]["name"] != "sys_set_mtime":
        raise SystemExit("sys_set_mtime が kapi.json の末尾に無い ([ABI2])")

    shared = (ROOT / "sdk/include/os32/os32_kapi_shared.h").read_text(
        encoding="utf-8")
    m = re.search(r"#define\s+KAPI_VERSION\s+(\d+)", shared)
    if not m or int(m.group(1)) != int(kapi["version"]):
        raise SystemExit("KAPI_VERSION が kapi.json の version と違う")

    gen = (ROOT / "sdk/include/os32/os32_kapi_generated.h").read_text(
        encoding="utf-8")
    m = re.search(r"#define\s+KAPI_FUNC_COUNT\s+(\d+)", gen)
    if not m or int(m.group(1)) != len(api):
        raise SystemExit("生成ヘッダの KAPI_FUNC_COUNT が kapi.json と違う "
                         "(再生成していない / 手編集した)")

    # hsync は新 API を呼ぶので app.conf の要求版を上げてあること
    conf = (ROOT / "build/app.conf").read_text(encoding="utf-8")
    m = re.search(r"^userland/system/hsync\s+(\d+)", conf, re.M)
    if not m:
        raise SystemExit("build/app.conf に userland/system/hsync が無い")
    if int(m.group(1)) < int(kapi["version"]):
        raise SystemExit("build/app.conf の hsync の要求 API 版が v%s 未満"
                         % kapi["version"])

    print("KAPI SLOT PASS (sys_set_mtime = slot %d, v%s, "
          "app.conf hsync>=%s)" % (len(api) - 1, kapi["version"], m.group(1)),
          flush=True)


def build_host(tmp, src, name, extra=()):
    exe = tmp / name
    cmd = ["gcc", *HOST_FLAGS, *HOST_INC, *extra,
           str(ROOT / src), "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    print("HOST GNU89 -Werror COMPILE PASS (%s)" % src, flush=True)
    return exe


def build_vfs(tmp, name, extra=()):
    exe = tmp / name
    inc = ["-I" + str(ROOT / p)
           for p in ("include", "fs", "lib", "kernel", "drivers",
                     "sdk/include/os32")]
    cmd = ["gcc", "-std=gnu89", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-parameter", "-Wno-sign-compare",
           "-Wdeclaration-after-statement", "-D__cdecl=", *inc, *extra,
           str(ROOT / "tools/tests/vfs_set_mtime_host.c"), "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    print("HOST GNU89 -Werror COMPILE PASS (real fs/vfs.c)", flush=True)
    return exe


def target_compile(tmp):
    jobs = [
        (TARGET_USER, "userland/system/hsync.c", []),
        (TARGET_KERNEL, "fs/hostdrvfs.c", ["-Wno-address-of-packed-member"]),
        (TARGET_KERNEL, "fs/ext2_vfs.c", []),
        (TARGET_KERNEL, "fs/fatfs_vfs.c", []),
        (TARGET_KERNEL, "fs/iso9660.c", []),
        (TARGET_KERNEL, "fs/vfs.c", ["-Wno-sign-compare",
                                     "-Wno-unused-parameter"]),
        (TARGET_KERNEL, "kapi/kapi_sys.c", []),
    ]
    for flags, src, extra in jobs:
        out = tmp / (pathlib.Path(src).stem + ".o")
        subprocess.run(["i386-elf-gcc", *flags, *extra, "-c", src,
                        "-o", str(out)], cwd=ROOT, check=True)
        print("TARGET i386-elf -Werror COMPILE PASS (%s)" % src, flush=True)


# 変異: 「日時が不明 (0) でも、両側が同じなら読まずに省略する」版。
# これは票 §8 が禁じた実装そのもの。試験が落ちなければ規則を見ていない。
MUTATIONS = [
    ("mtime_unknown_is_same",
     "int mtime_known = (ss.st_mtime != 0 && ds.st_mtime != 0);",
     "int mtime_known = 1;"),
    # 変異 2: 内容が同じでも「日時が違う」だけで本体をコピーしてしまう版。
    # コピーするかどうかを決めるのは**内容比較**であって日時ではない
    # (A03 が copied=0 / metadata_updated=1 を期待するので落ちる)。
    ("copy_on_mtime_diff",
     "        cmp = compare_files(src_path, dst_path, size);",
     "        cmp = compare_files(src_path, dst_path, size);\n"
     "        if (cmp == 0 && !mtime_same) cmp = 1;"),
    # 変異 6: --verify を無視して日時でスキップする版。
    ("verify_ignored",
     "        if (!g_verify && mtime_same) {",
     "        if (mtime_same) {"),
    # 変異 7: 設定失敗を握り潰して成功と言う版 (metadata_failed の否定側)。
    ("swallow_set_failure",
     "    fail_file(dst_path, HR_META_FAILED, rc);\n    return -1;",
     "    return 0;"),
    # 変異 3: 時刻の保存を省く版。A03 / A04 が落ちなければ保存を見ていない。
    ("no_set_mtime",
     "    rc = api->sys_set_mtime(dst_path, src_mtime);",
     "    rc = 0;"),
    # 変異 4: FILETIME の起点差を足し忘れる (1601 起点のまま返す) 版。
    ("filetime_no_epoch_shift",
     "    secs = (ft - HDRV_FT_EPOCH_DIFF_100NS) / HDRV_FT_PER_SEC;",
     "    secs = ft / HDRV_FT_PER_SEC;"),
    # 変異 5: os_time_t の範囲外を切り詰めてしまう (wrap する) 版。
    ("filetime_wrap",
     "    if (secs > HDRV_STAT_MAX_TIME) return 0;         /* (c) 範囲外 */",
     "    /* mutated: 範囲検査を外す */"),
]


def run_mutations(tmp):
    """変異させたソースで試験が **落ちる** ことを確かめる。"""
    targets = {
        "mtime_unknown_is_same": "userland/system/hsync.c",
        "copy_on_mtime_diff": "userland/system/hsync.c",
        "no_set_mtime": "userland/system/hsync.c",
        "filetime_no_epoch_shift": "fs/hostdrv_stat_rules.inc",
        "filetime_wrap": "fs/hostdrv_stat_rules.inc",
        "verify_ignored": "userland/system/hsync.c",
        "swallow_set_failure": "userland/system/hsync.c",
    }
    bad = 0
    for name, old, new in MUTATIONS:
        path = ROOT / targets[name]
        original = path.read_text(encoding="utf-8")
        if old not in original:
            print("MUTATE %-26s SKIP (目印が見つからない)" % name, flush=True)
            bad += 1
            continue
        try:
            path.write_text(original.replace(old, new, 1), encoding="utf-8")
            try:
                exe = build_host(tmp, "tools/tests/hsync_h3_host.c",
                                 "mut-" + name)
            except subprocess.CalledProcessError:
                print("MUTATE %-26s RED (コンパイルが通らない)" % name,
                      flush=True)
                continue
            rc = subprocess.run([str(exe)], cwd=ROOT, timeout=120,
                                capture_output=True).returncode
            if rc == 0:
                print("MUTATE %-26s **GREEN のまま = 試験が規則を見ていない**"
                      % name, flush=True)
                bad += 1
            else:
                print("MUTATE %-26s RED (期待どおり落ちた)" % name, flush=True)
        finally:
            path.write_text(original, encoding="utf-8")
    return bad


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="os32-hsync-h3-") as tmp:
        tmp = pathlib.Path(tmp)
        failed = 0

        check_kapi_slot()

        exe = build_host(tmp, "tools/tests/hsync_h3_host.c", "hsync-h3")
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=120).returncode
        print("EXIT hsync_h3_host=%d" % rc, flush=True)
        failed += rc != 0

        exe = build_vfs(tmp, "vfs-set-mtime")
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60).returncode
        print("EXIT vfs_set_mtime_host=%d" % rc, flush=True)
        failed += rc != 0

        if "--target" in sys.argv:
            target_compile(tmp)

        if "--mutate" in sys.argv:
            failed += run_mutations(tmp)

        sys.exit(1 if failed else 0)
