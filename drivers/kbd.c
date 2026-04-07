/* ======================================================================== */
/*  KBD.C — PC-98 キーボードドライバ                                       */
/*                                                                          */
/*  μPD8251A経由でスキャンコードを取得し、ASCII変換してリングバッファに格納 */
/*  IRQ1割り込みで駆動                                                      */
/*                                                                          */
/*  出典: PC9800Bible §2-5 表2-14                                           */
/*  参照: FreeBSD sys/pc98/cbus/pckbd.c                                     */
/*    - IO_KBD = 0x041 (ベースアドレス)                                     */
/*    - KBD_DATA_PORT = base + 0 = 0x41                                     */
/*    - KBD_STATUS_PORT = base + 2 = 0x43 (PC-98はI/O 2バイト間隔)         */
/*    - KBDS_BUFFER_FULL = 0x0002 (8251 RxRDY)                              */
/*    - FreeBSD init_keyboard() は空 → BIOS初期化済みを前提                */
/* ======================================================================== */

#include "kbd.h"
#include "io.h"
#include "serial.h"

/* 外部: irq_enable (idt.c で定義) */
extern void irq_enable(unsigned int irq);

/* rshellモード判定用 (shell.cで定義) */
extern int rshell_active;

/* ======== シフトキー状態 ======== */
volatile u8 kbd_shift_state = 0;

/* ======== リングバッファ (u16: 上位=スキャンコード, 下位=ASCII) ======== */
static volatile u16 kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static volatile int kbd_count = 0;

/* ======================================================================== */
/*  スキャンコード → ASCII 変換テーブル                                    */
/*  PC9800Bible §2-5 表2-14 の「通常」列から抽出                           */
/*  インデックス = キーコード (0x00〜0x6B)                                  */
/* ======================================================================== */

/* 通常状態 (Shift なし) */
static const u8 scancode_to_ascii[128] = {
    /* 0x00-0x0F */
    0x1B, '1', '2', '3', '4', '5', '6', '7',   /* ESC, 1-7 */
    '8',  '9', '0', '-', '^', '\\', 0x08, 0x09, /* 8-0,-,^,\,BS,TAB */
    /* 0x10-0x1F */
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',   /* Q-I */
    'o',  'p', '@', '[', 0x0D, 'a', 's', 'd',   /* O-P,@,[,ENTER,A-D */
    /* 0x20-0x2F */
    'f',  'g', 'h', 'j', 'k', 'l', ';', ':',   /* F-L,;,: */
    ']',  'z', 'x', 'c', 'v', 'b', 'n', 'm',   /* ],Z-M */
    /* 0x30-0x3F */
    ',',  '.', '/', 0,   ' ', 0,   0x12,0x03,   /* ,./ _,SPACE,XFER,RLUP(12),RLDN(03) */
    0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,  /* INS(16),DEL,↑,←,→,↓,HOME(01),HELP(05) */
    /* 0x40-0x4F (テンキー) */
    '-',  '/', '7', '8', '9', '*', '4', '5',
    '6',  '+', '1', '2', '3', '=', '0', ',',
    /* 0x50-0x5F */
    '.',  0,   0,   0,   0,   0,   0,   0,      /* NFER,vf1-vf5 */
    0,    0,   0,   0,   0,   0,   0,   0,
    /* 0x60-0x6F (STOP,COPY,F1-F10) */
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    /* 0x70-0x7F (SHIFT,CAPS,KANA,GRPH,CTRL) */
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
};

/* Shift状態 */
static const u8 scancode_to_ascii_shift[128] = {
    /* 0x00-0x0F */
    0x1B, '!', '"', '#', '$', '%', '&', '\'',
    '(',  ')', 0,   '=', '`', '|', 0x08, 0x09,
    /* 0x10-0x1F */
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O',  'P', '~', '{', 0x0D, 'A', 'S', 'D',
    /* 0x20-0x2F */
    'F',  'G', 'H', 'J', 'K', 'L', '+', '*',
    '}',  'Z', 'X', 'C', 'V', 'B', 'N', 'M',
    /* 0x30-0x3F */
    '<',  '>', '?', '_', ' ', 0,   0x12,0x03,
    0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,
    /* 0x40-0x7F: テンキー以降はShiftでも同じ */
    '-',  '/', '7', '8', '9', '*', '4', '5',
    '6',  '+', '1', '2', '3', '=', '0', ',',
    '.',  0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,
};

/* ======================================================================== */
/*  kbd_irq_handler — IRQ1 割り込みハンドラ (Cレベル)                      */
/*  ASMスタブから呼ばれる                                                    */
/* ======================================================================== */
void kbd_irq_handler(void)
{
    u8 scancode;
    u8 keycode;
    u8 ascii;
    int is_break;

    /* μPD8251Aからスキャンコード読み取り */
    scancode = (u8)inp(KBD_DATA);
    is_break = scancode & SCANCODE_BREAK;
    keycode  = scancode & SCANCODE_KEY;

    /* シフトキー状態の更新 */
    if (keycode == KEY_SHIFT) {
        if (is_break) kbd_shift_state &= ~SHIFT_SHIFT;
        else          kbd_shift_state |=  SHIFT_SHIFT;
        return;
    }
    if (keycode == KEY_CTRL) {
        if (is_break) kbd_shift_state &= ~SHIFT_CTRL;
        else          kbd_shift_state |=  SHIFT_CTRL;
        return;
    }
    if (keycode == KEY_CAPS) {
        if (!is_break) kbd_shift_state ^= SHIFT_CAPS;
        return;
    }
    if (keycode == KEY_KANA) {
        if (!is_break) kbd_shift_state ^= SHIFT_KANA;
        return;
    }
    if (keycode == KEY_GRPH) {
        if (is_break) kbd_shift_state &= ~SHIFT_GRPH;
        else          kbd_shift_state |=  SHIFT_GRPH;
        return;
    }

    /* ブレイク(キー離し)は無視 */
    if (is_break) return;

    /* スキャンコード → ASCII変換 */
    if (kbd_shift_state & SHIFT_SHIFT) {
        ascii = scancode_to_ascii_shift[keycode];
    } else {
        ascii = scancode_to_ascii[keycode];
    }

    /* CAPS時の大文字小文字切替 */
    if (kbd_shift_state & SHIFT_CAPS) {
        if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
        else if (ascii >= 'A' && ascii <= 'Z') ascii += 32;
    }

    /* CTRL+文字 → コントロールコード */
    if ((kbd_shift_state & SHIFT_CTRL) && ascii >= 'a' && ascii <= 'z') {
        ascii = ascii - 'a' + 1;
    }

    /* バッファに格納: 全メイクキーイベントを格納（修飾キーは上で既にreturn済み）*/
    if (kbd_count < KBD_BUF_SIZE) {
        u16 entry = ((u16)keycode << 8) | ascii;
        kbd_buf[kbd_tail] = entry;
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        kbd_count++;
    }
}

/* ======================================================================== */
/*  kbd_init — キーボード初期化                                             */
/*  μPD8251Aの初期化とIRQ1有効化                                            */
/* ======================================================================== */
void kbd_init(void)
{
    u8 dummy;

    /*
     * μPD8251A 初期化
     *
     * FreeBSDのPC-98 pckbd.c では init_keyboard() は空関数で、
     * BIOSが既に8251Aを初期化済みであることを前提としている。
     *
     * 我々のベアメタルOSでも、ブートローダ経由でBIOSが起動時に
     * 8251Aを初期化しているため、基本的にはそのまま使える。
     * ただし安全のため、エラーリセットと受信イネーブルのみ行う。
     */

    /* 既存のデータを読み捨て (バッファフラッシュ) */
    while (inp(KBD_CMD) & KBD_STAT_RXRDY) {
        dummy = (u8)inp(KBD_DATA);
    }
    (void)dummy;

    /* コマンド: エラーリセット(D4=1) + 受信イネーブル(D2=1) */
    outp(KBD_CMD, KBD_CMD_ERRRST_RXE);

    /* バッファクリア */
    kbd_head = 0;
    kbd_tail = 0;
    kbd_count = 0;
    kbd_shift_state = 0;

    /* キーボードIRQを有効化 */
    irq_enable(KBD_IRQ);
}

/* ======================================================================== */
/*  公開API                                                                */
/* ======================================================================== */

int kbd_has_key(void)
{
    return kbd_count > 0;
}

int kbd_trygetchar(void)
{
    u16 entry;
    
    /* rshellモード: シリアル入力もチェック */
    if (rshell_active) {
        int sch;
        sch = serial_trygetchar();
        if (sch >= 0) return sch;
    }
    
    if (kbd_count == 0) return -1;

    _disable();
    entry = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
    kbd_count--;
    _enable();

    return (int)(entry & 0xFF);
}

int kbd_getchar(void)
{
    u16 entry;
    u32 timeout_ticks;

    /* rshellモード: KBD_TIMEOUT_TICKS タイムアウト (デフォルト300 ticks @ 100Hz) */
    timeout_ticks = rshell_active ? KBD_TIMEOUT_TICKS : 0;

    {
        u32 waited = 0;

        for (;;) {
            /* キーボードバッファ */
            if (kbd_count > 0) {
                _disable();
                entry = kbd_buf[kbd_head];
                kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
                kbd_count--;
                _enable();
                return (int)(entry & 0xFF);
            }

            /* rshellモード: シリアル入力もチェック */
            if (rshell_active) {
                int sch;
                sch = serial_trygetchar();
                if (sch >= 0) return sch;
            }

            __asm__ volatile("hlt");

            /* rshellタイムアウト: スペースキーを自動返却 */
            if (timeout_ticks > 0) {
                waited++;
                if (waited >= timeout_ticks) return ' ';
            }
        }
    }
}

/* u16キーコードを返す (上位=スキャンコード, 下位=ASCII) */
int kbd_getkey(void)
{
    u16 entry;
    while (kbd_count == 0) {
        __asm__ volatile("hlt");
    }

    _disable();
    entry = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
    kbd_count--;
    _enable();

    return (int)entry;
}

/* ノンブロッキング版: キーコードデータ(u16)を返す。なければ-1 */
int kbd_trygetkey(void)
{
    u16 entry;

    /* rshellモード: シリアル入力もチェック */
    if (rshell_active) {
        int sch;
        sch = serial_trygetchar();
        if (sch >= 0) return sch; /* シリアルはASCIIのみ(下位バイト) */
    }

    if (kbd_count == 0) return -1;

    _disable();
    entry = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
    kbd_count--;
    _enable();

    return (int)entry;  /* 上位=キーコード, 下位=ASCII */
}

/* 修飾キー(Ctrl/Shift/Alt等)の押下状態を取得 */
u32 kbd_get_modifiers(void)
{
    return (u32)kbd_shift_state;
}
