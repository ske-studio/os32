"""起動ログ (kernel/bootlog.c) のホスト試験。

記録: tools/tests/bootlog_tdd.md

実物の kernel/bootlog.c を 1 行も写さずに tools/tests/bootlog_host.c が
#include して回す。書き出しの手順は偽の VFS (呼ばれた順と段ごとの失敗注入)。
ホスト側だけ -DBOOTLOG_NO_IRQ_LOCK (CPL=3 では cli/popfl を実行できない)。
クロス側は付けないので、include/io.h の irq_save/irq_restore を使う本番の
形もカーネルと同じ i386-elf -Werror で通す。

  python3 -B tools/tests/test_bootlog.py            # ホストで全ケース + 静的検査
  python3 -B tools/tests/test_bootlog.py --target   # + i386-elf -Werror
  python3 -B tools/tests/test_bootlog.py --mutate   # 否定側 (変異が RED か)

変異は「組めたもの」だけを数える (コンパイルが通らない変異は RED に数えず
SKIP と出す)。恒等の対照 (何も変えない置換) は GREEN でなければならない —
RED なら変異の仕組みそのものが壊れている。
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/tests/bootlog_host.c"
SRC = ROOT / "kernel/bootlog.c"

CASES = ["collect", "overflow", "utf8_latch", "stop", "header", "compose",
         "plan", "save", "save_mkdir"]

FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement", "-D__cdecl=", "-DBOOTLOG_NO_IRQ_LOCK"]
INCLUDES = ["-I" + str(ROOT / p) for p in ("include", "sdk/include/os32")]

TARGET_FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
                "-fno-stack-protector", "-nostdlib", "-mno-red-zone", "-fcommon",
                "-fsigned-char", "-fno-short-enums", "-O2", "-Wall", "-Wextra",
                "-Werror", "-Wdeclaration-after-statement", "-D__KERNEL_BUILD__"]
TARGET_INCLUDES = ["-I" + str(ROOT / p) for p in
                   (".", "include", "arch/x86", "platform/pc98", "sdk/include",
                    "sdk/include/os32", "kernel", "drivers", "fs", "exec", "lib",
                    "kapi")]

# 否定側。実装を 1 か所だけ壊して RED になることを見る。
# (パターン, 置換, 説明)。先頭は恒等の対照で、GREEN でなければならない。
IDENTITY = (r"g_bl_len \+= n;", "g_bl_len += n;", "恒等の対照 (何も変えない)")
MUTATIONS = [
    (r"    if \(g_bl_full\) \{", "    if (0) {",
     "一度あふれても後から来た短い行を積む (途中が抜けた本文になる)"),
    (r"        while \(n > 0 && \(\(u8\)buf\[n\] & 0xC0u\) == 0x80u\) n--;\n", "",
     "あふれの境目で UTF-8 の文字を割る"),
    (r"g_bl_dropped \+= len - n;", "g_bl_dropped += 1;",
     "境目で捨てたバイト数を数え違える"),
    (r"        g_bl_dropped \+= len;\n", "",
     "あふれた後に捨てた分を数えない"),
    (r"    if \(n > room\) \{", "    if (n > room + 1) {",
     "容量を 1 バイト踏み越える (末尾行の置き場を壊す)"),
    (r"    g_bl_stopped = 1;", "    g_bl_stopped = 0;",
     "止めても積み続ける (書き出しの後の出力が混ざる)"),
    (r"    if \(g_bl_len > 0 && g_bl_buf\[end - 1\] != '\\n'\) g_bl_buf\[end\+\+\] = '\\n';",
     "", "本文が行の途中で終わると末尾行がくっつく"),
    (r"    buf\[pos\+\+\] = '\\n';", "    buf[pos++] = ' ';",
     "ヘッダが改行で終わらない"),
    (r"for \(sh = 28;", "for (sh = 24;",
     "Image CRC の上位桁を落とす (ver の %08x と突き合わせられない)"),
    (r"    if \(kind == BOOTLOG_FS_FAT\)  return SYS_BOOTLOG_OLD_83;",
     "    if (kind == BOOTLOG_FS_FAT)  return SYS_BOOTLOG_OLD;",
     "FAT でも boot.log.1 (8.3 でない名前) を使う"),
    (r"    return BOOTLOG_FS_SKIP;    /\*", "    return BOOTLOG_FS_EXT2;    /*",
     "HostDrv / iso9660 ルートにも書く"),
    (r"    if \(bl_streq\(fstype, SYS_BOOTLOG_FS_FAT\)\)  return BOOTLOG_FS_FAT;\n", "",
     "FAT (FD 起動) では書かない"),
    (r"    rc = ops->mkdir\(SYS_BOOTLOG_VAR_DIR\);\n    if \(rc != 0 && rc != OS32_ERR_EXIST\)",
     "    rc = ops->mkdir(SYS_BOOTLOG_VAR_DIR);\n    if (rc != 0)",
     "既にある /var を失敗と読む (2 回目の起動から書けない)"),
    (r"        return BOOTLOG_ST_MKDIR_VAR;\n", "        (void)0;\n",
     "/var が作れなくても続ける"),
    (r"        return BOOTLOG_ST_MKDIR_LOG;\n", "        (void)0;\n",
     "/var/log が作れなくても続ける"),
    (r"    rc = ops->rm\(old\);\n", "    rc = 0;\n",
     "前回分を消さずに付け替える (FAT の rename が EXIST で落ちる)"),
    (r"        rotate_ok = 0;\n", "        rotate_ok = 1;\n",
     "前回分を消せなくても付け替える"),
    (r"            bl_fail\(&first, &first_rc, BOOTLOG_ST_ROTATE, rc\);",
     "            { if (fail_rc) *fail_rc = rc; return BOOTLOG_ST_ROTATE; }",
     "付け替えに失敗したら今回分を書かずに止める"),
    (r"    if \(rc < 0 \|\| \(u32\)rc != len\) \{", "    if (rc < 0) {",
     "書いた量の不足を見逃す"),
    (r"        rc = ops->sync\(\);\n", "        rc = 0;\n",
     "sync しない (電源断で消える)"),
    (r"    if \(\*first == BOOTLOG_ST_OK\) \{", "    if (1) {",
     "後の失敗で最初の失敗を上書きする"),
    (r"        rc = ops->rename\(SYS_BOOTLOG_FILE, old\);",
     "        rc = ops->rename(old, SYS_BOOTLOG_FILE);",
     "付け替えの向きが逆"),
]


def host_build(tmp, source_text=None, tag="host"):
    tmp = pathlib.Path(tmp)
    exe = tmp / ("bootlog-" + tag)
    inc = []
    if source_text is not None:
        d = tmp / ("src-" + tag)
        d.mkdir(exist_ok=True)
        (d / "bootlog.c").write_text(source_text, encoding="utf-8")
        inc = ["-I" + str(d)]
    else:
        inc = ["-I" + str(ROOT / "kernel")]
    subprocess.run(["gcc", *FLAGS, *inc, *INCLUDES, str(HARNESS), "-o", str(exe)],
                   cwd=ROOT, check=True,
                   stderr=None if source_text is None else subprocess.DEVNULL)
    return exe


def run_cases(exe, cases, quiet=False):
    failed = 0
    for case in cases:
        r = subprocess.run([str(exe), case], cwd=ROOT,
                           stdout=subprocess.DEVNULL if quiet else None)
        if not quiet:
            print(f"EXIT {case}={r.returncode}", flush=True)
        failed += r.returncode != 0
    if not quiet:
        print(f"SUMMARY {len(cases) - failed}/{len(cases)} PASS", flush=True)
    return failed


def build_target(tmp):
    for rel in ("kernel/bootlog.c", "kernel/bootlog_save.c"):
        subprocess.run(["i386-elf-gcc", *TARGET_FLAGS, *TARGET_INCLUDES, "-c",
                        str(ROOT / rel),
                        "-o", str(pathlib.Path(tmp) / (pathlib.Path(rel).stem + ".o"))],
                       cwd=ROOT, check=True)
    print("TARGET i386-elf GNU89 -Wextra -Werror PASS (bootlog.c, bootlog_save.c)",
          flush=True)


def check_wiring():
    """結線の静的検査: console の入口 4 か所、kernel.c の呼ぶ位置、止める順。"""
    bad = 0
    con = (ROOT / "kernel/console.c").read_text(encoding="utf-8")
    sink = con.count("con_sink_push_print(")
    blog = con.count("bootlog_push(")
    print(f"WIRING console.c con_sink x{sink} bootlog x{blog}", flush=True)
    if blog != 4 or sink != blog:
        print("  FAIL console の入口 (con_sink を積む所) と同じ 4 か所で積む", flush=True)
        bad += 1

    k = (ROOT / "kernel/kernel.c").read_text(encoding="utf-8")
    n = k.count("bootlog_save();")
    root = k.find('vfs_mount("/", root_dev, root_fs)')
    call = k.find("bootlog_save();")
    shell = k.find("rc = exec_run(cur_shell);")
    selftest = k.find("kselftest_run();")
    print(f"WIRING kernel.c bootlog_save x{n}", flush=True)
    if n != 1 or not (0 <= root < selftest < call < shell):
        print("  FAIL ルートのマウントと自己試験の後、シェルの exec の前に 1 回だけ",
              flush=True)
        bad += 1

    sv = (ROOT / "kernel/bootlog_save.c").read_text(encoding="utf-8")
    body = sv[sv.find("void bootlog_save(void)"):]
    stop = body.find("bootlog_stop();")
    first_io = min(i for i in (body.find("vfs_fstype"), body.find("kprintf"))
                   if i >= 0)
    if stop < 0 or stop > first_io:
        print("  FAIL bootlog_save は最初に止める (自分の kprintf を溜めない)",
              flush=True)
        bad += 1
    return bad


def mutate(tmp):
    original = SRC.read_text(encoding="utf-8")
    bad = 0
    counted = red = skipped = 0

    def apply(i, pattern, repl, why, want_red):
        nonlocal bad, counted, red, skipped
        mutated, n = re.subn(pattern, lambda m: repl, original, count=1)
        if n != 1:
            print(f"MUTATION {i} NOT APPLICABLE: {why}", flush=True)
            bad += 1
            return
        try:
            exe = host_build(tmp, mutated, tag=f"m{i}")
        except subprocess.CalledProcessError:
            print(f"MUTATION {i} SKIP (組めない、数えない): {why}", flush=True)
            skipped += 1
            return
        hits = run_cases(exe, CASES, quiet=True)
        if want_red:
            counted += 1
            red += bool(hits)
            status = "RED" if hits else "**GREEN (見逃し)**"
            bad += not hits
        else:
            status = "GREEN (対照)" if not hits else "**RED (対照が落ちた)**"
            bad += bool(hits)
        print(f"MUTATION {i} {status} ({hits} 件): {why}", flush=True)

    apply(0, *IDENTITY, want_red=False)
    for i, (pattern, repl, why) in enumerate(MUTATIONS, 1):
        apply(i, pattern, repl, why, want_red=True)
    print(f"MUTATIONS {red}/{counted} RED (組めずに除外 {skipped})", flush=True)
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="os32-bootlog-") as tmp:
        exe = host_build(tmp)
        print("HOST GNU89 -Werror compile PASS (real kernel/bootlog.c)", flush=True)
        if "--target" in args:
            build_target(tmp)
        rc = run_cases(exe, [a for a in args if not a.startswith("--")] or CASES)
        rc += check_wiring()
        if "--mutate" in args:
            rc += mutate(tmp)
        print("RESULT " + ("PASS" if rc == 0 else "FAIL"), flush=True)
        sys.exit(bool(rc))
