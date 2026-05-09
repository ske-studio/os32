/* ======================================================================== */
/*  V86_PIT.H — V86 PIT (8253A互換TCU) 仮想化ヘッダ                        */
/*                                                                          */
/*  PC-98 PIT (ポート 0x71/0x73/0x75/0x77) のV86仮想化を行う。             */
/*  Counter#0 (タイマ割り込み) の設定を記録し、カウンタ読み出しに応答する。  */
/* ======================================================================== */

#ifndef V86_PIT_H
#define V86_PIT_H

#include "types.h"

/* PIT仮想化コンテキスト初期化 */
void v86_pit_init(void);

/* I/Oポートハンドラ
 * port: I/Oポートアドレス
 * val:  書き込み値 (OUT時) / 読み取り結果格納先 (IN時)
 * is_write: 1=OUT, 0=IN
 * 戻り値: 1=処理済み(仮想化ポート), 0=非対象ポート */
int v86_pit_io(u16 port, u8 *val, int is_write);

/* §5 タイマレート反映: IRQ0注入の分周比を返す
 * Counter#0 の reload_value と OS32ベースレートから
 * 「何 OS32-ticks に 1 回 IRQ0 を注入すべきか」を計算して返す。
 * 戻り値: 1 = 毎tick注入 (100Hz), 2 = 2tickに1回 (50Hz), ... */
u32 v86_pit_get_irq_divisor(void);

/* Counter#0 のリロード値を返す (デバッグダンプ用) */
u16 v86_pit_get_counter0_reload(void);

#endif /* V86_PIT_H */
