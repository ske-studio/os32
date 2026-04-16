/* ======================================================================== */
/*  LZSS.C — 非常にリソース軽量なLZSSデコーダ                               */
/*  (C) PC-9801向けOS32カーネル用 軽量解凍ルーチン                          */
/* ======================================================================== */

#include "lzss.h"

#define LZSS_N         4096   /* 辞書サイズ (4KB) */
#define LZSS_F         18     /* 最大一致長 */
#define LZSS_THRESHOLD 2      /* これよりもマッチ長が長い場合のみ圧縮する */

int lzss_decode_stream(lzss_getc_cb getc_cb, u32 src_len, u8 *dst, u32 dst_size)
{
    u8 text_buf[LZSS_N];
    int r = LZSS_N - LZSS_F;
    u32 src_idx = 0;
    u32 dst_idx = 0;
    u16 flags = 0;
    int i, j, k, ch;

    /* 初期化: 辞書の空き領域をスペースで埋める */
    for (i = 0; i < LZSS_N; i++) {
        text_buf[i] = ' ';
    }

    /* メインデコードループ */
    for (;;) {
        if (src_idx >= src_len) break;
        
        flags >>= 1;
        if ((flags & 256) == 0) {
            if (src_idx >= src_len) break;
            ch = getc_cb(); if (ch < 0) return -1;
            src_idx++;
            flags = (u16)ch | 0xFF00;
        }

        if (flags & 1) {
            /* 生データ */
            if (src_idx >= src_len) break;
            ch = getc_cb(); if (ch < 0) return -1;
            src_idx++;
            
            if (dst_idx < dst_size) {
                u8 c = (u8)ch;
                dst[dst_idx++] = c;
                text_buf[r++] = c;
                r &= (LZSS_N - 1);
            }
        } else {
            /* 圧縮データ */
            if (src_idx + 1 >= src_len) break;
            
            i = getc_cb(); if (i < 0) return -1; src_idx++;
            j = getc_cb(); if (j < 0) return -1; src_idx++;
            
            i |= ((j & 0xF0) << 4);
            j = (j & 0x0F) + LZSS_THRESHOLD;

            /* 辞書からコピー */
            for (k = 0; k <= j; k++) {
                if (dst_idx >= dst_size) break;
                u8 c = text_buf[(i + k) & (LZSS_N - 1)];
                dst[dst_idx++] = c;
                text_buf[r++] = c;
                r &= (LZSS_N - 1);
            }
        }
    }

    return (int)dst_idx;
}

/* ======================================================================== */
/*  LZSS エンコーダ                                                         */
/*  os32_server.py の lzss_encode() と完全互換                              */
/* ======================================================================== */

int lzss_encode(const u8 *src, u32 src_len, u8 *dst, u32 dst_size)
{
    u8 text_buf[LZSS_N];
    int r = LZSS_N - LZSS_F;
    u32 src_idx = 0;
    u32 dst_idx = 0;
    u32 flag_pos = 0;
    u8  flags = 0;
    int flag_bit = 1;
    int i;

    /* 辞書の初期化 */
    for (i = 0; i < LZSS_N; i++) {
        text_buf[i] = ' ';
    }

    while (src_idx < src_len) {
        int max_look, match_len, match_pos, mc, ii;

        /* フラグバイトの確保 */
        if (flag_bit == 1) {
            flag_pos = dst_idx;
            if (dst_idx >= dst_size) return -1;
            dst[dst_idx++] = 0;
        }

        /* 先読み長 */
        max_look = (int)(src_len - src_idx);
        if (max_look > LZSS_F) max_look = LZSS_F;

        /* 辞書内の最長一致検索 */
        match_len = 0;
        match_pos = 0;
        for (ii = 0; ii < LZSS_N; ii++) {
            /* 現在の書き込み位置 r〜r+F-1 は除外 */
            if (((ii - r) & (LZSS_N - 1)) < (u32)LZSS_F) continue;

            mc = 0;
            while (mc < max_look &&
                   text_buf[(ii + mc) & (LZSS_N - 1)] == src[src_idx + mc]) {
                mc++;
            }
            if (mc > match_len) {
                match_len = mc;
                match_pos = ii;
                if (match_len == max_look) break;
            }
        }

        if (match_len <= LZSS_THRESHOLD) {
            /* 生データ出力 */
            u8 c = src[src_idx];
            if (dst_idx >= dst_size) return -1;
            dst[dst_idx++] = c;
            flags |= (u8)flag_bit;
            text_buf[r] = c;
            r = (r + 1) & (LZSS_N - 1);
            src_idx++;
        } else {
            /* 圧縮参照出力 */
            u8 i_val = (u8)(match_pos & 0xFF);
            u8 j_val = (u8)(((match_pos >> 4) & 0xF0) |
                           ((match_len - LZSS_THRESHOLD - 1) & 0x0F));
            if (dst_idx + 1 >= dst_size) return -1;
            dst[dst_idx++] = i_val;
            dst[dst_idx++] = j_val;

            for (i = 0; i < match_len; i++) {
                text_buf[r] = src[src_idx + i];
                r = (r + 1) & (LZSS_N - 1);
            }
            src_idx += (u32)match_len;
        }

        flag_bit <<= 1;
        if (flag_bit > 128) {
            dst[flag_pos] = flags;
            flags = 0;
            flag_bit = 1;
        }
    }

    /* 最後のフラグバイトを書き出し */
    if (flag_bit > 1) {
        dst[flag_pos] = flags;
    }

    return (int)dst_idx;
}
