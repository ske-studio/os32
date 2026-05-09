/* ======================================================================== */
/*  V86_PIT.C — V86 PIT (8253A互換TCU) 仮想化                              */
/*                                                                          */
/*  PC-98 PIT ポートアドレス:                                               */
/*    Counter#0: 0x71 (R/W) — タイマ割り込み (100Hz)                       */
/*    Counter#1: 0x73 (R/W) — ビープ音周波数                               */
/*    Counter#2: 0x75 (R/W) — RS-232C用 (通常は触らない)                   */
/*    コマンド:  0x77 (W)   — モード設定                                    */
/*                                                                          */
/*  重複ポート (3FD9h/3FDBh/3FDDh/3FDFh) もトラップ対象。                   */
/* ======================================================================== */

#include "v86_pit.h"
#include "io.h"

/* OS32のグローバルtickカウント (isr_stub.asm で100Hzインクリメント) */
extern volatile u32 tick_count;

/* カウンタ仮想ステート */
struct pit_counter {
    u8  mode;           /* カウンタモード (0-5) */
    u8  rw_mode;        /* R/Wモード: 1=LSBのみ, 2=MSBのみ, 3=LSB→MSB */
    u16 reload_value;   /* リロード値 */
    u8  write_phase;    /* 書き込みフェーズ (0=LSB待ち, 1=MSB待ち) */
    u8  read_phase;     /* 読み出しフェーズ (0=LSB, 1=MSB) */
    u8  latched;        /* ラッチ済みフラグ */
    u16 latch_value;    /* ラッチされた値 */
    u32 last_tick;      /* 最後にリロードした時のtick_count */
};

static struct pit_counter counters[3];

/* PC-98デフォルト: 100Hz
 * UNDOCUMENTED io_tcu.md:
 *   5M/10MHz系 (BDA 0501h bit7=0) → 入力クロック 1.9968MHz → 0x4E00
 *   8MHz系     (BDA 0501h bit7=1) → 入力クロック 2.4576MHz → 0x6000
 * NP21/Wは5M/10MHz系相当 (BDA 0501h=0x24, bit7=0) のため 0x4E00 を使用 */
#define DEFAULT_RELOAD  0x4E00

/* ====================================================================== */
/*  v86_pit_init — PIT仮想化初期化                                         */
/* ====================================================================== */
void v86_pit_init(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        counters[i].mode = 3;        /* mode3: 方形波 */
        counters[i].rw_mode = 3;     /* LSB→MSB */
        counters[i].reload_value = DEFAULT_RELOAD;
        counters[i].write_phase = 0;
        counters[i].read_phase = 0;
        counters[i].latched = 0;
        counters[i].latch_value = 0;
        counters[i].last_tick = tick_count;
    }
}

/* ====================================================================== */
/*  ポートアドレス → カウンタ番号変換                                      */
/*  戻り値: 0-2 = Counter#0-2, -1 = 非対象                                */
/* ====================================================================== */
static int port_to_counter(u16 port)
{
    switch (port) {
    case 0x71: case 0x3FD9: return 0;
    case 0x73: case 0x3FDB: return 1;
    case 0x75: case 0x3FDD: return 2;
    default: return -1;
    }
}

/* コマンドポート判定 */
static int is_cmd_port(u16 port)
{
    return (port == 0x77 || port == 0x3FDF);
}

/* ====================================================================== */
/*  現在のカウンタ推定値を計算                                              */
/*  tick_count (100Hz) をベースに、reload_value からダウンカウントを推定    */
/* ====================================================================== */
static u16 estimate_counter(struct pit_counter *c)
{
    u32 rv;
    u32 elapsed_ticks;
    u32 elapsed_pit;
    u32 progress;

    rv = (u32)c->reload_value;
    if (rv == 0) rv = 0x10000UL;  /* 0 は 65536 を意味する */

    elapsed_ticks = tick_count - c->last_tick;

    /* OS32 の 1 tick = 10ms = PIT の DEFAULT_RELOAD (0x4E00=19968) カウント分。
     * elapsed_ticks * DEFAULT_RELOAD が経過した PIT カウント数。
     * これを reload_value の周期で剰余をとり、サイクル内の進行位置を得る。
     * mode 2/3: カウンタは rv → 0 へダウンカウントし、0 で rv にリロード。 */
    elapsed_pit = elapsed_ticks * (u32)DEFAULT_RELOAD;
    progress = elapsed_pit % rv;

    return (u16)(rv - progress);
}

/* ====================================================================== */
/*  v86_pit_io — PIT I/Oポートハンドラ                                     */
/* ====================================================================== */
int v86_pit_io(u16 port, u8 *val, int is_write)
{
    int cnum;
    struct pit_counter *c;

    /* コマンドレジスタ (0x77 / 0x3FDF) — 書き込みのみ */
    if (is_cmd_port(port)) {
        if (!is_write) {
            /* コマンドポートのREADは無効 */
            *val = 0xFF;
            return 1;
        }
        /* コマンドデコード */
        {
            u8 cmd = *val;
            int sc = (cmd >> 6) & 0x03;   /* カウンタ選択 */
            int rl = (cmd >> 4) & 0x03;   /* R/Wモード */
            int md = (cmd >> 1) & 0x07;   /* モード */

            if (sc >= 3) return 1;  /* SC=11はリードバック (無視) */

            c = &counters[sc];

            if (rl == 0) {
                /* ラッチコマンド */
                c->latched = 1;
                c->latch_value = estimate_counter(c);
                c->read_phase = 0;
            } else {
                /* モード設定 */
                c->rw_mode = (u8)rl;
                c->mode = (u8)md;
                c->write_phase = 0;
                c->read_phase = 0;
                c->latched = 0;
            }
            /* Counter 1 (ビープ音) のコマンドはハードウェアにもパススルー */
            if (sc == 1) {
                outp(port, *val);
            }
        }
        return 1;
    }

    /* カウンタデータポート */
    cnum = port_to_counter(port);
    if (cnum < 0) return 0;  /* 非対象 */

    c = &counters[cnum];

    if (is_write) {
        /* カウンタへの書き込み (リロード値設定) */
        switch (c->rw_mode) {
        case 1: /* LSBのみ */
            c->reload_value = (c->reload_value & 0xFF00) | *val;
            c->last_tick = tick_count;
            break;
        case 2: /* MSBのみ */
            c->reload_value = (c->reload_value & 0x00FF) | ((u16)*val << 8);
            c->last_tick = tick_count;
            break;
        case 3: /* LSB→MSB */
            if (c->write_phase == 0) {
                c->reload_value = (c->reload_value & 0xFF00) | *val;
                c->write_phase = 1;
            } else {
                c->reload_value = (c->reload_value & 0x00FF) | ((u16)*val << 8);
                c->write_phase = 0;
                c->last_tick = tick_count;
            }
            break;
        }
        /* Counter 1 (ビープ音) はハードウェアにもパススルー */
        if (cnum == 1) {
            outp(port, *val);
        }
    } else {
        /* カウンタからの読み出し */
        u16 value;

        if (c->latched) {
            value = c->latch_value;
        } else {
            value = estimate_counter(c);
        }

        switch (c->rw_mode) {
        case 1: /* LSBのみ */
            *val = (u8)(value & 0xFF);
            c->latched = 0;
            break;
        case 2: /* MSBのみ */
            *val = (u8)((value >> 8) & 0xFF);
            c->latched = 0;
            break;
        case 3: /* LSB→MSB */
            if (c->read_phase == 0) {
                *val = (u8)(value & 0xFF);
                c->read_phase = 1;
            } else {
                *val = (u8)((value >> 8) & 0xFF);
                c->read_phase = 0;
                c->latched = 0;
            }
            break;
        default:
            *val = 0;
            break;
        }
    }
    return 1;
}

/* ====================================================================== */
/*  v86_pit_get_irq_divisor — IRQ0 注入分周比を計算                        */
/*                                                                          */
/*  OS32 はベースレート 100Hz (reload=0x4E00) で動作する。                  */
/*  ゲストが Counter#0 を別の値に設定した場合:                              */
/*    divisor = guest_reload / DEFAULT_RELOAD (切り上げ, 最小1)             */
/*                                                                          */
/*  例:                                                                     */
/*    reload=0x4E00 (デフォルト) → divisor=1 (毎tick = 100Hz)             */
/*    reload=0x9C00 (50Hz)      → divisor=2 (2tick毎 = 50Hz)              */
/*    reload=0x0100 (速い)      → divisor=1 (早めても100Hz以上は無意味)    */
/* ====================================================================== */
u32 v86_pit_get_irq_divisor(void)
{
    u32 rv = (u32)counters[0].reload_value;
    u32 div;
    if (rv == 0) rv = 0x10000UL;  /* 0 は 65536 を意味する */
    /* divisor = rv / DEFAULT_RELOAD (最小1) */
    div = rv / DEFAULT_RELOAD;
    if (div == 0) div = 1;
    return div;
}

/* ====================================================================== */
/*  v86_pit_get_counter0_reload — Counter#0 のリロード値を返す             */
/*  デバッグログ用。ゲストが設定した値をそのまま返す。                    */
/* ====================================================================== */
u16 v86_pit_get_counter0_reload(void)
{
    return counters[0].reload_value;
}
