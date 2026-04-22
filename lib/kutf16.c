/* ======================================================================== */
/*  KUTF16.C — UTF-8 ↔ UTF-16LE 変換ユーティリティ                        */
/*                                                                          */
/*  BMP範囲 (U+0000〜U+FFFF) のみサポート。サロゲートペア非対応。          */
/* ======================================================================== */

#include "kutf16.h"

/* ------------------------------------------------------------------------ */
/*  kutf8_to_utf16le — UTF-8 → UTF-16LE 変換                               */
/* ------------------------------------------------------------------------ */
int kutf8_to_utf16le(const char *utf8, u16 *utf16, int max_words)
{
    const u8 *s = (const u8 *)utf8;
    int out = 0;
    u32 cp;

    if (!utf8 || !utf16 || max_words < 1) return 0;

    while (*s && out < max_words - 1) {
        if (*s < 0x80) {
            /* ASCII: 1バイト */
            cp = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            /* 2バイトシーケンス: U+0080〜U+07FF */
            cp = (*s++ & 0x1F) << 6;
            if ((*s & 0xC0) != 0x80) break;
            cp |= (*s++ & 0x3F);
        } else if ((*s & 0xF0) == 0xE0) {
            /* 3バイトシーケンス: U+0800〜U+FFFF */
            cp = (*s++ & 0x0F) << 12;
            if ((*s & 0xC0) != 0x80) break;
            cp |= (*s++ & 0x3F) << 6;
            if ((*s & 0xC0) != 0x80) break;
            cp |= (*s++ & 0x3F);
        } else {
            /* 4バイト以上 (BMP外): スキップ */
            s++;
            while ((*s & 0xC0) == 0x80) s++;
            continue;
        }
        /* BMP範囲チェック */
        if (cp > 0xFFFF) continue;
        utf16[out++] = (u16)cp;
    }
    utf16[out++] = 0;   /* NULL終端 */
    return out;
}

/* ------------------------------------------------------------------------ */
/*  kutf16le_to_utf8 — UTF-16LE → UTF-8 変換                               */
/* ------------------------------------------------------------------------ */
int kutf16le_to_utf8(const u16 *utf16, int utf16_len,
                     char *utf8, int max_bytes)
{
    int i;
    int words = utf16_len / 2;  /* バイト数→WORD数 */
    int out = 0;
    u16 cp;

    if (!utf16 || !utf8 || max_bytes < 1) return 0;

    for (i = 0; i < words; i++) {
        cp = utf16[i];
        if (cp == 0) break;

        if (cp < 0x80) {
            /* ASCII */
            if (out + 1 >= max_bytes) break;
            utf8[out++] = (char)cp;
        } else if (cp < 0x800) {
            /* 2バイトUTF-8 */
            if (out + 2 >= max_bytes) break;
            utf8[out++] = (char)(0xC0 | (cp >> 6));
            utf8[out++] = (char)(0x80 | (cp & 0x3F));
        } else {
            /* 3バイトUTF-8 */
            if (out + 3 >= max_bytes) break;
            utf8[out++] = (char)(0xE0 | (cp >> 12));
            utf8[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[out++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    utf8[out++] = '\0';
    return out;
}
