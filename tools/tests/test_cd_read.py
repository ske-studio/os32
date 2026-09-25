"""CD の読み (drivers/atapi.c + fs/iso9660.c + userland/lib/rt/pkg.c) のホスト試験。

記録: tools/tests/cd_read_tdd.md
症状: 実機 PC-9821Ra266 の cdinst (Full) が 10 分を超えても NORMAL.PKG の途中
      (実効 20KB/s 未満)。READ(10) を 1 セクタずつ出し、VFS の区切り (4KB) ごとに
      根のディレクトリを読み直していた。

実物を 1 行も写さずに #include して回す。
  cd_read_host.c  atapi.c + iso9660.c。ポートは tools/tests/atapi_hostshim/io.h が
                  ATAPI デバイスの模型へ回す (READ(10) の数・セクタ数を数える)
  cd_pkg_host.c   pkg.c の pkg_extract (cdinst の無圧縮の展開) の区切り方。
                  記録した読みの列を cd_read_host の replay で再生する

  python3 -B tools/tests/test_cd_read.py            # ホストで全ケース
  python3 -B tools/tests/test_cd_read.py --target   # + i386-elf で通す
  python3 -B tools/tests/test_cd_read.py --mutate   # 否定側 (変異が RED になるか)

ホストは -fsanitize=address,undefined で組む (窓からはみ出す変異を確実に落とす)。
ATAPI_READ_MAX_SECTORS を 32 にした版 (1 回の転送が 64KB = byte count limit の
16 ビットを超える形) も同じケースで回す。

変異の判定: RED = どれかのケースが落ちた / ERROR = コンパイルできなかった
(**RED に数えない**) / SURVIVED = 全ケースが通った (見逃し)。対照 (何も変え
ない変異) は SURVIVED でなければならない。
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
READ_HARNESS = "tools/tests/cd_read_host.c"
PKG_HARNESS = "tools/tests/cd_pkg_host.c"
SHIM = "tools/tests/atapi_hostshim"
TREE_FILES = ["drivers/atapi.c", "drivers/atapi.h", "fs/iso9660.c", "fs/iso9660.h",
              "userland/lib/rt/pkg.c", "userland/lib/rt/pkg.h",
              READ_HARNESS, PKG_HARNESS, SHIM + "/io.h"]

READ_CASES = ["stream_4k", "stream_odd", "stream_32k", "read_file", "multi_fallback",
              "short_transfer", "bad_sector", "lru_order", "ua_mount", "multi_drq",
              "path_cache", "dir_lru", "list_reentrant", "unit_attention",
              "idle_rule", "no_window"]
# N = 32 の版で回すもの (1 回 64KB)
WIDE_CASES = ["stream_4k", "stream_32k", "read_file", "multi_drq", "multi_fallback"]
PKG_CASES = ["aligned_chunks", "nomem", "open_fail_frees"]

COMMON = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
          "-Wdeclaration-after-statement", "-D__cdecl=",
          "-Wno-unused-function", "-Wno-unused-parameter"]
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]


def read_includes(tree):
    return ["-I" + str(tree / SHIM), "-I" + str(tree / "drivers"), "-I" + str(tree / "fs"),
            "-I" + str(tree)] + ["-I" + str(ROOT / p) for p in
                                 ("include", "drivers", "fs", "lib", "kernel",
                                  "sdk/include/os32")] + ["-I" + str(ROOT)]


def pkg_includes(tree):
    return ["-I" + str(tree), "-I" + str(tree / "userland/lib/rt")] + [
        "-I" + str(ROOT / p) for p in ("sdk/include", "sdk/include/os32", "userland/lib")]


def build(tmp, tree):
    """3 本の実行ファイルを組む。コンパイルできなければ CalledProcessError。"""
    tmp = pathlib.Path(tmp)
    exes = {"read": tmp / "cd-read", "wide": tmp / "cd-read-wide", "pkg": tmp / "cd-pkg"}
    common = dict(cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run(["gcc", *COMMON, *SAN, *read_includes(tree),
                    str(tree / READ_HARNESS), "-o", str(exes["read"])], **common)
    subprocess.run(["gcc", *COMMON, *SAN, "-DATAPI_READ_MAX_SECTORS=32",
                    *read_includes(tree), str(tree / READ_HARNESS),
                    "-o", str(exes["wide"])], **common)
    subprocess.run(["gcc", *COMMON, *SAN, *pkg_includes(tree),
                    str(tree / PKG_HARNESS), "-o", str(exes["pkg"])], **common)
    return exes


def run1(exe, case, env=None, quiet=False):
    e = dict(os.environ)
    e["ASAN_OPTIONS"] = "detect_leaks=0"
    if env:
        e.update(env)
    try:
        r = subprocess.run([str(exe), case], cwd=ROOT, timeout=60, env=e,
                           capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return 124, ""
    if not quiet:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
    return r.returncode, r.stdout


def run_all(exes, tmp, quiet=False):
    """全ケース。落ちた数を返す。"""
    failed = 0
    total = 0

    def one(label, exe, case, env=None):
        nonlocal failed, total
        rc, out = run1(exe, case, env, quiet)
        total += 1
        if not quiet:
            print(f"EXIT {label}={rc}", flush=True)
        failed += rc != 0
        return rc, out

    for c in READ_CASES:
        one(c, exes["read"], c)
    for c in WIDE_CASES:
        one("wide:" + c, exes["wide"], c)
    trace = pathlib.Path(tmp) / "pkg.trace"
    pkg_len = None
    for c in PKG_CASES:
        rc, out = one("pkg:" + c, exes["pkg"], c, {"CD_PKG_TRACE": str(trace)})
        m = re.search(r"pkg_len=(\d+)", out)
        if m:
            pkg_len = int(m.group(1))
    # pkg.c の読み方をそのまま実物の iso9660 + atapi で再生する
    if pkg_len is None or not trace.exists():
        total += 1
        failed += 1
        if not quiet:
            print("EXIT replay=FAIL (pkg の記録が無い)")
    else:
        secs = (pkg_len + 2047) // 2048
        mx = -(-secs // 16) + 3
        one("replay", exes["read"], "replay",
            {"CD_READ_TRACE": str(trace), "CD_READ_BIGSIZE": str(pkg_len),
             "CD_READ_TRACE_MAX": str(mx)})
    if not quiet:
        print(f"SUMMARY {total - failed}/{total} PASS", flush=True)
    return failed


# 否定側。実装を 1 か所だけ壊して RED になることを見る。
# (ファイル, パターン, 置換, 説明)。最後の 1 本は対照 (何も変えない)。
MUTATIONS = [
    # --- drivers/atapi.c
    ("drivers/atapi.c",
     r"u32 n = \(count < ATAPI_READ_MAX_SECTORS\) \? count : ATAPI_READ_MAX_SECTORS;",
     "u32 n = 1;",
     "READ(10) を 1 セクタずつ出す (直す前の姿)"),
    ("drivers/atapi.c",
     r"if \(ret != ATAPI_OK && n > 1\) \{",
     "if (0) {",
     "複数セクタが落ちても 1 セクタずつ読み直さない"),
    ("drivers/atapi.c",
     r"                ret = atapi_read10\(lba \+ i, 1, p \+ i \* ATAPI_SECTOR_SIZE\);\n                if \(ret != ATAPI_OK\) break;",
     "                ret = atapi_read10(lba + i, 1, p + i * ATAPI_SECTOR_SIZE);",
     "1 セクタずつの読み直しが落ちても先へ進む (最後の結果だけ見る)"),
    ("drivers/atapi.c",
     r"            if \(got != n \* ATAPI_SECTOR_SIZE\) return ATAPI_ERR_IO;\n",
     "",
     "渡されたバイト数を確かめない (足りない転送を成功にする)"),
    ("drivers/atapi.c",
     r"for \(i = 0; i < IDE_SEL_SETTLE; i\+\+\) \(void\)inp\(IDE_ALT_STATUS\);\n}\n\n/\* 12",
     "for (i = 0; i < 0; i++) (void)inp(IDE_ALT_STATUS);\n}\n\n/* 12",
     "DRQ のブロックの後に 400ns 置かない (古い DRQ を次のブロックと取り違える)"),
    ("drivers/atapi.c",
     r"bcl = \(buf_size < ATAPI_PIO_BCL_MAX\) \? buf_size : ATAPI_PIO_BCL_MAX;",
     "bcl = buf_size;",
     "byte count limit を上限で抑えない (64KB で 16 ビットに入らない)"),
    ("drivers/atapi.c",
     r"        s_media_gen\+\+;\n",
     "",
     "UNIT ATTENTION で媒体の世代を進めない"),
    ("drivers/atapi.c",
     r"if \(s_last_sense != ATAPI_SK_UNIT_ATTENTION\) break;",
     "break;",
     "UNIT ATTENTION の後に出し直さない (入れ替え直後の mount が落ちる)"),
    ("drivers/atapi.c",
     r"cdb\[7\] = \(u8\)\(n >> 8\);\n        cdb\[8\] = \(u8\)\(n & 0xFF\);",
     "cdb[7] = 0;\n        cdb[8] = 1;",
     "CDB の転送長を 1 のまま (旧 CDB)"),
    # --- fs/iso9660.c: 覚えたパス
    ("fs/iso9660.c",
     r"if \(ctx->pc_valid && kstrcmp\(ctx->pc_path, path\) == 0\) \{",
     "if (0) {",
     "覚えたパスを使わない (区切りごとに根からたどる)"),
    ("fs/iso9660.c",
     r"if \(ctx->pc_valid && kstrcmp\(ctx->pc_path, path\) == 0\) \{",
     "if (ctx->pc_valid) {",
     "覚えたパスの名前を比べない (別のファイルを返す)"),
    # --- 捨てる合図
    ("fs/iso9660.c",
     r"    ctx->pc_valid = 0;\n    ctx->ra_valid = 0;\n",
     "    ctx->ra_valid = 0;\n",
     "捨てるときに覚えたパスを残す"),
    ("fs/iso9660.c",
     r"    ctx->pc_valid = 0;\n    ctx->ra_valid = 0;\n",
     "    ctx->pc_valid = 0;\n",
     "捨てるときに先読みの窓を残す"),
    ("fs/iso9660.c",
     r"    for \(i = 0; i < ISO_SCACHE_SLOTS; i\+\+\) ctx->scache\[i\]\.valid = 0;\n    ctx->stats\.drops\+\+;",
     "    (void)i;\n    ctx->stats.drops++;",
     "捨てるときにセクタの LRU を残す"),
    ("fs/iso9660.c",
     r"if \(gen != ctx->media_gen\n        \|\| ",
     "if (",
     "媒体の世代を見ない"),
    ("fs/iso9660.c",
     r"\(u32\)\(now - ctx->last_tick\) > \(u32\)ISO_IDLE_TICKS",
     "(u32)(now - ctx->last_tick) >= (u32)ISO_IDLE_TICKS",
     "2 秒規則の境目を 1 tick 早める"),
    ("fs/iso9660.c",
     r"\|\| \(ctx->touched && \(u32\)\(now - ctx->last_tick\) > \(u32\)ISO_IDLE_TICKS\)",
     "|| ((void)now, 0)",
     "2 秒規則を当てない"),
    ("fs/iso9660.c",
     r"    ctx->media_gen = gen;\n}",
     "    ctx->media_gen = gen;\n    ctx->last_tick = now;\n}",
     "キャッシュの当たりでも時刻を進める (読み続ける限り捨てない)"),
    ("fs/iso9660.c",
     r"if \(atapi_media_gen\(\) == gen0\) \{",
     "if (atapi_media_gen() == gen0 || 1) {",
     "読みの途中で世代が進んでも読み直さない"),
    # --- セクタの LRU
    ("fs/iso9660.c",
     r"if \(victim < 0 \|\| s->used < ctx->scache\[victim\]\.used\) victim = i;",
     "if (victim < 0) victim = i;",
     "LRU でない (いつも最初のスロットを追い出す)"),
    ("fs/iso9660.c",
     r"if \(s->valid && s->lba == lba\) \{",
     "if (s->valid) {",
     "LRU が LBA を見ない"),
    ("fs/iso9660.c",
     r"            kmemcpy\(sector, sec, ISO_SECTOR_SIZE\);\n            cur = sector;",
     "            cur = sec;",
     "list_dir がキャッシュを指したまま cb を呼ぶ (cb の読みで入れ替わる)"),
    ("fs/iso9660.c",
     r"        if \(sect_off == 0 \|\| !sector\) \{\n            sector = iso_get_sector\(ctx, sect_lba\);",
     "        if (!sector) {\n            sector = iso_get_sector(ctx, sect_lba);",
     "ディレクトリの 2 セクタ目を読まない"),
    # --- 先読みの窓・まとめ読み
    ("fs/iso9660.c",
     r"            if \(n > ISO_RA_SECTORS\) n = ISO_RA_SECTORS;\n",
     "",
     "窓の大きさで抑えない (窓からはみ出して書く)"),
    ("fs/iso9660.c",
     r"            n = file_secs - sect_idx;\n",
     "            n = ISO_RA_SECTORS;\n",
     "窓をファイルの外 (媒体の終わりの先) まで広げる"),
    ("fs/iso9660.c",
     r"&& left >= \(ctx->ra_buf \? ISO_RA_BYTES : \(u32\)ISO_SECTOR_SIZE\)\) \{",
     "&& left >= ISO_SECTOR_SIZE) {",
     "窓より小さい揃った読みも直接読む (4KB ごとに READ(10))"),
    ("fs/iso9660.c",
     r"            ctx->ra_valid = 0;\n            if \(iso_dev_read\(ctx, lba, n, ctx->ra_buf\) != 0\)",
     "            if (iso_dev_read(ctx, lba, n, ctx->ra_buf) != 0)",
     "窓を読む前に捨てない (失敗の後に上書きされかけの窓を当てる)"),
    ("fs/iso9660.c",
     r"            u32 chunk = ctx->ra_count \* ISO_SECTOR_SIZE - pos;",
     "            u32 chunk = ctx->ra_count * ISO_SECTOR_SIZE;",
     "窓の中の位置を引かずに写す長さを決める"),
    ("fs/iso9660.c",
     r"                u32 secs = \(u32\)\(\(\(file_size - 1\) / ISO_SECTOR_SIZE\) \+ 1\);",
     "                u32 secs = (u32)(file_size / ISO_SECTOR_SIZE);",
     "ファイルのセクタ数を切り捨てる (末尾の半端なセクタで窓が 0 本)"),
    # --- userland/lib/rt/pkg.c
    ("userland/lib/rt/pkg.c",
     r"u32 room = cap - \(pos % PKG_STREAM_ALIGN\);",
     "u32 room = cap;",
     "区切りをセクタ境界に揃えない"),
    ("userland/lib/rt/pkg.c",
     r"                pos \+= \(u32\)r;\n",
     "",
     "読み位置を進めない"),
    ("userland/lib/rt/pkg.c",
     r"u8 \*tbuf = \(u8 \*\)api->mem_alloc\(PKG_STREAM_CHUNK\);",
     "u8 *tbuf = (u8 *)0;",
     "大きなバッファを取らない (4KB のまま)"),
    ("userland/lib/rt/pkg.c",
     r"        if \(tbuf != small\) api->mem_free\(tbuf\);\n",
     "",
     "取ったバッファを返さない"),
    # 対照: 何も変えない。SURVIVED でなければ試験が不安定 (偽の RED)。
    ("fs/iso9660.c", r"(#include \"iso9660\.h\")", r"\1", "対照 (何も変えない)"),
]
CONTROL = len(MUTATIONS)


def make_tree(tmp, rel=None, text=None):
    tree = pathlib.Path(tmp) / "tree"
    for f in TREE_FILES:
        dst = tree / f
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text((ROOT / f).read_text(encoding="utf-8"), encoding="utf-8")
    if rel is not None:
        (tree / rel).write_text(text, encoding="utf-8")
    return tree


def mutate(verbose_fail=False):
    red = survived = error = 0
    control_ok = False
    only = {int(x) for x in os.environ.get("CD_MUT_ONLY", "").split(",") if x}
    for idx, (rel, pat, rep, desc) in enumerate(MUTATIONS, 1):
        if only and idx not in only:
            continue
        src = (ROOT / rel).read_text(encoding="utf-8")
        new, n = re.subn(pat, rep, src, count=1)
        if n != 1:
            print(f"MUT {idx:2d} NOMATCH  {desc}", flush=True)
            error += 1
            continue
        with tempfile.TemporaryDirectory(prefix="os32-cdmut-") as tmp:
            tree = make_tree(tmp, rel, new)
            try:
                exes = build(tmp, tree)
            except subprocess.CalledProcessError:
                print(f"MUT {idx:2d} ERROR    {desc} (コンパイル不可、数えない)", flush=True)
                error += 1
                continue
            failed = run_all(exes, tmp, quiet=True)
        if idx == CONTROL:
            control_ok = failed == 0
            print(f"MUT {idx:2d} {'CONTROL' if control_ok else 'CTRL-RED'} {desc}", flush=True)
            continue
        if failed:
            red += 1
            print(f"MUT {idx:2d} RED      {desc}", flush=True)
        else:
            survived += 1
            print(f"MUT {idx:2d} SURVIVED {desc}", flush=True)
    real = CONTROL - 1
    print(f"MUTATION {red}/{real} RED, {survived} SURVIVED, {error} ERROR/NOMATCH, "
          f"control={'OK' if control_ok else 'BROKEN'}", flush=True)
    return survived == 0 and error == 0 and control_ok


def build_target(tmp):
    """カーネル / ユーザーランドと同じ i386-elf で通す。"""
    base = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386", "-ffreestanding",
            "-fno-pie", "-fno-stack-protector", "-nostdlib", "-mno-red-zone", "-fcommon",
            "-fsigned-char", "-fno-short-enums", "-O2", "-Wall", "-Werror",
            "-Wdeclaration-after-statement"]
    kinc = ["-D__KERNEL_BUILD__"] + ["-I" + str(ROOT / p) for p in
                                     (".", "include", "arch/x86", "platform/pc98",
                                      "sdk/include", "sdk/include/os32", "drivers", "fs",
                                      "kernel", "lib")]
    for rel in ("drivers/atapi.c", "fs/iso9660.c"):
        subprocess.run([*base, *kinc, "-c", str(ROOT / rel), "-o",
                        str(pathlib.Path(tmp) / (rel.replace("/", "_") + ".o"))],
                       cwd=ROOT, check=True)
    uinc = ["-I" + str(ROOT / p) for p in ("sdk/include", "sdk/include/os32",
                                           "userland/lib", "userland/lib/rt")]
    subprocess.run([*base, *uinc, "-c", str(ROOT / "userland/lib/rt/pkg.c"), "-o",
                    str(pathlib.Path(tmp) / "pkg.o")], cwd=ROOT, check=True)
    print("TARGET i386-elf GNU89 -Werror PASS (atapi.c / iso9660.c / pkg.c)", flush=True)


def main():
    args = sys.argv[1:]
    do_target = "--target" in args
    do_mutate = "--mutate" in args
    cases = [a for a in args if not a.startswith("--")]
    ok = True
    with tempfile.TemporaryDirectory(prefix="os32-cdread-") as tmp:
        tree = make_tree(tmp)
        try:
            exes = build(tmp, tree)
        except subprocess.CalledProcessError as e:
            sys.stderr.write(e.stderr.decode(errors="replace"))
            print("HOST COMPILE FAIL")
            sys.exit(1)
        print("HOST COMPILE GNU89 -Werror (asan/ubsan) PASS", flush=True)
        if cases:
            failed = 0
            for c in cases:
                label, _, name = c.rpartition(":")
                exe = exes["wide"] if label == "wide" else exes["pkg"] if label == "pkg" \
                    else exes["read"]
                rc, _ = run1(exe, name)
                print(f"EXIT {c}={rc}", flush=True)
                failed += rc != 0
            ok = failed == 0
        else:
            ok = run_all(exes, tmp) == 0
        if do_target:
            build_target(tmp)
    if do_mutate and not cases:
        ok = mutate() and ok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
