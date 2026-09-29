/* =========================================================================
 *  PEGC_MODE_HOST.C — PEGC 640x480 へ入る / 戻る OUT 列と GDC の FIFO 待ち
 *
 *  実行: python3 -B tools/tests/test_pegc_mode.py [--mutate]
 *  記録: tools/tests/pegc_mode_tdd.md
 *  票:   docs/tasks/realhw/TASK_PEGC480_REALHW.md (§2 H2・H3・H5、§4)
 *        docs/tasks/gui/v21/TASK_PEGC_RA266_TIMING.md
 *
 *  実物の gfx/backend_pegc.c を 1 行も写さずに #include し、ポート I/O だけを
 *  偽物 (tools/tests/pegc_hostshim/io.h → pegc_shim_inp / pegc_shim_outp) に
 *  する。偽の I/O は IN / OUT を順に記録し、GDC のステータス (60h / A0h) は
 *  ケースごとに「FIFO が詰まっている回数」を決めて返す。
 *
 *  見るもの:
 *    - 480 ラインへ入る OUT 列 (ポート・値・順序) が期待どおり — 09A8h →
 *      6Ah クロック (83h, 85h) → SYNC ×2 → PITCH → SCROLL → 68h → START ×2 →
 *      6Ah 69h / 21h。値は include/pegc.h §10 から組む (差し替えても直さない)。
 *    - 戻る列が起動時のクロック (09A0h で読んだ値) と対称。PITCH は 40 / 80。
 *      記録が無ければクロックと PITCH に触らない (従来の列)。
 *    - 09A8h へは bit1,0 しか書かない (起動時の読みが 81h でも)。
 *    - GDC への書き込みの直前に必ずステータスを読み、コマンドは FIFO EMPTY、
 *      パラメータは FIFO FULL でないことを確かめてから書く。
 *    - 待ちには上限がある (PEGC_GDC_FIFO_POLLS)。詰まったままでも終わり、
 *      打ち切りを数え、列そのものは変わらない。
 *    - 既定値が NP21/W の記録 (PITCH 80、6Ah 83h+85h) のまま (`defaults`)。
 *      **実機の記録で pegc.h §10 を差し替えたら、このケースだけ直す。**
 * ========================================================================= */
#include "types.h"

#include "../../gfx/backend_pegc.c"

/* ---- 出力と終了 (libc なし、ILP32 の int 0x80) ---- */
static void output(const char *s)
{
    unsigned int len = 0;
    while (s[len]) len++;
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(s), "d"(len) : "memory");
}
static void finish(int code) __attribute__((noreturn));
static void finish(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    for (;;) { }
}
static const char *s_case = "";
static void fail(const char *message, int line)
{
    char num[12];
    int i = 11;
    num[i] = 0;
    do { num[--i] = (char)('0' + line % 10); line /= 10; } while (line && i);
    output("ASSERT FAIL ["); output(s_case); output("] line ");
    output(num + i); output(": "); output(message); output("\n");
    finish(1);
}
#define CHECK(c) do { if (!(c)) fail(#c, __LINE__); } while (0)

/* ---- 偽の I/O ---- */
#define EV_MAX 262144  /* 詰まったまま (fifo_stuck) は 1 バイト 5000 回読む */
static u16 ev_port[EV_MAX];
static u8  ev_val[EV_MAX];
static u8  ev_out[EV_MAX];     /* 1 = OUT / 0 = IN */
static int ev_n;

static int fake_full_reads;    /* FIFO FULL を返す残り回数 (-1 = ずっと) */
static int fake_busy_reads;    /* FIFO EMPTY を返さない残り回数 (-1 = ずっと) */
static u8  fake_09a0_sel;
static u8  fake_09a0_clk;      /* 09A0h sel 09h の読み: bit0 = CLK1, bit1 = CLK2 */
static u8  fake_09a8_raw;
static u32 delay_calls;
static int fake_full_after_cmd; /* 次に GDC へコマンドを書いたら FULL をこの回数 */

static void ev_add(int out, unsigned int port, unsigned int val)
{
    if (ev_n >= EV_MAX) fail("event log overflow", __LINE__);
    ev_out[ev_n] = (u8)out;
    ev_port[ev_n] = (u16)port;
    ev_val[ev_n] = (u8)val;
    ev_n++;
}

static u8 gdc_status(void)
{
    u8 st = 0;
    if (fake_full_reads != 0) {
        st |= GDC_STAT_FFUL;
        if (fake_full_reads > 0) fake_full_reads--;
    }
    if (fake_busy_reads != 0) {
        if (fake_busy_reads > 0) fake_busy_reads--;
    } else if (!(st & GDC_STAT_FFUL)) {
        st |= GDC_STAT_FEMP;
    }
    return st;
}

unsigned int pegc_shim_inp(unsigned int port)
{
    u8 v = 0;
    if (port == GDC_TEXT_STAT || port == GDC_GFX_STAT) {
        v = gdc_status();
    } else if (port == PEGC_STAT_PORT) {
        /* sel 09h: CLK1 は bit0、CLK2 は選択に関係なく bit1。sel 0Ah (拡張
         * モードか) は 0 = 標準 — リニア窓 (MMIO) へは書かせない。 */
        v = (u8)(fake_09a0_clk & PEGC_STAT_RD_GDCCLK2);
        if (fake_09a0_sel == PEGC_STAT_SEL_GDCCLK1) {
            v |= (u8)(fake_09a0_clk & PEGC_STAT_BIT);
        }
    } else if (port == PEGC_HSYNC_PORT) {
        v = fake_09a8_raw;
    }
    ev_add(0, port, v);
    return v;
}

void pegc_shim_outp(unsigned int port, unsigned int value)
{
    if (port == PEGC_STAT_PORT) fake_09a0_sel = (u8)value;
    if ((port == GDC_TEXT_CMD || port == GDC_GFX_CMD) && fake_full_after_cmd) {
        fake_full_reads = fake_full_after_cmd;
        fake_full_after_cmd = 0;
    }
    ev_add(1, port, value);
}

void cpu_delay_us(u32 us) { (void)us; delay_calls++; }

/* ---- 残りの依存 (この試験の経路では呼ばれない、または無害) ---- */
GfxCounters gfx_counters;
int gfx_current_height, gfx_flip_enabled, gfx_display_page;
void *kmemset(void *dst, int val, u32 n)
{
    u8 *p = (u8 *)dst;
    while (n--) *p++ = (u8)val;
    return dst;
}
void *kmemcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int val, u32 n) { return kmemset(dst, val, n); }
void kprintf(unsigned char attr, const char *fmt, ...) { (void)attr; (void)fmt; }
/* master AS ではない扱い = bios_flag は 0 を返す (ワークエリアを読まない) */
u32 paging_current_cr3(void) { return 1; }
u32 paging_kernel_pd_phys(void) { return 2; }
int paging_map_phys(u32 v, u32 p, u32 n, u32 f)
{ (void)v; (void)p; (void)n; (void)f; fail("paging_map_phys", __LINE__); return -1; }
void pgalloc_mark_used(u32 s, int n) { (void)s; (void)n; fail("pgalloc_mark_used", __LINE__); }
int pgalloc_range_has_ram(u32 f, u32 e) { (void)f; (void)e; return 1; }
u32 sys_usable_mem_end(void) { return 0; }
u32 sys_reserve_top(u32 b) { (void)b; return 0; }
void palette_init(void) { }
const PaletteEntry *palette_get_all(void) { return (const PaletteEntry *)0; }
void palette_shadow_set(int i, u8 r, u8 g, u8 b) { (void)i; (void)r; (void)g; (void)b; }

/* ---- 期待列 ---- */
static const u8 x_msync_480[] = PEGC_GDC_MSYNC_480;
static const u8 x_ssync_480[] = PEGC_GDC_SSYNC_480;
static const u8 x_scroll_480[] = PEGC_GDC_SCROLL_480;
static const u8 x_msync_400[] = PEGC_GDC_MSYNC_400;
static const u8 x_ssync_400_2m5[] = PEGC_GDC_SSYNC_400_2M5;
static const u8 x_ssync_400_5m[] = PEGC_GDC_SSYNC_400_5M;
static const u8 x_msync_400_31k[] = PEGC_GDC_MSYNC_400_31K;
static const u8 x_ssync_400_31k_2m5[] = PEGC_GDC_SSYNC_400_31K_2M5;
static const u8 x_ssync_400_31k_5m[] = PEGC_GDC_SSYNC_400_31K_5M;
static const u8 x_scroll_400_2m5[] = PEGC_GDC_SCROLL_400_2M5;
static const u8 x_scroll_400_5m[] = PEGC_GDC_SCROLL_400_5M;

static u16 xp[512];
static u8  xv[512];
static int xn;

static void x_reset(void) { xn = 0; }
static void x_out(unsigned int port, unsigned int val)
{
    xp[xn] = (u16)port;
    xv[xn] = (u8)val;
    xn++;
}
static void x_gdc(unsigned int cmd_port, unsigned int prm_port, u8 cmd,
                  const u8 *para, int n)
{
    int i;
    x_out(cmd_port, cmd);
    for (i = 0; i < n; i++) x_out(prm_port, para[i]);
}
static void x_ff2(u8 v)
{
    x_out(MODE_FF2_PORT, PEGC_FF2_UNLOCK);
    x_out(MODE_FF2_PORT, v);
    x_out(MODE_FF2_PORT, PEGC_FF2_LOCK);
}
/* 表示タイミング一式 (pegc_apply_timing の順序を**独立に**書き下したもの) */
static void x_timing(u8 hs, int clock, u8 clk1, u8 clk2, const u8 *ms,
                     const u8 *ss, u8 pitch, const u8 *sc)
{
    x_out(PEGC_HSYNC_PORT, hs);
    if (clock) {
        x_out(MODE_FF2_PORT, clk1);
        x_out(MODE_FF2_PORT, clk2);
    }
    x_gdc(GDC_TEXT_CMD, GDC_TEXT_PARAM, GDC_CMD_SYNC, ms, 8);
    x_gdc(GDC_GFX_CMD, GDC_GFX_PARAM, GDC_CMD_SYNC, ss, 8);
    if (clock) x_gdc(GDC_GFX_CMD, GDC_GFX_PARAM, GDC_CMD_PITCH, &pitch, 1);
    x_gdc(GDC_GFX_CMD, GDC_GFX_PARAM, GDC_CMD_SCROLL, sc, 4);
    x_out(MODE_FF1_PORT, MFF1_DISP_ON);
    x_out(GDC_TEXT_CMD, GDC_CMD_START);
    x_out(GDC_GFX_CMD, GDC_CMD_START);
}

/* 記録の OUT だけを期待列と突き合わせる */
static void check_outs(void)
{
    int i, k = 0;
    for (i = 0; i < ev_n; i++) {
        if (!ev_out[i]) continue;
        if (k >= xn) fail("more OUTs than expected", __LINE__);
        if (ev_port[i] != xp[k] || ev_val[i] != xv[k]) {
            fail("OUT differs from expected (port/value/order)", __LINE__);
        }
        k++;
    }
    if (k != xn) fail("fewer OUTs than expected", __LINE__);
}

static int is_gdc_cmd(u16 p) { return p == GDC_TEXT_CMD || p == GDC_GFX_CMD; }
static int is_gdc_prm(u16 p) { return p == GDC_TEXT_PARAM || p == GDC_GFX_PARAM; }
static u16 stat_of(u16 p)
{
    return (p == GDC_TEXT_CMD || p == GDC_TEXT_PARAM) ? GDC_TEXT_STAT : GDC_GFX_STAT;
}

/* GDC への書き込みの直前の事象は、その GDC のステータスの読みで、条件を
 * 満たしている (fifo_ok = 1) か、上限まで読み続けた (fifo_ok = 0) こと。 */
static void check_fifo_gate(int fifo_ok)
{
    int i, gdc_writes = 0;
    for (i = 0; i < ev_n; i++) {
        u16 p = ev_port[i];
        if (!ev_out[i] || !(is_gdc_cmd(p) || is_gdc_prm(p))) continue;
        gdc_writes++;
        CHECK(i > 0);
        CHECK(!ev_out[i - 1] && ev_port[i - 1] == stat_of(p));
        if (fifo_ok) {
            if (is_gdc_cmd(p)) CHECK(ev_val[i - 1] & GDC_STAT_FEMP);
            else               CHECK(!(ev_val[i - 1] & GDC_STAT_FFUL));
        } else {
            int j, reads = 0;
            for (j = i - 1; j >= 0 && !ev_out[j] && ev_port[j] == stat_of(p); j--) {
                reads++;
            }
            CHECK(reads == PEGC_GDC_FIFO_POLLS);
        }
    }
    CHECK(gdc_writes > 0);
}

static int count_gdc_writes(void)
{
    int i, n = 0;
    for (i = 0; i < ev_n; i++) {
        if (ev_out[i] && (is_gdc_cmd(ev_port[i]) || is_gdc_prm(ev_port[i]))) n++;
    }
    return n;
}

static void world_reset(void)
{
    ev_n = 0;
    fake_full_reads = 0;
    fake_busy_reads = 0;
    fake_09a0_sel = 0;
    fake_09a0_clk = 0;
    fake_09a8_raw = 0;
    delay_calls = 0;
    fake_full_after_cmd = 0;
    pegc_gdc_fifo_timeouts = 0;
    s_boot_recorded = 0;
    s_boot_hsync = -1;
    s_boot_clk1 = -1;
    s_boot_clk2 = -1;
    s_probed = 1;
    s_probe_ok = 1;
    s_active = 0;
}

/* 起動時の記録を偽の読みで走らせる (pegc_prepare の一部) */
static void boot_with(u8 hs_raw, u8 clk)
{
    fake_09a8_raw = hs_raw;
    fake_09a0_clk = clk;
    pegc_boot_sync_record();
    ev_n = 0;
}

/* ---- ケース ---- */

/* 1. 480 ラインへ入る列 (pegc_init のポート部分) */
static void case_enter_480(void)
{
    s_case = "enter_480";
    world_reset();
    boot_with(0x00, 0x00);
    pegc_enter_480_ports();

    x_reset();
    x_timing(PEGC_HSYNC_31KHZ, 1, PEGC_GDC_CLK1_480, PEGC_GDC_CLK2_480,
             x_msync_480, x_ssync_480, PEGC_GDC_PITCH_480, x_scroll_480);
    x_ff2(PEGC_FF2_VRAM_800L);
    x_ff2(PEGC_FF2_EXT_GFX);
    check_outs();
    check_fifo_gate(1);
    CHECK(pegc_gdc_fifo_timeouts == 0);
    /* 起動時が 5MHz でも入る値は同じ (起動時の状態に依らない — H5) */
    world_reset();
    boot_with(0x81, 0x03);
    pegc_enter_480_ports();
    check_outs();
}

/* 2. 戻り: 起動時 31kHz (実機 Ra266 の 81h)・2.5MHz */
static void case_restore_31k(void)
{
    s_case = "restore_31k";
    world_reset();
    boot_with(0x81, 0x00);
    CHECK(s_boot_hsync == PEGC_HSYNC_31KHZ);
    CHECK(s_boot_clk1 == 0 && s_boot_clk2 == 0);
    pegc_restore_text_sync(1);

    x_reset();
    x_ff2(PEGC_FF2_STD_GFX);
    x_ff2(PEGC_FF2_VRAM_400L);
    x_timing(PEGC_HSYNC_31KHZ, 1, PEGC_FF2_GDC_CLK1_2M5, PEGC_FF2_GDC_CLK2_2M5,
             x_msync_400_31k, x_ssync_400_31k_2m5, PEGC_GDC_PITCH_400_2M5,
             x_scroll_400_2m5);
    x_out(GDC_GFX_CMD, GDC_CMD_STOP);
    /* 先頭に 09A0h の読み (拡張モードか) が入るので OUT だけ見る */
    {
        int i, k = 0;
        for (i = 0; i < ev_n; i++) {
            if (!ev_out[i] || ev_port[i] == PEGC_STAT_PORT) continue;
            CHECK(k < xn);
            CHECK(ev_port[i] == xp[k] && ev_val[i] == xv[k]);
            k++;
        }
        CHECK(k == xn);
    }
    check_fifo_gate(1);
}

/* 3. 戻り (pegc_shutdown の後半): 24kHz / 起動時 5MHz → PITCH 80・83h・85h */
static void case_restore_24k_5m(void)
{
    s_case = "restore_24k_5m";
    world_reset();
    boot_with(0x00, 0x03);
    CHECK(s_boot_clk1 == 1 && s_boot_clk2 == 1);
    CHECK(pegc_restore_pitch() == PEGC_GDC_PITCH_400_5M);
    pegc_text_sync_400(pegc_restore_hsync());

    x_reset();
    x_timing(PEGC_HSYNC_24KHZ, 1, PEGC_FF2_GDC_CLK1_5M, PEGC_FF2_GDC_CLK2_5M,
             x_msync_400, x_ssync_400_5m, PEGC_GDC_PITCH_400_5M, x_scroll_400_5m);
    x_out(GDC_GFX_CMD, GDC_CMD_STOP);
    check_outs();
    check_fifo_gate(1);
}

/* 4. 起動時のクロックの読み分け: 片方だけ 5MHz は 2.5MHz ([U] 006Ah 82h の注) */
static void case_boot_clock(void)
{
    s_case = "boot_clock";
    world_reset();
    boot_with(0x00, 0x01);
    CHECK(s_boot_clk1 == 1 && s_boot_clk2 == 0);
    CHECK(pegc_restore_pitch() == PEGC_GDC_PITCH_400_2M5);
    pegc_text_sync_400(PEGC_HSYNC_24KHZ);
    x_reset();
    x_timing(PEGC_HSYNC_24KHZ, 1, PEGC_FF2_GDC_CLK1_5M, PEGC_FF2_GDC_CLK2_2M5,
             x_msync_400, x_ssync_400_2m5, PEGC_GDC_PITCH_400_2M5, x_scroll_400_2m5);
    x_out(GDC_GFX_CMD, GDC_CMD_STOP);
    check_outs();

    world_reset();
    boot_with(0x00, 0x02);
    CHECK(s_boot_clk1 == 0 && s_boot_clk2 == 1);
    CHECK(pegc_restore_pitch() == PEGC_GDC_PITCH_400_2M5);

    /* 記録の読み: 09A0h へ 09h を書いてから読む */
    world_reset();
    fake_09a0_clk = 0x03;
    pegc_boot_sync_record();
    {
        int i, sel_at = -1, rd_at = -1;
        for (i = 0; i < ev_n; i++) {
            if (ev_out[i] && ev_port[i] == PEGC_STAT_PORT &&
                ev_val[i] == PEGC_STAT_SEL_GDCCLK1) sel_at = i;
            if (!ev_out[i] && ev_port[i] == PEGC_STAT_PORT && sel_at >= 0 &&
                rd_at < 0) rd_at = i;
        }
        CHECK(sel_at >= 0 && rd_at > sel_at);
    }
    /* 2 回目は何もしない (起動時の値を 480 の後で読み直さない) */
    ev_n = 0;
    fake_09a0_clk = 0x00;
    pegc_boot_sync_record();
    CHECK(ev_n == 0);
    CHECK(s_boot_clk1 == 1 && s_boot_clk2 == 1);
}

/* 5. 記録が無い (PEGC の probe が通っていない) 戻りはクロックと PITCH に
 *    触らない = 従来の列 */
static void case_restore_unrecorded(void)
{
    s_case = "restore_unrecorded";
    world_reset();
    s_probed = 1;
    s_probe_ok = 0;
    pegc_restore_text_sync(0);
    x_reset();
    /* 従来の組: 5MHz 用 SYNC + IM=0 (起動時のクロックが分からない) */
    x_timing(PEGC_HSYNC_24KHZ, 0, 0, 0, x_msync_400, x_ssync_400_5m, 0,
             x_scroll_400_2m5);
    x_out(GDC_GFX_CMD, GDC_CMD_STOP);
    check_outs();
    check_fifo_gate(1);
}

/* 6. 09A8h へは bit1,0 だけ ([U] io_disp.md 09A8h「bit 7〜2 は常に 0」) */
static void case_hsync_bits(void)
{
    int i, seen = 0;
    s_case = "hsync_bits";
    world_reset();
    boot_with(0xFF, 0x00);
    CHECK(s_boot_hsync == PEGC_HSYNC_31KHZ);
    pegc_text_sync_400(pegc_restore_hsync());
    pegc_enter_480_ports();
    pegc_restore_text_sync(1);
    for (i = 0; i < ev_n; i++) {
        if (ev_out[i] && ev_port[i] == PEGC_HSYNC_PORT) {
            CHECK((ev_val[i] & ~PEGC_HSYNC_MASK) == 0);
            CHECK(ev_val[i] == PEGC_HSYNC_31KHZ);
            seen++;
        }
    }
    CHECK(seen == 3);
}

/* 7. FIFO が一時的に詰まる: 詰まっている間は書かず、待ちを挟んで読み直す */
static void case_fifo_busy(void)
{
    int n_ok;
    s_case = "fifo_busy";
    world_reset();
    boot_with(0x00, 0x00);
    pegc_enter_480_ports();
    n_ok = count_gdc_writes();

    world_reset();
    boot_with(0x00, 0x00);
    fake_full_reads = 3;
    fake_busy_reads = 5;
    pegc_enter_480_ports();
    CHECK(count_gdc_writes() == n_ok);
    check_fifo_gate(1);
    CHECK(delay_calls == 5);          /* 最初のコマンドの前に 5 回待つ */
    CHECK(pegc_gdc_fifo_timeouts == 0);
    x_reset();
    x_timing(PEGC_HSYNC_31KHZ, 1, PEGC_GDC_CLK1_480, PEGC_GDC_CLK2_480,
             x_msync_480, x_ssync_480, PEGC_GDC_PITCH_480, x_scroll_480);
    x_ff2(PEGC_FF2_VRAM_800L);
    x_ff2(PEGC_FF2_EXT_GFX);
    check_outs();

    /* パラメータの途中で FULL: 最初のコマンド (テキストの SYNC) を書いた
     * 直後から 4 回 FULL。最初のパラメータはその 4 回の後に書く。 */
    world_reset();
    boot_with(0x00, 0x00);
    fake_full_after_cmd = 4;
    pegc_apply_timing(&s_timing_480);
    check_fifo_gate(1);
    CHECK(delay_calls == 4);
    {
        int i, cmd_at = -1, prm_at = -1, reads = 0;
        for (i = 0; i < ev_n; i++) {
            if (ev_out[i] && ev_port[i] == GDC_TEXT_CMD && cmd_at < 0) cmd_at = i;
            if (ev_out[i] && ev_port[i] == GDC_TEXT_PARAM) { prm_at = i; break; }
        }
        CHECK(cmd_at >= 0 && prm_at > cmd_at);
        for (i = cmd_at + 1; i < prm_at; i++) {
            CHECK(!ev_out[i] && ev_port[i] == GDC_TEXT_STAT);
            reads++;
        }
        CHECK(reads == 5);            /* FULL ×4 + 空き 1 */
    }
}

/* 8. FIFO が詰まったまま: 1 バイトごとに上限まで読んで諦め、数えて先へ */
static void case_fifo_stuck(void)
{
    int n_ok;
    s_case = "fifo_stuck";
    world_reset();
    boot_with(0x00, 0x00);
    pegc_enter_480_ports();
    n_ok = count_gdc_writes();

    world_reset();
    boot_with(0x00, 0x00);
    fake_full_reads = -1;
    fake_busy_reads = -1;
    pegc_enter_480_ports();
    CHECK(count_gdc_writes() == n_ok);
    CHECK(pegc_gdc_fifo_timeouts == (u32)n_ok);
    CHECK(delay_calls == (u32)n_ok * PEGC_GDC_FIFO_POLLS);
    check_fifo_gate(0);
    x_reset();
    x_timing(PEGC_HSYNC_31KHZ, 1, PEGC_GDC_CLK1_480, PEGC_GDC_CLK2_480,
             x_msync_480, x_ssync_480, PEGC_GDC_PITCH_480, x_scroll_480);
    x_ff2(PEGC_FF2_VRAM_800L);
    x_ff2(PEGC_FF2_EXT_GFX);
    check_outs();
}

/* ---- 資料の規則で、実際に出た OUT を**独立に**検査する (Codex レビュー P2) ----
 * 期待列はヘッダの定数から組むので、ヘッダの選び方 (どの組を使うか) が
 * 誤っていても列の比較では見えない。そこで出た OUT から グラフィック GDC の
 * SYNC・SCROLL・PITCH と 6Ah のクロックを拾い、資料の数値と照らす:
 *   [B] 2-6 表2-27: グラフィック 2.5MHz = C/R 26h・HS 03h・HFP 04h・HBP 03h、
 *                   5MHz = C/R 4Eh・HS 07h・HFP 09h・HBP 07h、
 *                   共通 VS 08h・VFP 07h・VBP 19h・L/F 190h (24kHz 400 ライン)。
 *   [B] 2-7: SCROLL 第 4 バイト bit6 (IM) は 2.5MHz で 0、5MHz で 1。
 *            PITCH は 2.5MHz で 40、5MHz で 80。
 * 数値はここに直に書く (include/pegc.h を見ない)。 */
#define R_CR_2M5   0x26
#define R_CR_5M    0x4E
#define R_IM       0x40
typedef struct {
    int n_sync, n_scroll, n_pitch;
    u8 sync[8], scroll[4], pitch;
    int clk1, clk2;          /* 最後に書いた 6Ah 82h〜85h (-1 = 無し) */
    int hs;                  /* 最後に書いた 09A8h (-1 = 無し) */
} Seen;

static void collect_gfx(Seen *z)
{
    int i, want = 0, which = 0;
    kmemset(z, 0, sizeof(*z));
    z->clk1 = z->clk2 = z->hs = -1;
    for (i = 0; i < ev_n; i++) {
        if (!ev_out[i]) continue;
        if (ev_port[i] == MODE_FF2_PORT) {
            if (ev_val[i] == 0x82 || ev_val[i] == 0x83) z->clk1 = ev_val[i] & 1;
            if (ev_val[i] == 0x84 || ev_val[i] == 0x85) z->clk2 = ev_val[i] & 1;
        } else if (ev_port[i] == PEGC_HSYNC_PORT) {
            z->hs = ev_val[i];
        } else if (ev_port[i] == GDC_GFX_CMD) {
            want = 0;
            if (ev_val[i] == 0x0E) { which = 1; want = 8; z->n_sync = 0; }
            if (ev_val[i] == 0x70) { which = 2; want = 4; z->n_scroll = 0; }
            if (ev_val[i] == 0x47) { which = 3; want = 1; z->n_pitch = 0; }
        } else if (ev_port[i] == GDC_GFX_PARAM && want > 0) {
            if (which == 1) z->sync[z->n_sync++] = ev_val[i];
            if (which == 2) z->scroll[z->n_scroll++] = ev_val[i];
            if (which == 3) { z->pitch = ev_val[i]; z->n_pitch++; }
            want--;
        }
    }
}

/* 出たグラフィック GDC の設定が、出た (または起動時の) クロックと矛盾しない */
static void check_gfx_by_rule(int is5m)
{
    Seen z;
    collect_gfx(&z);
    CHECK(z.n_sync == 8 && z.n_scroll == 4);
    CHECK(z.sync[1] == (is5m ? R_CR_5M : R_CR_2M5));
    CHECK((z.scroll[3] & R_IM) == (is5m ? R_IM : 0));
    if (z.n_pitch) CHECK(z.pitch == (is5m ? 80 : 40));
    if (z.clk1 >= 0) CHECK((z.clk1 && z.clk2) == is5m);
    if (z.hs == 0) {
        /* 24kHz / 400 ラインは表2-27 の全項目 */
        u8 *p = z.sync;
        CHECK((p[2] & 0x1F) == (is5m ? 0x07 : 0x03));          /* HS */
        CHECK((p[3] >> 2) == (is5m ? 0x09 : 0x04));            /* HFP */
        CHECK((p[4] & 0x3F) == (is5m ? 0x07 : 0x03));          /* HBP */
        CHECK((((p[3] & 3) << 3) | (p[2] >> 5)) == 0x08);      /* VS */
        CHECK((p[5] & 0x3F) == 0x07);                          /* VFP */
        CHECK((p[7] >> 2) == 0x19);                            /* VBP */
        CHECK((p[6] | ((p[7] & 3) << 8)) == 0x190);            /* L/F */
    }
}

/* 10. 戻りの組: 起動時の周波数 2 通り × クロック 2 通り (+ 片方だけ 5MHz) */
static void case_restore_matrix(void)
{
    static const u8 hs_raw[2] = { 0x00, 0x81 };
    static const u8 clk[3] = { 0x00, 0x03, 0x01 };
    int h, c;
    s_case = "restore_matrix";
    for (h = 0; h < 2; h++) {
        for (c = 0; c < 3; c++) {
            int is5m = (clk[c] == 0x03);
            world_reset();
            boot_with(hs_raw[h], clk[c]);
            pegc_restore_text_sync(h);      /* v86 -g の FALLBACK の口 */
            check_gfx_by_rule(is5m);
            world_reset();
            boot_with(hs_raw[h], clk[c]);
            pegc_text_sync_400(pegc_restore_hsync());   /* pegc_shutdown の口 */
            check_gfx_by_rule(is5m);
        }
    }
    /* 480 ラインへ入る側は 5MHz の組 (C/R 4Eh・IM 1・PITCH 80) */
    world_reset();
    boot_with(0x00, 0x00);
    pegc_enter_480_ports();
    check_gfx_by_rule(1);
}

/* 9. 既定値 = NP21/W の記録 (票 §3「段 1 の NP21/W での記録」) と資料。
 *    **実機の `v86 -g` の記録で pegc.h §10 を差し替えたら、ここを直す。** */
static void case_defaults(void)
{
    s_case = "defaults";
    CHECK(PEGC_GDC_PITCH_480 == 80);              /* O s480 00a2 0047 / 00a0 0050 */
    CHECK(PEGC_GDC_CLK1_480 == 0x83);             /* O s480 006a 0083 */
    CHECK(PEGC_GDC_CLK2_480 == 0x85);             /* [B] 3-2 表3-2 83H と 85H */
    CHECK(PEGC_GDC_PITCH_400_2M5 == 40);          /* O back 00a0 0028、[B] 2-7 */
    CHECK(PEGC_GDC_PITCH_400_5M == 80);           /* [B] 2-7 */
    CHECK(PEGC_FF2_GDC_CLK1_2M5 == 0x82 && PEGC_FF2_GDC_CLK2_2M5 == 0x84);
    CHECK(PEGC_STAT_SEL_GDCCLK1 == 0x09 && PEGC_STAT_RD_GDCCLK2 == 0x02);
    CHECK(PEGC_GDC_FIFO_POLLS > 0 && PEGC_GDC_FIFO_POLL_US > 0);
    /* 1 バイトあたりの待ちの上限が 1 フレームを大きく超えない (100ms 未満) */
    CHECK((u32)PEGC_GDC_FIFO_POLLS * PEGC_GDC_FIFO_POLL_US < 100000UL);
    CHECK(x_scroll_480[3] == 0x40);               /* IM=1 (5MHz) */
    /* 戻りの SYNC / SCROLL の組の数値 (NP21/W bios18.c gdcslavesync) */
    {
        static const u8 l24[8] = { 0x02, 0x26, 0x03, 0x11, 0x83, 0x07, 0x90, 0x65 };
        static const u8 m24[8] = { 0x02, 0x4E, 0x07, 0x25, 0x87, 0x07, 0x90, 0x65 };
        static const u8 l31[8] = { 0x02, 0x26, 0x41, 0x0C, 0x83, 0x0D, 0x90, 0x89 };
        static const u8 m31[8] = { 0x02, 0x4E, 0x47, 0x0C, 0x87, 0x0D, 0x90, 0x89 };
        int i;
        for (i = 0; i < 8; i++) {
            CHECK(x_ssync_400_2m5[i] == l24[i] && x_ssync_400_5m[i] == m24[i]);
            CHECK(x_ssync_400_31k_2m5[i] == l31[i] && x_ssync_400_31k_5m[i] == m31[i]);
        }
        CHECK(x_scroll_400_2m5[3] == 0x00 && x_scroll_400_5m[3] == 0x40);
    }
}

void _start(void)
{
    /* 資料の規則での検査を先に回す (変異がヘッダ由来の期待列ではなくこちらで
     * 落ちることを --mutate の行で見られるように) */
    case_restore_matrix();
    case_defaults();
    case_enter_480();
    case_restore_31k();
    case_restore_24k_5m();
    case_boot_clock();
    case_restore_unrecorded();
    case_hsync_bits();
    case_fifo_busy();
    case_fifo_stuck();
    output("PASS pegc_mode_host (10 cases)\n");
    finish(0);
}
