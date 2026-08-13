/* ======================================================================== */
/*  CONSOLE.C — テキストコンソールドライバ                                   */
/*                                                                          */
/*  テキストVRAM操作、カーソル管理、shell_print等のコンソール出力を提供。    */
/*  shell.cから分離。外部プログラム(shell.bin含む)はKernelAPI経由で利用。    */
/* ======================================================================== */

#include "types.h"
#include "serial.h"
#include "utf8.h"
#include "tvram.h"
#include "io.h"
#include "pc98.h"
#include "kprintf.h"

/* V86 セッション中の画面描画抑止 (kernel/v86.c)。
 * セッション中は低位 640KB がゲスト用バッキング RAM に差し替わっており、
 * Unicode-JIS 表 (0x4A000) を読むとゲストのメモリをテーブルとして解釈して
 * 文字化けする。TVRAM/KCG への書き込みもゲストの画面と CG 状態を汚す。
 * 描画だけを抑止し、シリアル出力 (rshell ログ) は通す。 */
extern int v86_is_active(void);

/* テキストVRAM定義 (tvram.hとpc98.hの定義を使用) */
#define TVRAM_TEXT  TVRAM_BASE

/* デフォルト属性 (os32_kapi_shared.h で定義済みの場合はスキップ) */
#ifndef ATTR_WHITE
#define ATTR_WHITE   TATTR_WHITE
#endif

/* rshellフラグ (外部から設定可) */
int rshell_active = 0;

/* カーソル位置 */
static int cursor_x = 0;
static int cursor_y = 0;

/* スクロール保護行数 (下から数えた固定行)。0=全行 */
static int g_scroll_reserve = 0;

#define CONSOLE_LAST_ROW  (TVRAM_ROWS - 1 - g_scroll_reserve)

void tvram_set_scroll_reserve(int rows)
{
    if (rows < 0) {
        rows = 0;
    }
    if (rows >= TVRAM_ROWS) {
        rows = TVRAM_ROWS - 1;
    }
    g_scroll_reserve = rows;
}

/* ======================================================================== */
/*  TVRAM低レベル操作                                                       */
/* ======================================================================== */

void tvram_clear(void)
{
    volatile u16 *text = (volatile u16 *)TVRAM_TEXT;
    volatile u8  *attr;
    int i;
    for (i = 0; i < TVRAM_COLS * TVRAM_ROWS; i++) {
        text[i] = 0x0020;
        attr = (volatile u8 *)(TVRAM_ATTR + (u32)i * 2);
        *attr = ATTR_WHITE;
    }
    cursor_x = 0;
    cursor_y = 0;
}

void tvram_putchar_at(int x, int y, char ch, u8 color)
{
    u32 offset = (u32)y * TVRAM_BPR + (u32)x * 2;
    *(volatile u16 *)(TVRAM_TEXT + offset) = (u16)(u8)ch;
    *(volatile u8 *)(TVRAM_ATTR + offset) = color;
}

/* TVRAM 1セル読み取り (文字コード + 属性) */
void tvram_readchar_at(int x, int y, u16 *code, u8 *attr)
{
    u32 offset = (u32)y * TVRAM_BPR + (u32)x * 2;
    if (code) *code = *(volatile u16 *)(TVRAM_TEXT + offset);
    if (attr) *attr = *(volatile u8 *)(TVRAM_ATTR + offset);
}

/* TVRAM 反転トグル (漢字対応)
 * 指定位置の属性反転ビット(0x04)をXORでトグルする。
 * 漢字右半分の場合は左半分に自動調整し、2セル同時反転。
 * 戻り値: 反転したセル数 (1=ANK, 2=漢字) */
int tvram_reverse_cell(int x, int y)
{
    u16 code;
    u8 attr;
    int width, i, rx;

    if (x < 0 || x >= TVRAM_COLS || y < 0 || y >= TVRAM_ROWS) return 0;

    rx = x;

    /* 漢字判定: 右半分 (bit7セット) なら左半分に揃える */
    code = *(volatile u16 *)(TVRAM_TEXT + (u32)y * TVRAM_BPR + (u32)rx * 2);
    if ((code >> 8) != 0 && (code & 0x80)) {
        if (rx > 0) rx--;
    }

    /* 左半分の文字コードで幅を判定 */
    code = *(volatile u16 *)(TVRAM_TEXT + (u32)y * TVRAM_BPR + (u32)rx * 2);
    if ((code >> 8) != 0 && !(code & 0x80) && rx < TVRAM_COLS - 1) {
        width = 2;  /* 漢字 */
    } else {
        width = 1;  /* ANK */
    }

    /* 反転ビットをトグル */
    for (i = 0; i < width; i++) {
        u32 offset = (u32)y * TVRAM_BPR + (u32)(rx + i) * 2;
        volatile u8 *p = (volatile u8 *)(TVRAM_ATTR + offset);
        *p ^= 0x04;
    }

    return width;
}

void tvram_scroll(void)
{
    volatile u16 *text = (volatile u16 *)TVRAM_TEXT;
    volatile u16 *attr = (volatile u16 *)TVRAM_ATTR;
    int rows = TVRAM_ROWS - g_scroll_reserve;
    int i;

    /* 0 .. rows-2 行目までを1行引き上げ */
    for (i = 0; i < TVRAM_COLS * (rows - 1); i++) {
        text[i] = text[i + TVRAM_COLS];
        attr[i] = attr[i + TVRAM_COLS];
    }
    /* rows-1 行目を空白化 */
    for (i = TVRAM_COLS * (rows - 1); i < TVRAM_COLS * rows; i++) {
        text[i] = 0x0020;
        attr[i] = ATTR_WHITE;
    }
}

/* 全角漢字1文字をTVRAMに書き込み
 * PC9800Bible §2-6-2 */
void tvram_putkanji_at(int x, int y, u16 jis, u8 color)
{
    u32 offset = (u32)y * TVRAM_BPR + (u32)x * 2;
    u8 jh = (u8)((jis >> 8) & 0xFF);
    u8 jl = (u8)(jis & 0xFF);
    *(volatile u16 *)(TVRAM_TEXT + offset) = (u16)(jh - 0x20) | ((u16)jl << 8);
    *(volatile u8 *)(TVRAM_ATTR + offset) = color;
    *(volatile u16 *)(TVRAM_TEXT + offset + 2) = (u16)(jh - 0x20 + 0x80) | ((u16)jl << 8);
    *(volatile u8 *)(TVRAM_ATTR + offset + 2) = color;
}

/* ======================================================================== */
/*  コンソール出力 (カーソル追従)                                            */
/* ======================================================================== */

/* 1文字出力 */
void shell_putchar(char ch, u8 color)
{
    if (ch == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (ch == '\r') {
        cursor_x = 0;
    } else if (ch == 0x08) {
        if (cursor_x > 0) {
            cursor_x--;
            tvram_putchar_at(cursor_x, cursor_y, ' ', ATTR_WHITE);
        }
        return;
    } else {
        tvram_putchar_at(cursor_x, cursor_y, ch, color);
        cursor_x++;
    }
    if (cursor_x >= TVRAM_COLS) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y > CONSOLE_LAST_ROW) {
        tvram_scroll();
        cursor_y = CONSOLE_LAST_ROW;
    }
}

/* 文字列表示 */
void shell_print(const char *str, u8 color)
{
    int render = !v86_is_active();
    while (*str) {
        if (render) shell_putchar(*str, color);
        if (rshell_active) serial_putchar(*str);
        str++;
    }
}

/* 10進表示 (変換は kprintf.h の kutoa_dec に統一) */
void shell_print_dec(u32 val, u8 color)
{
    char buf[12];
    kutoa_dec(val, buf, (int)sizeof(buf));
    shell_print(buf, color);
}

/* 32ビット16進表示 */
void shell_print_hex32(u32 val, u8 color)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 9; i >= 2; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[10] = '\0';
    shell_print(buf, color);
}

/* UTF-8文字列表示 (漢字対応) */
void shell_print_utf8(const char *utf8_str, u8 color)
{
    const u8 *p = (const u8 *)utf8_str;

    if (rshell_active) {
        const char *s = utf8_str;
        while (*s) serial_putchar(*s++);
    }
    if (v86_is_active()) return;    /* 描画抑止 (シリアルには出した) */
    while (*p) {
        utf8_decode_t dec;
        u32 cp;
        u8 ank;
        u16 jis;

        if (*p == '\n') {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > CONSOLE_LAST_ROW) {
                tvram_scroll();
                cursor_y = CONSOLE_LAST_ROW;
            }
            p++;
            continue;
        }
        if (*p == '\r') { cursor_x = 0; p++; continue; }
        if (*p == '\t') { cursor_x = (cursor_x + 4) & ~3; p++; continue; }

        dec = utf8_decode(p);
        cp = dec.codepoint;
        p += dec.bytes_used;

        if (cp == 0xFEFF || cp < 0x20) continue;

        ank = unicode_to_ank(cp);
        if (ank) {
            tvram_putchar_at(cursor_x, cursor_y, (char)ank, color);
            cursor_x++;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            continue;
        }

        jis = unicode_to_jis(cp);
        if (jis) {
            if (cursor_x >= TVRAM_COLS - 1) {
                cursor_x = 0;
                cursor_y++;
                if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            }
            tvram_putkanji_at(cursor_x, cursor_y, jis, color);
            cursor_x += 2;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            continue;
        }

        /* 変換不可: □を表示 */
        if (cursor_x >= TVRAM_COLS - 1) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
        }
        tvram_putkanji_at(cursor_x, cursor_y, 0x2222, color);  /* □ (JIS 0x2222) */
        cursor_x += 2;
        if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
        if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
    }
}

/* UTF-8ストリーム出力 (サイズ指定) */
void console_write(const char *buf, u32 size, u8 color)
{
    const u8 *p = (const u8 *)buf;
    u32 remaining = size;

    if (rshell_active) {
        u32 i;
        for (i = 0; i < size; i++) serial_putchar(buf[i]);
    }

    if (v86_is_active()) return;    /* 描画抑止 (シリアルには出した) */

    while (remaining > 0) {
        utf8_decode_t dec;
        u32 cp;
        u8 ank;
        u16 jis;

        if (*p == '\n') {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > CONSOLE_LAST_ROW) {
                tvram_scroll();
                cursor_y = CONSOLE_LAST_ROW;
            }
            p++; remaining--;
            continue;
        }
        if (*p == '\r') { cursor_x = 0; p++; remaining--; continue; }
        if (*p == '\t') { cursor_x = (cursor_x + 4) & ~3; p++; remaining--; continue; }

        if (remaining < 4) {
            u8 tmp[4] = {0, 0, 0, 0};
            u32 k;
            for (k = 0; k < remaining; k++) tmp[k] = p[k];
            dec = utf8_decode(tmp);
            if (dec.bytes_used > remaining) {
                /* 分断された文字: ここではスキップして進める */
                p += remaining;
                remaining = 0;
                continue;
            }
        } else {
            dec = utf8_decode(p);
        }
        
        cp = dec.codepoint;
        p += dec.bytes_used;
        remaining -= dec.bytes_used;

        if (cp == 0xFEFF || cp < 0x20) continue;

        ank = unicode_to_ank(cp);
        if (ank) {
            tvram_putchar_at(cursor_x, cursor_y, (char)ank, color);
            cursor_x++;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            continue;
        }

        jis = unicode_to_jis(cp);
        if (jis) {
            if (cursor_x >= TVRAM_COLS - 1) {
                cursor_x = 0;
                cursor_y++;
                if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            }
            tvram_putkanji_at(cursor_x, cursor_y, jis, color);
            cursor_x += 2;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
            continue;
        }

        /* 変換不可: □を表示 */
        if (cursor_x >= TVRAM_COLS - 1) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
        }
        tvram_putkanji_at(cursor_x, cursor_y, 0x2222, color);  /* □ (JIS 0x2222) */
        cursor_x += 2;
        if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
        if (cursor_y > CONSOLE_LAST_ROW) { tvram_scroll(); cursor_y = CONSOLE_LAST_ROW; }
    }
}

/* カーソル位置取得/設定 (外部プログラム用) */
int console_get_cursor_x(void) { return cursor_x; }
int console_get_cursor_y(void) { return cursor_y; }
void console_set_cursor(int x, int y) 
{ 
    u16 offset;
    cursor_x = x; 
    cursor_y = y; 

    /* PC-98 GDC (テキスト) カーソル更新 */
    /* CSONコマンドでカーソル表示を明示的に有効化 */
    outp(GDC_TEXT_CMD, GDC_CMD_CSON);
    
    /* CSRWコマンドで位置設定 */
    offset = (u16)(y * TVRAM_COLS + x);
    outp(GDC_TEXT_CMD, GDC_CMD_CSRW);
    outp(GDC_TEXT_PARAM, (u8)(offset & 0xFF));
    outp(GDC_TEXT_PARAM, (u8)((offset >> 8) & 0xFF));
}

/* コンソール画面サイズ取得 */
void console_get_size(int *w, int *h)
{
    if (w) *w = TVRAM_COLS;
    if (h) *h = TVRAM_ROWS;
}
