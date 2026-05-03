/* ======================================================================== */
/*  V86_BIOS.C — V86 BIOS割り込みエミュレーション (Phase 1)                 */
/*                                                                          */
/*  PC-98テキストBIOS (INT 18h) および DOS出力 (INT 29h) の                 */
/*  最小限のエミュレーションを提供する。                                    */
/*                                                                          */
/*  出典: PC9800Bible §2-6 テキストBIOS一覧                                */
/*        undocumented/memsys.md (BDA カーソル位置等)                       */
/*                                                                          */
/*  Phase 1 実装範囲:                                                      */
/*    INT 18h AH=0Ah  テキスト画面モード設定 (ack のみ)                    */
/*    INT 18h AH=0Ch  テキスト画面表示開始 (ack のみ)                      */
/*    INT 18h AH=0Dh  テキスト画面表示停止 (ack のみ)                      */
/*    INT 18h AH=11h  カーソル表示開始 (ack のみ)                          */
/*    INT 18h AH=12h  カーソル表示停止 (ack のみ)                          */
/*    INT 18h AH=13h  カーソル位置設定                                     */
/*    INT 18h AH=16h  テキストVRAMクリア                                   */
/*    INT 29h          DOS 1文字高速出力                                    */
/* ======================================================================== */

#include "v86_bios.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_debug.h"
#include "tvram.h"
#include "io.h"
#include "rtc.h"
#include "kbd.h"

extern void serial_puts(const char *s);

#define TVRAM_LINE_BYTES  (TVRAM_COLS * 2)  /* 1行のバイト数 (80桁 x 2バイト) */

/* 仮想カーソル位置 (V86 BIOS内で管理) */
static u16 v86_cursor_x = 0;
static u16 v86_cursor_y = 0;

/* ====================================================================== */
/*  ヘルパー: TVRAM 1行スクロールアップ                                    */
/* ====================================================================== */
static void tvram_scroll_up(void)
{
    volatile u16 *char_area = (volatile u16 *)TVRAM_BASE;
    volatile u16 *attr_area = (volatile u16 *)TVRAM_ATTR;
    int i;

    /* 行1〜24 を 行0〜23 にコピー */
    for (i = 0; i < TVRAM_COLS * (TVRAM_ROWS - 1); i++) {
        char_area[i] = char_area[i + TVRAM_COLS];
        attr_area[i] = attr_area[i + TVRAM_COLS];
    }
    /* 最下行をクリア */
    for (i = TVRAM_COLS * (TVRAM_ROWS - 1); i < TVRAM_COLS * TVRAM_ROWS; i++) {
        char_area[i] = 0x0020;  /* スペース */
        attr_area[i] = 0x00E1;  /* 白色 */
    }
}

/* ====================================================================== */
/*  ヘルパー: TVRAMに1文字書き込み (カーソル位置)                          */
/* ====================================================================== */
static void tvram_putchar(u8 ch, u8 attr)
{
    volatile u16 *char_ptr;
    volatile u8 *attr_ptr;
    u32 offset;

    if (v86_cursor_x >= TVRAM_COLS) {
        v86_cursor_x = 0;
        v86_cursor_y++;
    }
    if (v86_cursor_y >= TVRAM_ROWS) {
        tvram_scroll_up();
        v86_cursor_y = TVRAM_ROWS - 1;
    }

    offset = v86_cursor_y * TVRAM_LINE_BYTES + v86_cursor_x * 2;

    /* 文字エリア: JISコード(下位=文字, 上位=0x00 for ANK) */
    char_ptr = (volatile u16 *)(TVRAM_BASE + offset);
    *char_ptr = (u16)ch;

    /* アトリビュートエリア: 偶数アドレスのみ */
    attr_ptr = (volatile u8 *)(TVRAM_ATTR + offset);
    *attr_ptr = attr;

    v86_cursor_x++;
}

/* ====================================================================== */
/*  ヘルパー: テキストVRAMクリア                                           */
/* ====================================================================== */
static void tvram_clear_all(void)
{
    volatile u16 *char_area = (volatile u16 *)TVRAM_BASE;
    volatile u16 *attr_area = (volatile u16 *)TVRAM_ATTR;
    int i;
    int total = TVRAM_COLS * TVRAM_ROWS;

    for (i = 0; i < total; i++) {
        char_area[i] = 0x0020;  /* スペース */
        attr_area[i] = 0x00E1;  /* 白色 */
    }

    v86_cursor_x = 0;
    v86_cursor_y = 0;
}

/* ====================================================================== */
/*  v86_bios_int18 — INT 18h エミュレーション                              */
/*                                                                          */
/*  regs: V86スタックフレーム内レジスタ配列                                */
/*  戻り値: 1=V86終了要求, 0=処理済み(V86継続)                             */
/* ====================================================================== */
int v86_bios_int18(u32 *regs)
{
    u8 ah = (u8)((regs[V86_REG_EAX] >> 8) & 0xFF);

    switch (ah) {
    /* ================================================================ */
    /*  AH=00h: キーボード入力 (ブロッキング)                           */
    /*  キーがあればバッファから取得して返す。                           */
    /*  キーがなければ STI + HLT でIRQ到着を待ってから、               */
    /*  INT命令を再実行させる (return -2 → EIP加算スキップ)。          */
    /*  GPハンドラはIF=0で動作するため、明示的にSTIしないと             */
    /*  キーボードIRQ (IRQ1) が配信されない。                           */
    /* ================================================================ */
    case 0x00: {
        int key = kbd_trygetkey();
        if (key >= 0) {
            /* キーデータ: 上位=スキャンコード, 下位=ASCII */
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL)
                               | ((u32)key & 0xFFFF);
        } else {
            /* バッファ空: INT再実行 (DOSはBDAバッファを直接ポーリング) */
            return -2;
        }
        break;
    }

    /* ================================================================ */
    /*  AH=01h: キーバッファ状態の取得 (ノンブロッキング)               */
    /*  出力: BH=01h (データあり) / BH=00h (バッファ空)                 */
    /*         AX=先頭キーデータ (BH=01の場合、消費しない=peek)         */
    /* ================================================================ */
    case 0x01: {
        int key = kbd_peekkey(); /* peek: 消費しない */
        if (key >= 0) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL)
                               | ((u32)key & 0xFFFF);
            regs[V86_REG_EBX] = (regs[V86_REG_EBX] & 0xFFFF00FFUL)
                               | 0x0100;
        } else {
            regs[V86_REG_EBX] = (regs[V86_REG_EBX] & 0xFFFF00FFUL);
        }
        break;
    }

    /* ================================================================ */
    /*  AH=02h: シフトキー状態の検査                                    */
    /*  出力: AL = シフト状態 (OS32 kbd_shift_state)                     */
    /* ================================================================ */
    case 0x02:
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL)
                           | (u32)kbd_shift_state;
        break;

    /* ================================================================ */
    /*  AH=03h: キーボードインターフェースの初期化                      */
    /*  キーバッファをクリアしてインターフェースをリセットする           */
    /* ================================================================ */
    case 0x03:
        /* ack のみ (V86では物理キーボードハードウェアは操作しない) */
        break;

    /* ================================================================ */
    /*  AH=04h: キー入力状態の取得                                      */
    /*  入力: AL = キーコードグループ番号                               */
    /*  出力: AH = 0 (どのキーも押されていない)                        */
    /* ================================================================ */
    case 0x04:
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        break;

    /* ================================================================ */
    /*  AH=05h: キーバッファからのキーコードの取得 (ノンブロッキング)   */
    /*  出力: BX=0000h (データ無効=バッファ空)                         */
    /* ================================================================ */
    case 0x05:
        regs[V86_REG_EBX] = regs[V86_REG_EBX] & 0xFFFF0000UL;
        break;

    /* ================================================================ */
    /*  AH=0Ah: テキスト画面モードの設定                                */
    /*  入力: AL = モードコード                                        */
    /*  Phase 1: ack のみ (80×25 ノーマルモード固定)                   */
    /* ================================================================ */
    case 0x0A:
        /* 何もしない — 常に80×25モード */
        break;

    /* ================================================================ */
    /*  AH=0Bh: テキスト画面モードの取得                                */
    /*  出力: AL = 現在のモード                                        */
    /* ================================================================ */
    case 0x0B:
        /* 80×25 ノーマルモードを返す */
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFF00) | 0x00;
        break;

    /* ================================================================ */
    /*  AH=0Ch: テキスト画面表示開始                                    */
    /*  AH=0Dh: テキスト画面表示停止                                    */
    /*  Phase 1: ack のみ                                              */
    /* ================================================================ */
    case 0x0C:
        /* テキスト画面表示開始: GDC STARTコマンド */
        outp(0x62, 0x0D);
        break;
    case 0x0D:
        /* テキスト画面表示停止: GDC STOPコマンド */
        outp(0x62, 0x0C);
        break;

    /* ================================================================ */
    /*  AH=11h: カーソル表示開始                                        */
    /*  AH=12h: カーソル表示停止                                        */
    /*  Phase 1: ack のみ                                              */
    /* ================================================================ */
    case 0x11:
    case 0x12:
        break;

    /* ================================================================ */
    /*  AH=13h: カーソル位置の設定                                      */
    /*  入力: DH = 行 (0-24), DL = 桁 (0-79)                          */
    /* ================================================================ */
    case 0x13: {
        u8 row = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        u8 col = (u8)(regs[V86_REG_EDX] & 0xFF);

        if (col >= TVRAM_COLS) col = TVRAM_COLS - 1;
        if (row >= TVRAM_ROWS) row = TVRAM_ROWS - 1;

        v86_cursor_x = col;
        v86_cursor_y = row;

        /* GDC CSRWコマンドでカーソル位置を反映 */
        {
            u32 ead = (u32)row * 80 + (u32)col;
            while (!(inp(0x60) & 0x04)) { /* FIFO EMPTY待ち */ }
            outp(0x62, 0x49);             /* CSRWコマンド */
            outp(0x60, ead & 0xFF);       /* EAD下位 */
            outp(0x60, (ead >> 8) & 0xFF); /* EAD上位 */
            outp(0x60, 0x00);              /* dAD=0 */
        }
        break;
    }

    /* ================================================================ */
    /*  AH=0Fh: カーソル位置の取得                                      */
    /*  出力: DH = 行, DL = 桁                                        */
    /* ================================================================ */
    case 0x0F:
        regs[V86_REG_EDX] = ((u32)v86_cursor_y << 8) | v86_cursor_x;
        break;

    /* ================================================================ */
    /*  AH=16h: テキストVRAMのクリア                                    */
    /* ================================================================ */
    case 0x16:
        tvram_clear_all();
        break;

    /* ================================================================ */
    /*  AH=10h: カーソルブリンクの有無設定                               */
    /*  入力: AL = 0(ブリンクなし), 1(ブリンクあり)                    */
    /*  Phase 1: ack のみ                                              */
    /* ================================================================ */
    case 0x10:
        break;

    /* ================================================================ */
    /*  AH=0Eh: ファンクションキーライン表示制御                        */
    /*  入力: AL = 1(表示), 0(非表示)                                  */
    /*  Phase 2: ack のみ (OS32はファンクションキーラインを持たない)    */
    /* ================================================================ */
    case 0x0E:
        break;

    /* ================================================================ */
    /*  AH=14h: テキストVRAMに1文字書き込み                             */
    /*  入力: DH = 行, DL = 桁, AL = 文字コード                       */
    /*  PC-98 CRT BIOSの基本的な文字出力関数                           */
    /* ================================================================ */
    case 0x14: {
        u8 row = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        u8 col = (u8)(regs[V86_REG_EDX] & 0xFF);
        u8 ch = (u8)(regs[V86_REG_EAX] & 0xFF);
        u32 offset;
        volatile u16 *char_ptr;
        volatile u16 *attr_ptr;

        if (col >= TVRAM_COLS) col = TVRAM_COLS - 1;
        if (row >= TVRAM_ROWS) row = TVRAM_ROWS - 1;
        offset = (u32)row * TVRAM_LINE_BYTES + (u32)col * 2;
        char_ptr = (volatile u16 *)(TVRAM_BASE + offset);
        attr_ptr = (volatile u16 *)(TVRAM_ATTR + offset);
        *char_ptr = (u16)ch;
        *attr_ptr = 0x00E1;
        break;
    }

    /* ================================================================ */
    /*  AH=15h: テキストVRAMから1文字読み出し                           */
    /*  入力: DH = 行, DL = 桁                                        */
    /*  出力: AL = 文字コード                                          */
    /* ================================================================ */
    case 0x15: {
        u8 row = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        u8 col = (u8)(regs[V86_REG_EDX] & 0xFF);
        u32 offset;
        volatile u16 *char_ptr;

        if (col >= TVRAM_COLS) col = TVRAM_COLS - 1;
        if (row >= TVRAM_ROWS) row = TVRAM_ROWS - 1;
        offset = (u32)row * TVRAM_LINE_BYTES + (u32)col * 2;
        char_ptr = (volatile u16 *)(TVRAM_BASE + offset);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL)
                           | ((u16)*char_ptr & 0xFF);
        break;
    }

    /* ================================================================ */
    /*  AH=17h: アトリビュート設定                                      */
    /*  入力: DH = 行, DL = 桁, AL = アトリビュート                    */
    /*  FreeDOS(98) int29dc.c がカラー設定時に使用                     */
    /* ================================================================ */
    case 0x17: {
        u8 row = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        u8 col = (u8)(regs[V86_REG_EDX] & 0xFF);
        u8 attr = (u8)(regs[V86_REG_EAX] & 0xFF);
        u32 offset;
        volatile u8 *attr_ptr;

        if (col >= TVRAM_COLS) col = TVRAM_COLS - 1;
        if (row >= TVRAM_ROWS) row = TVRAM_ROWS - 1;
        offset = (u32)row * TVRAM_LINE_BYTES + (u32)col * 2;
        attr_ptr = (volatile u8 *)(TVRAM_ATTR + offset);
        *attr_ptr = attr;
        break;
    }

    /* ================================================================ */
    /*  AH=1Ah: テキスト画面行スクロールアップ                          */
    /*  入力: DH = スクロール行数                                      */
    /*  FreeDOS(98) コンソール出力で使用                               */
    /* ================================================================ */
    case 0x1A: {
        u8 lines = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        int l;
        if (lines == 0) lines = 1;
        if (lines > TVRAM_ROWS) lines = TVRAM_ROWS;
        for (l = 0; l < (int)lines; l++) {
            tvram_scroll_up();
        }
        break;
    }

    /* ================================================================ */
    /*  AH=1Bh: テキスト画面行スクロールダウン                          */
    /*  Phase 2: 暫定NOP (必要に応じて実装)                            */
    /* ================================================================ */
    case 0x1B:
        break;

    /* ================================================================ */
    /*  AH=40h: グラフィック画面表示開始                                 */
    /*  AH=41h: グラフィック画面表示停止                                 */
    /*  Phase 2: 実ハードウェアに転送                                   */
    /* ================================================================ */
    case 0x40:
        outp(0xA2, 0x0D);
        break;
    case 0x41:
        outp(0xA2, 0x0C);
        break;

    /* ================================================================ */
    /*  AH=42h: パレット設定                                            */
    /*  AH=43h: パレット取得                                            */
    /*  Phase 2: ack のみ                                              */
    /* ================================================================ */
    case 0x42:
    case 0x43:
        break;

    /* ================================================================ */
    /*  AH=18h/19h: MS-DOS内部使用 (PC9800Bible一覧表に無い機能)        */
    /*  IO.SYSがブート時に呼び出す。ack応答でV86を安全に続行させる。    */
    /* ================================================================ */
    case 0x18:
    case 0x19:
        break;

    /* ================================================================ */
    /*  未実装の機能: IVTのダミーIRETハンドラに任せる                    */
    /*  戻り値 -1 で「未処理」を返し、呼び出し元がIVT転送する           */
    /* ================================================================ */
    default: {
        static int unhandled_count = 0;
        if (unhandled_count < 10) {
            serial_puts("\r\n[V86 BIOS] Unhandled INT 18h AH=");
            v86_dbg_hex8(ah);
            serial_puts("\r\n");
            unhandled_count++;
        }
        return -1;
    }
    }

    return 0;
}

/* ====================================================================== */
/*  v86_bios_int29 — INT 29h エミュレーション (DOS 1文字高速出力)          */
/*                                                                          */
/*  MS-DOSがINT 21h AH=02h等から呼び出す内部関数。                         */
/*  入力: AL = 出力文字                                                    */
/* ====================================================================== */
int v86_bios_int29(u32 *regs)
{
    u8 ch = (u8)(regs[V86_REG_EAX] & 0xFF);

    switch (ch) {
    case '\r':  /* CR — カーソルを行頭に */
        v86_cursor_x = 0;
        break;
    case '\n':  /* LF — 次の行に */
        v86_cursor_y++;
        if (v86_cursor_y >= TVRAM_ROWS) {
            tvram_scroll_up();
            v86_cursor_y = TVRAM_ROWS - 1;
        }
        break;
    case '\b':  /* BS — カーソルを1つ戻す */
        if (v86_cursor_x > 0) v86_cursor_x--;
        break;
    default:
        tvram_putchar(ch, 0xE1);  /* 白色で出力 */
        break;
    }

    return 0;
}

/* ====================================================================== */
/*  v86_bios_int1c — INT 1Ch エミュレーション (カレンダBIOS)               */
/*                                                                          */
/*  出典: PC9800Bible §2-4 カレンダ時計BIOS                                */
/*                                                                          */
/*  AH=00h: 日付、時刻の読み出し                                           */
/*    入力: ES:BX = 書き込み先アドレス (6バイト)                           */
/*    出力: ES:BX に以下の形式で書き込み                                   */
/*                                                                          */
/*    PC-98 カレンダBIOS データフォーマット (6バイト, BCD):                 */
/*      byte 0: 年 (BCD, 例: 0x26 = 2026年)                               */
/*      byte 1: 月 (16進, 1-12)                                            */
/*      byte 2: 曜日<<4 | 日の十の位                                       */
/*      byte 3: 日の一の位<<4 | 時の十の位                                 */
/*      byte 4: 時の一の位<<4 | 分の十の位                                 */
/*      byte 5: 分の一の位<<4 | 秒の十の位  (秒の一の位はなし)             */
/*                                                                          */
/*  AH=01h: 日付、時刻の設定 (ack のみ)                                    */
/* ====================================================================== */
int v86_bios_int1c(u32 *regs)
{
    u8 ah = (u8)((regs[V86_REG_EAX] >> 8) & 0xFF);

    switch (ah) {
    case 0x00: {
        /* 日時読み出し: ES:BX に6バイトのBCDデータを書き込む */
        RTC_Time t;
        u8 *dst;
        u16 es_val = (u16)(regs[V86_REG_ES] & 0xFFFF);
        u16 bx_val = (u16)(regs[V86_REG_EBX] & 0xFFFF);
        u8 bcd_year, bcd_day10, bcd_day1;
        u8 bcd_hour10, bcd_hour1, bcd_min10, bcd_min1, bcd_sec10;

        rtc_read(&t);

        /* BCD変換 */
        bcd_year  = (u8)(((t.year / 10) << 4) | (t.year % 10));
        bcd_day10 = (u8)(t.day / 10);
        bcd_day1  = (u8)(t.day % 10);
        bcd_hour10 = (u8)(t.hour / 10);
        bcd_hour1  = (u8)(t.hour % 10);
        bcd_min10  = (u8)(t.min / 10);
        bcd_min1   = (u8)(t.min % 10);
        bcd_sec10  = (u8)(t.sec / 10);

        /* V86リニアアドレス → カーネル用リニアアドレス変換
         * バッキングRAMにリマップされた領域を正しく変換する */
        dst = v86_phys_addr(es_val, bx_val);

        dst[0] = bcd_year;
        dst[1] = t.month;
        dst[2] = (u8)((t.wday << 4) | bcd_day10);
        dst[3] = (u8)((bcd_day1 << 4) | bcd_hour10);
        dst[4] = (u8)((bcd_hour1 << 4) | bcd_min10);
        dst[5] = (u8)((bcd_min1 << 4) | bcd_sec10);

        break;
    }

    case 0x01:
        /* 日時設定: ack のみ (V86からRTCは操作しない) */
        break;

    default:
        return -1;
    }

    return 0;
}

/* ====================================================================== */
/*  v86_bios_int11 — INT 11h エミュレーション (機器構成取得)               */
/*                                                                          */
/*  出力: AX = 機器構成フラグ                                               */
/*  PC-98: bit 15-14=RS232C数, bit 11=プリンタ, bit 5-4=FDD台数            */
/*  VDM: FDD 1台、プリンタなし、コプロなしの最小構成を返す                  */
/* ====================================================================== */
int v86_bios_int11(u32 *regs)
{
    /* FDD 1台 (bit 5-4 = 00 = 1台) */
    regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | 0x0000;
    return 0;
}

/* ====================================================================== */
/*  v86_bios_int12 — INT 12h エミュレーション (メモリサイズ取得)            */
/*                                                                          */
/*  出力: AX = コンベンショナルメモリサイズ (KB単位)                         */
/*  BDA 0000:0413h (WORD) の値を返す。v86_mem.c で 640 に設定済み。         */
/* ====================================================================== */
int v86_bios_int12(u32 *regs)
{
    u8 *bda = v86_phys_addr(0, 0x0413);
    u16 memsz = (u16)bda[0] | ((u16)bda[1] << 8);
    regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | memsz;
    return 0;
}
