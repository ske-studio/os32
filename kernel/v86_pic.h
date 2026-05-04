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

/* 仮想PICの状態への直接アクセス (タイマ/キーボード割り込み注入等で使用)
 * idx: 0=マスタPIC, 1=スレーブPIC */
u8  v86_pic_get_imr(int idx);
u8  v86_pic_get_isr(int idx);
void v86_pic_set_isr(int idx, u8 val);
u32 v86_pic_get_eoi_count(int idx);

/* §4 IRR 更新 API: v86_set_pending_irq から呼び、OCW3リードに対応する */
void v86_pic_set_irr(int idx, u8 val);
u8   v86_pic_get_irr(int idx);

#endif /* V86_PIC_H */
