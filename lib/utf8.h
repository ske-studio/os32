/* ======================================================================== */
/*  UTF8.H — UTF-8デコーダ / Unicode→JIS変換                                */
/*                                                                          */
/*  システム標準エンコーディング: UTF-8                                        */
/* ======================================================================== */

#ifndef UTF8_H
#define UTF8_H

#include "types.h"

/* UTF-8デコード結果 */
typedef struct {
    u32 codepoint;    /* Unicodeコードポイント (0-0x10FFFF) */
    int bytes_used;   /* 消費バイト数 (1-4, エラー時1) */
} utf8_decode_t;

/* UTF-8 → Unicode デコード (1文字分) */
utf8_decode_t utf8_decode(const u8 *src);

/* Unicode → JIS変換 (0=変換不可) */
u16 unicode_to_jis(u32 codepoint);

/* Unicode → ANKコード (0=非ANK) */
u8 unicode_to_ank(u32 codepoint);

#endif /* UTF8_H */
