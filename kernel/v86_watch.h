/* ======================================================================== */
/*  V86_WATCH.H - V86 メモリウォッチポイント (T2.4)                         */
/*                                                                          */
/*  PTE NOT_PRESENT 方式でメモリアクセスを監視する。                         */
/*  ゲストが監視アドレスにアクセスすると #PF → 記録 → TF で1命令 → #DB    */
/*  → PTE 再設定のシーケンスで透過的に動作する。                            */
/*                                                                          */
/*  ★高負荷: 1回のアクセスにつき #PF + #DB の2例外が発生する。             */
/*  通常は OFF にしておき、特定フェーズのみ ON にすること。                  */
/* ======================================================================== */

#ifndef V86_WATCH_H
#define V86_WATCH_H

#include "types.h"

/* 最大同時ウォッチポイント数 */
#define V86_WATCH_MAX 4

/* ウォッチポイント設定
 * addr: 監視するリニアアドレス (V86空間: 0x00000-0xFFFFF)
 * size: 監視サイズ (1-4バイト) */
void v86_watch_set(u32 addr, u32 size);

/* ウォッチポイント解除 */
void v86_watch_clear(u32 addr);

/* 全ウォッチポイント解除 */
void v86_watch_clear_all(void);

/* #PF ハンドラから呼ばれる: ウォッチポイント該当チェック
 * fault_addr: #PF の CR2 値
 * regs: PUSHAD + CPUフレーム配列
 * 戻り値: 1 = ウォッチポイント処理済み, 0 = 通常の #PF */
int v86_watch_check_pf(u32 fault_addr, u32 *regs);

/* #DB ハンドラから呼ばれる: ウォッチポイント復帰処理
 * (TF によるシングルステップ後に PTE を再度 NOT_PRESENT にする)
 * 戻り値: 1 = ウォッチポイント復帰処理済み, 0 = 通常の #DB */
int v86_watch_check_db(u32 *regs);

/* ウォッチポイントヒットログの件数を取得 */
u32 v86_watch_get_hit_count(void);

/* ウォッチポイントログエントリ */
struct v86_watch_hit {
    u32 tick;
    u32 addr;
    u16 cs;
    u16 ip;
    u32 value;  /* アクセス時のメモリ値 */
};

/* ウォッチポイントヒットログを取得 (最大 max_entries 件)
 * 戻り値: 実際のエントリ数 */
u32 v86_watch_get_log(struct v86_watch_hit *out, u32 max_entries);

#endif /* V86_WATCH_H */
