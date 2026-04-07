/* ======================================================================== */
/*  UTF8.C — UTF-8デコーダ / Unicode→JIS変換 / UTF-8文字列描画              */
/*                                                                          */
/*  システム標準エンコーディング: UTF-8                                        */
/*                                                                          */
/*  変換フロー:                                                              */
/*    UTF-8バイト列 → Unicodeコードポイント → JIS/ANKコード → KCG描画       */
/*                                                                          */
/*  変換の計算量:                                                            */
/*    - UTF-8デコード: O(1) ビット演算のみ                                   */
/*    - ひらがな/カタカナ: O(1) オフセット計算                                */
/*    - 漢字テーブル検索: O(log2(3160)) ≈ 12回比較                           */
/*    - テーブル検索はKCG I/Oアクセス(500μs)の2.6%以下                       */
/* ======================================================================== */

#include "utf8.h"
#include "memmap.h"

static const u16 *unicode_jis_table = (const u16 *)MEM_UNICODE_TABLE_BASE;

/* ======================================================================== */
/*  UTF-8デコード                                                           */
/*                                                                          */
/*  UTF-8エンコーディング:                                                   */
/*    1バイト: 0xxxxxxx                    → U+0000-U+007F                  */
/*    2バイト: 110xxxxx 10xxxxxx           → U+0080-U+07FF                  */
/*    3バイト: 1110xxxx 10xxxxxx 10xxxxxx  → U+0800-U+FFFF                  */
/*    4バイト: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx → U+10000+              */
/* ======================================================================== */
utf8_decode_t utf8_decode(const u8 *src)
{
    utf8_decode_t result;
    u8 b0 = src[0];

    if (b0 < 0x80) {
        /* ASCII (1バイト) */
        result.codepoint = b0;
        result.bytes_used = 1;
    }
    else if ((b0 & 0xE0) == 0xC0) {
        /* 2バイトシーケンス */
        u8 b1 = src[1];
        if ((b1 & 0xC0) == 0x80) {
            result.codepoint = ((u32)(b0 & 0x1F) << 6) | (b1 & 0x3F);
            result.bytes_used = 2;
        } else {
            result.codepoint = 0xFFFD;  /* 置換文字 */
            result.bytes_used = 1;
        }
    }
    else if ((b0 & 0xF0) == 0xE0) {
        /* 3バイトシーケンス (日本語はここ) */
        u8 b1 = src[1], b2 = src[2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            result.codepoint = ((u32)(b0 & 0x0F) << 12) |
                               ((u32)(b1 & 0x3F) << 6)  |
                               (b2 & 0x3F);
            result.bytes_used = 3;
        } else {
            result.codepoint = 0xFFFD;
            result.bytes_used = 1;
        }
    }
    else if ((b0 & 0xF8) == 0xF0) {
        /* 4バイトシーケンス (絵文字等) */
        u8 b1 = src[1], b2 = src[2], b3 = src[3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            result.codepoint = ((u32)(b0 & 0x07) << 18) |
                               ((u32)(b1 & 0x3F) << 12) |
                               ((u32)(b2 & 0x3F) << 6)  |
                               (b3 & 0x3F);
            result.bytes_used = 4;
        } else {
            result.codepoint = 0xFFFD;
            result.bytes_used = 1;
        }
    }
    else {
        /* 不正バイト */
        result.codepoint = 0xFFFD;
        result.bytes_used = 1;
    }

    return result;
}

/* ======================================================================== */
/*  Unicode → ANKコード変換                                                 */
/*  ANK描画可能なら文字コードを返す、不可なら0                                */
/* ======================================================================== */
u8 unicode_to_ank(u32 cp)
{
    /* ASCII */
    if (cp >= 0x20 && cp <= 0x7E) {
        return (u8)cp;
    }

    /* 全角英数 → 半角ASCII にフォールバック */
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        return (u8)(cp - 0xFF01 + 0x21);
    }

    /* 半角カナ (U+FF61-U+FF9F → ANK 0xA1-0xDF) */
    if (cp >= 0xFF61 && cp <= 0xFF9F) {
        return (u8)(cp - 0xFF61 + 0xA1);
    }

    /* バックスラッシュ/円記号 */
    if (cp == 0x00A5) return 0x5C;  /* ¥ → \ */

    return 0;
}

/* ======================================================================== */
/*  Unicode → JIS変換 (O(1) テーブル参照)                                   */
/* ======================================================================== */
u16 unicode_to_jis(u32 cp)
{
    /* ひらがな: U+3041-U+3093 → JIS 0x2421-0x2473 */
    if (cp >= 0x3041 && cp <= 0x3093) {
        return (u16)(0x2421 + (cp - 0x3041));
    }

    /* カタカナ: U+30A1-U+30F6 → JIS 0x2521-0x2576 */
    if (cp >= 0x30A1 && cp <= 0x30F6) {
        return (u16)(0x2521 + (cp - 0x30A1));
    }

    /* BMP範囲外 → 変換不可 */
    if (cp > 0xFFFF) return 0;

    /* O(1) でテーブルから直接引く */
    return unicode_jis_table[cp];
}
