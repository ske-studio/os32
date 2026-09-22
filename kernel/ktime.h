/* ======================================================================== */
/*  KTIME.H — µs 時計の**試験専用の注入口** (票 TASK_HAL_WIRING §1-5)       */
/*                                                                          */
/*  時計そのもの (`sys_time_now`) の宣言は include/sys.h。ここに在るのは     */
/*  kselftest だけが触る hook で、製品経路は 1 バイトも見ない (n = 0 が既定)。*/
/*                                                                          */
/*  なぜ要るか (往復 8 の中継 1): 実 PIT では呼び出しから p1 読みまでに      */
/*  周期境界を越える機械があり、位相を待っても p1/p2 の 3 分岐を撃ち分け     */
/*  られない。そこで**入力列 {p1, p2, count} を与え**、                     */
/*  `time_branch_hits[]` (kernel/time_math.h) でどの分岐を踏んだかを数える。 */
/*  実 PIT 側は「踏めた分岐の記録」と「踏めなかった分岐の未検証報告」に留める。*/
/* ======================================================================== */

#ifndef __KTIME_H
#define __KTIME_H

struct time_test_feed {
    int p1;      /* 0 / 1 */
    int p2;      /* 0 / 1 */
    int count;   /* < 0 = 実測のラッチ値をそのまま使う */
};

/* 試行 i は feed[min(i, n-1)] を使う (最後の要素を繰り返す)。
 * n = 0 (既定) なら注入しない。3 = sys_time_now の再試行上限。 */
extern struct time_test_feed time_test_feed[3];
extern volatile int time_test_feed_n;

void time_test_feed_clear(void);

#endif /* __KTIME_H */
