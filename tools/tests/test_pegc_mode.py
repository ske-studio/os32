"""PEGC 640x480 へ入る / 戻る OUT 列と GDC の FIFO 待ち (gfx/backend_pegc.c)。

記録: tools/tests/pegc_mode_tdd.md
票:   docs/tasks/realhw/TASK_PEGC480_REALHW.md (§2 H2・H3・H5、§4)

tools/tests/pegc_mode_host.c が実物の gfx/backend_pegc.c を 1 行も写さずに
#include し、ポート I/O だけを偽物 (tools/tests/pegc_hostshim/io.h) にして
ILP32 で回す。送る OUT 列 (ポート・値・順序) を期待列と比べ、GDC への書き込みの
直前に FIFO のステータスを見ていること・待ちに上限があることを確かめる。

  python3 -B tools/tests/test_pegc_mode.py [--mutate]

--mutate は否定側。順序・値・FIFO 待ち・上限を 1 か所ずつ壊した版を写しの木で
組み、この試験が RED になることを見る。make・エミュレータ・配備には触れない。
"""
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutpar                                                   # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FLAGS = ["-std=gnu89", "-m32", "-march=i386", "-ffreestanding", "-fno-pie",
         "-fno-stack-protector", "-Wall", "-Wextra", "-Werror",
         "-Wdeclaration-after-statement"]
# build/config.mk の INC_GFX と同じ探索先。io.h だけを偽物に差し替える。
TARGET_INCLUDES = ["-I" + str(ROOT / p)
                   for p in ("include", "arch/x86", "platform/pc98",
                             "sdk/include/os32",
                             "gfx", "drivers", "fs", "lib", "kernel")]
SHIM = "-I" + str(ROOT / "tools/tests/pegc_hostshim")
SRC = ROOT / "tools/tests/pegc_mode_host.c"
MUT_TARGET = "gfx/backend_pegc.c"
HDR = "include/pegc.h"


def host_cmd(exe):
    return ["gcc", *FLAGS, "-Wno-unused-function", "-O0", "-nostdlib",
            "-static", "-no-pie", SHIM, *TARGET_INCLUDES, str(SRC),
            "-o", str(exe)]


def target_cmd(obj):
    return ["i386-elf-gcc", *FLAGS, "-O2", *TARGET_INCLUDES,
            "-c", str(ROOT / MUT_TARGET), "-o", str(obj)]


MUTATIONS = [
    # (名前, 対象ファイル, 元, 変異後)
    # --- FIFO 待ち (H3) ---
    ("no_wait_cmd", MUT_TARGET,
     "    (void)gdc_wait_status(g->stat, GDC_STAT_FEMP, GDC_STAT_FEMP);\n", ""),
    ("no_wait_prm", MUT_TARGET,
     "        (void)gdc_wait_status(g->stat, GDC_STAT_FFUL, 0);\n", ""),
    ("prm_waits_full", MUT_TARGET,
     "gdc_wait_status(g->stat, GDC_STAT_FFUL, 0);",
     "gdc_wait_status(g->stat, GDC_STAT_FFUL, GDC_STAT_FFUL);"),
    ("cmd_waits_not_full", MUT_TARGET,
     "gdc_wait_status(g->stat, GDC_STAT_FEMP, GDC_STAT_FEMP);",
     "gdc_wait_status(g->stat, GDC_STAT_FFUL, 0);"),
    ("wait_wrong_port", MUT_TARGET,
     "static const PegcGdcPorts s_gdc_text = { GDC_TEXT_CMD, GDC_TEXT_PARAM, GDC_TEXT_STAT };",
     "static const PegcGdcPorts s_gdc_text = { GDC_TEXT_CMD, GDC_TEXT_PARAM, GDC_GFX_STAT };"),
    ("poll_bound_off_by_one", MUT_TARGET,
     "for (i = 0; i < PEGC_GDC_FIFO_POLLS; i++) {",
     "for (i = 0; i <= PEGC_GDC_FIFO_POLLS; i++) {"),
    ("no_timeout_count", MUT_TARGET,
     "    pegc_gdc_fifo_timeouts++;\n", ""),
    ("no_delay_between_polls", MUT_TARGET,
     "        cpu_delay_us(PEGC_GDC_FIFO_POLL_US);\n", ""),
    ("stop_bypasses_fifo", MUT_TARGET,
     "    gdc_send(&s_gdc_gfx, GDC_CMD_STOP, (const u8 *)0, 0);\n",
     "    _out(GDC_GFX_CMD, GDC_CMD_STOP);\n"),
    ("start_bypasses_fifo", MUT_TARGET,
     "    gdc_send(&s_gdc_text, GDC_CMD_START, (const u8 *)0, 0);\n",
     "    _out(GDC_TEXT_CMD, GDC_CMD_START);\n"),
    # --- 順序 (H2) ---
    ("clock_after_sync", MUT_TARGET,
     "    if (t->set_clock) {\n"
     "        _out(MODE_FF2_PORT, t->clk1);\n"
     "        _out(MODE_FF2_PORT, t->clk2);\n"
     "        gfx_counters.io_accesses += 2;\n"
     "    }\n\n"
     "    gdc_send(&s_gdc_text, GDC_CMD_SYNC, t->msync, PEGC_GDC_SYNC_LEN);\n"
     "    gdc_send(&s_gdc_gfx,  GDC_CMD_SYNC, t->ssync, PEGC_GDC_SYNC_LEN);\n",
     "    gdc_send(&s_gdc_text, GDC_CMD_SYNC, t->msync, PEGC_GDC_SYNC_LEN);\n"
     "    gdc_send(&s_gdc_gfx,  GDC_CMD_SYNC, t->ssync, PEGC_GDC_SYNC_LEN);\n"
     "    if (t->set_clock) {\n"
     "        _out(MODE_FF2_PORT, t->clk1);\n"
     "        _out(MODE_FF2_PORT, t->clk2);\n"
     "        gfx_counters.io_accesses += 2;\n"
     "    }\n"),
    ("hsync_after_sync", MUT_TARGET,
     "    _out(PEGC_HSYNC_PORT, t->hsync & PEGC_HSYNC_MASK);\n"
     "    gfx_counters.io_accesses++;\n",
     "    gfx_counters.io_accesses++;\n"),
    ("ff2_ext_before_timing", MUT_TARGET,
     "    pegc_apply_timing(&s_timing_480);\n"
     "    ff2_locked_write(PEGC_FF2_VRAM_800L);\n"
     "    ff2_locked_write(PEGC_FF2_EXT_GFX);\n",
     "    ff2_locked_write(PEGC_FF2_VRAM_800L);\n"
     "    ff2_locked_write(PEGC_FF2_EXT_GFX);\n"
     "    pegc_apply_timing(&s_timing_480);\n"),
    # --- 値 (H5) ---
    ("no_clk2", MUT_TARGET,
     "        _out(MODE_FF2_PORT, t->clk2);\n", ""),
    ("no_pitch_480", MUT_TARGET,
     "    s_msync_480, s_ssync_480, PEGC_GDC_PITCH_480, s_scroll_480\n",
     "    s_msync_480, s_ssync_480, 40, s_scroll_480\n"),
    ("pitch_skipped", MUT_TARGET,
     "    if (t->set_clock) {\n        pitch[0] = t->pitch;",
     "    if (0 && t->set_clock) {\n        pitch[0] = t->pitch;"),
    ("clock_not_set_on_enter", MUT_TARGET,
     "    PEGC_HSYNC_31KHZ, 1, PEGC_GDC_CLK1_480, PEGC_GDC_CLK2_480,",
     "    PEGC_HSYNC_31KHZ, 0, PEGC_GDC_CLK1_480, PEGC_GDC_CLK2_480,"),
    ("clk_2m5_values_swapped", HDR,
     "#define PEGC_FF2_GDC_CLK1_2M5  0x82", "#define PEGC_FF2_GDC_CLK1_2M5  0x84"),
    ("pitch5m_either_clock", MUT_TARGET,
     "if (s_boot_clk1 == 1 && s_boot_clk2 == 1) return PEGC_GDC_PITCH_400_5M;",
     "if (s_boot_clk1 == 1 || s_boot_clk2 == 1) return PEGC_GDC_PITCH_400_5M;"),
    ("restore_clock_ignores_boot", MUT_TARGET,
     "    t.clk1 = (s_boot_clk1 == 1) ? PEGC_FF2_GDC_CLK1_5M : PEGC_FF2_GDC_CLK1_2M5;",
     "    t.clk1 = PEGC_FF2_GDC_CLK1_2M5;"),
    ("restore_touches_clock_unrecorded", MUT_TARGET,
     "    t.set_clock = known;",
     "    t.set_clock = 1;"),
    ("clk2_read_from_bit0", MUT_TARGET,
     "    s_boot_clk2 = (clk & PEGC_STAT_RD_GDCCLK2) ? 1 : 0;",
     "    s_boot_clk2 = (clk & PEGC_STAT_BIT) ? 1 : 0;"),
    ("clk_read_without_select", MUT_TARGET,
     "    _out(PEGC_STAT_PORT, PEGC_STAT_SEL_GDCCLK1);\n    clk =",
     "    clk ="),
    ("hsync_upper_bits", MUT_TARGET,
     "    _out(PEGC_HSYNC_PORT, t->hsync & PEGC_HSYNC_MASK);",
     "    _out(PEGC_HSYNC_PORT, t->hsync | (u8)(s_boot_hsync_raw & 0x80));"),
    # --- 戻りの SYNC / SCROLL をクロックとの組で選ぶ (Codex レビュー P2) ---
    ("restore_ssync_always_5m", MUT_TARGET,
     "        t.ssync = ss5m ? s_ssync_400_5m : s_ssync_400_2m5;",
     "        t.ssync = s_ssync_400_5m;"),
    ("restore_ssync31k_always_5m", MUT_TARGET,
     "        t.ssync = ss5m ? s_ssync_400_31k_5m : s_ssync_400_31k_2m5;",
     "        t.ssync = s_ssync_400_31k_5m;"),
    ("restore_ssync_always_2m5", MUT_TARGET,
     "        t.ssync = ss5m ? s_ssync_400_5m : s_ssync_400_2m5;",
     "        t.ssync = s_ssync_400_2m5;"),
    ("restore_scroll_always_im0", MUT_TARGET,
     "    t.scroll = (known && is5m) ? s_scroll_400_5m : s_scroll_400_2m5;",
     "    t.scroll = s_scroll_400_2m5;"),
    ("restore_scroll_always_im1", MUT_TARGET,
     "    t.scroll = (known && is5m) ? s_scroll_400_5m : s_scroll_400_2m5;",
     "    t.scroll = s_scroll_400_5m;"),
    ("hdr_ssync_2m5_cr_4e", HDR,
     "#define PEGC_GDC_SSYNC_400_2M5     { 0x02, 0x26,",
     "#define PEGC_GDC_SSYNC_400_2M5     { 0x02, 0x4E,"),
    ("hdr_ssync_2m5_hbp", HDR,
     "{ 0x02, 0x26, 0x03, 0x11, 0x83, 0x07, 0x90, 0x65 }",
     "{ 0x02, 0x26, 0x03, 0x11, 0x87, 0x07, 0x90, 0x65 }"),
    ("hdr_scroll_480_no_im", HDR,
     "#define PEGC_GDC_SCROLL_480    { 0x00, 0x00, 0x00, 0x40 }",
     "#define PEGC_GDC_SCROLL_480    { 0x00, 0x00, 0x00, 0x00 }"),
    ("hdr_scroll_400_5m_no_im", HDR,
     "#define PEGC_GDC_SCROLL_400_5M     { 0x00, 0x00, 0x00, 0x40 }",
     "#define PEGC_GDC_SCROLL_400_5M     { 0x00, 0x00, 0x00, 0x00 }"),
    ("restore_31k_uses_24k_sync", MUT_TARGET,
     "        t.msync = s_msync_400_31k;", "        t.msync = s_msync_400;"),
]


def one_mutation(item):
    name, target, old, new = item
    original = (ROOT / target).read_text(encoding="utf-8")
    if name == "hsync_upper_bits":
        # 起動時の生の読み (81h など) を書き戻す誤り。値を持つ変数ごと足す。
        original_new = original.replace(
            "static int s_boot_hsync    = -1;",
            "static u8  s_boot_hsync_raw = 0;\nstatic int s_boot_hsync    = -1;", 1)
        original_new = original_new.replace(
            "    raw    = (u8)_in(PEGC_HSYNC_PORT);\n",
            "    raw    = (u8)_in(PEGC_HSYNC_PORT);\n    s_boot_hsync_raw = raw;\n", 1)
        if original_new == original:
            return "MUTATE %-34s SKIP (目印が見つからない)" % name, 1
        base = original_new
    else:
        base = original
    if old not in base:
        return "MUTATE %-34s SKIP (目印が見つからない)" % name, 1
    with tempfile.TemporaryDirectory(prefix="os32-pegc-mode-mut-") as td:
        exe = pathlib.Path(td) / ("mut-" + name)
        try:
            tree = mutpar.build_in_tree(
                ROOT, td, {target: base.replace(old, new, 1)},
                [host_cmd(exe)], capture_output=True)
        except subprocess.CalledProcessError:
            return "MUTATE %-34s RED (コンパイルが通らない)" % name, 0
        try:
            out = subprocess.run([str(exe)], cwd=str(tree), timeout=60,
                                 capture_output=True)
        except subprocess.TimeoutExpired:
            return "MUTATE %-34s RED (時間切れ)" % name, 0
    if out.returncode == 0:
        return ("MUTATE %-34s **GREEN のまま = 試験が規則を見ていない**"
                % name, 1)
    last = out.stdout.decode("utf-8", "replace").strip().splitlines()[-1:]
    return "MUTATE %-34s RED (期待どおり落ちた: %s)" % (
        name, last[0] if last else "rc=%d" % out.returncode), 0


if __name__ == "__main__":
    failed = 0
    with tempfile.TemporaryDirectory(prefix="os32-pegc-mode-") as tmp:
        tmp = pathlib.Path(tmp)
        exe = tmp / "pegc-mode"
        subprocess.run(host_cmd(exe), cwd=ROOT, check=True)
        print("HOST ILP32 GNU89 COMPILE PASS (real gfx/backend_pegc.c)",
              flush=True)
        rc = subprocess.run([str(exe)], cwd=ROOT, timeout=60).returncode
        print("EXIT pegc_mode_host=%d" % rc, flush=True)
        failed += rc != 0
        subprocess.run(target_cmd(tmp / "backend_pegc.o"), cwd=ROOT,
                       check=True)
        print("TARGET i386-elf GNU89 -Werror COMPILE PASS (backend_pegc.c)",
              flush=True)
        if "--mutate" in sys.argv:
            failed += mutpar.run_with_control(one_mutation, MUTATIONS,
                                              ("control", MUT_TARGET, "", ""))
    sys.exit(1 if failed else 0)
