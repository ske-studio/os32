/* ======================================================================== */
/*  V86_DEBUG.H - V86 デバッグ機能                                          */
/*                                                                          */
/*  V86セッション終了後のデバッグダンプ、ログファイル書き出しを行う。       */
/*  v86_debug_enabled フラグで有効/無効を制御する。                         */
/* ======================================================================== */

#ifndef V86_DEBUG_H
#define V86_DEBUG_H

#include "types.h"

/* デバッグ出力の有効/無効フラグ (vdos -d で有効化) */
extern int v86_debug_enabled;

/* V86セッション終了後の全デバッグダンプ
 * シリアル出力 + ファイル書き出しを一括実行する */
void v86_debug_dump_session(void);

/* シリアルポートへの16進数出力ヘルパー (v86_debug.c で定義) */
void v86_dbg_hex8(u8 val);
void v86_dbg_hex16(u16 val);
void v86_dbg_hex32(u32 val);

#endif /* V86_DEBUG_H */
