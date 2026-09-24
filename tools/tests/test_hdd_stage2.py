"""票 TASK_HDD_INSTALL 段 2 (インストーラ cdinst / install) のホスト試験。

記録: tools/tests/hdd_stage2_tdd.md
票  : docs/tasks/realhw/TASK_HDD_INSTALL.md 段 2 (9〜11) / §1-v3 (N4・N6・N8・R3-1)

実物を 1 行も写さずに回す:
  tools/tests/hdd_stage2_host.c   userland/system/inst_disk.c (+ pc98pt.c・
                                  hdprep_plan.c・ext2_layout.c) の判定
  tools/tests/cdinst_host.c       userland/system/cdinst.c を main から
                                  (+ inst_hdd.c・pkg.c、PKG は試験が組む)
  tools/tests/install_fresh_host.c userland/system/install.c を main から
                                  (段 2 の 6 段、test_install_fresh.py と共有)
  tools/tests/ext2_mini_host.c    boot/ext2_mini.c (mke2fs + debugfs の像)
見るもの:
  - 8/17 と 16/63 で区画表 (標準配置) と IPL の [8]/[9] の幾何が一致し、
    開始は 1632 / 2016、長さは 256MiB まで
  - 空・再作成 (標準 / 旧配置 8/17)・未知・2 項目・開始違い・55AA・壊れ・旧配置 16/63
  - 事前検査 (IPL / ローダ / vmkernel の大きさ・必須の中身・容量・前置の溢れ・
    幾何・マウント) の失敗で 1 セクタも書かない
  - format / 区画表の読み戻し / マウント / ローダ / IPL / 展開 / sync の失敗で
    INCOMPLETE と出し「完了」と言わない
  - 容量の見積もりの空きが実物の ext2_format_at の像 (dumpe2fs) と一致する
  - ext2_mini は 508KiB を超えるファイルを切り詰めずにエラーにする

  python3 -B tools/tests/test_hdd_stage2.py            # 全部
  python3 -B tools/tests/test_hdd_stage2.py --target   # + i386-elf -Werror で新しいソース
  python3 -B tools/tests/test_hdd_stage2.py --mutate   # 否定側 (変異が RED になるか)
"""
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PURE = ROOT / "tools/tests/hdd_stage2_host.c"
CDI = ROOT / "tools/tests/cdinst_host.c"
INS = ROOT / "tools/tests/install_fresh_host.c"
MINI = ROOT / "tools/tests/ext2_mini_host.c"

PURE_CASES = ["classify817", "classify1663", "paths", "bootfiles", "blocks", "space", "iplpt"]
CDI_CASES = ["ok817", "ok1663", "modes", "preflight", "incomplete", "paths"]
INS_CASES = ["nokernel", "precheck", "boot_fail", "sync_fail", "geom817", "geom1663",
             "modes", "preflight", "incomplete", "rerun"]
MAX_IMAGE = 508 * 1024
# (像の中の名前, 大きさ, 期待: 大きさ か -2 = EXT2M_ERR_TOO_BIG)
MINI_FILES = [("small", 1000, 1000), ("exact", MAX_IMAGE, MAX_IMAGE),
              ("over1", MAX_IMAGE + 1, -2), ("over", 600 * 1024, -2)]
# 空きを実物の format_at と突き合わせる大きさ (test_hdd_stage1.py の像と同じ)
ROOM_SIZES = [16652, 20160, 36000, 62496]

SHARED = ["userland/system/inst_disk.c", "drivers/pc98pt.c",
          "userland/shell/hdprep_plan.c", "fs/ext2_layout.c"]
INS_SHARED = ["userland/system/inst_hdd.c"] + SHARED

HOST = ["gcc", "-std=gnu89", "-Wall", "-Wextra", "-Werror",
        "-Wdeclaration-after-statement", "-D__cdecl="]
ILP32 = ["gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
         "-fno-stack-protector", "-fno-builtin", "-nostdlib", "-static", "-O1",
         "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
         "-Wno-unused-function", "-Wdeclaration-after-statement", "-D__cdecl="]

MIRROR = ["userland/system/inst_disk.c", "userland/system/inst_disk.h",
          "userland/system/inst_hdd.c", "userland/system/inst_hdd.h",
          "userland/system/cdinst.c", "userland/system/install.c",
          "userland/system/install_recover.inc",
          "userland/shell/hdprep_plan.c", "userland/shell/hdprep_plan.h",
          "drivers/pc98pt.c", "drivers/pc98pt.h", "fs/ext2_layout.c", "fs/ext2_layout.h",
          "boot/ext2_mini.c", "boot/boot_defs.h",
          "userland/lib/rt/pkg.c", "userland/lib/rt/pkg.h", "userland/lib/rt/dbgserial.h"]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, **kw)


def _harness(src, tmp, root):
    """root が ROOT でなければ、ハーネスの "../../x" を写しの root/x に向ける。"""
    if root == ROOT:
        return src
    text = src.read_text(encoding="utf-8").replace('"../../', '"' + str(root) + "/")
    out = pathlib.Path(tmp) / src.name
    out.write_text(text, encoding="utf-8")
    return out


def _cc(cmd, quiet):
    r = run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        if not quiet:
            sys.stderr.write(r.stderr)
        return False
    return True


def build_all(tmp, root=ROOT, quiet=False):
    """4 本を組む。どれかが組めなければ None。"""
    tmp = pathlib.Path(tmp)
    exes = {k: tmp / k for k in ("pure", "cdi", "ins", "mini")}
    inc = ["-I" + str(root), "-I" + str(root / "include"), "-I" + str(ROOT / "include")]
    ok = _cc([*HOST, *inc, str(_harness(PURE, tmp, root)),
              *[str(root / f) for f in SHARED], "-o", str(exes["pure"])], quiet)
    cinc = ["-I" + str(root), "-I" + str(root / "userland/lib"),
            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "sdk/include/os32"),
            "-I" + str(ROOT / "tools/tests/mtar_freestanding")]
    ok = ok and _cc([*ILP32, *cinc, str(_harness(CDI, tmp, root)),
                     "-o", str(exes["cdi"])], quiet)
    iinc = ["-I" + str(root / "userland/system"), "-I" + str(root)] + \
           ["-I" + str(ROOT / p) for p in ("include", "sdk/include", "sdk/include/os32",
                                          "userland/lib")]
    ok = ok and _cc(["gcc", "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                     "-Wdeclaration-after-statement", "-Wno-unused-function",
                     "-Wno-pointer-to-int-cast", "-D__cdecl=", "-D__OS32_USERLAND__", "-O0",
                     *iinc, str(_harness(INS, tmp, root)),
                     *[str(root / f) for f in INS_SHARED], "-o", str(exes["ins"])], quiet)
    ok = ok and _cc([*ILP32, "-I" + str(root / "boot"), str(_harness(MINI, tmp, root)),
                     "-o", str(exes["mini"])], quiet)
    return exes if ok else None


def make_mini_image(tmp):
    """mke2fs (1KiB ブロック、inode 128 B、機能なし) + debugfs で /boot に並べる。"""
    tmp = pathlib.Path(tmp)
    img = tmp / "mini.img"
    run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024", "-I", "128", "-O", "none",
         "-N", "64", str(img), "2048"], check=True, capture_output=True)
    cmds = ["mkdir boot"]
    for name, size, _ in MINI_FILES:
        f = tmp / ("f_" + name)
        f.write_bytes(bytes(((i * 7 + 3) & 0xFF) for i in range(size)))
        cmds.append(f"write {f} boot/{name}")
    script = tmp / "debugfs.cmd"
    script.write_text("\n".join(cmds) + "\n")
    run(["debugfs", "-w", "-f", str(script), str(img)], check=True, capture_output=True)
    return img


ROOM_EXPECT = {}   # {大きさ: (空きブロック, 空き inode)} — room_cross_check が実物の像から埋める


def run_cases(exes, img, quiet=False):
    failed = 0
    for size, want in ROOM_EXPECT.items():
        got = run([str(exes["pure"]), "room", str(size)], capture_output=True,
                  text=True).stdout.split()
        failed += len(got) != 2 or (int(got[0]), int(got[1])) != want
    for key, cases in (("pure", PURE_CASES), ("cdi", CDI_CASES), ("ins", INS_CASES)):
        for c in cases:
            r = run([str(exes[key]), c], capture_output=True, text=True)
            if not quiet:
                print(f"EXIT {key}:{c}={r.returncode}", flush=True)
                if r.returncode != 0:
                    sys.stdout.write(r.stdout[-3000:] + r.stderr[-3000:])
            failed += r.returncode != 0
    for name, _, want in MINI_FILES:
        r = run([str(exes["mini"]), str(img), f"/boot/{name}", str(want)],
                capture_output=True, text=True)
        if not quiet:
            print(f"EXIT mini:{name}={r.returncode} {r.stderr.strip()}", flush=True)
        failed += r.returncode != 0
    return failed


def room_cross_check(pure_exe, tmp):
    """容量の見積もりの空き (ブロック・inode) = 実物の ext2_format_at の像の dumpe2fs。"""
    spec = importlib.util.spec_from_file_location("hdd1", ROOT / "tools/tests/test_hdd_stage1.py")
    hdd1 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hdd1)
    pexe = hdd1.build_part(tmp)
    if pexe is None:
        print("  room: ext2_part_host のビルド失敗")
        return 1
    bad = 0
    for size in ROOM_SIZES:
        img = pathlib.Path(tmp) / f"room_{size}.img"
        with open(img, "wb") as f:
            r = subprocess.run([str(pexe), "dump", "2016", str(size)], stdout=f)
        if r.returncode != 0:
            bad += 1
            continue
        d = run(["dumpe2fs", "-h", str(img)], capture_output=True, text=True).stdout
        fb = int(re.search(r"^Free blocks:\s+(\d+)", d, re.M).group(1))
        fi = int(re.search(r"^Free inodes:\s+(\d+)", d, re.M).group(1))
        got = run([str(pure_exe), "room", str(size)], capture_output=True,
                  text=True).stdout.split()
        ok = [int(got[0]), int(got[1])] == [fb, fi]
        ROOM_EXPECT[size] = (fb, fi)
        print(f"  room {size}: estimate {got[0]}/{got[1]} image {fb}/{fi} "
              f"{'ok' if ok else 'MISMATCH'}", flush=True)
        bad += not ok
    return bad


def consts_cross_check(pure_exe):
    """試験の外の正典と定数が一致する。"""
    got = run([str(pure_exe), "consts"], capture_output=True, text=True).stdout.split()
    kmax, lmax, groups, off_h, off_s = (int(x) for x in got)
    bad = 0
    bd = (ROOT / "boot/boot_defs.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+MAX_IMAGE_SIZE\s+\((\d+)\s*\*\s*(\d+)\)", bd)
    bad += not (m and int(m.group(1)) * int(m.group(2)) == kmax)
    ctx = (ROOT / "fs/ext2_ctx.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+EXT2_MAX_GROUPS\s+(\d+)", ctx)
    bad += not (m and int(m.group(1)) == groups)
    nhd = (ROOT / "tools/nhd_deploy.py").read_text(encoding="utf-8")
    m = re.search(r"^LOADER_MAX_SECTORS\s*=\s*(\d+)", nhd, re.M)
    bad += not (m and int(m.group(1)) * 512 == lmax)
    # IPL がローダを読むセクタ数 (boot_hdd.asm の「LBA 2 から」の mov cx) × 512
    asm = (ROOT / "boot/boot_hdd.asm").read_text(encoding="utf-8")
    m = re.search(r"mov\s+ax,\s*2\s*;;[^\n]*\n\s*mov\s+cx,\s*(\d+)", asm)
    bad += not (m and int(m.group(1)) * 512 == lmax)
    ipl = ROOT / "boot/boot_hdd.bin"
    if ipl.is_file():
        b = ipl.read_bytes()
        # boot_hdd.asm の geo_heads / geo_spt / geo_da の既定 8 / 17 / 80h がその位置にある
        bad += not (b[off_h] == 8 and b[off_s] == 17 and b[off_s + 1] == 0x80)
    else:
        print("  consts: boot/boot_hdd.bin が無い (make all の前) — IPL の位置は見ない")
    print(f"  consts: kernel {kmax} loader {lmax} groups {groups} ipl [{off_h}]/[{off_s}] "
          f"{'ok' if bad == 0 else 'MISMATCH'}", flush=True)
    return bad


def real_pkg_paths(pure_exe):
    """packages/*.PKG (make all の成果物) の全項目のパスが inst_check_path を通る。"""
    import struct
    paths = []
    for f in sorted((ROOT / "packages").glob("*.PKG")):
        b = f.read_bytes()
        off = 32
        while b[off]:
            n = b[off]
            paths.append(b[off + 1:off + 1 + n].decode("utf-8"))
            off += 1 + n + 5
    if not paths:
        print("  real PKG paths: packages/*.PKG が無い (make all の前) — 見ない")
        return 0
    r = run([str(pure_exe), "pathok", *paths], capture_output=True, text=True)
    print(f"  real PKG paths: {r.stdout.strip()}", flush=True)
    return r.returncode != 0


# CPL=3 が読んでよい文字列の返り先 (カーネル帯の static を返さない実体)。
# 2026-09-24、NP21/W で cdinst が vfs_devname の返り値 (カーネルのマウント表) を
# 読んで fault kill された — ホスト試験の贋物は利用者の文字列を返すので見えない。
USER_SAFE_STR_TARGETS = {"vfs_cwd_user", "vfs_devname_user", "kapi_db_last_error",
                         "kapi_db_column_text"}
INSTALLER_SRCS = ["userland/system/cdinst.c", "userland/system/install.c",
                  "userland/system/inst_hdd.c", "userland/system/inst_disk.c",
                  "userland/lib/rt/pkg.c", "userland/system/install_recover.inc"]


def str_return_guard():
    """インストーラ (CPL=3) が呼ぶ `const char *` を返す KAPI は、どれも CPL=3 に
    写しを返す実体につながっている。vfs_devname は vfs_devname_user を通る。"""
    import json

    def walk(o):
        if isinstance(o, dict):
            if "name" in o and "args" in o:
                yield o
            for v in o.values():
                yield from walk(v)
        elif isinstance(o, list):
            for v in o:
                yield from walk(v)

    k = json.load(open(ROOT / "sdk/kapi.json", encoding="utf-8"))
    strfn = {f["name"]: f.get("target", f["name"]) for f in walk(k)
             if "char" in f["ret"] and "*" in f["ret"]}
    bad = []
    for rel in INSTALLER_SRCS:
        src = (ROOT / rel).read_text(encoding="utf-8")
        for name in re.findall(r"(?:api|g_api|ops)\s*->\s*(\w+)\s*\(", src):
            if name in strfn and strfn[name] not in USER_SAFE_STR_TARGETS:
                bad.append(f"{rel}: {name} -> {strfn[name]}")
    gen = (ROOT / "kapi/kapi_generated.c").read_text(encoding="utf-8")
    if "return vfs_devname_user(prefix);" not in gen:
        bad.append("kapi/kapi_generated.c: wrap_vfs_devname が vfs_devname_user を通らない (make all の前?)")
    exe = (ROOT / "exec/exec.c").read_text(encoding="utf-8")
    m = re.search(r"const char \*vfs_devname_user\(const char \*prefix\)\n\{(.*?)\n\}", exe, re.S)
    if not m or "ring3_user_str(ring3_in_syscall" not in m.group(1):
        bad.append("exec/exec.c: vfs_devname_user がトランポリンの写しを返さない")
    for b in bad:
        print("  str-return: " + b)
    print(f"  str-return guard: {'ok' if not bad else 'NG'} "
          f"({len(strfn)} string KAPIs, installer sources {len(INSTALLER_SRCS)})", flush=True)
    return 1 if bad else 0


def build_target(tmp):
    base = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
            "-fno-pie", "-fno-stack-protector", "-nostdlib", "-mno-red-zone", "-fcommon",
            "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
            "-D__OS32_USERLAND__", "-I.", "-Iinclude", "-Isdk/include", "-Isdk/include/os32",
            "-Iuserland/lib"]
    for rel in ("userland/system/inst_disk.c", "userland/system/inst_hdd.c",
                "userland/system/cdinst.c", "userland/system/install.c"):
        extra = ["-Wno-unused-function"] if rel.endswith("install.c") else []
        run([*base, *extra, "-c", rel, "-o", str(pathlib.Path(tmp) / (pathlib.Path(rel).stem + ".o"))],
            check=True)
    run(["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
         "-fno-stack-protector", "-nostdlib", "-Os", "-Wall", "-Werror", "-Iboot", "-c",
         "boot/ext2_mini.c", "-o", str(pathlib.Path(tmp) / "em.o")], check=True)
    print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)


# ---- 否定側: (ファイル, 前, 後, 説明) -------------------------------------------
MUTATIONS = [
    ("userland/system/inst_disk.c",
     "        if (lba0[510] == 0x55 && lba0[511] == 0xAA) return HDPREP_E_MBR_SIG;",
     "        (void)0;", "空の表で LBA 0 の 55AA を見ない"),
    ("userland/system/inst_disk.c", "    if (used != 1) return INST_E_MULTI;",
     "    if (used < 1) return INST_E_MULTI;", "2 項目以上を作り直す"),
    ("userland/system/inst_disk.c",
     "    if (e.sys_id != PC98PT_SID_OS32 || !name_is_os32(raw)) return INST_E_FOREIGN;",
     "    if (e.sys_id != PC98PT_SID_OS32 || (!name_is_os32(raw) && 0)) return INST_E_FOREIGN;",
     "名前 OS32 を見ない"),
    ("userland/system/inst_disk.c",
     "    if (e.sys_id != PC98PT_SID_OS32 || !name_is_os32(raw)) return INST_E_FOREIGN;",
     "    if (!name_is_os32(raw)) return INST_E_FOREIGN;", "sid を見ない (未知の区画を消す)"),
    ("userland/system/inst_disk.c",
     "        if (st != expect_start) return INST_E_START;\n        *out_mode = INST_MODE_RECREATE;",
     "        *out_mode = INST_MODE_RECREATE;", "標準配置の開始を見ない"),
    ("userland/system/inst_disk.c",
     "        if (st != expect_start) return INST_E_START;\n        *out_mode = INST_MODE_RECREATE_OLD;",
     "        *out_mode = INST_MODE_RECREATE_OLD;", "旧配置の開始を見ない (16/63 のシリンダ 12 を消す)"),
    ("userland/system/inst_disk.c", "    if (pc98pt_os32_is_legacy(lba1, heads, spt, disk_total)) {",
     "    if (0) {", "旧配置の NHD を入れ直せない"),
    ("userland/system/inst_disk.c", "        if (c != ' ' && c != 0) return 0;", "        (void)c;",
     "名前の後ろ (OS32X) を見ない"),
    ("userland/system/inst_disk.c", "loader_len > INST_LOADER_MAX)", "loader_len > INST_LOADER_MAX + 1UL)",
     "ローダ 8193 B を通す"),
    ("userland/system/inst_disk.c", "kernel_len > INST_KERNEL_MAX)", "kernel_len > INST_KERNEL_MAX + 1UL)",
     "vmkernel 508KiB + 1 を通す"),
    ("userland/system/inst_disk.c", "ipl_len > INST_IPL_MAX)", "ipl_len > INST_IPL_MAX + 1UL)",
     "IPL 513 B を通す"),
    ("userland/system/inst_disk.c", "    if (data > 12UL) meta += 1UL;", "    (void)0;",
     "単一間接ブロックを数えない"),
    ("userland/system/inst_disk.c", "    if (need_blocks > free_blocks) return INST_E_SPACE;\n", "",
     "ブロックの不足を見ない"),
    ("userland/system/inst_disk.c", "    if (need_inodes > free_inodes) return INST_E_INODES;\n", "",
     "inode の不足を見ない"),
    ("userland/system/inst_disk.c", "    ipl[INST_IPL_OFF_HEADS] = (unsigned char)heads;",
     "    ipl[INST_IPL_OFF_HEADS] = (unsigned char)(heads * 0UL + 8UL);", "IPL のヘッド数を 8 に固定 (旧 cdinst)"),
    ("userland/system/inst_disk.c", "    for (i = 0; i < PC98PT_SECTOR_SIZE; i++) sect[i] = 0;",
     "    for (i = 0; i < 0; i++) sect[i] = 0;",
     "区画表の残りの項目を消さない"),
    ("userland/system/inst_disk.c", "    meta += EXT2L_FIRST_DATA_BLOCK + EXT2L_ROOT_DATA_BLOCKS;",
     "    meta += EXT2L_FIRST_DATA_BLOCK;", "空きの見積もりがルートのブロックを数えない"),
    ("userland/system/inst_hdd.c",
     "    if (rc != 0 || !ih_memeq(ih_sect, ih_back, IH_SECT)) {\n        inst_hdd_incomplete(api, \"partition table",
     "    if (rc != 0) {\n        inst_hdd_incomplete(api, \"partition table", "区画表を読み戻して比べない"),
    ("userland/system/inst_hdd.c", "    rc = api->sys_mount(INST_MOUNT, INST_DEV, \"ext2\");",
     "    rc = 0;", "通常のマウントで確かめない"),
    ("userland/system/inst_hdd.c", "        rc = api->sys_umount_checked(INST_MOUNT);", "        rc = 0;",
     "マウント中の hd0 を外さない"),
    ("userland/system/inst_hdd.c", "    if (api->dev_mount_count(INST_DRIVE) != 0) {", "    if (0) {",
     "外した後にマウントが残っていても書く"),
    ("userland/system/inst_hdd.c",
     "    root_hd0 = (rootdev && ih_streq(rootdev, INST_DEV)) ? 1 : 0;",
     "    root_hd0 = (rootdev && 0) ? 1 : 0;",
     "ルートの hd0 を書く"),
    ("userland/system/inst_hdd.c", "    inst_patch_ipl(ih_sect, t->plan.heads, t->plan.spt);",
     "    inst_patch_ipl(ih_sect, t->hg.ata_def_heads, t->hg.ata_def_spt);",
     "IPL に IDENTIFY の幾何を書く (旧 install、F15)"),
    ("userland/system/inst_hdd.c", "    return ih_memeq(buf, ih_back, IH_SECT) ? 0 : -1;", "    return 0;",
     "ローダ / IPL を読み戻して比べない"),
    ("userland/system/inst_hdd.c", "    rc = api->ext2_format_at(INST_DRIVE, t->plan.start, t->plan.len);",
     "    rc = api->ext2_format_at(INST_DRIVE, t->plan.start, t->g.ata_total - t->plan.start);",
     "ディスク全体を format する (旧動作)"),
    ("userland/system/inst_hdd.c",
     "    if (rc != 0) { ih_refuse(api, rc); return rc; }\n    return 0;\n}\n\nvoid inst_hdd_describe",
     "    (void)rc;\n    return 0;\n}\n\nvoid inst_hdd_describe", "容量の不足を断らない"),
    ("userland/system/inst_hdd.c", "    if (t->mode != INST_MODE_EMPTY)\n", "    if (0)\n",
     "作り直しでデータが消えることを表示しない"),
    ("userland/system/inst_hdd.c", "    t->mounts = api->dev_mount_count(INST_DRIVE);",
     "    t->mounts = 0;", "検査でマウントを数えない (外さずに書きに行く)"),
    ("userland/system/cdinst.c", "    if (*kernel_len == 0 || !have_shell) {",
     "    if (0 && (*kernel_len == 0 || !have_shell)) {",
     "MINIMAL の必須の中身を見ない"),
    ("userland/system/cdinst.c",
     "            i = pkg_first_overflow(&info, 4);   /* strlen(\"/hd0\") */\n            if (i >= 0) {",
     "            i = -1;\n            if (i >= 0) {", "前置の溢れを書く前に見ない"),
    ("userland/system/cdinst.c", "        if (choice < need_from[b]) continue;",
     "        if (0 && choice < need_from[b]) continue;",
     "選ばない型のパッケージも容量に数える"),
    ("userland/system/cdinst.c", "    if (info.header.flags & PKG_FLAG_LZSS) {\n        println(COL_RED, \"  BOOT.PKG must not",
     "    if (0) {\n        println(COL_RED, \"  BOOT.PKG must not", "LZSS の BOOT.PKG を通す"),
    ("userland/system/cdinst.c",
     "    if (ret != PKG_OK) inst_hdd_incomplete(api, \"package installation failed\", ret);",
     "    (void)ret;", "追加パッケージの失敗を INCOMPLETE にしない"),
    ("userland/system/cdinst.c", "    boot_img_free(&boot);\n    if (ret != 0) return;",
     "    boot_img_free(&boot);\n    (void)ret;", "IPL の失敗の後も展開する"),
    ("userland/system/cdinst.c", "    if (preflight(choice, &boot, &tgt) != 0) return;",
     "    (void)preflight(choice, &boot, &tgt);", "事前検査の失敗でも書く"),
    ("userland/system/install.c", "    if (measure_need(sizes[MEDIA_KERNEL], &need) != 0) {",
     "    if (measure_need(sizes[MEDIA_KERNEL], &need) != 0 && 0) {", "FD の列挙の失敗を書く前に見ない"),
    ("userland/system/install.c", "    if (inst_hdd_release(api, &tgt) != 0) goto end;", "",
     "install が hd0 を外さずに書く"),
    ("userland/system/install.c", "    if (inst_hdd_check_media(api, &tgt, sizes[MEDIA_IPL]",
     "    if (0 && inst_hdd_check_media(api, &tgt, sizes[MEDIA_IPL]", "install が大きさと容量を見ない"),
    ("userland/system/install.c", "        inst_hdd_incomplete(api, \"sync failed\", ret);\n", "",
     "install の sync の失敗を INCOMPLETE と出さない"),
    # ---- 実装レビュー往復 1 (Codex P1-1〜3・P2-4〜6、Fable minor) ----
    ("userland/system/cdinst.c", "    if (sum != info->header.orig_size) {", "    if (0) {",
     "P1-1 項目の和と orig_size を比べない (orig_size 0 のヘッダを通す)"),
    ("userland/system/cdinst.c",
     "        st.st_size != info->data_offset + info->header.comp_size) {",
     "        0) {", "P1-1 PKG の長さを見ない (データ部が切れた媒体を通す)"),
    ("userland/system/cdinst.c", "            if (!pkg_data_ok(path, &info)) return PKG_ERR_CORRUPT;\n", "",
     "P1-1 追加パッケージのデータ部を書く前に見ない"),
    ("userland/system/cdinst.c", "    if (!pkg_data_ok(PKG_BOOT, &info)) return PKG_ERR_CORRUPT;\n", "",
     "P1-1 BOOT.PKG のデータ部を見ない"),
    ("userland/system/cdinst.c", "PKG_NEED_SHELL) && ent->size > 0) have_shell = 1;",
     "PKG_NEED_SHELL)) have_shell = 1;", "P1-1 空の shell.bin を必須として通す"),
    ("userland/system/cdinst.c",
     "            if (pkg_paths_ok(path, &info) != 0) return PKG_ERR_CORRUPT;\n            if (!pkg_data_ok",
     "            if (!pkg_data_ok", "P1-2 パスを書く前に見ない (展開の途中で気付く)"),
    ("userland/system/inst_disk.c",
     "        if (len == 2 && c[0] == '.' && c[1] == '.') return INST_E_PATH;\n", "",
     "P1-2 '..' を通す (/hd0 の外へ書く)"),
    ("userland/system/inst_disk.c", "        if (len == 1 && c[0] == '.') return INST_E_PATH;\n", "",
     "P1-2 '.' を通す"),
    ("userland/system/inst_disk.c", "        if (len == 0) return INST_E_PATH;", "        (void)0;",
     "P1-2 空の要素 ('//'・末尾 '/') を通す"),
    ("userland/system/inst_disk.c", "    if (!path || path[0] != '/') return INST_E_PATH;",
     "    if (!path || !path[0]) return INST_E_PATH;", "P1-2 絶対でないパスを通す"),
    ("userland/system/inst_disk.c", "        if (depth + INST_PREFIX_DEPTH > INST_VFS_MAX_DEPTH) return INST_E_DEPTH;",
     "        if (depth > INST_VFS_MAX_DEPTH) return INST_E_DEPTH;", "P2-5 前置 /hd0 の 1 要素を数えない"),
    ("userland/system/install.c", "    for (i = 0; i < MEDIA_COUNT; i++) {",
     "    for (i = 0; i < MEDIA_COUNT - 1; i++) {", "P1-3 FD の /sys/shell.bin を見ない"),
    ("userland/system/install.c", "        if ((st.st_mode & OS_S_IFMT) != OS_S_IFREG) {", "        if (0) {",
     "P1-3 FD の必須の種別 (通常のファイル) を見ない"),
    ("userland/system/inst_disk.c", "    if (size > INST_EXT2_MAX_FILE) n->too_big++;\n", "",
     "P2-4 1 ファイルの上限を数えない"),
    ("userland/system/inst_disk.c", "    if (n->too_big) return INST_E_FILE_SIZE;\n", "",
     "P2-4 1 ファイルの上限で断らない"),
    ("userland/system/inst_hdd.c", "REBOOT, then run the installer again", "Run the installer again",
     "P2-6 再起動を案内しない"),
    ("userland/system/inst_hdd.c", "        ih_refuse(api, rc);\n        ih_host_hint(api);\n", "        ih_refuse(api, rc);\n",
     "P2-6 直せない表にホスト側の手当てを案内しない"),
    ("userland/system/inst_hdd.c", "        if (t->mounts == 1 && dev && ih_streq(dev, INST_DEV)) {",
     "        if (dev && ih_streq(dev, INST_DEV)) {", "Fable 別の prefix にもマウントされた hd0 を承認後に外しに行く"),
    ("userland/system/inst_hdd.c", "        ih_refuse(api, INST_E_GEOM);", "        ih_refuse(api, INST_E_ARG);",
     "Fable hdd_geom_info の失敗を bad argument と出す"),
    ("userland/system/inst_disk.c", "    need_inodes = n->files + n->dirs + INST_SPACE_MARGIN_INODES;",
     "    need_inodes = n->files + n->dirs;", "Fable 自動で作る親ディレクトリの inode の余白を持たない"),
    ("userland/system/inst_disk.c", "        out->free_blocks = 0;\n", "",
     "Fable 失敗のとき room を埋めない"),
    ("boot/ext2_mini.c", "    if (file_size > max_size) return EXT2M_ERR_TOO_BIG;",
     "    if (file_size > max_size) file_size = max_size;", "上限で切り詰める (旧動作)"),
    ("boot/ext2_mini.c", "    if (file_size > max_size) return EXT2M_ERR_TOO_BIG;\n", "",
     "上限を見ない"),
]


def _tally(counts, status, why):
    counts[status] = counts.get(status, 0) + 1
    print(f"MUTATION {status}: {why}", flush=True)


def mutate(img, counts):
    """ビルドが通って試験が落ちたものだけ RED。ビルドが通らない変異は ERROR。
    変異させるファイルごとに恒等変異 (対照) を当て、SURVIVED を確かめる。"""
    files = []
    for m in MUTATIONS:
        if m[0] not in files:
            files.append(m[0])
    plan = [(f, None, None, "対照 (恒等): " + f) for f in files] + list(MUTATIONS)
    for rel, before, after, why in plan:
        with tempfile.TemporaryDirectory(prefix="os32-hdd2-mut-") as tmp:
            troot = pathlib.Path(tmp) / "root"
            for m in MIRROR:
                dst = troot / m
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / m, dst)
            path = troot / rel
            text = path.read_text(encoding="utf-8")
            control = before is None
            if control:
                text += "\n/* identity mutation (control) */\n"
            elif text.count(before) != 1:
                _tally(counts, "NOT_APPLIED", why + f" (置き換え元 {text.count(before)} 件)")
                continue
            else:
                text = text.replace(before, after, 1)
            path.write_text(text, encoding="utf-8")
            exes = build_all(tmp, troot, quiet=True)
            if exes is None:
                status = "ERROR"
            else:
                try:
                    status = "RED" if run_cases(exes, img, quiet=True) else "SURVIVED"
                except subprocess.TimeoutExpired:
                    status = "RED"
            if control:
                _tally(counts, "CONTROL_OK" if status == "SURVIVED" else "CONTROL_BAD",
                       why + " → " + status)
            else:
                _tally(counts, status, why + (" (ビルドが通らない)" if status == "ERROR" else ""))
    return len(files)


def main(argv):
    failed = 0
    with tempfile.TemporaryDirectory(prefix="os32-hdd2-") as tmp:
        exes = build_all(tmp)
        if exes is None:
            print("BUILD FAIL")
            return 1
        print("HOST COMPILE PASS (hdd_stage2_host / cdinst_host ILP32 / install_fresh_host / "
              "ext2_mini_host ILP32, -Werror)", flush=True)
        img = make_mini_image(tmp)
        failed += run_cases(exes, img)
        failed += room_cross_check(exes["pure"], tmp)
        failed += consts_cross_check(exes["pure"])
        failed += real_pkg_paths(exes["pure"])
        failed += str_return_guard()
        total = len(PURE_CASES) + len(CDI_CASES) + len(INS_CASES) + len(MINI_FILES) + \
            len(ROOM_SIZES) + 3
        print(f"SUMMARY {total - failed}/{total} PASS", flush=True)

        if "--target" in argv:
            build_target(tmp)

        if "--mutate" in argv and failed == 0:
            counts = {}
            nctl = mutate(img, counts)
            n = len(MUTATIONS)
            red = counts.get("RED", 0)
            print("MUTATIONS {}/{} RED (ERROR {}, SURVIVED {}, NOT_APPLIED {}); "
                  "CONTROLS {}/{} SURVIVED (期待どおり)".format(
                      red, n, counts.get("ERROR", 0), counts.get("SURVIVED", 0),
                      counts.get("NOT_APPLIED", 0), counts.get("CONTROL_OK", 0), nctl),
                  flush=True)
            if red != n or counts.get("CONTROL_OK", 0) != nctl:
                failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
