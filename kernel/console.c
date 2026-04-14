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

void tvram_scroll(void)
{
    volatile u16 *text = (volatile u16 *)TVRAM_TEXT;
    volatile u16 *attr = (volatile u16 *)TVRAM_ATTR;
    int i;
    for (i = 0; i < TVRAM_COLS * (TVRAM_ROWS - 1); i++) {
        text[i] = text[i + TVRAM_COLS];
        attr[i] = attr[i + TVRAM_COLS];
    }
    for (i = TVRAM_COLS * (TVRAM_ROWS - 1); i < TVRAM_COLS * TVRAM_ROWS; i++) {
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
    if (cursor_y >= TVRAM_ROWS) {
        tvram_scroll();
        cursor_y = TVRAM_ROWS - 1;
    }
}

/* 文字列表示 */
void shell_print(const char *str, u8 color)
{
    while (*str) {
        shell_putchar(*str, color);
        if (rshell_active) serial_putchar(*str);
        str++;
    }
}

/* 10進表示 */
void shell_print_dec(u32 val, u8 color)
{
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (val == 0) {
        shell_print("0", color);
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i] = '0' + (val % 10);
        val /= 10;
        i--;
    }
    shell_print(&buf[i + 1], color);
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
    while (*p) {
        utf8_decode_t dec;
        u32 cp;
        u8 ank;
        u16 jis;

        if (*p == '\n') {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= TVRAM_ROWS) {
                tvram_scroll();
                cursor_y = TVRAM_ROWS - 1;
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
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            continue;
        }

        jis = unicode_to_jis(cp);
        if (jis) {
            if (cursor_x >= TVRAM_COLS - 1) {
                cursor_x = 0;
                cursor_y++;
                if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            }
            tvram_putkanji_at(cursor_x, cursor_y, jis, color);
            cursor_x += 2;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            continue;
        }

        /* 変換不可: □を表示 */
        if (cursor_x >= TVRAM_COLS - 1) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
        }
        tvram_putkanji_at(cursor_x, cursor_y, 0x2222, color);  /* □ (JIS 0x2222) */
        cursor_x += 2;
        if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
        if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
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
    
    while (remaining > 0) {
        utf8_decode_t dec;
        u32 cp;
        u8 ank;
        u16 jis;

        if (*p == '\n') {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= TVRAM_ROWS) {
                tvram_scroll();
                cursor_y = TVRAM_ROWS - 1;
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
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            continue;
        }

        jis = unicode_to_jis(cp);
        if (jis) {
            if (cursor_x >= TVRAM_COLS - 1) {
                cursor_x = 0;
                cursor_y++;
                if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            }
            tvram_putkanji_at(cursor_x, cursor_y, jis, color);
            cursor_x += 2;
            if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
            continue;
        }

        /* 変換不可: □を表示 */
        if (cursor_x >= TVRAM_COLS - 1) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
        }
        tvram_putkanji_at(cursor_x, cursor_y, 0x2222, color);  /* □ (JIS 0x2222) */
        cursor_x += 2;
        if (cursor_x >= TVRAM_COLS) { cursor_x = 0; cursor_y++; }
        if (cursor_y >= TVRAM_ROWS) { tvram_scroll(); cursor_y = TVRAM_ROWS - 1; }
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
