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

/* T2.1: MEMSW スナップショット (v86_debug.c で定義)
 * v86_mem_setup() 直後と V86終了直前にそれぞれ呼び出す */
void v86_debug_snapshot_memsw_init(void);
void v86_debug_snapshot_memsw_exit(void);

/* T1.3: BDA スナップショット多段化 (v86_debug.c で定義)
 * tag: "init" / "post_ipl" / "pre_dos" / "exit"
 * /host/debug/v86_bda_{tag}.bin に 512B raw バイナリを出力 */
void v86_debug_dump_bda_named(const char *tag);

/* T2.3: IVT 差分検出 (v86_debug.c で定義)
 * v86_mem_setup() 直後に呼び出して初期 IVT をスナップショット */
void v86_debug_snapshot_ivt_init(void);

/* T1.4: ROM CALL トラッカー (v86_debug.c で定義)
 * CS >= F000h への CALL FAR を記録する */
void v86_debug_rom_call_record(u32 tick, u16 caller_cs, u16 caller_ip,
                               u16 target_cs, u16 target_ip,
                               u16 ax, u16 bx);

/* V86 #PF 発生時の診断情報 (page_fault_handler でセット)
 * v86_debug_dump_session() で [EXIT] セクションに出力される */
extern u32 v86_pf_cr2;           /* フォルトアドレス (CR2) */
extern u32 v86_pf_error_code;    /* エラーコード */
extern u16 v86_pf_cs;            /* フォルト時の CS */
extern u16 v86_pf_ip;            /* フォルト時の IP */
extern int v86_pf_recorded;      /* 1 = #PF情報あり */

/* セッション終了理由を取得する (v86_debug.c から current_session にアクセス) */
int v86_debug_get_exit_reason(void);

#endif /* V86_DEBUG_H */
