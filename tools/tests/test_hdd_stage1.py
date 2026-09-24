"""票 TASK_HDD_INSTALL 段 1 (HDD の一時置き場) のホスト試験。

記録: tools/tests/hdd_stage1_tdd.md
票  : docs/tasks/realhw/TASK_HDD_INSTALL.md 段 1 / §1-v3 (N3〜N7、R3-1)

実物を 1 行も写さずに回す:
  tools/tests/hdd_stage1_host.c  drivers/pc98pt.c・drivers/ide_addr.c・
                                 fs/ext2_layout.c・userland/shell/hdprep_plan.c
  tools/tests/ext2_part_host.c   fs/ext2_super.c・ext2_fmt.c (区画探索と format_at)
  tools/pc98pt.py / tools/nhd_deploy.py (migrate-pt) — Python 側
見るもの:
  - 区画表の標準配置 (8/17・16/63)、fatfs の PC98PartEntry との一致、C と Python の
    書き手が 1 バイトも違わない
  - ext2_find_partition は見つからない / 読めない / 幾何なし / 旧配置で失敗する
  - ATA の LBA28 / 現在の CHS / 既定の CHS の選択と範囲検査 (lba=268435456)
  - format の大きさの固定点 (32/33 グループ、最終グループの切り下げ、16,652 セクタ)
  - format_at の像が e2fsck -fn で clean、範囲の外を書かない
  - hdprep の断る条件の全部と計画
  - migrate-pt: 旧配置の NHD を作って変換 → e2fsck -fn clean、開始 LBA 不変、
    FS のバイト列は不変、断るときは NHD を 1 バイトも変えない

  python3 -B tools/tests/test_hdd_stage1.py            # 全部
  python3 -B tools/tests/test_hdd_stage1.py --target   # + i386-elf -Werror で新しいソース
  python3 -B tools/tests/test_hdd_stage1.py --mutate   # 否定側 (変異が RED になるか)
"""
import hashlib
import importlib.util
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/tests/hdd_stage1_host.c"
PART_HARNESS = ROOT / "tools/tests/ext2_part_host.c"
PT_SRC = ROOT / "drivers/pc98pt.c"
ADDR_SRC = ROOT / "drivers/ide_addr.c"
LAYOUT_SRC = ROOT / "fs/ext2_layout.c"
HDPREP_SRC = ROOT / "userland/shell/hdprep_plan.c"
SUPER_SRC = ROOT / "fs/ext2_super.c"
FMT_SRC = ROOT / "fs/ext2_fmt.c"
PY_PT = ROOT / "tools/pc98pt.py"
PY_NHD = ROOT / "tools/nhd_deploy.py"

PURE_CASES = ["pt_offsets", "pt_817", "pt_1663", "pt_reject", "ata_lba28",
              "ata_range", "ata_chs", "layout", "hdprep_geom", "hdprep_disk",
              "hdprep_mounts", "hdprep_plan"]
PART_CASES = ["find_bios", "find_fail", "format_clamp", "format_at_16652",
              "format_at_refuse", "mount_bounds", "format_geom", "legacy_hint"]
# e2fsck にかける format_at の大きさ (開始 2016、16/63 の RAM ディスク 64,512 セクタ)
FSCK_SIZES = [16652, 20160, 36000, 62496]

HOST_FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
              "-Wdeclaration-after-statement", "-D__cdecl="]
PART_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
              "-fno-stack-protector", "-nostdlib", "-static", "-O1",
              "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-parameter", "-Wno-sign-compare",
              "-Wdeclaration-after-statement", "-D__cdecl="]
PART_INC_DIRS = ["include", "fs", "lib", "kernel", "drivers", "sdk/include/os32"]

# ---- 否定側 (C): (ファイル, 前, 後, 説明) -----------------------------------
C_MUTATIONS = [
    ("drivers/pc98pt.h", "#define PC98PT_OFF_SSECT      8", "#define PC98PT_OFF_SSECT      6",
     "開始セクタを旧配置の +6 で読み書きする"),
    ("drivers/pc98pt.c", "    if (disk_total != 0 && end_excl > disk_total) return PC98PT_ERR_RANGE;",
     "    if (0 && disk_total != 0 && end_excl > disk_total) return PC98PT_ERR_RANGE;",
     "終わりがディスクの外でも通す"),
    ("drivers/pc98pt.c", "    if (end_excl <= start) return PC98PT_ERR_RANGE;\n", "",
     "終わり <= 開始を通す (旧配置を別の場所として読む)"),
    ("drivers/pc98pt.c", "    if ((start % cylsz) != 0 || (len % cylsz) != 0) return PC98PT_ERR_ALIGN;\n", "",
     "シリンダ境界に無い区画を書く"),
    ("drivers/pc98pt.c", "        if (e.sys_id != PC98PT_SID_OS32) continue;", "        if (e.sys_id == 0) continue;",
     "OS32 以外 (FAT) の項目を ext2 として拾う"),
    ("drivers/pc98pt.c", "    if (head >= heads || sect >= spt || cyl > PC98PT_MAX_CYL)\n        return PC98PT_ERR_RANGE;\n", "",
     "開始ヘッド・セクタが幾何の外でも通す"),
    ("drivers/ide_addr.c", "IDE_DRVHEAD_BASE | IDE_DRVHEAD_LBA | slave |", "IDE_DRVHEAD_BASE | slave |",
     "LBA28 なのに DRV/HEAD の bit6 を立てない (CHS として解釈される)"),
    ("drivers/ide_addr.c", "    if (count > limit - lba) return 0;\n", "",
     "lba + count の末尾を見ない"),
    ("drivers/ide_addr.c", "        if (limit == 0 || limit > IDE_LBA28_LIMIT) limit = IDE_LBA28_LIMIT;",
     "        if (limit == 0) limit = IDE_LBA28_LIMIT;",
     "LBA28 の上限を見ない (lba=268435456 が LBA 0 に化ける)"),
    ("drivers/ide_addr.c", "    } else if ((g->w53 & 0x0001) &&", "    } else if (0 && (g->w53 & 0x0001) &&",
     "現在の幾何 (word 53-56) を無視して既定で CHS にする (F13)"),
    ("fs/ext2_layout.c", "        if (l.num_groups <= 1) return EXT2L_ERR_SMALL;\n        tb = EXT2L_FIRST_DATA_BLOCK",
     "        *out = l;\n        return EXT2L_OK;\n        tb = EXT2L_FIRST_DATA_BLOCK",
     "最終グループが足りなくても落とさない (F12)"),
    ("fs/ext2_layout.c", "    if (last == 0) l->last_group_need += EXT2L_ROOT_DATA_BLOCKS;\n", "",
     "グループ 0 のルートのデータを数えない"),
    ("fs/ext2_layout.c", "    if (ext2_layout_is_sparse(g)) n += 1UL + l->gdt_blocks;",
     "    if (0 && ext2_layout_is_sparse(g)) n += 1UL + l->gdt_blocks;",
     "スパースグループの SB / GDT を数えない"),
    ("fs/ext2_layout.c", "    if (l->num_groups > max_groups) return EXT2L_ERR_GROUPS;",
     "    if (0 && l->num_groups > max_groups) return EXT2L_ERR_GROUPS;",
     "32 グループの上限を見ない"),
    ("userland/shell/hdprep_plan.c", "    if (g->addr_mode != HDPREP_AMODE_LBA28 &&",
     "    if (0 && g->addr_mode != HDPREP_AMODE_LBA28 &&",
     "LBA も現在の CHS も無いドライブに書く"),
    ("userland/shell/hdprep_plan.c", "    if (lba0[510] == 0x55 && lba0[511] == 0xAA) return HDPREP_E_MBR_SIG;\n", "",
     "LBA 0 の 55AA を見ない"),
    ("userland/shell/hdprep_plan.c", "    if (root_is_hd0) return HDPREP_E_ROOT;",
     "    if (0 && root_is_hd0) return HDPREP_E_ROOT;",
     "ルートの hd0 を外しに行く"),
    ("userland/shell/hdprep_plan.c", "    if (g->bios_seclen != HDPREP_SECLEN) return HDPREP_E_SECLEN;\n", "",
     "BX ≠ 512 を通す"),
    ("userland/shell/hdprep_plan.c",
     "    start = ((HDPREP_BOOT_RESERVE_LBA + cyl - 1UL) / cyl) * cyl;",
     "    start = HDPREP_BOOT_RESERVE_LBA;",
     "開始をシリンダ境界に揃えない (16/63 で 1632)"),
    ("userland/shell/hdprep_plan.c",
     "    if (g->ata_total < limit) limit = g->ata_total;\n", "",
     "IDENTIFY の総数を見ない"),
    ("userland/shell/hdprep_plan.c", "    if (pc98pt_count_used(lba1) != 0) return HDPREP_E_PT_USED;\n", "",
     "既存の区画項目を上書きする"),
    ("fs/ext2_super.c",
     "        return EXT2_ERR_NOPART;\n    }\n\n    *out_start",
     "        start = 1088; len = 16384;\n    }\n\n    *out_start",
     "見つからないとき LBA 1088 にフォールバックする (旧動作)"),
    ("fs/ext2_super.c", "    src = bootinfo_part_geom(ide_drive, &heads, &spt);\n",
     "    src = bootinfo_part_geom(ide_drive, &heads, &spt);\n"
     "    heads = info.heads; spt = info.sectors;\n",
     "区画表の CHS を IDENTIFY の幾何で LBA にする (F4)"),
    ("fs/ext2_super.c", "    return (block_num < ctx->part_len / 2U) ? 1 : 0;", "    return 1;",
     "ブロック I/O が区画の外を書ける"),
    ("fs/ext2_super.c", "    if (ctx->sb_info.total_blocks > ctx->part_len / 2U) {",
     "    if (0) {", "FS が区画より大きくてもマウントする"),
    ("fs/ext2_super.c", "    if (dev_blk_read_lba(dev, PC98PT_LBA, 1, pt_sect) != 0) return EXT2_ERR_IO;\n",
     "    (void)dev_blk_read_lba(dev, PC98PT_LBA, 1, pt_sect);\n",
     "区画表が読めなくても続ける"),
    ("fs/ext2_fmt.c", "    if (length > info.total_sectors - start_lba) return EXT2_ERR_INVAL;\n", "",
     "format_at がディスクの外まで書く"),
    ("fs/ext2_fmt.c", "    if (start_lba < (u32)EXT2_FMT_MIN_LBA) return EXT2_ERR_INVAL;\n", "",
     "format_at が IPL / 区画表 / ローダを潰す"),
    ("fs/ext2_fmt.c", "    if (sectors > part_len) sectors = part_len;\n", "",
     "ext2_format が区画の長さで頭打ちにしない"),
    ("userland/shell/hdprep_plan.c", "    if (g->ata_total == 0) return HDPREP_E_NO_TOTAL;", "",
     "総数の申告が無いドライブに探りを書く (C3)"),
    ("userland/shell/hdprep_plan.c", "        if (chs < limit) limit = chs;", "        (void)chs;",
     "現在の CHS の容量を計画の上限にしない (C4)"),
    ("userland/shell/hdprep_plan.c", "        if (limit > HDPREP_LBA28_LIMIT) limit = HDPREP_LBA28_LIMIT;",
     "        (void)0;", "LBA28 の上限を計画の上限にしない (C4)"),
    ("fs/ext2_fmt.c", "    if (!ide_range_ok(ide_drive, start_lba, length)) return EXT2_ERR_INVAL;\n", "",
     "format_at が ATA の方式の上限を見ない (C4)"),
    ("fs/ext2_fmt.c", "        if (src != BOOTINFO_GEOM_BIOS) {", "        if (0) {",
     "ext2_format が IDENTIFY の幾何で見つけた区画に書く (m3)"),
    ("fs/ext2_fmt.c", "    if (total_sectors > EXT2L_MAX_SECTORS(EXT2_MAX_GROUPS))\n", "    if (0)\n",
     "ext2_format が 32 グループを超える区画で頭打ちにせず断る (m2)"),
    ("drivers/pc98pt.c", "        return 1;\n    }\n    return 0;\n}\n\nint pc98pt_make_os32(",
     "        return 0;\n    }\n    return 0;\n}\n\nint pc98pt_make_os32(",
     "旧配置を検出しない (カーネルが移行の案内を出せない、M2)"),
    ("fs/ext2_fmt.c", "    if (!ide_range_ok(ide_drive, base_lba, lay.total_blocks * 2UL))\n        return EXT2_ERR_INVAL;\n", "",
     "ext2_format の頭打ちの後の範囲を ATA の上限と照合しない (ラリー 2 の 3)"),
    ("drivers/pc98pt.c", "            return 0;                       /* 標準配置で読める */",
     "            (void)0;", "標準配置の表を旧配置と誤認する"),
]

# ---- 否定側 (Python) ------------------------------------------------------------
PY_MUTATIONS = [
    ("tools/pc98pt.py", "OFF_SSECT, OFF_SHEAD, OFF_SCYL = 8, 9, 10", "OFF_SSECT, OFF_SHEAD, OFF_SCYL = 6, 7, 8",
     "Python の書き手が旧配置で書く"),
    ("tools/pc98pt.py", "    e[OFF_ESECT] = spt - 1\n", "    e[OFF_ESECT] = 0\n",
     "Python の書き手の終了セクタが C と違う"),
    ("tools/nhd_deploy.py", "        raise MigrateError(\"旧配置の開始 LBA {} に ext2 が無い\".format(start))\n",
     "        fs_sect = 0\n",
     "migrate-pt が ext2 の無い位置を区画にする"),
    ("tools/nhd_deploy.py", "    if len(used) != 1:\n", "    if len(used) < 1:\n",
     "migrate-pt が 2 つ以上の項目を持つ表を書き換える"),
    ("tools/nhd_deploy.py", "    if fs_sect > length:\n", "    if False:\n",
     "migrate-pt が区画より大きい FS を通す"),
    ("tools/nhd_deploy.py", "    if log != 0:\n", "    if False:\n",
     "migrate-pt が 1KiB 以外のブロック長を通す (C2)"),
    ("tools/nhd_deploy.py", "    if not loader_data or len(loader_data) > LOADER_MAX_SECTORS * 512:\n",
     "    if not loader_data:\n", "ローダの大きさを書き込みの後で見る (C1)"),
    ("tools/nhd_deploy.py", "    if ksize == 0 or ksize > KERNEL_MAX_BYTES:\n", "    if False:\n",
     "カーネルの大きさを見ない (C1)"),
    ("tools/nhd_deploy.py", "        ok, reason = (stamp_check or verify_pull_stamp)()\n",
     "        ok, reason = True, ''\n", "push の来歴を書き込みの後で見る (C1)"),
    ("tools/nhd_deploy.py", "    if state == 'standard':\n        return True\n    if state == 'not_nhd':",
     "    if True:\n        return True\n    if state == 'not_nhd':",
     "旧配置の NHD へ v64 のカーネルを配る (M2)"),
    ("tools/nhd_deploy.py", "    if state == 'standard':\n        return True\n    if state == 'not_nhd':",
     "    if state == 'standard' or (state == 'legacy' and auto is not None):\n        return True\n    if state == 'not_nhd':",
     "自動で移行できない旧配置を通す (ラリー 2 の 1)"),
    ("tools/nhd_deploy.py", "    if state == 'standard':\n        return True\n    if state == 'not_nhd':",
     "    if state in ('standard', 'broken', 'none'):\n        return True\n    if state == 'not_nhd':",
     "OS32 の項目が無い・壊れている NHD へ配る"),
    ("tools/nhd_deploy.py", "    if _GUARD_NEXT_MOUNT and not legacy_pt_guard():",
     "    if False and not legacy_pt_guard():",
     "取り込みの後の門を掛けない (ラリー 2 の 2)"),
    ("tools/nhd_deploy.py", "        return legacy_pt_guard() and ensure_mounted()",
     "        return ensure_mounted()", "マウント済みの NHD を門に通さない"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, **kw)


# ======================================================================== #
#  C のハーネス                                                             #
# ======================================================================== #

def build_pure(tmp, root=ROOT):
    exe = pathlib.Path(tmp) / "hdd-stage1-host"
    harness = HARNESS
    if root != ROOT:
        text = HARNESS.read_text(encoding="utf-8").replace('"../../', '"')
        harness = pathlib.Path(tmp) / "hdd_stage1_host.c"
        harness.write_text(text, encoding="utf-8")
    inc = ["-I" + str(root), "-I" + str(root / "include"), "-I" + str(root / "drivers"),
           "-I" + str(root / "fs"), "-I" + str(ROOT / "include"),
           "-I" + str(ROOT / "drivers"), "-I" + str(ROOT / "fs")]
    r = run(["gcc", *HOST_FLAGS, *inc, str(harness), "-o", str(exe)],
            capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return None
    return exe


def build_part(tmp, root=ROOT):
    exe = pathlib.Path(tmp) / "ext2-part-host"
    harness = PART_HARNESS
    if root != ROOT:
        text = PART_HARNESS.read_text(encoding="utf-8").replace('"../../', '"')
        harness = pathlib.Path(tmp) / "ext2_part_host.c"
        harness.write_text(text, encoding="utf-8")
    inc = ["-I" + str(root)]
    inc += ["-I" + str(root / d) for d in PART_INC_DIRS]
    inc += ["-I" + str(ROOT / d) for d in PART_INC_DIRS]
    r = run(["gcc", *PART_FLAGS, *inc, str(harness), "-o", str(exe)],
            capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return None
    return exe


def run_cases(exe, cases, quiet=False):
    failed = 0
    for case in cases:
        r = run([str(exe), case], capture_output=quiet, text=True)
        if not quiet:
            print(f"EXIT {case}={r.returncode}", flush=True)
        failed += r.returncode != 0
    return failed


def fsck_image(exe, start, size, tmp, quiet=False):
    """format_at の像を書き出して e2fsck -fn。clean なら 0。"""
    img = pathlib.Path(tmp) / f"fa_{size}.img"
    with open(img, "wb") as f:
        r = subprocess.run([str(exe), "dump", str(start), str(size)], stdout=f)
    if r.returncode != 0:
        if not quiet:
            print(f"  dump {size}: exit {r.returncode}")
        return 1
    fr = subprocess.run(["e2fsck", "-fn", str(img)], capture_output=True, text=True)
    if not quiet:
        tail = fr.stdout.strip().splitlines()[-1] if fr.stdout.strip() else ""
        print(f"  e2fsck -fn format_at({start}, {size}): rc={fr.returncode} {tail}", flush=True)
    if fr.returncode != 0 and not quiet:
        sys.stdout.write(fr.stdout + fr.stderr)
    return 0 if fr.returncode == 0 else 1


# ======================================================================== #
#  Python 側 (pc98pt.py / nhd_deploy.py migrate-pt)                          #
# ======================================================================== #

def load_tools(tools_dir):
    """pc98pt と nhd_deploy を tools_dir から読み込む (NP21W_DIR は触らない値に)。"""
    os.environ["NP21W_DIR"] = "/nonexistent-np21w"
    sys.path.insert(0, str(tools_dir))
    for name in ("pc98pt", "nhd_deploy", "deploy_protect", "deploy_manifests"):
        sys.modules.pop(name, None)
    import pc98pt  # noqa: E402
    import nhd_deploy  # noqa: E402
    return pc98pt, nhd_deploy


def py_cross_check(pc98pt, c_exe):
    """C の書き手 (pc98pt_make_os32) と Python の書き手が同じバイト列を出す。"""
    bad = 0
    combos = [(1632, 407864, 8, 17), (2016, 258048, 16, 63), (1632, 136, 8, 17),
              (16, 16 * 255 * 5, 16, 255), (1632, (65536 - 12) * 136, 8, 17),
              (1633, 136, 8, 17), (1632, 0, 8, 17), (1632, (65537 - 12) * 136, 8, 17)]
    for st, ln, h, s in combos:
        out = run([str(c_exe), "dump_entry", str(st), str(ln), str(h), str(s)],
                  capture_output=True, text=True).stdout.strip()
        try:
            py = pc98pt.make_os32(st, ln, h, s).hex()
        except pc98pt.PtError:
            py = "ERR"
        c = "ERR" if out.startswith("ERR") else out
        if c != py:
            print(f"  cross-check ({st},{ln},{h},{s}): C={c} Py={py}")
            bad += 1
    return bad


def make_legacy_nhd(pc98pt, path, cyls=300, heads=8, spt=17, extra_entry=False,
                    sid=0xE2, fs_blocks=None, no_fs=False, end_cyl=None, bs=1024):
    """旧配置の区画表 (cdinst / install が書いた形) と mkfs.ext2 の NHD を作る。"""
    total = cyls * heads * spt
    start = 1632
    with open(path, "wb") as f:
        f.write(pc98pt.make_nhd_header(cyls, heads, spt))
        f.truncate(512 + total * 512)
    pt = bytearray(512)
    ecyl = total // (heads * spt) - 1 if end_cyl is None else end_cyl
    pt[0], pt[1] = 0x80, sid
    pt[6], pt[7] = 0, 0
    struct.pack_into("<H", pt, 8, start // (heads * spt))
    pt[10], pt[11] = spt - 1, heads - 1
    struct.pack_into("<H", pt, 12, ecyl)
    pt[16:32] = b"OS32" + b" " * 12
    if extra_entry:
        pt[32], pt[33] = 0x80, 0xA1
    with open(path, "r+b") as f:
        f.seek(512 + 512)
        f.write(pt)
    if not no_fs:
        blocks = fs_blocks if fs_blocks is not None else (total - start) * 512 // bs
        r = subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", str(bs), "-I", "128",
                            "-L", "OS32_HDD", "-E", "offset={}".format(512 + start * 512),
                            str(path), str(blocks)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("mke2fs: " + r.stderr)
    return total, start


def region_hash(path, off, size):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(off)
        h.update(f.read(size))
    return h.hexdigest()


def e2fsck_part(path, start, length, tmp):
    part = pathlib.Path(tmp) / "part.img"
    with open(path, "rb") as f, open(part, "wb") as o:
        f.seek(512 + start * 512)
        o.write(f.read(length * 512))
    r = subprocess.run(["e2fsck", "-fn", str(part)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def py_migrate_cases(pc98pt, nhd, tmp, quiet=False):
    """migrate-pt の純粋部 (plan_migrate_pt / migrate_pt_raw)。失敗の数を返す。"""
    bad = 0

    def say(msg):
        if not quiet:
            print(msg)

    tmp = pathlib.Path(tmp)
    loader = bytes((i * 13 + 5) & 0xFF for i in range(4515))

    # --- 変換: 開始 LBA 不変・FS のバイト列不変・e2fsck clean・ローダが入る ---
    p = tmp / "legacy.nhd"
    total, start = make_legacy_nhd(pc98pt, p)
    length = total - start
    fs_hash = region_hash(p, 512 + start * 512, length * 512)
    rc0, _ = e2fsck_part(p, start, length, tmp)
    try:
        plan = nhd.migrate_pt_raw(str(p), loader)
    except Exception as exc:  # noqa: BLE001
        say(f"  migrate: 例外 {exc!r}")
        return bad + 1
    with open(p, "rb") as f:
        f.seek(512 + 512)
        sect = f.read(512)
        f.seek(512 + 2 * 512)
        ld = f.read(len(loader))
    found = pc98pt.find_os32(sect, 8, 17, total)
    rc1, out1 = e2fsck_part(p, start, length, tmp)
    checks = [
        ("state legacy", plan["state"] == "legacy"),
        ("開始 LBA 不変 (1632)", plan["start"] == start and found is not None and found[1] == start),
        ("長さ不変", found is not None and found[2] == length),
        ("標準配置の項目 = pc98pt.make_os32", sect[:32] == pc98pt.make_os32(start, length, 8, 17)),
        ("他の項目は空", sect[32:] == bytes(480)),
        ("ローダ LBA 2〜", ld == loader),
        ("FS のバイト列は不変", region_hash(p, 512 + start * 512, length * 512) == fs_hash),
        ("移行前 e2fsck clean", rc0 == 0),
        ("移行後 e2fsck -fn clean", rc1 == 0),
    ]
    for name, ok in checks:
        say(f"  {'ok  ' if ok else 'FAIL'} migrate: {name}")
        bad += not ok
    if rc1 != 0 and not quiet:
        print(out1)

    # --- 2 回目は「もう標準配置」: 何も書かない ---
    h_before = region_hash(p, 0, 512 + total * 512)
    plan2 = nhd.migrate_pt_raw(str(p), loader)
    ok = plan2["state"] == "standard" and region_hash(p, 0, 512 + total * 512) == h_before
    say(f"  {'ok  ' if ok else 'FAIL'} migrate: 2 回目は standard で何も書かない")
    bad += not ok

    # --- 断る: NHD を 1 バイトも変えない ---
    refusals = [
        ("項目が 2 つ", dict(extra_entry=True)),
        ("sid が OS32 でない", dict(sid=0xA1)),
        ("ext2 が無い", dict(no_fs=True)),
        ("FS が区画より大きい", dict(end_cyl=200)),
        ("終わりがディスクの外", dict(end_cyl=400)),
        ("ext2 のブロック長が 4KiB (C2)", dict(bs=4096)),
    ]
    for name, kw in refusals:
        q = tmp / "refuse.nhd"
        total_q, _ = make_legacy_nhd(pc98pt, q, **kw)
        h0 = region_hash(q, 0, 512 + total_q * 512)
        try:
            nhd.migrate_pt_raw(str(q), loader)
            ok = False
        except nhd.MigrateError:
            ok = True
        ok = ok and region_hash(q, 0, 512 + total_q * 512) == h0
        say(f"  {'ok  ' if ok else 'FAIL'} migrate 断る: {name} (NHD 不変)")
        bad += not ok

    # --- ローダが 8KB を超える ---
    q = tmp / "refuse2.nhd"
    total_q, _ = make_legacy_nhd(pc98pt, q)
    h0 = region_hash(q, 0, 512 + total_q * 512)
    try:
        nhd.migrate_pt_raw(str(q), bytes(8193))
        ok = False
    except nhd.MigrateError:
        ok = True
    ok = ok and region_hash(q, 0, 512 + total_q * 512) == h0
    say(f"  {'ok  ' if ok else 'FAIL'} migrate 断る: ローダ 8193 バイト (NHD 不変)")
    bad += not ok

    # --- do_migrate_pt 全体: **全部の検査が最初の書き込みより前** (Codex C1) ---
    #     マウント・コピー・push は贋物にして、呼ばれたかと NHD の不変を見る
    calls = []
    real_deploy = nhd.do_deploy
    nhd.is_mounted = lambda: False
    nhd.ensure_local_nhd = lambda: True
    nhd.do_umount = lambda: True
    nhd.do_copy = lambda *a, **k: calls.append(("copy", a, k)) or True
    nhd.do_deploy = lambda *a, **k: calls.append(("deploy",)) or True
    stamp = {"ok": True}
    nhd.verify_pull_stamp = lambda *a: (stamp["ok"], "stamp mismatch (試験)")
    good_loader = tmp / "loader.bin"
    good_loader.write_bytes(loader)
    big_loader = tmp / "loader_big.bin"
    big_loader.write_bytes(bytes(8193))
    good_kernel = tmp / "vmkernel.lz4"
    good_kernel.write_bytes(b"VK32" + bytes(1000))
    big_kernel = tmp / "vmkernel_big.lz4"
    big_kernel.write_bytes(bytes(nhd.KERNEL_MAX_BYTES + 1))
    flows = [
        ("ローダ 8193B", big_loader, good_kernel, False, True),
        ("カーネル 508KiB+1", good_loader, big_kernel, False, True),
        ("空のカーネル", good_loader, tmp / "empty.lz4", False, True),
        ("push で来歴が崩れている", good_loader, good_kernel, True, False),
        ("ext2 が 4KiB ブロック", good_loader, good_kernel, False, True),
    ]
    (tmp / "empty.lz4").write_bytes(b"")
    for name, ld_path, k_path, push, stamp_ok in flows:
        q = tmp / "flow.nhd"
        total_q, _ = make_legacy_nhd(pc98pt, q, bs=4096 if "4KiB" in name else 1024)
        nhd.NHD_LOCAL = str(q)
        stamp["ok"] = stamp_ok
        del calls[:]
        h0 = region_hash(q, 0, 512 + total_q * 512)
        rc = _silent_err(nhd.do_migrate_pt, str(ld_path), str(k_path), push)
        ok = rc is False and not calls and region_hash(q, 0, 512 + total_q * 512) == h0
        say(f"  {'ok  ' if ok else 'FAIL'} do_migrate_pt 断る: {name} (コピーも push もせず NHD 不変)")
        bad += not ok
    # 通る場合: カーネルのコピー 1 回 → 区画表が標準になる (push しない)
    q = tmp / "flow_ok.nhd"
    total_q, _ = make_legacy_nhd(pc98pt, q)
    nhd.NHD_LOCAL = str(q)
    stamp["ok"] = True
    del calls[:]
    rc = _silent_err(nhd.do_migrate_pt, str(good_loader), str(good_kernel), False)
    with open(q, "rb") as f:
        f.seek(1024)
        sect = f.read(512)
    ok = (rc is True and [c[0] for c in calls] == ["copy"]
          and pc98pt.find_os32(sect, 8, 17, total_q) == (0, 1632, total_q - 1632))
    say(f"  {'ok  ' if ok else 'FAIL'} do_migrate_pt: カーネルのコピー → 標準配置 (--no-push)")
    bad += not ok

    # --- 旧配置の NHD へ v64 以降のカーネルを配らない (Opus M2) ---
    q = tmp / "guard.nhd"
    total_q, _ = make_legacy_nhd(pc98pt, q)
    checks = [
        ("旧配置 + v64 → 断る", _silent_err(nhd.legacy_pt_guard, str(q), 64) is False),
        ("旧配置 + v63 → 通す", _silent_err(nhd.legacy_pt_guard, str(q), 63) is True),
    ]
    nhd.NHD_LOCAL = str(q)
    copied = []
    real_copy2 = nhd.shutil.copy2
    nhd.shutil.copy2 = lambda *a, **k: copied.append(a)
    nhd.write_pull_stamp = lambda *a, **k: None     # 門を抜けた場合も例外にしない
    try:
        dep = _silent_err(real_deploy, False)
    finally:
        nhd.shutil.copy2 = real_copy2
    checks.append(("do_deploy が旧配置 + このツリー (v64) で断る (NP21/W へ写さない)",
                   dep is False and not copied))
    _silent(nhd.update_partition_table, str(q))
    checks.append(("標準配置 + v64 → 通す", _silent_err(nhd.legacy_pt_guard, str(q), 64) is True))
    junk = tmp / "junk.nhd"
    junk.write_bytes(bytes(4096))
    checks.append(("NHD でない → 通す (判定できない)", _silent_err(nhd.legacy_pt_guard, str(junk), 64) is True))
    checks.append(("ファイルが無い → 通す (取り込みの後にもう一度見る)",
                   _silent_err(nhd.legacy_pt_guard, str(tmp / "absent.nhd"), 64) is True))
    # 旧配置 + 別の区画 → migrate-pt は断る (項目 2 個) が、門は**断る** (ラリー 2 の 1)
    q2 = tmp / "guard2.nhd"
    make_legacy_nhd(pc98pt, q2, extra_entry=True)
    checks.append(("旧配置 + 別の区画 (自動の移行はできない) → 断る",
                   _silent_err(nhd.legacy_pt_guard, str(q2), 64) is False))
    # 旧配置で FS が区画より大きい (移行はできない) → 断る
    q3 = tmp / "guard3.nhd"
    make_legacy_nhd(pc98pt, q3, end_cyl=200)
    checks.append(("旧配置 + 移行できない FS → 断る",
                   _silent_err(nhd.legacy_pt_guard, str(q3), 64) is False))
    # OS32 の項目が標準でも旧配置でも読めない → 断る
    q4 = tmp / "guard4.nhd"
    make_legacy_nhd(pc98pt, q4, end_cyl=400)
    checks.append(("OS32 の項目が壊れている → 断る",
                   _silent_err(nhd.legacy_pt_guard, str(q4), 64) is False))
    # OS32 の項目が無い (FAT だけ) → 断る
    q5 = tmp / "guard5.nhd"
    make_legacy_nhd(pc98pt, q5, sid=0xA1)
    checks.append(("OS32 の項目が無い → 断る",
                   _silent_err(nhd.legacy_pt_guard, str(q5), 64) is False))
    # 取り込みの後・マウントの前にも門が掛かる (ラリー 2 の 2)。NHD_LOCAL が無い
    # 状態から ensure_local_nhd が旧配置の NHD を「取り込む」。losetup は呼ばれないこと
    pulled = tmp / "pulled.nhd"
    remote = tmp / "remote_legacy.nhd"        # 取り込み元 (subprocess を差し替える前に作る)
    make_legacy_nhd(pc98pt, remote)
    nhd.NHD_LOCAL = str(pulled)
    real_eln, real_run, real_mounted = nhd.ensure_local_nhd, nhd.subprocess.run, nhd.is_mounted
    runs = []

    def fake_pull():
        if not pulled.exists():
            shutil.copyfile(remote, pulled)
        return True
    nhd.ensure_local_nhd = fake_pull
    nhd.is_mounted = lambda: False
    nhd.subprocess.run = lambda *a, **k: runs.append(a) or subprocess.CompletedProcess(a, 1, "", "")
    try:
        r1 = _silent_err(nhd.legacy_pt_guard)          # sync-from-hostdrv の最初の門 (無いので通る)
        r2 = _silent_err(nhd.ensure_mounted_for_kernel)
        checks.append(("取り込み前の門は通り、取り込み後・losetup の前で断る",
                       r1 is True and r2 is False and not runs))
        # 既にマウント済みでも断る
        nhd.is_mounted = lambda: True
        checks.append(("マウント済みの旧配置 NHD → 断る",
                       _silent_err(nhd.ensure_mounted_for_kernel) is False))
        # migrate-pt のカーネルの写し (通常の ensure_mounted) は門を通らない
        nhd.is_mounted = lambda: False
        del runs[:]
        _silent_err(nhd.ensure_mounted)
        checks.append(("通常の ensure_mounted は門を通らず losetup へ進む", bool(runs)))
    finally:
        nhd.ensure_local_nhd, nhd.subprocess.run, nhd.is_mounted = real_eln, real_run, real_mounted
    for name, ok in checks:
        say(f"  {'ok  ' if ok else 'FAIL'} legacy_pt_guard: {name}")
        bad += not ok

    # --- update_partition_table (init の書き手) は標準配置を書く ---
    q = tmp / "init.nhd"
    total_q, _ = make_legacy_nhd(pc98pt, q)
    st, ln = nhd.update_partition_table(str(q)) if quiet is False else _silent(
        nhd.update_partition_table, str(q))
    with open(q, "rb") as f:
        f.seek(1024)
        sect = f.read(512)
    ok = pc98pt.find_os32(sect, 8, 17, total_q) == (0, 1632, total_q - 1632)
    say(f"  {'ok  ' if ok else 'FAIL'} update_partition_table: 標準配置 (1632, {total_q - 1632})")
    bad += not ok
    return bad


def _silent_err(fn, *a):
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fn(*a)


def _silent(fn, *a):
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a)


def py_main(tools_dir, c_exe, quiet):
    """子プロセスの入口 (変異した tools/ を読み込むため)。"""
    pc98pt, nhd = load_tools(tools_dir)
    bad = py_cross_check(pc98pt, c_exe)
    with tempfile.TemporaryDirectory(prefix="os32-hdd1-py-") as tmp:
        bad += py_migrate_cases(pc98pt, nhd, tmp, quiet=quiet)
    return bad


def run_py(tools_dir, c_exe, quiet=False):
    r = subprocess.run([sys.executable, "-B", __file__, "--py", str(tools_dir), str(c_exe)]
                       + (["--quiet"] if quiet else []),
                       cwd=ROOT, capture_output=quiet, text=True)
    return r.returncode


# ======================================================================== #
#  i386-elf で新しいソースを -Werror                                        #
# ======================================================================== #

def build_target(tmp):
    base = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
            "-fno-pie", "-fno-stack-protector", "-nostdlib", "-mno-red-zone",
            "-fcommon", "-O2", "-Wall", "-Werror", "-Wdeclaration-after-statement",
            "-D__KERNEL_BUILD__", "-I" + str(ROOT), "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "drivers"), "-I" + str(ROOT / "fs"),
            "-I" + str(ROOT / "sdk/include/os32"), "-I" + str(ROOT / "arch/x86"),
            "-I" + str(ROOT / "platform/pc98")]
    for rel in ("drivers/pc98pt.c", "drivers/ide_addr.c", "fs/ext2_layout.c",
                "userland/shell/hdprep_plan.c"):
        out = pathlib.Path(tmp) / (pathlib.Path(rel).stem + ".o")
        run([*base, "-c", str(ROOT / rel), "-o", str(out)], check=True)
    # ローダと同じフラグ (-Os、-Iboot の型) でも共有部が通る
    run(["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
         "-fno-pie", "-fno-stack-protector", "-nostdlib", "-mno-red-zone", "-Os",
         "-Wall", "-Werror", "-fcommon", "-Iboot", "-Idrivers", "-c",
         str(ROOT / "boot/boot_main.c"), "-o", str(pathlib.Path(tmp) / "bm.o")], check=True)
    print("TARGET i386-elf GNU89 -Werror COMPILE PASS", flush=True)


# ======================================================================== #
#  変異                                                                     #
# ======================================================================== #

MIRROR = ["drivers/ide.h", "drivers/pc98pt.c", "drivers/pc98pt.h", "drivers/ide_addr.c",
          "drivers/ide_addr.h", "fs/ext2_layout.c", "fs/ext2_layout.h",
          "userland/shell/hdprep_plan.c", "userland/shell/hdprep_plan.h",
          "fs/ext2_super.c", "fs/ext2_fmt.c", "fs/ext2_inode.c", "fs/ext2_dir.c",
          "fs/ext2_file.c"]
PY_MIRROR = ["tools/pc98pt.py", "tools/nhd_deploy.py", "tools/deploy_protect.py",
             "tools/deploy_manifests.py"]


def _tally(counts, status, why):
    counts[status] = counts.get(status, 0) + 1
    print(f"MUTATION {status}: {why}", flush=True)


# 恒等変異 (対照、Opus ラリー 2 の a)。変異させるファイルごとに**意味を変えない**
# 書き換えを 1 本当て、SURVIVED になることを確かめる。RED / ERROR になるなら
# 変異の土台 (写し・インクルード・読み込み) が壊れていて、ほかの RED は信用できない。
C_IDENTITY_TAIL = "\n/* identity mutation (control) */\n"
PY_IDENTITY_TAIL = "\n# identity mutation (control)\n"


def _controls(mutations):
    seen = []
    for m in mutations:
        if m[0] not in seen:
            seen.append(m[0])
    return seen


def mutate_c(counts):
    """C の変異。**ビルドが通って**試験が落ちたものだけを RED に数える。
    ビルドが通らない変異は ERROR (何も確かめていない — Opus M1)。"""
    plan = [(rel, None, None, "対照 (恒等): " + rel) for rel in _controls(C_MUTATIONS)]
    plan += list(C_MUTATIONS)
    for rel, before, after, why in plan:
        with tempfile.TemporaryDirectory(prefix="os32-hdd1-mut-") as tmp:
            troot = pathlib.Path(tmp) / "root"
            for m in MIRROR:
                dst = troot / m
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / m, dst)
            path = troot / rel
            text = path.read_text(encoding="utf-8")
            control = before is None
            if control:
                text = text + C_IDENTITY_TAIL
            elif before not in text:
                _tally(counts, "NOT_APPLIED", why)
                continue
            else:
                text = text.replace(before, after, 1)
            path.write_text(text, encoding="utf-8")
            exe = build_pure(tmp, troot)
            pexe = build_part(tmp, troot)
            if exe is None or pexe is None:
                status = "ERROR"
            else:
                failed = run_cases(exe, PURE_CASES, quiet=True)
                failed += run_cases(pexe, PART_CASES, quiet=True)
                status = "RED" if failed else "SURVIVED"
            if control:
                _tally(counts, "CONTROL_OK" if status == "SURVIVED" else "CONTROL_BAD",
                       why + " → " + status)
            else:
                _tally(counts, status, why + (" (ビルドが通らない)" if status == "ERROR" else ""))


def mutate_py(c_exe, counts):
    """Python の変異。試験の失敗 (終了 1) だけを RED、読み込み・例外 (終了 3) は ERROR。"""
    plan = [(rel, None, None, "対照 (恒等): " + rel) for rel in _controls(PY_MUTATIONS)]
    plan += list(PY_MUTATIONS)
    for rel, before, after, why in plan:
        with tempfile.TemporaryDirectory(prefix="os32-hdd1-pymut-") as tmp:
            tdir = pathlib.Path(tmp) / "tools"
            tdir.mkdir()
            for m in PY_MIRROR:
                shutil.copy2(ROOT / m, tdir / pathlib.Path(m).name)
            # nhd_deploy.tree_kapi_version は PROJ_DIR/sdk/... を読む (旧配置の門、M2)
            hdr = pathlib.Path(tmp) / "sdk/include/os32"
            hdr.mkdir(parents=True)
            shutil.copy2(ROOT / "sdk/include/os32/os32_kapi_shared.h", hdr)
            path = tdir / pathlib.Path(rel).name
            text = path.read_text(encoding="utf-8")
            control = before is None
            if control:
                text = text + PY_IDENTITY_TAIL
            elif before not in text:
                _tally(counts, "NOT_APPLIED", why)
                continue
            else:
                text = text.replace(before, after, 1)
            path.write_text(text, encoding="utf-8")
            rc = run_py(tdir, c_exe, quiet=True)
            status = "RED" if rc == 1 else ("SURVIVED" if rc == 0 else "ERROR")
            if control:
                _tally(counts, "CONTROL_OK" if status == "SURVIVED" else "CONTROL_BAD",
                       why + " → " + status)
            else:
                _tally(counts, status, why)


def main(argv):
    if argv and argv[0] == "--py":
        quiet = "--quiet" in argv
        try:
            return 1 if py_main(pathlib.Path(argv[1]), pathlib.Path(argv[2]), quiet) else 0
        except Exception:  # noqa: BLE001 — 変異で壊れた読み込み・例外は ERROR (3)
            import traceback
            if not quiet:
                traceback.print_exc()
            return 3

    failed = 0
    with tempfile.TemporaryDirectory(prefix="os32-hdd1-") as tmp:
        exe = build_pure(tmp)
        if exe is None:
            print("BUILD FAIL (hdd_stage1_host.c)")
            return 1
        print("HOST GNU89 -Werror COMPILE PASS (hdd_stage1_host.c)", flush=True)
        failed += run_cases(exe, PURE_CASES)
        pexe = build_part(tmp)
        if pexe is None:
            print("BUILD FAIL (ext2_part_host.c)")
            return 1
        print("HOST ILP32 GNU89 -Werror COMPILE PASS (ext2_part_host.c)", flush=True)
        failed += run_cases(pexe, PART_CASES)
        for size in FSCK_SIZES:
            failed += fsck_image(pexe, 2016, size, tmp)
        rc = run_py(ROOT / "tools", exe)
        print(f"EXIT python(pc98pt.py / migrate-pt)={rc}", flush=True)
        failed += rc != 0
        total = len(PURE_CASES) + len(PART_CASES) + len(FSCK_SIZES) + 1
        print(f"SUMMARY {total - failed}/{total} PASS", flush=True)

        if "--target" in argv:
            build_target(tmp)

        if "--mutate" in argv and failed == 0:
            n = len(C_MUTATIONS) + len(PY_MUTATIONS)
            counts = {}
            mutate_c(counts)
            mutate_py(exe, counts)
            red = counts.get("RED", 0)
            nctl = len(_controls(C_MUTATIONS)) + len(_controls(PY_MUTATIONS))
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
