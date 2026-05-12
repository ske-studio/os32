/* ======================================================================== */
/*  V86_EVENT.C - V86 イベントログ実装 (T1.1)                              */
/*                                                                          */
/*  GP/INT/IRQ イベントを 16B 固定長レコードで時系列保存する。              */
/*  /host/debug/v86_events.log にバイナリ形式で HostDrv 定期追記。          */
/*                                                                          */
/*  Flush トリガ (ハイブリッド):                                            */
/*    1. バッファ 80% 充填 (head >= 1638)                                   */
/*    2. 定期タイマ 30 tick = 300ms 経過                                    */
/*    3. クリティカルイベント (ROM_CALL/EXIT/TIMEOUT/UNKNOWN_OP) → 即時     */
/*    4. セッション終了時 (v86_event_close)                                 */
/* ======================================================================== */

#include "v86_event.h"
#include "v86_debug.h"
#include "vfs.h"
#include "kprintf.h"

/* tick_count (isr_stub.asm で 100Hz インクリメント) */
extern volatile u32 tick_count;

/* ====================================================================== */
/*  バッファ                                                                */
/* ====================================================================== */
#define V86_EVENT_BUF_SIZE  512    /* 512 x 16B = 8KB */
#define V86_EVENT_FLUSH_THR 409    /* 512 * 0.8 */
#define V86_EVENT_TICK_INTVL 30    /* 300ms */

static struct v86_event ev_buf[V86_EVENT_BUF_SIZE];
static volatile u32 ev_head = 0;       /* 次に書く位置 */
static u32 ev_lost = 0;                /* オーバーフローで失われた件数 */
static int ev_fd = -1;                 /* ログファイル fd */
static u32 last_flush_tick = 0;        /* 最終 flush の tick */

/* T3.6: シリアル ライブ ストリーム有効フラグ (デフォルト=OFF) */
int v86_debug_stream_enabled = 0;

/* ====================================================================== */
/*  内部 flush — バッファ内容をファイルに追記しヘッドをリセット             */
/* ====================================================================== */
static void v86_event_flush(void)
{
    u32 to_write;

    if (ev_fd < 0 || ev_head == 0) return;

    to_write = ev_head * sizeof(struct v86_event);
    vfs_write_fd(ev_fd, ev_buf, to_write);
    ev_head = 0;
    last_flush_tick = tick_count;
}

/* ====================================================================== */
/*  v86_event_init — セッション開始時にログファイルをオープン               */
/* ====================================================================== */
void v86_event_init(void)
{
    if (!v86_debug_enabled) return;

    vfs_mkdir("/host/debug");
    ev_fd = vfs_open("/host/debug/v86_events.log",
                     O_WRONLY | O_CREAT | O_TRUNC);
    ev_head = 0;
    ev_lost = 0;
    last_flush_tick = tick_count;

    if (ev_fd < 0) {
        kprintf(0x0E, "[V86_EV] event log open failed\n");
    }
}

/* ====================================================================== */
/*  v86_event_close — セッション終了時にフラッシュしてクローズ             */
/* ====================================================================== */
void v86_event_close(void)
{
    if (ev_fd < 0) return;

    /* 最終フラッシュ */
    v86_event_flush();

    /* lost カウントがあればサマリを追記 (4バイトマーカー) */
    if (ev_lost > 0) {
        struct v86_event marker;
        marker.tick = ev_lost;
        marker.cs = 0xFFFF;
        marker.ip = 0xFFFF;
        marker.kind = 0xFF;
        marker.arg1 = 0;
        marker.arg2 = 0;
        marker.arg3 = 0;
        marker.cx = 0;
        marker.reserved = 0;
        vfs_write_fd(ev_fd, &marker, sizeof(marker));
        kprintf(0x0E, "[V86_EV] %u events lost (buffer overflow)\n",
                (unsigned)ev_lost);
    }

    vfs_close(ev_fd);
    ev_fd = -1;
}

/* ====================================================================== */
/*  v86_event_record — イベントを記録                                       */
/*                                                                          */
/*  critical イベントは即時 flush する。                                    */
/*  バッファが満杯の場合は ev_lost をインクリメントして記録をスキップ。     */
/* ====================================================================== */
void v86_event_record(u8 kind, u16 cs, u16 ip,
                      u8 arg1, u8 arg2, u8 arg3, u16 cx)
{
    struct v86_event *e;
    int is_critical;

    if (ev_fd < 0 && !v86_debug_stream_enabled) return;

    /* バッファフル → lost カウント (ファイル出力時のみ) */
    if (ev_fd >= 0 && ev_head >= V86_EVENT_BUF_SIZE) {
        ev_lost++;
        /* シリアルストリームは独立して続行 */
    } else if (ev_fd >= 0) {
        /* レコード書き込み */
        e = &ev_buf[ev_head];
        e->tick = tick_count;
        e->cs = cs;
        e->ip = ip;
        e->kind = kind;
        e->arg1 = arg1;
        e->arg2 = arg2;
        e->arg3 = arg3;
        e->cx = cx;
        e->reserved = 0;
        ev_head++;

        /* クリティカルイベント判定 */
        is_critical = (kind == V86_EV_ROM_CALL ||
                       kind == V86_EV_EXIT     ||
                       kind == V86_EV_TIMEOUT  ||
                       kind == V86_EV_UNKNOWN_OP);

        /* 即時 flush: クリティカル or バッファ 80% 充填 */
        if (is_critical || ev_head >= V86_EVENT_FLUSH_THR) {
            v86_event_flush();
        }
    }

    /* T3.6: シリアル ライブ ストリーム */
    if (v86_debug_stream_enabled) {
        static const char hex[] = "0123456789ABCDEF";
        char line[40];
        int p = 0;

        /* プレフィックス */
        line[p++] = '<'; line[p++] = '<';
        line[p++] = 'V'; line[p++] = '8'; line[p++] = '6';
        line[p++] = '>'; line[p++] = '>';

        /* kind (1桁) */
        line[p++] = hex[kind & 0xF];
        line[p++] = ':';

        /* CS:IP (4:4) */
        line[p++] = hex[(cs >> 12) & 0xF];
        line[p++] = hex[(cs >> 8) & 0xF];
        line[p++] = hex[(cs >> 4) & 0xF];
        line[p++] = hex[cs & 0xF];
        line[p++] = ':';
        line[p++] = hex[(ip >> 12) & 0xF];
        line[p++] = hex[(ip >> 8) & 0xF];
        line[p++] = hex[(ip >> 4) & 0xF];
        line[p++] = hex[ip & 0xF];
        line[p++] = ':';

        /* arg1:arg2:arg3 */
        line[p++] = hex[(arg1 >> 4) & 0xF]; line[p++] = hex[arg1 & 0xF];
        line[p++] = ':';
        line[p++] = hex[(arg2 >> 4) & 0xF]; line[p++] = hex[arg2 & 0xF];
        line[p++] = ':';
        line[p++] = hex[(arg3 >> 4) & 0xF]; line[p++] = hex[arg3 & 0xF];

        line[p++] = '\r';
        line[p++] = '\n';
        line[p] = '\0';

        {
            extern void serial_puts(const char *s);
            serial_puts(line);
        }
    }
}

/* ====================================================================== */
/*  v86_event_tick_check — 定期 flush チェック (timer handler から呼ぶ)     */
/*                                                                          */
/*  30 tick (300ms) 経過でバッファ内容をフラッシュする。                    */
/* ====================================================================== */
void v86_event_tick_check(void)
{
    if (ev_fd < 0 || ev_head == 0) return;

    if ((tick_count - last_flush_tick) >= V86_EVENT_TICK_INTVL) {
        v86_event_flush();
    }
}
