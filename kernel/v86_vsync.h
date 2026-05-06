/* ======================================================================== */
/*  V86_VSYNC.H — V86 VSYNC (CRTV) 割り込み仮想化ヘッダ                    */
/*                                                                          */
/*  PC-98 VSYNC仕様:                                                       */
/*    I/O 0x64 への WRITE = 次のVSYNC時に INT 0Ah を1回発生させる           */
/*    (ワンショット方式 — 毎回 OUT 0x64 で再アーミング必要)                 */
/*    GDCステータス (I/O 0x60/0xA0) bit5 = VERTICAL SYNC                   */
/*                                                                          */
/*  出典: PC9800Bible §1-4, §2-6                                           */
/*        Undocumented io_disp.md I/O 0064h                                 */
/* ======================================================================== */

#ifndef V86_VSYNC_H
#define V86_VSYNC_H

#include "types.h"

/* VSYNC仮想化初期化 */
void v86_vsync_init(void);

/* VSYNC仮想化クリーンアップ (V86終了時) */
void v86_vsync_cleanup(void);

/* I/Oポートハンドラ (I/O 0x64 WRITE をトラップ)
 * port: I/Oポートアドレス
 * val:  書き込み値 (OUT時) / 読み取り結果格納先 (IN時)
 * is_write: 1=OUT, 0=IN
 * 戻り値: 1=処理済み, 0=非対象ポート */
int v86_vsync_io(u16 port, u8 *val, int is_write);

/* VSYNC IRQ注入 (タイマIRQから呼ばれる)
 * タイマ割り込み (100Hz) ごとに呼ばれ、armed状態なら
 * ゲストIVT[0x0A] に INT 0Ah を注入する。
 * regs: ハードウェアIRQスタックフレーム */
void v86_inject_vsync_irq(u32 *regs);

/* デバッグ: 注入カウンタ */
extern u32 v86_vsync_arm_count;     /* OUT 0x64 回数 */
extern u32 v86_vsync_inject_count;  /* INT 0Ah 注入回数 */

#endif /* V86_VSYNC_H */
