/* ======================================================================== */
/*  V86_SSTEP.H - V86 シングルステップ (TF) モード (T3.4)                   */
/*                                                                          */
/*  EFLAGS.TF を立てた状態で V86 に入り、1命令ごとに #DB 例外を発火させて  */
/*  極詳細トレースを取得する。                                              */
/*                                                                          */
/*  ★注意: TF を立てると命令ごとに割り込みが発火するため、                 */
/*  ゲストの動作タイミングが完全に崩れる。短時間のデバッグ専用。             */
/* ======================================================================== */

#ifndef V86_SSTEP_H
#define V86_SSTEP_H

#include "types.h"

/* シングルステップ有効/無効フラグ */
extern int v86_singlestep_enabled;

/* シングルステップ上限 (0=無制限) */
extern u32 v86_singlestep_max_count;

/* シングルステップカウンタ (現在の命令数) */
extern u32 v86_singlestep_count;

/* シングルステップ有効化/無効化 */
void v86_singlestep_set(int enabled, u32 max_count);

/* #DB ハンドラから呼ばれる V86 シングルステップ処理
 * regs: isr_stub.asm の PUSHAD + CPUフレーム配列
 * 戻り値: 0=V86続行, 1=V86終了 */
int v86_db_handler(u32 *regs);

#endif /* V86_SSTEP_H */
