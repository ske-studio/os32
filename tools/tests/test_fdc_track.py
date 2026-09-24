"""FD のトラック単位の読み出し (drivers/fdc_track.c + drivers/fdc.c) のホスト試験。

記録: tools/tests/fdc_track_tdd.md
票  : docs/tasks/realhw/TASK_FDC_REALHW.md (実機 PC-9821Ra266 の FD 起動が遅い件)

実物の drivers/fdc_track.c / fdc_decide.c / fdc.c を 1 行も写さずに #include
して回す。fdc.c のポートは tools/tests/fdc_hostshim/io.h が試験側の µPD765A
の模型へ回し、tick_count は「読むたびに 1 進む」時計に差し替える。

  python3 -B tools/tests/test_fdc_track.py            # ホストで全ケース
  python3 -B tools/tests/test_fdc_track.py --target   # + i386-elf で通す
  python3 -B tools/tests/test_fdc_track.py --mutate   # 否定側 (変異が RED になるか)

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
HARNESS = ROOT / "tools/tests/fdc_track_host.c"
SHIM = ROOT / "tools/tests/fdc_hostshim"
SOURCES = ["drivers/fdc_track.c", "drivers/fdc.c", "drivers/fdc_decide.c",
           "fs/fatfs/diskio.c"]

CASES = ["split_2hd", "split_144", "readahead_count1", "cross_boundary",
         "fallback_single", "single_fail", "cache_rules", "oversize_geom",
         "timeout_math", "fdc_multi_cmd", "fdc_seek_skip", "fdc_forget_rules",
         "fdc_end_to_end"]

# fdc.c の fdc_motor_off() は元から未使用の static (test_fdc_seek.py と同じ)。
# tick_count の差し替えは「volatile u32 * を返す関数」の宣言になるので
# -Wignored-qualifiers は出ないが、念のため戻り値の修飾は許す。
FLAGS = ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement", "-D__cdecl=",
         "-Wno-unused-function", "-Wno-unused-parameter",
         "-Dtick_count=(*fdc_fake_tick_ptr())"]


def includes(first=None):
    dirs = ([first] if first else []) + [SHIM] + [
        ROOT / p for p in ("include", "drivers", "lib", "sdk/include/os32")]
    return ["-I" + str(d) for d in dirs]


# 否定側。実装を 1 か所だけ壊して RED になることを見る。
# (ファイル, パターン, 置換, 説明)。最後の 1 本は対照 (何も変えない)。
MUTATIONS = [
    ("drivers/fdc_track.c",
     r"in_track = \(count < left\) \? count : left;",
     "in_track = (count < left + 1) ? count : left + 1;",
     "トラックの境目を 1 つ越えて切る (EOT を越えたコマンドを出す)"),
    ("drivers/fdc_track.c",
     r"left = spt - \(lba % spt\);",
     "left = spt * heads - (lba % (spt * heads));",
     "ヘッドの境目で切らない (MT 無しでヘッド 1 まで読もうとする)"),
    ("drivers/fdc_track.c",
     r"if \(run->sect < c->first\) return 0;",
     "",
     "持っていない手前のセクタを当てる"),
    ("drivers/fdc_track.c",
     r"if \(c->geom != g\) return 0;",
     "",
     "ジオメトリが変わっても中身を当てる"),
    ("drivers/fdc_track.c",
     r"if \(c->drv != drv \|\| ",
     "if (",
     "別ドライブに中身を当てる"),
    ("drivers/fdc_track.c",
     r"            c->valid = 0;\n            if \(eot > 0 &&",
     "            if (eot > 0 &&",
     "まとめ読みの前に捨てない (失敗しても古い中身が残る)"),
    ("drivers/fdc_track.c",
     r"if \(fdc_track_read_singly\(ops, drv, g, &run, dst\) != 0\) \{\n                    return -1;",
     "if (1) {\n                    return -1;",
     "まとめ読みが失敗したら読み直さずに失敗にする"),
    ("drivers/fdc_track.c",
     r"    return \(int\)g->spt;\n#else",
     "    return run->sect + run->count - 1;\n#else",
     "先読みしない (count=1 の連続が 1 セクタ 1 コマンドのまま)"),
    ("drivers/fdc_track.c",
     r"if \(eot > want_last && fdc_track_is_bad\(c, drv, g, &run\)\) \{",
     "if (0) {",
     "先読みが落ちたトラックでも先読みを続ける (毎回失敗 + 1 セクタずつ)"),
    ("drivers/fdc.c",
     r"        fdc_abort_transfer\(\);\n        \(void\)fdc_recalibrate\(drv\);",
     "        fdc_abort_transfer();",
     "まとめ読みの失敗の後に RECALIBRATE しない"),
    ("drivers/fdc.c",
     r"if \(drv >= 0 && drv < FDC_MAX_DRIVES && s_known_cyl\[drv\] == cyl\) \{\n        return 0;\n    \}",
     "",
     "同じシリンダでもシークする (直す前の姿)"),
    # まとめ読みが DMA を積んだ後で落ちたときの「覚えた値を捨てる」は
    # fdc_abort_transfer (リセット) の fdc_forget_all と、続く RECALIBRATE の
    # 出す前の破棄の**二重**。片方だけ消す変異は等価なので、両方消す。
    ("drivers/fdc.c",
     [(r"    /\* 2\. FDC をリセットして実行フェーズを畳む。覚えているシリンダも捨てる。 \*/\n    fdc_forget_all\(\);",
       "    /* 2. */"),
      (r"    /\* ヘッドが動く。通るまでは知らないことにする。 \*/\n    fdc_note_drive\(drv\);\n    fdc_forget_cyl\(drv\);",
       "    fdc_note_drive(drv);")],
     None,
     "リセットと RECALIBRATE で覚えた値を捨てない (RECALIBRATE が落ちても古い値を信じる)"),
    # シークの失敗も二重 (fdc_seek の出す前の破棄 + まとめ読みの DMA 前の
    # 失敗経路の破棄)。DMA を積む前なのでリセットは通らない。両方消す。
    ("drivers/fdc.c",
     [(r"    fdc_forget_cyl\(drv\);\n\n    fdc_irq_fired = 0;\n    if \(fdc_send_byte\(FDC_CMD_SEEK\)",
       "\n    fdc_irq_fired = 0;\n    if (fdc_send_byte(FDC_CMD_SEEK)"),
      (r"    \} else \{\n        fdc_forget_cyl\(drv\);\n    \}\n    s_multi_fail\+\+;",
       "    }\n    s_multi_fail++;")],
     None,
     "シークの失敗の後も古い値を信じる"),
    ("drivers/fdc.c",
     r"    if \(drv != s_last_drv\) fdc_forget_all\(\);",
     "",
     "ドライブの切り替えで捨てない"),
    ("drivers/fdc.c",
     r"    s_geom\[drv\] = g;\n    /\* メディアが変わった。覚えているシリンダは信じない。 \*/\n    fdc_forget_cyl\(drv\);",
     "    s_geom[drv] = g;",
     "メディアの変更で捨てない"),
    ("drivers/fdc.c",
     r"if \(fdc_send_byte\(\(u8\)eot\) != 0\) goto fail;",
     "if (fdc_send_byte((u8)sect) != 0) goto fail;",
     "EOT を sect のまま出す (1 セクタしか読まない)"),
    ("drivers/fdc.c",
     r"if \(fdc_send_byte\(FDC_OPT_MF \| FDC_CMD_READ_DATA\) != 0\) goto fail;",
     "if (fdc_send_byte(FDC_OPT_MT | FDC_OPT_MF | FDC_CMD_READ_DATA) != 0) goto fail;",
     "MT を立てる"),
    ("drivers/fdc.c",
     r"if \(eot > \(int\)g->spt\) return -2;",
     "",
     "トラックをまたぐ引数を断らない"),
    ("drivers/fdc_decide.c",
     r"xfer = \(\(u32\)count \* rot_ticks \+ \(u32\)spt - 1\) / \(u32\)spt;",
     "xfer = ((u32)count * rot_ticks) / (u32)spt;",
     "時間上限の回転数を切り捨てる"),
    ("drivers/fdc_decide.c",
     r"return \(t < floor_ticks\) \? floor_ticks : t;",
     "return t;",
     "時間上限が単発の値を下回る"),
    # 対照: 何も変えない。SURVIVED でなければ試験が不安定 (偽の RED)。
    ("drivers/fdc_track.c", r"(#include \"fdc_track.h\")", r"\1",
     "対照 (何も変えない)"),
]
CONTROL = len(MUTATIONS)


def host_build(tmp, mutated=None):
    """ハーネスをコンパイルして実行ファイルのパスを返す。
    mutated = (相対パス, 本文) なら、変異させた写しの木から組む。"""
    tmpd = pathlib.Path(tmp)
    exe = tmpd / "fdc-track-host"
    if mutated is None:
        cmd = ["gcc", *FLAGS, *includes(), str(HARNESS), "-o", str(exe)]
    else:
        rel, text = mutated
        tree = tmpd / "tree"
        for sub in ("drivers", "tools/tests"):
            (tree / sub).mkdir(parents=True, exist_ok=True)
        for name in ("fdc_track.c", "fdc_track.h", "fdc.c", "fdc.h",
                     "fdc_decide.c", "fdc_decide.h"):
            (tree / "drivers" / name).write_text(
                (ROOT / "drivers" / name).read_text(encoding="utf-8"),
                encoding="utf-8")
        (tree / rel).write_text(text, encoding="utf-8")
        harness = tree / "tools/tests/fdc_track_host.c"
        harness.write_text(HARNESS.read_text(encoding="utf-8"), encoding="utf-8")
        cmd = ["gcc", *FLAGS, *includes(tree / "drivers"), str(harness),
               "-o", str(exe)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return exe


def run_cases(exe, cases, quiet=False):
    failed = 0
    for case in cases:
        rc = subprocess.run([str(exe), case], cwd=ROOT, timeout=60,
                            stderr=subprocess.DEVNULL if quiet else None).returncode
        if not quiet:
            print(f"EXIT {case}={rc}", flush=True)
        failed += rc != 0
    if not quiet:
        print(f"SUMMARY {len(cases) - failed}/{len(cases)} PASS", flush=True)
    return failed


def build_target(tmp):
    """カーネルと同じ i386-elf で通す。"""
    for rel in SOURCES:
        extra = ["-Wno-unused-function"] if rel == "drivers/fdc.c" else []
        cmd = ["i386-elf-gcc", "-std=gnu89", "-m32", "-march=i386",
               "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-nostdlib",
               "-mno-red-zone", "-fcommon", "-fsigned-char", "-fno-short-enums",
               "-O2", "-Wall", "-Werror", "-Wdeclaration-after-statement",
               "-D__KERNEL_BUILD__",
               "-I" + str(ROOT), "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "arch/x86"), "-I" + str(ROOT / "platform/pc98"),
               "-I" + str(ROOT / "sdk/include/os32"), "-I" + str(ROOT / "drivers"),
               "-I" + str(ROOT / "lib"), "-I" + str(ROOT / "fs/fatfs"),
               "-I" + str(ROOT / "kernel"),
               *extra, "-c", str(ROOT / rel),
               "-o", str(pathlib.Path(tmp) / (rel.replace("/", "_") + ".o"))]
        subprocess.run(cmd, cwd=ROOT, check=True)
    print("TARGET i386-elf GNU89 -Werror PASS", flush=True)


def mutate(tmp):
    """実装を 1 か所ずつ壊して RED になることを見る。対照は SURVIVED。"""
    bad = 0
    tally = {"RED": 0, "ERROR": 0, "SURVIVED": 0}
    for i, (rel, pattern, repl, why) in enumerate(MUTATIONS, 1):
        original = (ROOT / rel).read_text(encoding="utf-8")
        pairs = pattern if isinstance(pattern, list) else [(pattern, repl)]
        mutated, n = original, 1
        for pat, rp in pairs:
            mutated, k = re.subn(pat, rp, mutated, count=1)
            n = n and k == 1
        if n != 1:
            print(f"MUTATION {i} NOT APPLICABLE: {why}", flush=True)
            bad += 1
            continue
        sub = pathlib.Path(tmp) / f"m{i}"
        sub.mkdir()
        try:
            exe = host_build(sub, (rel, mutated))
        except subprocess.CalledProcessError:
            status = "ERROR"
            hits = 0
        else:
            hits = run_cases(exe, CASES, quiet=True)
            status = "RED" if hits else "SURVIVED"
        tally[status] += 1
        if i == CONTROL:
            ok = status == "SURVIVED" and mutated == original
        else:
            ok = status == "RED"
        mark = "" if ok else "  **想定外**"
        print(f"MUTATION {i} {status} ({hits} 件): {why}{mark}", flush=True)
        bad += not ok
    print("MUTATION TALLY RED={RED} ERROR={ERROR} SURVIVED={SURVIVED}"
          .format(**tally), flush=True)
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    tmp_root = os.environ.get("TMPDIR") or None
    with tempfile.TemporaryDirectory(prefix="os32-fdc-track-", dir=tmp_root) as tmp:
        exe = host_build(tmp)
        print("HOST GNU89 -Werror compile PASS (real fdc_track.c / fdc.c)",
              flush=True)
        if "--target" in args:
            build_target(tmp)
        rc = run_cases(exe, [a for a in args if not a.startswith("--")] or CASES)
        if "--mutate" in args:
            rc += mutate(tmp)
        sys.exit(bool(rc))
