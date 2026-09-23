/* ======================================================================== */
/*  PCM_CS4231_HOST.C — CS4231 再生ドライバをそのままホストで回す           */
/*                                                                          */
/*  実物 (drivers/pcm_cs4231_math.c と drivers/pcm_cs4231.c) を 1 行も写さずに */
/*  #include する。装置・8237・プール・割り込み・時計は模型に差し替え、      */
/*  **ポートアクセスを 1 回ずつ数える**。                                    */
/*                                                                          */
/*  **NP21/W では踏めない分岐がここの主目的** (票 §2-3 / E1):               */
/*    (a) 1 周回った観測 (同じ半分で p が戻る) — 実時間では作れない          */
/*    (b) 補充の余裕 (REFILL_MARGIN) を割った切り替え                        */
/*    (c) drain の 3 段階と「出た + staged > 0 は完了しない」                */
/*    (d) 初期化列 / 停止列 / RS の列の**順序**                              */
/*    (e) 入口ガードで装置アクセスが **0 回**であること                      */
/*    (f) close / reclaim が各状態から **1 度だけ**解放すること              */
/*                                                                          */
/*  記録: tools/tests/pcm_cs4231_tdd.md                                     */
/*  [C1] C89 / GNU89。                                                      */
/* ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "pcm_cs4231.h"
#include "dma8237.h"    /* DMA_DIR_* / DMA_MODE_* (実物の定数を使う) */
#include "irq_math.h"   /* irq_handler_fn / IRQ_* (実物の定数を使う) */

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); failed++; \
} } while (0)

static int failed;

/* ======================================================================== */
/*  1. 模型 — 装置 / 8237 / プール / 割り込み / 時計                        */
/* ======================================================================== */
int pcm_shim_irq_depth;

#define DEV_REGS 32
static u8  dev_reg[DEV_REGS];
static u8  dev_r0;              /* 最後に R0 へ書いた値 (INIT は読みで合成) */
static u8  dev_route;
static int dev_present;         /* 0 = 0F43h が 0xFF を返す */
static int dev_init_busy;       /* 1 = R0 の読みが 0x80 */
static int sim_tick_per_read;   /* 1 = 読むたびに tick が 1 進む (期限の試験) */

/* 時計は下の「時計と所有者」で定義するが、ポート模型が先に触る。 */
extern volatile u32 tick_count;
static u32 dev_r2_writes;

/* ポートアクセスの勘定 (E1 の「0 回」を数える) */
static u32 io_reads, io_writes;
#define IO_ACCESSES  (io_reads + io_writes)

/* 書きの記録 (列の照合に使う) */
#define TRACE_MAX 128
struct trace_ent { u16 port; u8 val; };
static struct trace_ent trace[TRACE_MAX];
static int trace_n;

static void trace_reset(void)
{
    trace_n = 0;
    io_reads = 0;
    io_writes = 0;
}

unsigned int pcm_shim_inp(unsigned int port)
{
    u8 idx;

    io_reads++;
    if (sim_tick_per_read) tick_count++;
    if (port == PCM_PORT_WSSID) return dev_present ? PCM_WSSID_VALUE : 0xFFu;
    if (port == PCM_PORT_R0)
        return (unsigned int)(dev_init_busy ? PCM_R0_INIT : dev_r0);
    if (port == PCM_PORT_R1) {
        if (dev_init_busy) return PCM_R0_INIT;
        idx = (u8)(dev_r0 & PCM_R0_IA_MASK);
        return dev_reg[idx];
    }
    if (port == PCM_PORT_R2) return 0;
    return 0xFFu;
}

void pcm_shim_outp(unsigned int port, unsigned int value)
{
    u8 idx, v = (u8)value;

    io_writes++;
    if (trace_n < TRACE_MAX) {
        trace[trace_n].port = (u16)port;
        trace[trace_n].val = v;
        trace_n++;
    }
    /* 0F40h の経路はコーデックの外 (C バスの結線) なので INIT とは無関係。 */
    if (port == PCM_PORT_ROUTE) { dev_route = v; return; }
    if (dev_init_busy) return;          /* INIT 中はコーデックの書きが無視される */
    if (port == PCM_PORT_R0)    { dev_r0 = v; return; }
    if (port == PCM_PORT_R2)    { dev_r2_writes++; return; }
    if (port != PCM_PORT_R1) return;

    idx = (u8)(dev_r0 & PCM_R0_IA_MASK);
    if (idx == PCM_I_ERRSTAT) return;                     /* 読み取り専用 */
    if (idx == PCM_I_MODEID) {                            /* ID3-0 は固定 */
        dev_reg[idx] = (u8)((v & 0xF0u) | PCM_ID_VALUE);
        return;
    }
    if (idx == PCM_I_IFACE && !(dev_r0 & PCM_R0_MCE)) {
        /* MCE の外では PEN (と CEN) しか書けない (DS139PP2 p.34) */
        dev_reg[idx] = (u8)((dev_reg[idx] & ~PCM_IFACE_PEN) |
                            (v & PCM_IFACE_PEN));
        return;
    }
    dev_reg[idx] = v;
}

/* ---- 8237 の模型 -------------------------------------------------------- */
static int sim_masked = 1;
static int sim_setup_fail;
static int sim_eagain;
static u32 sim_left = PCM_RING_BYTES;
static u32 sim_setup_calls, sim_mask_calls, sim_unmask_calls, sim_acktc_calls;

void dma_chan_mask(unsigned int ch)   { (void)ch; sim_masked = 1; sim_mask_calls++; }
void dma_chan_unmask(unsigned int ch) { (void)ch; sim_masked = 0; sim_unmask_calls++; }
void dma_chan_ack_tc(unsigned int ch) { (void)ch; sim_acktc_calls++; }

int dma_chan_setup(unsigned int ch, u32 phys, u32 bytes, int dir, int mode)
{
    (void)ch; (void)phys;
    sim_setup_calls++;
    if (sim_setup_fail) return OS32_ERR_INVAL;
    if (bytes != PCM_RING_BYTES) return OS32_ERR_INVAL;
    if (dir != DMA_DIR_FROM_MEM || mode != DMA_MODE_CYCLIC) return OS32_ERR_INVAL;
    return 0;
}

int dma_chan_remaining(unsigned int ch, u32 *bytes_left, int *tc_seen)
{
    (void)ch;
    if (sim_eagain) return OS32_ERR_AGAIN;
    *bytes_left = sim_left;
    *tc_seen = 0;
    return 0;
}

/* 装置の位置を frame 番号で置く (リング全体 0..4095)。 */
static void sim_set_pos(u32 frames)
{
    sim_left = (PCM_RING_BYTES - frames * PCM_FRAME_BYTES) % PCM_RING_BYTES;
    if (sim_left == 0) sim_left = PCM_RING_BYTES;
}

/* ---- DMA プールの模型 --------------------------------------------------- */
static u8  pool_mem[2][PCM_RING_BYTES];
static int pool_used[2];
static int pool_freed[2];
static int pool_leaked[2];
static int pool_fail;

void *dma_pool_alloc(u32 bytes, u32 align, u32 *phys_out)
{
    int i;

    (void)align;
    if (pool_fail) return (void *)0;
    for (i = 0; i < 2; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            if (phys_out) *phys_out = (u32)(0x2E8000UL + (u32)i * bytes);
            return pool_mem[i];
        }
    }
    return (void *)0;
}

static int pool_index(void *p)
{
    if (p == pool_mem[0]) return 0;
    if (p == pool_mem[1]) return 1;
    return -1;
}

int dma_pool_free(void *virt)
{
    int i = pool_index(virt);
    if (i < 0) return OS32_ERR_INVAL;
    pool_freed[i]++;
    pool_used[i] = 0;
    return 0;
}

int dma_pool_mark_leaked(void *virt)
{
    int i = pool_index(virt);
    if (i < 0) return OS32_ERR_INVAL;
    pool_leaked[i]++;
    return 0;
}

/* ---- KHEAP の模型 (ステージング) ---------------------------------------- */
static u8  heap_mem[PCM_STG_BYTES];
static int heap_used, heap_freed, heap_fail;

void *kmalloc(u32 size)
{
    if (heap_fail || heap_used || size != PCM_STG_BYTES) return (void *)0;
    heap_used = 1;
    return heap_mem;
}

void kfree(void *ptr)
{
    if (ptr != heap_mem) return;
    heap_freed++;
    heap_used = 0;
}

/* ---- 割り込みの模型 ----------------------------------------------------- */
static int sim_irq_registered;
static int sim_irq_fail;
static u32 sim_irq_reg_calls, sim_irq_unreg_calls;

int irq_register(unsigned int irq, irq_handler_fn fn, void *arg, unsigned int flags)
{
    (void)fn; (void)arg;
    sim_irq_reg_calls++;
    if (sim_irq_fail) return IRQ_ERR_BUSY;
    if (irq != PCM_IRQ || flags != IRQ_F_SHARED) return IRQ_ERR_INVAL;
    sim_irq_registered = 1;
    return 0;
}

int irq_unregister(unsigned int irq, irq_handler_fn fn, void *arg)
{
    (void)irq; (void)fn; (void)arg;
    sim_irq_unreg_calls++;
    sim_irq_registered = 0;
    return 0;
}

/* ---- 時計と所有者 ------------------------------------------------------- */
volatile u32 tick_count;
static u32 sim_now_us;
static int sim_time_fail;

int sys_time_now(u32 *lo, u32 *hi)
{
    if (sim_time_fail) return OS32_ERR_AGAIN;
    *lo = sim_now_us;
    *hi = 0;
    return 0;
}

static int sim_owner = 1;
int res_owner_get(void) { return sim_owner; }

/* ---- kstring / kprintf -------------------------------------------------- */
void *kmemcpy(void *dst, const void *src, u32 n) { return memcpy(dst, src, n); }
void *kmemset(void *dst, int val, u32 n) { return memset(dst, val, n); }
void kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }

/* ======================================================================== */
/*  2. 実物をそのまま取り込む                                                */
/* ======================================================================== */
#include "../../drivers/pcm_cs4231_math.c"
#include "../../drivers/pcm_cs4231.c"

/* ======================================================================== */
/*  3. 土台をきれいにする                                                    */
/* ======================================================================== */
static void reset_all(void)
{
    int i;

    memset(dev_reg, 0, sizeof(dev_reg));
    dev_reg[PCM_I_MODEID] = PCM_ID_VALUE;
    dev_reg[PCM_I_VERSION] = PCM_VER_CS4231;
    dev_r0 = 0;
    dev_route = 0;
    dev_present = 1;
    dev_init_busy = 0;
    sim_tick_per_read = 0;
    dev_r2_writes = 0;

    sim_masked = 1;
    sim_setup_fail = 0;
    sim_eagain = 0;
    sim_left = PCM_RING_BYTES;
    sim_setup_calls = sim_mask_calls = sim_unmask_calls = sim_acktc_calls = 0;
    sim_irq_registered = 0;
    sim_irq_fail = 0;
    sim_irq_reg_calls = sim_irq_unreg_calls = 0;
    pool_fail = 0;
    heap_used = heap_freed = heap_fail = 0;
    memset(heap_mem, 0, sizeof(heap_mem));
    for (i = 0; i < 2; i++) {
        pool_used[i] = pool_freed[i] = pool_leaked[i] = 0;
        memset(pool_mem[i], 0, sizeof(pool_mem[i]));
    }
    tick_count = 0;
    sim_now_us = 0;
    sim_time_fail = 0;
    sim_owner = 1;
    pcm_shim_irq_depth = 0;

    memset(&g_pcm, 0, sizeof(g_pcm));
    s_ring = 0;
    s_stg = 0;
    s_ring_phys = 0;
    s_owner = 0;
    s_irq_reg = 0;
    s_present = 0;
    s_version = 0;
    s_mce_busy = 0;
    s_deadline = 0;
    s_seq_at = 0;
    s_last_now = 0;
    s_fault_sticky = 0;
    trace_reset();
}

/* 観測の時刻を進める (tick と µs を一緒に)。 */
static void sim_advance_time(u32 ticks)
{
    tick_count += ticks;
    sim_now_us += ticks * 10000U;
}

/* ======================================================================== */
/*  4. 純粋部の試験                                                          */
/* ======================================================================== */

/* 連続性の判定表 (h0 × h1 × p の全組)。座標はリング全体の frame 番号。 */
static void case_cont(void)
{
    /* 同じ半分で前に進む = 境界なし */
    CHECK(pcm_cont(0, 0, 0, 0) == PCM_CONT_SAME);
    CHECK(pcm_cont(0, 10, 0, 10) == PCM_CONT_SAME);
    CHECK(pcm_cont(0, 10, 0, 2047) == PCM_CONT_SAME);
    CHECK(pcm_cont(1, 2048, 1, 4095) == PCM_CONT_SAME);

    /* 半分が変わった = 切り替え 1 回。**1 → 0 の折り返しも正常** */
    CHECK(pcm_cont(0, 2047, 1, 2048) == PCM_CONT_SWITCH);
    CHECK(pcm_cont(1, 4095, 0, 0) == PCM_CONT_SWITCH);
    CHECK(pcm_cont(1, 2100, 0, 5) == PCM_CONT_SWITCH);
    CHECK(pcm_cont(0, 5, 1, 4000) == PCM_CONT_SWITCH);

    /* 同じ半分で p が戻った = 1 周 (2 境界) = 喪失。
     * 理想的な単調転送では 1 半周期未満に到達しないので、遅れた観測。 */
    CHECK(pcm_cont(0, 2000, 0, 3) == PCM_CONT_LOST);
    CHECK(pcm_cont(1, 4000, 1, 2048) == PCM_CONT_LOST);
    CHECK(pcm_cont(0, 1, 0, 0) == PCM_CONT_LOST);

    /* **保証外**として記す: 2 半周期以上の空白では 0 回と 2 回、
     * 1 回と 3 回が区別できない (未補充の半分を再読しても p1 >= p0)。 */
    CHECK(pcm_cont(0, 10, 0, 20) == PCM_CONT_SAME);   /* 実は 2 境界かもしれない */
    CHECK(pcm_cont(0, 10, 1, 2060) == PCM_CONT_SWITCH); /* 実は 3 境界かも */
}

static void case_refill(void)
{
    /* 半分の末尾まで 512 frame 以上あれば写してよい */
    CHECK(pcm_refill_ok(0) == 1);
    CHECK(pcm_refill_ok(2048 - 512) == 1);       /* ちょうど 512 */
    CHECK(pcm_refill_ok(2048 - 513) == 1);
    CHECK(pcm_refill_ok(2048 - 511) == 0);       /* 511 しかない */
    CHECK(pcm_refill_ok(2047) == 0);
    CHECK(pcm_refill_ok(2048) == 1);             /* 半分 1 の先頭 */
    CHECK(pcm_refill_ok(4096 - 512) == 1);
    CHECK(pcm_refill_ok(4095) == 0);
}

static void case_drain(void)
{
    /* 「入るのを待つ」→ last_data_half に入ったら「読んでいる」 */
    CHECK(pcm_drain_next(PCM_DRAIN_WAIT, 1, 1) == PCM_DRAIN_IN);
    CHECK(pcm_drain_next(PCM_DRAIN_WAIT, 0, 1) == PCM_DRAIN_WAIT);
    /* 「読んでいる」→ 出たら「無音の半分」 */
    CHECK(pcm_drain_next(PCM_DRAIN_IN, 0, 1) == PCM_DRAIN_SIL);
    CHECK(pcm_drain_next(PCM_DRAIN_IN, 1, 1) == PCM_DRAIN_IN);
    /* 「無音の半分」→ さらに切り替えたら「出た」(相手は問わない) */
    CHECK(pcm_drain_next(PCM_DRAIN_SIL, 1, 1) == PCM_DRAIN_OUT);
    CHECK(pcm_drain_next(PCM_DRAIN_SIL, 0, 1) == PCM_DRAIN_OUT);
    CHECK(pcm_drain_next(PCM_DRAIN_OUT, 0, 1) == PCM_DRAIN_OUT);
}

static void case_start(void)
{
    u32 n0, n1, lh;
    int ds;

    /* close 起動の 2048 未満: 半分 0 だけ。**drain は「読んでいる」から** */
    pcm_start_plan(1, &n0, &n1, &lh, &ds);
    CHECK(n0 == 1 && n1 == 0 && lh == 0 && ds == PCM_DRAIN_IN);
    pcm_start_plan(2047, &n0, &n1, &lh, &ds);
    CHECK(n0 == 2047 && n1 == 0 && lh == 0 && ds == PCM_DRAIN_IN);
    pcm_start_plan(2048, &n0, &n1, &lh, &ds);
    CHECK(n0 == 2048 && n1 == 0 && lh == 0 && ds == PCM_DRAIN_IN);

    /* 自動開始の 4096: 両半分そろう。最後に置いたのは半分 1 */
    pcm_start_plan(2049, &n0, &n1, &lh, &ds);
    CHECK(n0 == 2048 && n1 == 1 && lh == 1 && ds == PCM_DRAIN_WAIT);
    pcm_start_plan(4096, &n0, &n1, &lh, &ds);
    CHECK(n0 == 2048 && n1 == 2048 && lh == 1 && ds == PCM_DRAIN_WAIT);

    pcm_start_plan(0, &n0, &n1, &lh, &ds);
    CHECK(n0 == 0 && n1 == 0 && lh == 0);
}

static void case_rate(void)
{
    u8 fmt = 0xEE;

    CHECK(pcm_fmt_for_rate(PCM_RATE_44100, &fmt) == 0 && fmt == 0x5B);
    CHECK(pcm_fmt_for_rate(PCM_RATE_22050, &fmt) == 0 && fmt == 0x57);
    fmt = 0xEE;
    CHECK(pcm_fmt_for_rate(48000, &fmt) == OS32_ERR_INVAL && fmt == 0xEE);
    CHECK(pcm_fmt_for_rate(0, &fmt) == OS32_ERR_INVAL && fmt == 0xEE);
    CHECK(pcm_fmt_for_rate(44099, &fmt) == OS32_ERR_INVAL && fmt == 0xEE);

    /* 半周期。期待値は**数で書く** (実装と同じ式で作らない) */
    CHECK(pcm_half_ms(PCM_RATE_44100) == 46);
    CHECK(pcm_half_ms(PCM_RATE_22050) == 92);
    /* 番犬 = 2 半周期のマイクロ秒 */
    CHECK(pcm_watchdog_us(PCM_RATE_44100) == 92879);
    CHECK(pcm_watchdog_us(PCM_RATE_22050) == 185759);
}

static void case_close_dl(void)
{
    /* staged = 0 は票の数そのもの */
    CHECK(pcm_close_ticks(0, PCM_RATE_44100) == 17);
    CHECK(pcm_close_ticks(0, PCM_RATE_22050) == 31);
    /* 1 frame でも 1 半分ぶん増える (ceil) */
    CHECK(pcm_close_ticks(1, PCM_RATE_44100) == 22);
    CHECK(pcm_close_ticks(2048, PCM_RATE_44100) == 22);
    CHECK(pcm_close_ticks(2049, PCM_RATE_44100) == 27);
    CHECK(pcm_close_ticks(4096, PCM_RATE_44100) == 27);
    CHECK(pcm_close_ticks(4096, PCM_RATE_22050) == 50);
    /* 期限は必ず停止確認の 3 tick を含む */
    CHECK(pcm_close_ticks(0, PCM_RATE_44100) > PCM_STOP_TICKS);
}

static void case_vol(void)
{
    u8 r = 0xEE;

    CHECK(pcm_vol_reg(100, &r) == 0 && r == 0x00);        /* 0dB */
    CHECK(pcm_vol_reg(1, &r) == 0 && r == 0x3F);          /* 最大減衰 */
    CHECK(pcm_vol_reg(0, &r) == 0 && r == (PCM_DA_MUTE | 0x3F));  /* ミュート */
    CHECK(pcm_vol_reg(50, &r) == 0 && r == 31);
    CHECK(pcm_vol_reg(99, &r) == 0 && r == 0);
    /* 1〜100 でミュートは立たない */
    CHECK(pcm_vol_reg(1, &r) == 0 && (r & PCM_DA_MUTE) == 0);
    r = 0xEE;
    CHECK(pcm_vol_reg(101, &r) == OS32_ERR_INVAL && r == 0xEE);
    CHECK(pcm_vol_reg(0xFFFFFFFFu, &r) == OS32_ERR_INVAL && r == 0xEE);
}

static void case_stg(void)
{
    struct pcm_stg s;
    u32 off, n, a, b;

    memset(&s, 0, sizeof(s));
    CHECK(pcm_stg_free(&s) == PCM_STG_FRAMES);

    /* 予約は空きで頭打ち。公開するまで staged は動かない */
    n = pcm_stg_reserve(&s, 100, &off);
    CHECK(n == 100 && off == 0 && s.staged == 0);
    pcm_stg_publish(&s, n);
    CHECK(s.staged == 100 && s.w == 100);
    CHECK(pcm_stg_free(&s) == PCM_STG_FRAMES - 100);

    n = pcm_stg_reserve(&s, PCM_STG_FRAMES, &off);
    CHECK(n == PCM_STG_FRAMES - 100 && off == 100);
    pcm_stg_publish(&s, n);
    CHECK(s.staged == PCM_STG_FRAMES && pcm_stg_free(&s) == 0);
    CHECK(pcm_stg_reserve(&s, 1, &off) == 0);           /* 満杯 */

    /* **消費し切った直後の空きは 4096** (w == r を「空」と「満杯」の
     * 両方に使うと 0 に見える) */
    n = pcm_stg_consume(&s, PCM_STG_FRAMES, &off);
    CHECK(n == PCM_STG_FRAMES && off == 0 && s.staged == 0);
    CHECK(pcm_stg_free(&s) == PCM_STG_FRAMES);
    CHECK(s.w == 0 && s.r == 0);

    /* 2047 + 2 + 消費 + 4096 の反例: 物理末尾を跨ぐ予約が 2 分割になる */
    memset(&s, 0, sizeof(s));
    pcm_stg_publish(&s, 2047);
    n = pcm_stg_consume(&s, 2047, &off);
    CHECK(n == 2047 && off == 0 && s.r == 2047 && s.w == 2047);
    n = pcm_stg_reserve(&s, 2, &off);
    CHECK(n == 2 && off == 2047);
    pcm_stg_publish(&s, 2);
    CHECK(s.w == 2049 && s.staged == 2);
    n = pcm_stg_consume(&s, 2, &off);
    CHECK(n == 2 && off == 2047 && s.staged == 0);
    n = pcm_stg_reserve(&s, PCM_STG_FRAMES, &off);
    CHECK(n == PCM_STG_FRAMES && off == 2049);
    pcm_split(off, n, &a, &b);
    CHECK(a == PCM_STG_FRAMES - 2049 && b == 2049 && a + b == n);
    pcm_stg_publish(&s, n);
    CHECK(s.w == 2049 && s.staged == PCM_STG_FRAMES);

    /* 分割: 跨がないときは 2 本目が 0 */
    pcm_split(0, 10, &a, &b);
    CHECK(a == 10 && b == 0);
    pcm_split(PCM_STG_FRAMES - 1, 1, &a, &b);
    CHECK(a == 1 && b == 0);
    pcm_split(PCM_STG_FRAMES - 1, 3, &a, &b);
    CHECK(a == 1 && b == 2);
    pcm_split(4094, 4, &a, &b);
    CHECK(a == 2 && b == 2);
}

static void case_pack(void)
{
    CHECK(pcm_counters_pack(0, 0, 0) == 0);
    CHECK(pcm_counters_pack(1, 2, 3) == 0x01020003u);
    CHECK(pcm_counters_pack(255, 255, 65535) == 0xFFFFFFFFu);
    /* 飽和 */
    CHECK(pcm_counters_pack(256, 300, 70000) == 0xFFFFFFFFu);
    CHECK(pcm_counters_pack(0, 0, 65536) == 0x0000FFFFu);
}

static void case_pos(void)
{
    CHECK(pcm_pos_frames(PCM_RING_BYTES) == 0);
    CHECK(pcm_pos_frames(PCM_RING_BYTES - 4) == 1);
    CHECK(pcm_pos_frames(PCM_RING_BYTES - 8192) == 2048);
    CHECK(pcm_pos_frames(4) == 4095);
    CHECK(pcm_pos_frames(0) == 0);
}

/* ======================================================================== */
/*  5. pcm_obs — 観測の統合 (装置には触らない)                              */
/* ======================================================================== */
static void obs_init(struct pcm_core *c, int state, u32 staged)
{
    memset(c, 0, sizeof(*c));
    c->state = (u8)state;
    c->rate = PCM_RATE_44100;
    c->filled[0] = PCM_HALF_FRAMES;
    c->filled[1] = PCM_HALF_FRAMES;
    c->stg.staged = staged;
    c->last_data_half = 1;
    c->drain = PCM_DRAIN_WAIT;
}

static void case_obs(void)
{
    struct pcm_core c;
    struct pcm_act act;

    /* (1) 同じ半分で進むだけ: 補充も切り替えも無い */
    obs_init(&c, PCM_ST_RUNNING, 0);
    pcm_obs(&c, 1, 100, 1000, &act);
    CHECK(act.refill == 0 && act.entered == 0);
    CHECK(c.pos == 100 && c.half == 0 && c.gen == 0);
    CHECK(c.last_progress == 1000 && c.state == PCM_ST_RUNNING);

    /* (2) 切り替え: 消費し終えた **old** を補充する */
    obs_init(&c, PCM_ST_RUNNING, PCM_HALF_FRAMES);
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(act.refill == 1 && act.to_half == 0 && act.frames == PCM_HALF_FRAMES);
    CHECK(c.gen == 1 && c.half == 1 && c.filled[0] == PCM_HALF_FRAMES);
    CHECK(c.underruns == 0);           /* 両半分満杯なので underrun は 0 */
    CHECK(c.last_data_half == 0);
    CHECK(c.stg.staged == 0);

    /* (3) underrun は**消す前に**数える (移った先が満杯でなかった) */
    obs_init(&c, PCM_ST_RUNNING, 0);
    c.filled[1] = 10;
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(c.underruns == 1);
    CHECK(act.refill == 1 && act.frames == 0);   /* ステージングは空 */
    CHECK(c.last_data_half == 1);                /* 0 frame では動かさない */

    /* (4) 補充の余裕が無い切り替え = 喪失扱い → RS_STOP */
    obs_init(&c, PCM_ST_RUNNING, PCM_HALF_FRAMES);
    pcm_obs(&c, 1, 2048 + 1600, 1000, &act);     /* 末尾まで 448 frame */
    CHECK(act.refill == 0 && act.entered == 1);
    CHECK(c.repeats == 1 && c.state == PCM_ST_RS_STOP);

    /* (5) 遅れた観測 (同じ半分で p1 < p0) = 喪失 → RS_STOP */
    obs_init(&c, PCM_ST_RUNNING, 0);
    c.pos = 2000;
    pcm_obs(&c, 1, 3, 1000, &act);
    CHECK(c.repeats == 1 && c.state == PCM_ST_RS_STOP && act.entered == 1);
    CHECK(c.pos == 2000);                        /* 位置は更新しない */

    /* (5') DRAINING での喪失は drain 失敗を記録して STOP_REQ */
    obs_init(&c, PCM_ST_DRAINING, 0);
    c.pos = 2000;
    pcm_obs(&c, 1, 3, 1000, &act);
    CHECK(c.state == PCM_ST_STOP_REQ && c.drain_failed == 1);

    /* (6) -EAGAIN では位置を更新しないが**番犬は動く** */
    obs_init(&c, PCM_ST_RUNNING, 0);
    c.pos = 77;
    c.half = 0;
    c.last_progress = 0;
    pcm_obs(&c, 0, 0, 1000, &act);
    CHECK(c.pos == 77 && c.half == 0 && c.state == PCM_ST_RUNNING);
    CHECK(c.now == 1000 && c.last_progress == 0);
    pcm_obs(&c, 0, 0, 92880, &act);              /* 2 半周期を超えた */
    CHECK(c.state == PCM_ST_RS_STOP && act.entered == 1);

    /* (7) 進行の番犬: 同じ位置が続いても last_progress は動かない */
    obs_init(&c, PCM_ST_RUNNING, 0);
    c.pos = 500;
    c.last_progress = 0;
    pcm_obs(&c, 1, 500, 92879, &act);            /* ちょうどは発火しない */
    CHECK(c.state == PCM_ST_RUNNING);
    pcm_obs(&c, 1, 500, 92880, &act);
    CHECK(c.state == PCM_ST_RS_STOP);

    /* (8) drain: 「出た」+ staged > 0 は完了しない */
    obs_init(&c, PCM_ST_DRAINING, 100);
    c.drain = PCM_DRAIN_SIL;
    c.last_data_half = 1;
    pcm_obs(&c, 1, 2048, 1000, &act);            /* 半分 0 → 1 の切り替え */
    CHECK(c.drain == PCM_DRAIN_WAIT);            /* 補充したので段階が戻る */
    CHECK(c.state == PCM_ST_DRAINING);
    CHECK(act.frames == 100 && c.last_data_half == 0);

    /* (8') 「出た」+ staged == 0 で完了 → STOP_REQ */
    obs_init(&c, PCM_ST_DRAINING, 0);
    c.drain = PCM_DRAIN_SIL;
    c.last_data_half = 1;
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(c.drain == PCM_DRAIN_OUT);
    CHECK(c.state == PCM_ST_STOP_REQ && act.entered == 1);

    /* (9) 0 frame の補充は last_data_half を動かさない (段階が進む) */
    obs_init(&c, PCM_ST_DRAINING, 0);
    c.drain = PCM_DRAIN_WAIT;
    c.last_data_half = 1;
    pcm_obs(&c, 1, 2048, 1000, &act);            /* h1 = 1 == last_data_half */
    CHECK(c.last_data_half == 1 && c.drain == PCM_DRAIN_IN);

    /* (10) DRAINING の末尾の 0 埋めは underrun に数えない */
    obs_init(&c, PCM_ST_DRAINING, 0);
    c.filled[1] = 0;
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(c.underruns == 0);

    /* (10') drain の段階は RUNNING でも追うが、**完了の遷移は DRAINING だけ**
     * (往復 6 R4)。RUNNING で「出た」に達しても再生は続く。 */
    obs_init(&c, PCM_ST_RUNNING, 0);
    c.drain = PCM_DRAIN_SIL;
    c.last_data_half = 1;
    c.filled[0] = 0;
    c.filled[1] = 0;
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(c.drain == PCM_DRAIN_OUT);
    CHECK(c.state == PCM_ST_RUNNING && act.entered == 0);

    /* (11) 1 回の観測で drain は 1 段だけ */
    obs_init(&c, PCM_ST_DRAINING, 0);
    c.drain = PCM_DRAIN_WAIT;
    c.last_data_half = 0;
    pcm_obs(&c, 1, 2048, 1000, &act);            /* h1 = 1 != 0 → WAIT のまま */
    CHECK(c.drain == PCM_DRAIN_WAIT);
}

/* write(1 frame) → close の丸ごと 1 周 (純粋部だけで追う) */
static void case_drain_short(void)
{
    struct pcm_core c;
    struct pcm_act act;
    u32 n0, n1, lh;
    int ds;

    memset(&c, 0, sizeof(c));
    c.rate = PCM_RATE_44100;
    c.stg.staged = 1;
    pcm_start_plan(c.stg.staged, &n0, &n1, &lh, &ds);
    CHECK(n0 == 1 && n1 == 0 && lh == 0 && ds == PCM_DRAIN_IN);
    (void)pcm_stg_consume(&c.stg, n0, &n1);
    c.filled[0] = 1;
    c.filled[1] = 0;
    c.last_data_half = (u8)lh;
    c.drain = (u8)ds;
    c.state = PCM_ST_DRAINING;

    /* 半分 0 → 1: 半分 0 を 0 で埋め、段階は「読んでいる」→「無音」 */
    pcm_obs(&c, 1, 2048, 1000, &act);
    CHECK(act.refill == 1 && act.to_half == 0 && act.frames == 0);
    CHECK(c.drain == PCM_DRAIN_SIL && c.state == PCM_ST_DRAINING);

    /* 半分 1 → 0: 「無音」から出たので完了 */
    pcm_obs(&c, 1, 0, 50000, &act);
    CHECK(c.drain == PCM_DRAIN_OUT);
    CHECK(c.state == PCM_ST_STOP_REQ && act.entered == 1);
    CHECK(c.drain_failed == 0);
}

/* ======================================================================== */
/*  6. 列 (ポート書きの順序)                                                 */
/* ======================================================================== */
static int seq_eq(const struct pcm_op *a, const struct pcm_op *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i].kind != b[i].kind || a[i].a != b[i].a || a[i].b != b[i].b)
            return 0;
    }
    return 1;
}

/* 表の中で kind/a の組が最初に出る位置。無ければ -1。 */
static int seq_find(const struct pcm_op *s, int n, u8 kind, u8 a)
{
    int i;
    for (i = 0; i < n; i++) if (s[i].kind == kind && s[i].a == a) return i;
    return -1;
}

static int seq_count(const struct pcm_op *s, int n, u8 kind)
{
    int i, c = 0;
    for (i = 0; i < n; i++) if (s[i].kind == kind) c++;
    return c;
}

static void case_seq(void)
{
    const struct pcm_op *s;
    struct pcm_op buf[PCM_SEQ_MAX];
    int n, i14, i15, i9, i24, i12, mask, drs, dl;

    /* --- open の前置き: 経路を先に結んでから装置レジスタを書く --- */
    n = pcm_seq_prologue(&s);
    {
        static const struct pcm_op want[] = {
            { PCM_OP_DMA_MASK, 0, 0 },
            { PCM_OP_PORT, PCM_POFS_ROUTE, PCM_ROUTE_INT41_DMA1 },
            { PCM_OP_PORT, PCM_POFS_R2, 0 },
            { PCM_OP_REG, PCM_I_IFACE, PCM_IFACE_CAL1 },
            { PCM_OP_REG, PCM_I_PINCTL, 0 }
        };
        CHECK(n == 5 && seq_eq(s, want, n));
    }
    /* 経路は装置レジスタより**先**。detach 状態で書かない (往復 1 B6) */
    CHECK(seq_find(s, n, PCM_OP_PORT, PCM_POFS_ROUTE) <
          seq_find(s, n, PCM_OP_REG, PCM_I_IFACE));

    /* --- 初期化列 --- */
    n = pcm_seq_init(PCM_FMT_22050, buf);
    CHECK(n == 17);
    CHECK(buf[3].kind == PCM_OP_REG &&
          buf[3].a == (PCM_R0_MCE | PCM_I_FMT) && buf[3].b == PCM_FMT_22050);
    n = pcm_seq_init(PCM_FMT_44100, buf);
    CHECK(buf[3].b == PCM_FMT_44100);

    /* I15 (下位) → I14 (上位) の順。**上位の書きが Current Count をロード** */
    i15 = seq_find(buf, n, PCM_OP_REG, PCM_I_BASE_LO);
    i14 = seq_find(buf, n, PCM_OP_REG, PCM_I_BASE_HI);
    CHECK(i15 >= 0 && i14 >= 0 && i15 < i14);
    CHECK(buf[i15].b == PCM_BASE_LO_2048 && buf[i14].b == PCM_BASE_HI_2048);

    /* I9 と I16 は **MCE を保ったまま** (bit6 が立っている) */
    i9 = seq_find(buf, n, PCM_OP_REG, (u8)(PCM_R0_MCE | PCM_I_IFACE));
    CHECK(i9 >= 0);
    CHECK(seq_find(buf, n, PCM_OP_REG, PCM_I_IFACE) < 0);  /* 素の I9 は無い */
    CHECK(seq_find(buf, n, PCM_OP_REG,
                   (u8)(PCM_R0_MCE | PCM_I_ALTFEAT1)) > i9);

    /* I24 (PI の ack) は **MODE2 を立てた後**。MODE1 では I8 に化ける */
    i12 = seq_find(buf, n, PCM_OP_REG, PCM_I_MODEID);
    i24 = seq_find(buf, n, PCM_OP_REG, PCM_I_ALTSTAT);
    CHECK(i12 >= 0 && i24 > i12);
    CHECK(buf[i12].b == PCM_MODE2);
    CHECK(seq_find(buf, n, PCM_OP_CHK_MODE2, 0) == i12 + 1);
    /* I16 も MODE2 の後 */
    CHECK(seq_find(buf, n, PCM_OP_REG, (u8)(PCM_R0_MCE | PCM_I_ALTFEAT1)) > i12);

    /* MCE を落とす R0 の直書きと、その**後の**再同期 + ACI 待ち */
    CHECK(seq_find(buf, n, PCM_OP_PORT, PCM_POFS_R0) > i9);
    CHECK(seq_find(buf, n, PCM_OP_WAIT_ACI, 0) >
          seq_find(buf, n, PCM_OP_PORT, PCM_POFS_R0));
    CHECK(seq_count(buf, n, PCM_OP_WAIT_INIT) == 3);
    /* DMA を積むのは Base を書く前 */
    CHECK(seq_find(buf, n, PCM_OP_DMA_SETUP, 0) < i15);

    /* --- pcm_start: 配ってから unmask、最後に PEN=1 --- */
    n = pcm_seq_start(&s);
    {
        static const struct pcm_op want[] = {
            { PCM_OP_RING_FILL, 0, 0 },
            { PCM_OP_ACK_TC, 0, 0 },
            { PCM_OP_REG, PCM_I_ALTSTAT, 0 },
            { PCM_OP_TIMEBASE, 0, 0 },
            { PCM_OP_DMA_UNMASK, 0, 0 },
            { PCM_OP_REG, PCM_I_IFACE, PCM_IFACE_CAL1 | PCM_IFACE_PEN }
        };
        CHECK(n == 6 && seq_eq(s, want, n));
    }

    /* --- 停止の入口: IEN=0 → PEN=0 → ack → 期限。**期限は入口だけ** --- */
    n = pcm_seq_stop_entry(&s);
    {
        static const struct pcm_op want[] = {
            { PCM_OP_REG, PCM_I_PINCTL, 0 },
            { PCM_OP_REG, PCM_I_IFACE, PCM_IFACE_CAL1 },
            { PCM_OP_PORT, PCM_POFS_R2, 0 },
            { PCM_OP_REG, PCM_I_ALTSTAT, 0 },
            { PCM_OP_DEADLINE, 0, PCM_STOP_TICKS }
        };
        CHECK(n == 5 && seq_eq(s, want, n));
    }
    dl = seq_find(s, n, PCM_OP_DEADLINE, 0);
    CHECK(dl >= 0);

    /* --- 後続の tick: **DRS を待ってから** mask。期限は再設定しない --- */
    n = pcm_seq_stop_tail(&s);
    drs = seq_find(s, n, PCM_OP_WAIT_DRS, 0);
    mask = seq_find(s, n, PCM_OP_DMA_MASK, 0);
    CHECK(n == 2 && drs == 0 && mask == 1);
    CHECK(seq_find(s, n, PCM_OP_DEADLINE, 0) < 0);

    /* --- RS_RESTART: mask 中に組み直し、unmask → PEN → IEN --- */
    n = pcm_seq_restart(&s);
    CHECK(n == 12);
    CHECK(s[0].kind == PCM_OP_DMA_MASK);
    CHECK(seq_find(s, n, PCM_OP_RING_CLEAR, 0) < seq_find(s, n, PCM_OP_DMA_SETUP, 0));
    i15 = seq_find(s, n, PCM_OP_REG, PCM_I_BASE_LO);
    i14 = seq_find(s, n, PCM_OP_REG, PCM_I_BASE_HI);
    CHECK(i15 >= 0 && i14 > i15);
    CHECK(seq_find(s, n, PCM_OP_DMA_SETUP, 0) < i15);
    CHECK(seq_find(s, n, PCM_OP_RING_FILL, 0) > i14);
    CHECK(seq_find(s, n, PCM_OP_DMA_UNMASK, 0) >
          seq_find(s, n, PCM_OP_TIMEBASE, 0));
    CHECK(s[n - 2].kind == PCM_OP_REG && s[n - 2].a == PCM_I_IFACE &&
          s[n - 2].b == (PCM_IFACE_CAL1 | PCM_IFACE_PEN));
    CHECK(s[n - 1].kind == PCM_OP_REG && s[n - 1].a == PCM_I_PINCTL &&
          s[n - 1].b == PCM_PINCTL_IEN);

    /* --- reclaim の abort: **DRS を待たずに** mask する --- */
    n = pcm_seq_abort(&s);
    CHECK(n == 5);
    CHECK(seq_find(s, n, PCM_OP_WAIT_DRS, 0) < 0);
    CHECK(s[n - 1].kind == PCM_OP_DMA_MASK);
    CHECK(seq_find(s, n, PCM_OP_REG, PCM_I_PINCTL) == 0);
}

/* ======================================================================== */
/*  7. 状態機械 (実物の driver を模型の上で回す)                            */
/* ======================================================================== */
static int open_ok(u32 rate)
{
    reset_all();
    return pcm_open(rate);
}

static void fill_staging(u32 frames)
{
    static u8 src[PCM_STG_BYTES];
    int rc;

    rc = pcm_write(src, frames * PCM_FRAME_BYTES);
    CHECK(rc == (int)(frames * PCM_FRAME_BYTES));
}

static void case_open(void)
{
    u32 freeb = 0, cnt = 0xEE;

    /* 検出できれば OPEN。経路が書かれ、IRQ が結ばれ、プールを 2 つ取る */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    CHECK(g_pcm.state == PCM_ST_OPEN);
    CHECK(dev_route == PCM_ROUTE_INT41_DMA1);
    CHECK(sim_irq_registered == 1);
    CHECK(pool_used[0] == 1 && pool_used[1] == 0 && heap_used == 1);
    CHECK(dev_reg[PCM_I_FMT] == PCM_FMT_44100);
    CHECK((dev_reg[PCM_I_MODEID] & PCM_MODE2) != 0);
    CHECK(dev_reg[PCM_I_PINCTL] == PCM_PINCTL_IEN);
    CHECK((dev_reg[PCM_I_IFACE] & PCM_IFACE_PEN) == 0);
    CHECK(dev_reg[PCM_I_ALTFEAT1] == PCM_ALT1_DACZ);
    CHECK(dev_reg[PCM_I_BASE_LO] == PCM_BASE_LO_2048);
    CHECK(dev_reg[PCM_I_BASE_HI] == PCM_BASE_HI_2048);
    CHECK(sim_setup_calls == 1 && sim_masked == 1);
    CHECK(pcm_shim_irq_depth == 0);          /* irq_save の釣り合い */

    /* 2 度目は BUSY (資源が満杯) */
    CHECK(pcm_open(PCM_RATE_44100) == PCM_ERR_BUSY);

    /* status は空きと 0 のカウンタ */
    CHECK(pcm_status(&freeb, &cnt) == 0);
    CHECK(freeb == PCM_STG_BYTES && cnt == 0);

    /* rate が違えば INVAL。装置には触らない */
    reset_all();
    trace_reset();
    CHECK(pcm_open(48000) == OS32_ERR_INVAL);
    CHECK(IO_ACCESSES == 0);
    CHECK(g_pcm.state == PCM_ST_CLOSED);

    /* 装置が無ければ NOSYS。プールも IRQ も取らない */
    reset_all();
    dev_present = 0;
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_NOSYS);
    CHECK(g_pcm.state == PCM_ST_CLOSED);
    CHECK(pool_used[0] == 0 && sim_irq_registered == 0);

    /* I25 が知らない版でも NOSYS */
    reset_all();
    dev_reg[PCM_I_VERSION] = 0x20;
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_NOSYS);

    /* I12 の ID が違えば NOSYS */
    reset_all();
    dev_reg[PCM_I_MODEID] = 0x05;
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_NOSYS);

    /* プールが取れなければ NOMEM。**IRQ は結ばない** */
    reset_all();
    pool_fail = 1;
    CHECK(pcm_open(PCM_RATE_44100) == PCM_ERR_NOMEM);
    CHECK(sim_irq_registered == 0 && g_pcm.state == PCM_ST_CLOSED);

    /* KHEAP が取れなくても NOMEM。**リングは返す** */
    reset_all();
    heap_fail = 1;
    CHECK(pcm_open(PCM_RATE_44100) == PCM_ERR_NOMEM);
    CHECK(sim_irq_registered == 0 && g_pcm.state == PCM_ST_CLOSED);
    CHECK(pool_used[0] == 0 && pool_freed[0] == 1);

    /* IRQ に結べなければ BUSY。**プールは返す** */
    reset_all();
    sim_irq_fail = 1;
    CHECK(pcm_open(PCM_RATE_44100) == PCM_ERR_BUSY);
    CHECK(pool_freed[0] == 1 && heap_freed == 1);
    CHECK(pool_used[0] == 0 && heap_used == 0);
    CHECK(g_pcm.state == PCM_ST_CLOSED);

    /* 初期化が期限切れなら IO。逆順に巻き戻す (IRQ も解除、プールも返す) */
    reset_all();
    sim_tick_per_read = 1;
    dev_init_busy = 1;                       /* R0 が 0x80 のまま */
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_NOSYS);   /* 検出で落ちる */
    CHECK(sim_irq_registered == 0);
    CHECK(dev_route == PCM_ROUTE_INT41_DMA1);            /* detach しない */

    /* 検出は通るのに ACI が落ちないなら IO。**逆順に巻き戻す** */
    reset_all();
    sim_tick_per_read = 1;
    dev_reg[PCM_I_ERRSTAT] = PCM_ERR_ACI;
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_IO);
    CHECK(sim_irq_unreg_calls == 1 && sim_irq_registered == 0);
    CHECK(pool_freed[0] == 1 && heap_freed == 1);
    CHECK(g_pcm.state == PCM_ST_CLOSED);
    CHECK(dev_route == PCM_ROUTE_INT41_DMA1);
}

static void case_write(void)
{
    static u8 src[PCM_STG_BYTES + 64];
    u32 freeb = 0;
    int rc;

    CHECK(open_ok(PCM_RATE_44100) == 0);

    /* frame の倍数に切り捨てる */
    CHECK(pcm_write(src, 3) == 0);
    CHECK(pcm_write(src, 7) == 4);
    CHECK(g_pcm.stg.staged == 1);
    CHECK(pcm_status(&freeb, (u32 *)0) == 0);
    CHECK(freeb == (PCM_STG_FRAMES - 1) * PCM_FRAME_BYTES);

    /* 満杯を作った write が **その場で開始する** (往復 6 R7) */
    rc = pcm_write(src, (PCM_STG_FRAMES - 1) * PCM_FRAME_BYTES);
    CHECK(rc == (int)((PCM_STG_FRAMES - 1) * PCM_FRAME_BYTES));
    CHECK(g_pcm.state == PCM_ST_RUNNING);
    CHECK(sim_masked == 0);
    CHECK((dev_reg[PCM_I_IFACE] & PCM_IFACE_PEN) != 0);
    CHECK(g_pcm.filled[0] == PCM_HALF_FRAMES && g_pcm.filled[1] == PCM_HALF_FRAMES);
    CHECK(g_pcm.stg.staged == 0);
    CHECK(g_pcm.last_data_half == 1);

    /* 満杯なら 0 を返す (アプリは sys_yield して再試行) */
    fill_staging(PCM_STG_FRAMES);
    CHECK(pcm_write(src, 4) == 0);

    /* owner が違えば INVAL */
    sim_owner = 7;
    CHECK(pcm_write(src, 4) == OS32_ERR_INVAL);
    CHECK(pcm_status(&freeb, (u32 *)0) == OS32_ERR_INVAL);
    CHECK(pcm_close() == OS32_ERR_INVAL);
    sim_owner = 1;

    /* 未 open では受けない */
    reset_all();
    CHECK(pcm_write(src, 4) == OS32_ERR_INVAL);
    CHECK(pcm_status(&freeb, (u32 *)0) == OS32_ERR_INVAL);
}

/* 実物の driver を 1 tick 進める (装置の位置を置いてから) */
static void step(u32 pos_frames, u32 ticks)
{
    sim_set_pos(pos_frames);
    sim_advance_time(ticks);
    pcm_tick();
}

static void case_close(void)
{
    static u8 src[PCM_STG_BYTES];

    /* (a) OPEN で未転送なし → 再生せずに解放 */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    CHECK(pcm_close() == 0);
    CHECK(g_pcm.state == PCM_ST_CLOSED);
    CHECK(pool_freed[0] == 1 && heap_freed == 1);
    CHECK(sim_irq_unreg_calls == 1);
    CHECK(sim_irq_registered == 0);

    /* (b) write(1 frame) → close: 開始して drain し、STOP_DONE で解放 */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    CHECK(pcm_write(src, PCM_FRAME_BYTES) == (int)PCM_FRAME_BYTES);
    CHECK(g_pcm.state == PCM_ST_OPEN);

    /* close は止まるまで待つので、待ちのあいだに tick を回す模型が要る。
     * ここでは状態機械を手で進めてから close の後始末だけを確かめる。 */
    g_pcm.close_pending = 1;
    pcm_start();
    CHECK(g_pcm.state == PCM_ST_DRAINING);
    CHECK(g_pcm.filled[0] == 1 && g_pcm.filled[1] == 0);
    CHECK(g_pcm.last_data_half == 0 && g_pcm.drain == PCM_DRAIN_IN);

    step(2048, 1);                       /* 半分 0 → 1 */
    CHECK(g_pcm.drain == PCM_DRAIN_SIL && g_pcm.state == PCM_ST_DRAINING);
    step(0, 1);                          /* 半分 1 → 0: 出た */
    CHECK(g_pcm.state == PCM_ST_STOP_REQ);
    CHECK(dev_reg[PCM_I_PINCTL] == 0);
    CHECK((dev_reg[PCM_I_IFACE] & PCM_IFACE_PEN) == 0);

    /* DRS が落ちるまで mask しない */
    dev_reg[PCM_I_ERRSTAT] = PCM_ERR_DRS;
    step(0, 1);
    CHECK(sim_masked == 0);
    CHECK(g_pcm.state == PCM_ST_STOP_REQ);
    dev_reg[PCM_I_ERRSTAT] = 0;
    step(0, 1);
    CHECK(sim_masked == 1);
    CHECK(g_pcm.state == PCM_ST_STOP_DONE);

    CHECK(pcm_close() == 0);
    CHECK(g_pcm.state == PCM_ST_CLOSED);
    CHECK(pool_freed[0] == 1 && heap_freed == 1);

    /* (c) DRS が落ちないまま期限切れ → FAULTED → leaked、再 open は IO */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    fill_staging(PCM_STG_FRAMES);
    CHECK(g_pcm.state == PCM_ST_RUNNING);
    g_pcm.state = PCM_ST_STOP_REQ;
    s_seq_at = 0;
    s_deadline = tick_count + PCM_STOP_TICKS;
    dev_reg[PCM_I_ERRSTAT] = PCM_ERR_DRS;
    step(0, PCM_STOP_TICKS + 1);
    CHECK(g_pcm.state == PCM_ST_FAULTED);
    CHECK(pcm_close() == OS32_ERR_IO);
    CHECK(pool_leaked[0] == 1 && heap_freed == 1);
    CHECK(pool_freed[0] == 0);
    CHECK(pcm_open(PCM_RATE_44100) == OS32_ERR_IO);   /* 再起動まで断る */
}

static void case_rs(void)
{
    /* 連続性の喪失 → RS_STOP → DRS を見て mask → RS_RESTART → RUNNING */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    fill_staging(PCM_STG_FRAMES);
    CHECK(g_pcm.state == PCM_ST_RUNNING);

    step(2000, 1);
    CHECK(g_pcm.state == PCM_ST_RUNNING);
    step(3, 1);                              /* 同じ半分で戻った */
    CHECK(g_pcm.state == PCM_ST_RS_STOP && g_pcm.repeats == 1);
    CHECK(dev_reg[PCM_I_PINCTL] == 0);

    step(3, 1);                              /* DRS = 0 なのでそのまま進む */
    CHECK(g_pcm.state == PCM_ST_RS_RESTART);
    CHECK(sim_masked == 1);

    pcm_tick();                              /* 組み直し */
    CHECK(g_pcm.state == PCM_ST_RUNNING);
    CHECK(g_pcm.resyncs == 1);
    CHECK(sim_masked == 0);
    CHECK(sim_setup_calls == 2);
    CHECK(dev_reg[PCM_I_PINCTL] == PCM_PINCTL_IEN);
    CHECK((dev_reg[PCM_I_IFACE] & PCM_IFACE_PEN) != 0);
    CHECK(g_pcm.half == 0 && g_pcm.pos == 0);

    /* RS_STOP の期限切れは STOP_REQ (失敗) へ */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    fill_staging(PCM_STG_FRAMES);
    g_pcm.state = PCM_ST_RS_STOP;
    s_seq_at = 0;
    s_deadline = tick_count + PCM_STOP_TICKS;
    dev_reg[PCM_I_ERRSTAT] = PCM_ERR_DRS;
    step(0, PCM_STOP_TICKS + 1);
    CHECK(g_pcm.state == PCM_ST_STOP_REQ);
    CHECK(g_pcm.drain_failed == 1);
}

/* 入口ガード: **装置アクセスが 0 回** (Index 書きも含めて) */
static void case_guard(void)
{
    int i;
    static const int quiet[] = { PCM_ST_CLOSED, PCM_ST_OPENING, PCM_ST_FAULTED };

    for (i = 0; i < 3; i++) {
        CHECK(open_ok(PCM_RATE_44100) == 0);
        g_pcm.state = (u8)quiet[i];
        trace_reset();
        pcm_tick();
        CHECK(IO_ACCESSES == 0);
        CHECK(trace_n == 0);
        CHECK(pcm_advance() == IRQ_NONE);
        CHECK(IO_ACCESSES == 0);
    }

    /* MCE の列の途中も同じ */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    g_pcm.state = PCM_ST_RUNNING;
    s_mce_busy = 1;
    trace_reset();
    pcm_tick();
    CHECK(IO_ACCESSES == 0);
    s_mce_busy = 0;

    /* PI があれば handled、無ければ NONE (受理の判定にだけ使う) */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    g_pcm.state = PCM_ST_OPEN;
    dev_reg[PCM_I_ALTSTAT] = 0;
    CHECK(pcm_advance() == IRQ_NONE);
    dev_reg[PCM_I_ALTSTAT] = PCM_ALT_PI;
    CHECK(pcm_advance() == IRQ_HANDLED);
    CHECK((dev_reg[PCM_I_ALTSTAT] & PCM_ALT_PI) == 0);   /* 消した */
    CHECK(pcm_advance() == IRQ_NONE);
}

/* reclaim は**どの状態からでも 1 度だけ**解放する */
static void case_reclaim(void)
{
    int i;
    static const int states[] = {
        PCM_ST_OPEN, PCM_ST_RUNNING, PCM_ST_DRAINING, PCM_ST_RS_STOP,
        PCM_ST_RS_RESTART, PCM_ST_STOP_REQ, PCM_ST_STOP_DONE
    };

    for (i = 0; i < (int)(sizeof(states) / sizeof(states[0])); i++) {
        CHECK(open_ok(PCM_RATE_44100) == 0);
        fill_staging(PCM_HALF_FRAMES);
        g_pcm.state = (u8)states[i];
        pcm_reclaim(1);
        CHECK(g_pcm.state == PCM_ST_CLOSED);
        CHECK(pool_freed[0] == 1 && heap_freed == 1);
        CHECK(sim_irq_unreg_calls == 1);
        /* 2 度目は何もしない (**1 度だけ**の契約) */
        pcm_reclaim(1);
        CHECK(pool_freed[0] == 1 && heap_freed == 1);
        CHECK(sim_irq_unreg_calls == 1);
        if (states[i] != PCM_ST_OPEN && states[i] != PCM_ST_STOP_DONE) {
            CHECK(sim_masked == 1);
            CHECK(dev_reg[PCM_I_PINCTL] == 0);
            CHECK((dev_reg[PCM_I_IFACE] & PCM_IFACE_PEN) == 0);
        }
    }

    /* CLOSED からは何もしない */
    reset_all();
    pcm_reclaim(1);
    CHECK(pool_freed[0] == 0 && sim_irq_unreg_calls == 0);

    /* 他人の ID では動かない */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    pcm_reclaim(9);
    CHECK(g_pcm.state == PCM_ST_OPEN && pool_freed[0] == 0);
    pcm_reclaim(0);
    CHECK(g_pcm.state == PCM_ST_OPEN);

    /* FAULTED からは leaked にして返す */
    CHECK(open_ok(PCM_RATE_44100) == 0);
    g_pcm.state = PCM_ST_FAULTED;
    pcm_reclaim(1);
    CHECK(pool_leaked[0] == 1 && heap_freed == 1);
    CHECK(pool_freed[0] == 0);
    CHECK(sim_irq_unreg_calls == 1);
}

/* 音量とミュートは I6/I7 の両方へ */
static void case_volume(void)
{
    CHECK(open_ok(PCM_RATE_44100) == 0);
    CHECK(pcm_set_volume(100) == 0);
    CHECK(dev_reg[PCM_I_LDA] == 0 && dev_reg[PCM_I_RDA] == 0);
    CHECK(pcm_set_volume(1) == 0);
    CHECK(dev_reg[PCM_I_LDA] == 0x3F && dev_reg[PCM_I_RDA] == 0x3F);
    CHECK(pcm_set_volume(0) == 0);
    CHECK((dev_reg[PCM_I_LDA] & PCM_DA_MUTE) != 0);
    CHECK((dev_reg[PCM_I_RDA] & PCM_DA_MUTE) != 0);
    CHECK(pcm_set_volume(101) == OS32_ERR_INVAL);
    sim_owner = 5;
    CHECK(pcm_set_volume(50) == OS32_ERR_INVAL);
    sim_owner = 1;
}

/* 起動時の検出だけ (pcm_init)。無い機械でも静かに CLOSED のまま */
static void case_init(void)
{
    reset_all();
    pcm_init();
    CHECK(pcm_present() == 1);
    CHECK(pcm_state() == PCM_ST_CLOSED);

    reset_all();
    dev_present = 0;
    pcm_init();
    CHECK(pcm_present() == 0);
    CHECK(pcm_state() == PCM_ST_CLOSED);
    /* 装置が無いのに tick が装置を触らない */
    trace_reset();
    pcm_tick();
    CHECK(IO_ACCESSES == 0);
}

/* ======================================================================== */
/*  8. 入口                                                                  */
/* ======================================================================== */
struct case_ent { const char *name; void (*fn)(void); };

static const struct case_ent cases[] = {
    { "cont",        case_cont },
    { "refill",      case_refill },
    { "drain",       case_drain },
    { "drain_short", case_drain_short },
    { "start",       case_start },
    { "rate",        case_rate },
    { "close_dl",    case_close_dl },
    { "vol",         case_vol },
    { "stg",         case_stg },
    { "pack",        case_pack },
    { "pos",         case_pos },
    { "obs",         case_obs },
    { "seq",         case_seq },
    { "open",        case_open },
    { "write",       case_write },
    { "close",       case_close },
    { "rs",          case_rs },
    { "guard",       case_guard },
    { "reclaim",     case_reclaim },
    { "volume",      case_volume },
    { "init",        case_init }
};

int main(int argc, char **argv)
{
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            reset_all();
            failed = 0;
            cases[i].fn();
            return failed ? 1 : 0;
        }
    }
    fprintf(stderr, "unknown case: %s\n", argv[1]);
    return 2;
}
