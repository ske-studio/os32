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
