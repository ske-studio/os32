/* ======================================================================== */
/*  V86_EVENT.H - V86 イベントログ (T1.1)                                  */
/*                                                                          */
/*  GP/INT/IRQ イベントを 16B 固定長レコードで時系列保存し、                */
/*  HostDrv 経由で /host/debug/v86_events.log にバイナリ追記する。          */
/*  ハードハング時にも直前のイベントが残るよう、ハイブリッド flush を採用。  */
/* ======================================================================== */

#ifndef V86_EVENT_H
#define V86_EVENT_H

#include "types.h"

/* イベント種別 */
enum v86_event_kind {
    V86_EV_GP        = 0,   /* #GP (非INT命令) */
    V86_EV_INT       = 1,   /* ソフトウェアINT */
    V86_EV_IRQ_INJ   = 2,   /* IRQ注入 */
    V86_EV_ROM_CALL  = 3,   /* BIOS ROM FAR CALL (critical) */
    V86_EV_EXIT      = 4,   /* V86終了 (critical) */
    V86_EV_TIMEOUT   = 5,   /* タイムアウト (critical) */
    V86_EV_UNKNOWN_OP= 6    /* 未知オペコード (critical) */
};

/* イベントレコード (16バイト固定長) */
struct v86_event {
    u32 tick;        /* tick_count スナップ */
    u16 cs;          /* フォルト時の CS */
    u16 ip;          /* フォルト時の IP */
    u8  kind;        /* enum v86_event_kind */
    u8  arg1;        /* INT番号 / IRQ番号 / EXIT理由 */
    u8  arg2;        /* AH / VECTOR */
    u8  arg3;        /* AL */
    u16 cx;          /* ECX下位16bit */
    u16 reserved;
};  /* 16 bytes */

/* イベントログ API */

/* セッション開始時にファイルをオープンする (v86_debug_write_header から呼ぶ) */
void v86_event_init(void);

/* セッション終了時にフラッシュしてファイルをクローズする */
void v86_event_close(void);

/* イベントを記録する (GPハンドラ / IRQ注入ハンドラから呼ぶ)
 * critical イベント (ROM_CALL / EXIT / TIMEOUT / UNKNOWN_OP) は即時 flush */
void v86_event_record(u8 kind, u16 cs, u16 ip,
                      u8 arg1, u8 arg2, u8 arg3, u16 cx);

/* 定期 flush チェック (timer handler から呼ぶ — 30 tick 経過 or 80% 充填) */
void v86_event_tick_check(void);

/* T3.6: シリアル ライブ ストリーム (HostDrv 不在時の緊急デバッグ)
 * 有効にすると v86_event_record() のたびに RS-232C へ1行テキスト送出。
 * フォーマット: <<V86>>kind:SSSS:PPPP:aa:bb:cc\r\n */
extern int v86_debug_stream_enabled;

#endif /* V86_EVENT_H */
