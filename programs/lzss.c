/* ======================================================================== */
/*  LZSS.C — LZSS圧縮/展開コマンド                                          */
/*                                                                          */
/*  使い方:                                                                 */
/*    lzss c INPUT OUTPUT   ... INPUT を圧縮して OUTPUT に書き出す           */
/*    lzss d INPUT OUTPUT   ... INPUT を展開して OUTPUT に書き出す           */
/*    lzss t INPUT          ... INPUT の圧縮率をテスト表示                   */
/* ======================================================================== */

#include "os32api.h"
#include <string.h>

/* LZSS定数 (lib/lzss.c と同一) */
#define LZSS_N         4096
#define LZSS_F         18
#define LZSS_THRESHOLD 2

/* LZSS圧縮ファイルヘッダ (8バイト) */
#define LZSS_MAGIC_0 'L'
#define LZSS_MAGIC_1 'Z'
#define LZSS_MAGIC_2 'S'
#define LZSS_MAGIC_3 'S'

/* バッファ (BSS配置) */
static u8 src_buf[65536];
static u8 dst_buf[65536 + 1024]; /* 圧縮時に膨張する可能性があるため余裕をもたせる */

/* lib/lzss.c のエンコーダ/デコーダプロトタイプ */
extern int lzss_encode(const u8 *src, u32 src_len, u8 *dst, u32 dst_size);

/* デコーダ用: バッファからの読み込みコールバック */
static const u8 *g_dec_ptr;
static int dec_getc(void)
{
    return *g_dec_ptr++;
}

extern int lzss_decode_stream(int (*getc_cb)(void), u32 src_len, u8 *dst, u32 dst_size);

int main(int argc, char **argv, KernelAPI *api)
{
    const char *mode;
    const char *input_path;
    const char *output_path;
    int fd, sz;

    if (argc < 3) {
        api->kprintf(0xE1, "%s", "Usage: lzss c|d|t INPUT [OUTPUT]\n");
        api->kprintf(0xE1, "%s", "  c  Compress INPUT to OUTPUT\n");
        api->kprintf(0xE1, "%s", "  d  Decompress INPUT to OUTPUT\n");
        api->kprintf(0xE1, "%s", "  t  Test compression ratio\n");
        return 1;
    }

    mode = argv[1];
    input_path = argv[2];
    output_path = (argc >= 4) ? argv[3] : (const char *)0;

    /* 入力ファイル読み込み */
    fd = api->sys_open(input_path, 0);
    if (fd < 0) {
        api->kprintf(0xE1 | 0x40, "lzss: %s not found\n", input_path);
        return 1;
    }
    sz = api->sys_read(fd, src_buf, sizeof(src_buf));
    api->sys_close(fd);
    if (sz <= 0) {
        api->kprintf(0xE1 | 0x40, "%s", "lzss: read error\n");
        return 1;
    }

    if (mode[0] == 'c') {
        /* 圧縮モード */
        int enc_sz;
        if (!output_path) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: output path required for compress\n");
            return 1;
        }

        enc_sz = lzss_encode(src_buf, (u32)sz, dst_buf + 8, sizeof(dst_buf) - 8);
        if (enc_sz < 0) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: compression failed\n");
            return 1;
        }

        /* ヘッダ書き込み: magic(4) + original_size(4) */
        dst_buf[0] = LZSS_MAGIC_0;
        dst_buf[1] = LZSS_MAGIC_1;
        dst_buf[2] = LZSS_MAGIC_2;
        dst_buf[3] = LZSS_MAGIC_3;
        dst_buf[4] = (u8)(sz & 0xFF);
        dst_buf[5] = (u8)((sz >> 8) & 0xFF);
        dst_buf[6] = (u8)((sz >> 16) & 0xFF);
        dst_buf[7] = (u8)((sz >> 24) & 0xFF);

        fd = api->sys_open(output_path, 1 | 0x0100 | 0x0200);
        if (fd < 0) {
            api->kprintf(0xE1 | 0x40, "lzss: cannot create %s\n", output_path);
            return 1;
        }
        if ((int)api->sys_write(fd, dst_buf, (u32)(enc_sz + 8)) != enc_sz + 8) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: write error\n");
            api->sys_close(fd);
            return 1;
        }
        api->sys_close(fd);

        api->kprintf(0xE1, "%s -> %s: %d -> %d bytes",
                     input_path, output_path, sz, enc_sz + 8);
        api->kprintf(0xE1, " (%u%%)\n",
                     (u32)(enc_sz + 8) * 100 / (u32)sz);

    } else if (mode[0] == 'd') {
        /* 展開モード */
        u32 orig_size;
        int dec_sz;

        if (!output_path) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: output path required for decompress\n");
            return 1;
        }

        /* ヘッダ検証 */
        if (sz < 8 ||
            src_buf[0] != LZSS_MAGIC_0 || src_buf[1] != LZSS_MAGIC_1 ||
            src_buf[2] != LZSS_MAGIC_2 || src_buf[3] != LZSS_MAGIC_3) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: not a LZSS file (bad magic)\n");
            return 1;
        }

        orig_size = (u32)src_buf[4] | ((u32)src_buf[5] << 8) |
                    ((u32)src_buf[6] << 16) | ((u32)src_buf[7] << 24);

        if (orig_size > sizeof(dst_buf)) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: decompressed size too large\n");
            return 1;
        }

        g_dec_ptr = src_buf + 8;
        dec_sz = lzss_decode_stream(dec_getc, (u32)(sz - 8), dst_buf, orig_size);
        if (dec_sz < 0) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: decompression failed\n");
            return 1;
        }

        fd = api->sys_open(output_path, 1 | 0x0100 | 0x0200);
        if (fd < 0) {
            api->kprintf(0xE1 | 0x40, "lzss: cannot create %s\n", output_path);
            return 1;
        }
        if ((int)api->sys_write(fd, dst_buf, (u32)dec_sz) != dec_sz) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: write error\n");
            api->sys_close(fd);
            return 1;
        }
        api->sys_close(fd);

        api->kprintf(0xE1, "%s -> %s: %d -> %d bytes\n",
                     input_path, output_path, sz, dec_sz);

    } else if (mode[0] == 't') {
        /* テストモード（圧縮率のみ表示） */
        int enc_sz = lzss_encode(src_buf, (u32)sz, dst_buf, sizeof(dst_buf));
        if (enc_sz < 0) {
            api->kprintf(0xE1 | 0x40, "%s", "lzss: compression failed\n");
            return 1;
        }
        api->kprintf(0xE1, "%s: %d -> %d bytes",
                     input_path, sz, enc_sz + 8);
        api->kprintf(0xE1, " (%u%%, saved %d bytes)\n",
                     (u32)(enc_sz + 8) * 100 / (u32)sz,
                     sz - (enc_sz + 8));
    } else {
        api->kprintf(0xE1 | 0x40, "lzss: unknown mode '%s'\n", mode);
        return 1;
    }

    return 0;
}
