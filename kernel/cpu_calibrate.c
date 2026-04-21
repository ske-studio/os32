/* ======================================================================== */
/*  CPU_CALIBRATE.C — CPU速度キャリブレーション                              */
/*                                                                          */
/*  PIT タイマー (100Hz = 10ms/tick) を基準として CPU の NOP ループ速度を     */
/*  起動時に計測する。Linux の bogomips と同じ発想。                         */
/*                                                                          */
/*  重要: キャリブレーションと遅延で**同一のループ関数**を使用し、           */
/*  反復あたりのコスト差による誤差を排除する。                               */
/*                                                                          */
/*  計測手順:                                                               */
/*    1. tick_count の変化を待ち、tick 境界に同期                            */
/*    2. 既知回数の nop_loop() を実行し、所要 tick 数を計測                  */
/*    3. loops_per_tick = 既知回数 / 経過 tick で算出                        */
/*                                                                          */
/*  注意: cpu_calibrate() は PIT 初期化後、_enable() 後に呼ぶこと。         */
/* ======================================================================== */

#include "cpu_calibrate.h"
#include "io.h"

/* タイマーティックカウンタ (isr_stub.asm でインクリメント) */
extern volatile u32 tick_count;

/* キャリブレーション結果 (静的変数) */
static u32 s_loops_per_tick = 0;

/* キャリブレーションで使用する固定ループ回数
 * 8MHz: ~125ms (12.5 tick), 33MHz: ~30ms (3 tick) */
#define CALIBRATE_LOOPS  200000UL

/* ======================================================================== */
/*  nop_loop — キャリブレーションと遅延の共通ループ                         */
/*                                                                          */
/*  noinline 指定により、キャリブレーションと遅延で完全に同一の              */
/*  マシンコードが実行されることを保証する。                                 */
/* ======================================================================== */
static void __attribute__((noinline)) nop_loop(u32 n)
{
    while (n > 0) {
        __asm__ volatile("nop");
        n--;
    }
}

/* ======================================================================== */
/*  cpu_calibrate — 共通ループの実測によるキャリブレーション                 */
/* ======================================================================== */
void cpu_calibrate(void)
{
    u32 start;
    u32 elapsed;

    /* tick 境界に同期: 次の tick 開始まで待つ */
    start = tick_count;
    while (tick_count == start) {
        /* 何もしない */
    }

    /* 既知回数の nop_loop を実行し、所要 tick 数を計測 */
    start = tick_count;
    nop_loop(CALIBRATE_LOOPS);
    elapsed = tick_count - start;

    /* ゼロ除算防止 (非常に高速なCPUでは 0 tick になりうる) */
    if (elapsed == 0) elapsed = 1;

    s_loops_per_tick = CALIBRATE_LOOPS / elapsed;

    /* 安全装置: 極端に小さい場合はフォールバック (8MHz相当) */
    if (s_loops_per_tick < 1000) {
        s_loops_per_tick = 10000;
    }
}

/* ======================================================================== */
/*  cpu_loops_per_tick — キャリブレーション結果を返す                        */
/* ======================================================================== */
u32 cpu_loops_per_tick(void)
{
    return s_loops_per_tick;
}

/* ======================================================================== */
/*  cpu_delay_us — CPU速度適応型マイクロ秒ディレイ                          */
/*                                                                          */
/*  1 tick = 10,000 µs                                                      */
/*  loops = loops_per_tick * us / 10000                                     */
/*                                                                          */
/*  オーバーフロー防止のため (lpt/100) * us / 100 で計算する。              */
/*  割り込み禁止区間でも使用可能 (PIT に依存しない)。                       */
/* ======================================================================== */
void cpu_delay_us(u32 us)
{
    u32 loops;

    if (s_loops_per_tick == 0) return;

    if (us > 100000UL) {
        us = 100000UL;
    }

    /* (lpt/100) * us / 100 でオーバーフロー回避しつつ精度確保 */
    loops = (s_loops_per_tick / 100UL) * us / 100UL;

    if (loops == 0) return;

    nop_loop(loops);
}
