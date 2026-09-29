/* =========================================================================
 *  CIRRUS_WIN_HOST.C — Cirrus の窓の可否を「物理地図」で決めるか
 *
 *  実行: python3 -B tools/tests/test_cirrus_win.py [--mutate]
 *  記録: tools/tests/cirrus_win_tdd.md
 *
 *  gfx/backend_cirrus.c の cirrus_win_usable() は、窓 (バンク窓 F60000h /
 *  リニア窓 01000000h) を張ってよいかを **RAM の上端** (sys_get_mem_kb) で
 *  決めていた。K6-RAM 以後、15MB + 高位 RAM の構成 (NP21/W の RAM
 *  17,408KB) では上端 17MB が 15-16MB の穴より上に出るので、穴の中の
 *  バンク窓まで「RAM が届いている」と誤判定し、probe が ID 判定の前に落ちる。
 *  PEGC で直した不具合 (docs/POLICY_DEBUG.md §4-34) と同じ形。
 *
 *  実物の backend_cirrus.c を 1 行も写さずに #include し、周りの関数だけを
 *  贋物にする。pgalloc_range_has_ram は**贋の物理地図** (RAM の span の表)
 *  から答え、sys_get_mem_kb は構成の上端を返す (旧判定を RED にするため)。
 *  ボードグルー wab_glue_xe10 も贋物にして、窓の番地と probe の到達を見る。
 * ========================================================================= */
#include "types.h"
#define NOINST __attribute__((no_instrument_function))

#include "../../gfx/backend_cirrus.c"
#include "wab_xe10.h"     /* 実物の窓の番地 (贋グルーに入れる) */

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
static void fail(const char *message)
{
    output("ASSERT FAIL: "); output(message); output("\n"); finish(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

/* ---- 贋の物理地図 ---- */
#define MAX_SPANS 4
static u32 ram_first[MAX_SPANS], ram_end[MAX_SPANS];  /* PFN 半開 */
static int ram_spans;
static u32 top_kb;          /* sys_get_mem_kb が返す「上端」 */
static int map_ready;       /* 0 = pgalloc 未初期化 */
static int has_ram_calls;

static void map_reset(u32 top)
{
    ram_spans = 0;
    top_kb = top;
    map_ready = 1;
    has_ram_calls = 0;
}
static void map_add(u32 base, u32 end)
{
    ram_first[ram_spans] = base / PAGE_SIZE;
    ram_end[ram_spans] = end / PAGE_SIZE;
    ram_spans++;
}

/* 実物 (kernel/pgalloc.c) と同じ約束: 範囲異常・未初期化は 1。 */
int pgalloc_range_has_ram(u32 first, u32 end)
{
    int i;
    has_ram_calls++;
    if (!map_ready || end <= first) return 1;
    for (i = 0; i < ram_spans; i++)
        if (ram_first[i] < end && first < ram_end[i]) return 1;
    return 0;
}
u32 sys_get_mem_kb(void) { return top_kb; }

/* ---- 贋のボードグルー ---- */
static int glue_probe_calls;
static int fake_glue_probe(void) { glue_probe_calls++; return 0; }
static void fake_linear_enable(int on) { (void)on; }
WabGlue wab_glue_xe10;

static void glue_reset(u32 win_base, u32 win_size, u32 lin_base, u32 lin_size)
{
    kmemset(&wab_glue_xe10, 0, sizeof(wab_glue_xe10));
    wab_glue_xe10.name = "fake-xe10";
    wab_glue_xe10.probe = fake_glue_probe;
    wab_glue_xe10.linear_enable = fake_linear_enable;
    wab_glue_xe10.win_base = win_base;
    wab_glue_xe10.win_size = win_size;
    wab_glue_xe10.lin_base = lin_base;
    wab_glue_xe10.lin_size = lin_size;
    glue_probe_calls = 0;
    s_probed = 0;
    s_probe_ok = 0;
}

/* ---- 残りの依存 (この試験の経路では呼ばれない) ---- */
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
int paging_map_phys(u32 v, u32 p, u32 n, u32 f)
{ (void)v; (void)p; (void)n; (void)f; fail("paging_map_phys"); return -1; }
int wab_cirrus_probe(WabGlue *g) { (void)g; return 0; }
int wab_cirrus_setup_8bpp(WabGlue *g, int w, int h, u32 p, u32 st)
{ (void)g; (void)w; (void)h; (void)p; (void)st; return 0; }
void wab_cirrus_shutdown(WabGlue *g) { (void)g; }
void wab_cirrus_dac_set(WabGlue *g, int i, u8 r, u8 gg, u8 b)
{ (void)g; (void)i; (void)r; (void)gg; (void)b; }
void wab_cirrus_set_fill_pattern(WabGlue *g, u32 o) { (void)g; (void)o; }
int wab_cirrus_fill(WabGlue *g, u32 d, u32 p, int w, int h, u8 c)
{ (void)g; (void)d; (void)p; (void)w; (void)h; (void)c; return 0; }
int wab_cirrus_copy(WabGlue *g, u32 d, u32 s, u32 dp, u32 sp, int w, int h)
{ (void)g; (void)d; (void)s; (void)dp; (void)sp; (void)w; (void)h; return 0; }
int wab_cirrus_wait_idle(WabGlue *g) { (void)g; return 0; }
const PaletteEntry *palette_get_all(void) { return (const PaletteEntry *)0; }
void palette_shadow_set(int i, u8 r, u8 g, u8 b) { (void)i; (void)r; (void)g; (void)b; }

/* ---- 構成 ---- */
#define KB(x)  ((u32)(x) * 1024UL)
#define MB(x)  ((u32)(x) * 1024UL * 1024UL)
#define BANK   WAB_XE10_WIN_BASE          /* F60000h */
#define BANK_N WAB_XE10_WIN_SIZE
#define LIN    WAB_XE10_LINEARWIN_BASE    /* 01000000h */
#define LIN_N  WAB_XE10_LINEARWIN_SIZE

/* 15MB + 高位 1MB (NP21/W ExMemory 16 の K6 以後の地図、上端 17,408KB)。 */
static void cfg_15m_high1(void)
{
    map_reset(17408);
    map_add(0, KB(640));
    map_add(MB(1), MB(15));
    map_add(MB(16), MB(17));
}

/* 1. 本件: 穴の中のバンク窓は張れる。上端 17MB では決めない。 */
static void bank_window_in_hole(void)
{
    cfg_15m_high1();
    CHECK(cirrus_win_usable(BANK, BANK_N));
    CHECK(has_ram_calls > 0);   /* 物理地図に問い合わせている */
    /* PEGC の窓 (F00000h、512KB) も同じ穴。同じ答えになること。 */
    CHECK(cirrus_win_usable(MEM_SYSTEM_SPACE_BASE, MB(1)));
}

/* 2. リニア窓 16MB はこの構成では高位 RAM と本当に重なる = 張れない。 */
static void linear_window_on_high_ram(void)
{
    cfg_15m_high1();
    CHECK(!cirrus_win_usable(LIN, LIN_N));
    /* 窓の末尾 1 ページだけ RAM でも拒む (部分一致を見落とさない)。 */
    map_reset(8192);
    map_add(MB(1), MB(8));
    map_add(LIN + LIN_N - PAGE_SIZE, LIN + LIN_N);
    CHECK(!cirrus_win_usable(LIN, LIN_N));
    /* 窓のすぐ後ろの RAM は窓を塞がない (範囲を広げすぎない)。 */
    map_reset(8192);
    map_add(MB(1), MB(8));
    map_add(LIN + LIN_N, LIN + LIN_N + MB(1));
    CHECK(cirrus_win_usable(LIN, LIN_N));
    /* 窓のすぐ前の RAM も塞がない。 */
    map_reset(8192);
    map_add(MB(1), LIN);
    CHECK(cirrus_win_usable(LIN, LIN_N));
}

/* 3. 16MB 丸ごと RAM (043Bh の 15-16MB を RAM にした構成): バンク窓は RAM。 */
static void system_space_is_ram(void)
{
    map_reset(16384);
    map_add(0, KB(640));
    map_add(MB(1), MB(16));
    CHECK(!cirrus_win_usable(BANK, BANK_N));
    /* 窓の最初の 1 ページだけ RAM でも拒む。 */
    map_reset(15744 + 4);
    map_add(MB(1), BANK + PAGE_SIZE);
    CHECK(!cirrus_win_usable(BANK, BANK_N));
}

/* 4. 8MB 機: どちらの窓にも RAM は無い (K6 以前と同じ答え)。 */
static void small_machine(void)
{
    map_reset(8192);
    map_add(0, KB(640));
    map_add(MB(1), MB(8));
    CHECK(cirrus_win_usable(BANK, BANK_N));
    CHECK(cirrus_win_usable(LIN, LIN_N));
}

/* 5. 物理地図が引けない (pgalloc 未初期化) なら張らせない。 */
static void map_not_ready(void)
{
    map_reset(8192);
    map_ready = 0;
    CHECK(!cirrus_win_usable(BANK, BANK_N));
}

/* 6. 大きさ 0・ページングの守備範囲の外・桁あふれは拒む (従来どおり)。 */
static void window_bounds(void)
{
    map_reset(8192);
    map_add(MB(1), MB(8));
    CHECK(!cirrus_win_usable(BANK, 0));
    CHECK(cirrus_win_usable(PAGING_MAP_SIZE - PAGE_SIZE, PAGE_SIZE));
    CHECK(!cirrus_win_usable(PAGING_MAP_SIZE - PAGE_SIZE, 2 * PAGE_SIZE));
    CHECK(!cirrus_win_usable(PAGING_MAP_SIZE, PAGE_SIZE));
    CHECK(!cirrus_win_usable(0xFFFFF000UL, 0x2000UL));
}

/* 7. probe の段: 15MB + 高位 RAM ではバンク窓を通り、リニア窓で落ちる
 *    (ボードの ID 判定には進まない)。リニア窓が空いていれば ID 判定へ進む。 */
static void probe_stages(void)
{
    cfg_15m_high1();
    glue_reset(BANK, BANK_N, LIN, LIN_N);
    CHECK(cirrus_probe() == 0);
    CHECK(glue_probe_calls == 0);

    /* 高位 RAM の無い 15MB 機: 両方の窓が空いている → ID 判定まで進む。 */
    map_reset(15360);
    map_add(0, KB(640));
    map_add(MB(1), MB(15));
    glue_reset(BANK, BANK_N, LIN, LIN_N);
    CHECK(cirrus_probe() == 0);     /* 贋グルーの ID 判定は 0 を返す */
    CHECK(glue_probe_calls == 1);

    /* バンク窓だけ RAM に当たる (16MB 丸ごと RAM) → ID 判定へ進まない。 */
    map_reset(16384);
    map_add(MB(1), MB(16));
    glue_reset(BANK, BANK_N, 2 * LIN, LIN_N);
    CHECK(cirrus_probe() == 0);
    CHECK(glue_probe_calls == 0);
}

void _start(void)
{
    CHECK(sizeof(u32) == 4);
    bank_window_in_hole();
    output("PASS bank window in the 15-16MB hole (top 17408KB)\n");
    linear_window_on_high_ram();
    output("PASS linear window vs high RAM, partial overlap, neighbours\n");
    system_space_is_ram();
    output("PASS system space registered as RAM rejects the bank window\n");
    small_machine();
    output("PASS 8MB machine: both windows free\n");
    map_not_ready();
    output("PASS map not ready: refuse\n");
    window_bounds();
    output("PASS size 0 / paging bound / overflow\n");
    probe_stages();
    output("PASS probe stages (bank ok, linear refused, glue not reached)\n");
    finish(0);
}
