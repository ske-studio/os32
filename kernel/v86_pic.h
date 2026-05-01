/* ======================================================================== */
/*  V86_PIC.H — V86 PIC (8259A) 仮想化ヘッダ                               */
/* ======================================================================== */

#ifndef V86_PIC_H
#define V86_PIC_H

#include "types.h"

/* PIC仮想化コンテキスト初期化 */
void v86_pic_init(void);

/* I/Oポートハンドラ
 * port: I/Oポートアドレス
 * val:  書き込み値 (OUT時) / 読み取り結果格納先 (IN時)
 * is_write: 1=OUT, 0=IN
 * 戻り値: 1=処理済み(仮想化ポート), 0=非対象ポート */
int v86_pic_io(u16 port, u8 *val, int is_write);

/* リブート検知 (OUT F0h)
 * 戻り値: 1=リブート検知(V86終了要求), 0=それ以外 */
int v86_pic_is_reboot(u16 port, u8 val);

#endif /* V86_PIC_H */
