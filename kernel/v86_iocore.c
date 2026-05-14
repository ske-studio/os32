/* ======================================================================== */
/*  V86_IOCORE.C — V86 I/Oポートディスパッチテーブル                        */
/*                                                                          */
/*  NP21/W iocore.c アーキテクチャに準拠した O(1) I/Oディスパッチ。          */
/*  各デバイスは _bind() 関数でポート別ハンドラを登録する。                 */
/*                                                                          */
/*  初期化順:                                                               */
/*    1. 全エントリをデフォルトハンドラ (実I/O直接) で初期化                */
/*    2. 保護ポートをブロックハンドラで上書き                               */
/*    3. 各デバイスの _bind() を呼んでハンドラ登録                          */
/* ======================================================================== */

#include "v86_iocore.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_fdc.h"
#include "v86_dma.h"
#include "v86_vsync.h"
#include "io.h"

/* ====================================================================== */
/*  ディスパッチテーブル (BSS: 2KB)                                         */
/* ====================================================================== */
v86_inp_fn v86_io_inp[256];
v86_out_fn v86_io_out[256];

/* ====================================================================== */
/*  16bit ワードターミネートテーブル (NP21/W iocore16.tbl 完全準拠)          */
/*                                                                          */
/*  NP21/W の word_term / active_term / plus_term / minus_term / ext08_term */
/*  をそのまま移植。port & 0xFF でインデックスする。                        */
/* ====================================================================== */
const u8 v86_io_terminate[256] = {
    /* 0x00-0x0F: PIC/DMA系 */
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    /* 0x10-0x1F: DMA系 */
    0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1,
    /* 0x20-0x2F: DMA (ext08) + 8251 (plus) */
    5, 1, 5, 1, 5, 1, 5, 1, 0, 1, 0, 0, 0, 0, 0, 0,
    /* 0x30-0x3F: RS-232C (plus) */
    3, 0, 3, 0, 3, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x40-0x4F: CRTC (plus) */
    3, 0, 3, 0, 3, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x50-0x5F: カレンダ/WAIT */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60-0x6F: GDCテキスト (minus) */
    4, 0, 4, 0, 4, 0, 0, 0, 4, 0, 4, 0, 4, 0, 0, 0,
    /* 0x70-0x7F: PIT (minus) + GDCグラフィック */
    4, 3, 4, 0, 4, 0, 4, 0, 0, 3, 4, 0, 4, 0, 0, 0,
    /* 0x80-0x8F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x90-0x9F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xA0-0xAF: GDCグラフィック (minus) */
    4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 0, 0, 4, 3, 0, 3,
    /* 0xB0-0xBF */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xC0-0xCF */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xD0-0xDF: GDC (active) */
    2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 0, 0, 2, 0, 2, 0,
    /* 0xE0-0xEF */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xF0-0xFF: リセット/FM音源系 */
    0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ====================================================================== */
/*  デフォルトハンドラ: 実ハードウェアにフォールスルー                      */
/* ====================================================================== */
static u8 v86_io_default_inp(u16 port)
{
    return inp(port);
}

static void v86_io_default_out(u16 port, u8 val)
{
    outp(port, val);
}

/* ====================================================================== */
/*  保護ポートハンドラ: HostDrv / RS-232C                                  */
/* ====================================================================== */
static u8 v86_io_blocked_inp(u16 port)
{
    (void)port;
    return 0xFF;
}

static void v86_io_blocked_out(u16 port, u8 val)
{
    (void)port;
    (void)val;
}

/* ====================================================================== */
/*  KBD 8251A ハンドラ                                                     */
/* ====================================================================== */

/* v86.c から参照される外部変数 (KBDバッファ) */
extern u8 v86_kbd_buf[];
extern volatile int v86_kbd_buf_head;
extern volatile int v86_kbd_buf_count;

#define V86_KBD_BUF_SIZE 32  /* v86.c と同値 */

static u8 v86_kbd_data_inp(u16 port)
{
    u8 val;
    (void)port;
    if (v86_kbd_buf_count > 0) {
        val = v86_kbd_buf[v86_kbd_buf_head];
        v86_kbd_buf_head = (v86_kbd_buf_head + 1) % V86_KBD_BUF_SIZE;
        v86_kbd_buf_count--;
        return val;
    }
    return 0xFF; /* バッファ空: ダミー値 */
}

static u8 v86_kbd_sts_inp(u16 port)
{
    (void)port;
    /* bit1 (RxRDY) = データあり */
    return (v86_kbd_buf_count > 0) ? 0x02 : 0x00;
}

static void v86_kbd_out_nop(u16 port, u8 val)
{
    /* ゲストからの書き込みは無視 — OS32がKBDハードウェアを管理 */
    (void)port;
    (void)val;
}

/* ====================================================================== */
/*  PIC iocore バインド                                                    */
/*                                                                          */
/*  既存の v86_pic_io(port, &val, is_write) を                             */
/*  ポート別の inp/out 関数に分解する。                                    */
/* ====================================================================== */
static u8 pic_master_cmd_inp(u16 port)
{
    u8 val;
    (void)port;
    v86_pic_io(0x00, &val, 0);
    return val;
}

static void pic_master_cmd_out(u16 port, u8 val)
{
    (void)port;
    v86_pic_io(0x00, &val, 1);
}

static u8 pic_master_data_inp(u16 port)
{
    u8 val;
    (void)port;
    v86_pic_io(0x02, &val, 0);
    return val;
}

static void pic_master_data_out(u16 port, u8 val)
{
    (void)port;
    v86_pic_io(0x02, &val, 1);
}

static u8 pic_slave_cmd_inp(u16 port)
{
    u8 val;
    (void)port;
    v86_pic_io(0x08, &val, 0);
    return val;
}

static void pic_slave_cmd_out(u16 port, u8 val)
{
    (void)port;
    v86_pic_io(0x08, &val, 1);
}

static u8 pic_slave_data_inp(u16 port)
{
    u8 val;
    (void)port;
    v86_pic_io(0x0A, &val, 0);
    return val;
}

static void pic_slave_data_out(u16 port, u8 val)
{
    (void)port;
    v86_pic_io(0x0A, &val, 1);
}

/* リセットポート (0xF0): OUT のみトラップ (リブート検知は別途) */
static void reset_port_out(u16 port, u8 val)
{
    (void)port;
    (void)val;
    /* リブート検知は v86_iocore_is_reboot() 側で行う。
     * ここではOUT自体を無害にするだけ。 */
}

/* ====================================================================== */
/*  PIT iocore バインド                                                    */
/* ====================================================================== */
static u8 pit_counter_inp(u16 port)
{
    u8 val;
    v86_pit_io(port, &val, 0);
    return val;
}

static void pit_counter_out(u16 port, u8 val)
{
    v86_pit_io(port, &val, 1);
}

static u8 pit_cmd_inp(u16 port)
{
    u8 val;
    v86_pit_io(port, &val, 0);
    return val;
}

static void pit_cmd_out(u16 port, u8 val)
{
    v86_pit_io(port, &val, 1);
}

/* ====================================================================== */
/*  FDC iocore バインド                                                    */
/* ====================================================================== */
static u8 fdc_inp(u16 port)
{
    u8 val;
    if (v86_fdc_io(port, &val, 0)) return val;
    return inp(port);
}

static void fdc_out(u16 port, u8 val)
{
    if (!v86_fdc_io(port, &val, 1)) {
        outp(port, val);
    }
}

/* ====================================================================== */
/*  DMA iocore バインド                                                    */
/* ====================================================================== */
static u8 dma_inp(u16 port)
{
    u8 val;
    if (v86_dma_io(port, &val, 0)) return val;
    return inp(port);
}

static void dma_out(u16 port, u8 val)
{
    if (!v86_dma_io(port, &val, 1)) {
        outp(port, val);
    }
}

/* ====================================================================== */
/*  VSYNC iocore バインド                                                  */
/* ====================================================================== */
static u8 vsync_inp(u16 port)
{
    u8 val;
    if (v86_vsync_io(port, &val, 0)) return val;
    return inp(port);
}

static void vsync_out(u16 port, u8 val)
{
    if (!v86_vsync_io(port, &val, 1)) {
        outp(port, val);
    }
}

/* GDCテキスト/グラフィック ステータス (0x60, 0xA0): VSYNC仮想化 */
static u8 gdc_status_inp(u16 port)
{
    u8 val;
    if (v86_vsync_io(port, &val, 0)) return val;
    return inp(port);
}

/* ====================================================================== */
/*  v86_iocore_init — テーブル初期化 + 全デバイスバインド                   */
/* ====================================================================== */
void v86_iocore_init(void)
{
    int i;

    /* ---- 1. デフォルト: 実ハードウェア直接アクセス ---- */
    for (i = 0; i < 256; i++) {
        v86_io_inp[i] = v86_io_default_inp;
        v86_io_out[i] = v86_io_default_out;
    }

    /* ---- 2. 保護ポート (HostDrv / RS-232C) ---- */
    /* HostDrv I/Oポート: 0x7EC, 0x7EE → lower byte 0xEC, 0xEE
     * ※ 上位バイトが異なるアクセスは default で処理されるため安全 */
    v86_io_inp[0xEC] = v86_io_blocked_inp;
    v86_io_out[0xEC] = v86_io_blocked_out;
    v86_io_inp[0xEE] = v86_io_blocked_inp;
    v86_io_out[0xEE] = v86_io_blocked_out;
    /* RS-232C (µPD8251A): 0x30, 0x32, 0x33, 0x35 */
    v86_io_inp[0x30] = v86_io_blocked_inp;
    v86_io_out[0x30] = v86_io_blocked_out;
    v86_io_inp[0x32] = v86_io_blocked_inp;
    v86_io_out[0x32] = v86_io_blocked_out;
    v86_io_inp[0x33] = v86_io_blocked_inp;
    v86_io_out[0x33] = v86_io_blocked_out;
    v86_io_inp[0x35] = v86_io_blocked_inp;
    v86_io_out[0x35] = v86_io_blocked_out;
    /* RS-232C ボーレート (PIT #2): 0x75, 0x77
     * ※ 0x75/0x77 は PIT Counter#2/コマンド と重複するが、
     *    保護が優先。PIT bindで上書きしないこと。 */
    v86_io_inp[0x75] = v86_io_blocked_inp;
    v86_io_out[0x75] = v86_io_blocked_out;
    v86_io_inp[0x77] = v86_io_blocked_inp;
    v86_io_out[0x77] = v86_io_blocked_out;

    /* ---- 3. KBD 8251A (0x41=データ, 0x43=ステータス) ---- */
    v86_io_inp[0x41] = v86_kbd_data_inp;
    v86_io_out[0x41] = v86_kbd_out_nop;
    v86_io_inp[0x43] = v86_kbd_sts_inp;
    v86_io_out[0x43] = v86_kbd_out_nop;

    /* ---- 4. PIC 8259A ---- */
    /* マスタPIC: 0x00 (CMD), 0x02 (DATA/IMR) */
    v86_io_inp[0x00] = pic_master_cmd_inp;
    v86_io_out[0x00] = pic_master_cmd_out;
    v86_io_inp[0x02] = pic_master_data_inp;
    v86_io_out[0x02] = pic_master_data_out;
    /* スレーブPIC: 0x08 (CMD), 0x0A (DATA/IMR) */
    v86_io_inp[0x08] = pic_slave_cmd_inp;
    v86_io_out[0x08] = pic_slave_cmd_out;
    v86_io_inp[0x0A] = pic_slave_data_inp;
    v86_io_out[0x0A] = pic_slave_data_out;
    /* リセットポート: 0xF0 (OUT のみ) */
    v86_io_out[0xF0] = reset_port_out;

    /* ---- 5. PIT 8253A ---- */
    /* Counter#0: 0x71, Counter#1: 0x73 */
    v86_io_inp[0x71] = pit_counter_inp;
    v86_io_out[0x71] = pit_counter_out;
    v86_io_inp[0x73] = pit_counter_inp;
    v86_io_out[0x73] = pit_counter_out;
    /* Counter#2 (0x75) と Command (0x77) はRS-232C保護で上書き済み
     * → PIT仮想化は Counter#0/Counter#1 のみ有効 */

    /* ---- 6. FDC µPD765 (0x90-0xBE 範囲) ---- */
    /* PC-98 FDC: 0x90(ステータス), 0x92(データ),
     *            0x94(モーター/リセット), 0xBE(入力) 等。
     *            偶数ポートのみ使用。 */
    for (i = 0x90; i <= 0xBE; i += 2) {
        v86_io_inp[i] = fdc_inp;
        v86_io_out[i] = fdc_out;
    }

    /* ---- 7. DMA 8237A ch2 ---- */
    /* 0x09: ch2アドレス, 0x0B: ch2ワードカウント,
     * 0x15: マスク, 0x17: モード, 0x19: FF_CLR,
     * 0x23: ch2バンク */
    v86_io_inp[0x09] = dma_inp;
    v86_io_out[0x09] = dma_out;
    v86_io_inp[0x0B] = dma_inp;
    v86_io_out[0x0B] = dma_out;
    v86_io_inp[0x15] = dma_inp;
    v86_io_out[0x15] = dma_out;
    v86_io_inp[0x17] = dma_inp;
    v86_io_out[0x17] = dma_out;
    v86_io_inp[0x19] = dma_inp;
    v86_io_out[0x19] = dma_out;
    v86_io_inp[0x23] = dma_inp;
    v86_io_out[0x23] = dma_out;

    /* ---- 8. VSYNC / GDCステータス ---- */
    /* 0x64: VSYNC割り込みトリガ */
    v86_io_inp[0x64] = vsync_inp;
    v86_io_out[0x64] = vsync_out;
    /* 0x60: GDCテキストステータス, 0xA0: GDCグラフィックステータス
     * VSYNC仮想化のために inp をフック */
    v86_io_inp[0x60] = gdc_status_inp;
    v86_io_inp[0xA0] = gdc_status_inp;
}

/* ====================================================================== */
/*  8bit I/O ディスパッチ                                                  */
/*                                                                          */
/*  PC-98 のシステムI/Oは10bitデコードのため、upper byte が異なっても       */
/*  同じデバイスにアクセスする。テーブルは port & 0xFF で引く。             */
/*  ただし保護ポート (HostDrv: 0x7EC, RS232C: 0x30等) は upper byte が     */
/*  異なる場合のみ実I/Oにフォールスルーさせる必要がある。                   */
/*  現時点では全てのアクセスを port & 0xFF で処理する。                    */
/* ====================================================================== */
u8 v86_iocore_inp8(u16 port)
{
    return v86_io_inp[port & 0xFF](port);
}

void v86_iocore_out8(u16 port, u8 val)
{
    v86_io_out[port & 0xFF](port, val);
}

/* ====================================================================== */
/*  16bit I/O ディスパッチ (NP21/W ワードターミネート対応)                  */
/*                                                                          */
/*  cur_ax: INW時の現在のAX値 (TERM_ACTIVE用)                              */
/*  戻り値: 16bit入力値                                                    */
/* ====================================================================== */
u16 v86_iocore_inp16(u16 port, u16 cur_ax)
{
    u8 lo, term;

    /* システムI/O範囲 (NP21/W: !(port & 0x0C00)) でターミネートチェック */
    if (!(port & 0x0C00)) {
        term = v86_io_terminate[port & 0xFF];
        switch (term) {
        case V86_TERM_WORD:
            return V86_WORD_TERMINATE;
        case V86_TERM_ACTIVE:
            lo = v86_iocore_inp8(port);
            return (cur_ax & 0xFF00) | lo;
        case V86_TERM_PLUS:
            lo = v86_iocore_inp8(port);
            return 0xFF00 | (u16)lo;
        case V86_TERM_MINUS:
            return (u16)v86_iocore_inp8(port);
        case V86_TERM_EXT08:
            lo = v86_iocore_inp8(port);
            return 0x0800 | (u16)lo;
        default:
            break;
        }
    }

    /* 通常: port, port+1 の連続8bit読み */
    lo = v86_iocore_inp8(port);
    return (u16)lo | ((u16)v86_iocore_inp8((u16)(port + 1)) << 8);
}

/* 16bit I/O出力 (ワードターミネート + リブート検知)
 * 戻り値: 1=リブート検知, 0=通常 */
int v86_iocore_out16(u16 port, u16 val)
{
    u8 lo = (u8)(val & 0xFF);
    u8 hi = (u8)((val >> 8) & 0xFF);

    /* リブート検知 (F0hポート) */
    if (v86_iocore_is_reboot(port, lo)) return 1;
    if (v86_iocore_is_reboot((u16)(port + 1), hi)) return 1;

    /* システムI/O範囲でのターミネートチェック */
    if (!(port & 0x0C00)) {
        u8 term = v86_io_terminate[port & 0xFF];
        switch (term) {
        case V86_TERM_WORD:
            return 0;  /* 書き込み無効 */
        case V86_TERM_ACTIVE:
        case V86_TERM_PLUS:
        case V86_TERM_MINUS:
        case V86_TERM_EXT08:
            /* 下位バイトのみ書き込み */
            v86_iocore_out8(port, lo);
            return 0;
        default:
            break;
        }
    }

    /* 通常: port, port+1 の連続8bit書き込み */
    v86_iocore_out8(port, lo);
    v86_iocore_out8((u16)(port + 1), hi);
    return 0;
}

/* リブート検知 */
int v86_iocore_is_reboot(u16 port, u8 val)
{
    (void)val;
    return (port == 0xF0) ? 1 : 0;
}
