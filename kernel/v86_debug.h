/* ======================================================================== */
/*  V86_DEBUG.H - V86 デバッグ機能                                          */
/*                                                                          */
/*  V86セッションの体系化デバッグログを /host/debug/ に出力する。           */
/*  v86_debug_enabled フラグで有効/無効を制御する。                         */
/*                                                                          */
/*  ログファイル: /host/debug/v86_YYMMDD_HHMMSS.log                        */
/*  セクション構成:                                                         */
/*    SESSION / EXIT / GP HANDLER / IRQ0 / VSYNC / PIC / PIT /             */
/*    I/O PORT / DISK I/O / GP TRACE / MEMORY DUMP / TIMEOUT               */
/* ======================================================================== */

#ifndef V86_DEBUG_H
#define V86_DEBUG_H

#include "types.h"

/* デバッグファイルログの有効/無効フラグ (vdos -d で有効化) */
extern int v86_debug_enabled;

/* シリアルダンプの有効/無効フラグ
 * デバッグファイルログ有効時は自動的に無効化される。
 * 手動で有効にしたい場合は v86_debug_serial_enabled = 1 にする。 */
extern int v86_debug_serial_enabled;

/* V86セッション開始時にヘッダセクションを即時書き込みする。
 * クラッシュ時にも起動情報が残る。
 * boot_mode: "FreeDOS" / "Native" / "PhysicalFDD"
 * image_path: ディスクイメージパス (NULLの場合 "N/A")
 * auto_cmd: Auto-Typerコマンド (NULLの場合 "none") */
void v86_debug_write_header(const char *boot_mode,
                            const char *image_path,
                            const char *auto_cmd);

/* V86セッション終了後の全デバッグダンプ
 * シリアル出力 (有効時) + ファイル書き出しを一括実行する */
void v86_debug_dump_session(void);

/* teardown前のメモリダンプ (バッキングRAM有効時に呼ぶ)
 * v86_session.cのインラインasm内から直接callされる */
void v86_debug_dump_memory_pre(void);

/* シリアルポートへの16進数出力ヘルパー (v86_debug.c で定義) */
void v86_dbg_hex8(u8 val);
void v86_dbg_hex16(u16 val);
void v86_dbg_hex32(u32 val);

#endif /* V86_DEBUG_H */
