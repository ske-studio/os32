/* ======================================================================== */
/*  VDPVIEW.C — VDPファイル表示プログラム (OS32外部プログラム)                */
/*                                                                          */
/*  VDP1/VDP2形式のファイルを読み込み、バックバッファに展開して表示する。     */
/*  デコードはこのプログラム内で完結し、libos32gfxのバックバッファに          */
/*  RAWプレーンデータを直接書き込む方式。                                    */
/*                                                                          */
/*  対応DataType:                                                           */
/*    0x00 (DUMP)  — 640x400 フルスクリーン                                 */
/*    0x01 (RAW)   — 任意サイズ RGBI プレーン                               */
/*  圧縮: 非圧縮(0x00) / PackBits RLE(0x01)                                */
/*                                                                          */
/*  使い方: exec vdpview FILENAME.VDP [-k]                                  */
/*    -k : 終了時にVRAMをクリアしない                                        */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ---- VDP定数 ---- */
#define VDP_HDR_SIZE     256
#define VDP_BPL          80   /* 640/8 = 80 bytes per line */
#define VDP_PLANE_SIZE   32000 /* 80 * 400 */

/* ---- VDPヘッダ解析結果 ---- */
typedef struct {
    u16 width;
    u16 height;
    u8  num_planes;
    u8  data_type;
    u8  palette[48];    /* RGB 各4bit (0-15), 16色分 */
    u8  compress;       /* 0x00=RAW, 0x01=PackBits */
    int is_v2;          /* 1=VDP2, 0=VDP1 */
} VdpInfo;

/* ======================================================================== */
/*  VDPヘッダ解析 (VDP1/VDP2 自動判定)                                      */
/* ======================================================================== */
static int parse_vdp_header(const u8 *buf, VdpInfo *info)
{
    int i;

    /* マジック確認 */
    if (buf[0] != 'V' || buf[1] != 'D' || buf[2] != 'P') return -1;
    if (buf[3] != '1' && buf[3] != '2') return -1;

    info->is_v2 = (buf[3] == '2') ? 1 : 0;

    /* 共通フィールド (同一オフセット) */
    info->width  = (u16)buf[4] | ((u16)buf[5] << 8);
    info->height = (u16)buf[6] | ((u16)buf[7] << 8);
    info->num_planes = buf[8];
    info->data_type  = buf[9];

    /* パレット (VDP1とVDP2でオフセットが異なる) */
    if (info->is_v2) {
        /* VDP2: パレット @ 0x10-0x3F */
        for (i = 0; i < 48; i++) {
            info->palette[i] = buf[0x10 + i];
        }
        /* VDP2: 圧縮フラグ @ 0x50 */
        info->compress = buf[0x50];
    } else {
        /* VDP1: パレット @ 0x0A-0x39 */
        for (i = 0; i < 48; i++) {
            info->palette[i] = buf[0x0A + i];
        }
        /* VDP1: 圧縮フラグ @ 0x5C */
        info->compress = buf[0x5C];
    }

    return 0;
}

/* ======================================================================== */
/*  PackBits デコーダ                                                       */
/*  入力: fd (ファイルディスクリプタ)                                        */
/*  出力: dst (デコード結果バッファ), size (出力バイト数)                    */
/* ======================================================================== */
static int decode_packbits(KernelAPI *api, int fd, u8 *dst, int size)
{
    int count = 0;
    while (count < size) {
        u8 cmd;
        if (api->sys_read(fd, &cmd, 1) != 1) return -1;

        if (cmd < 128) {
            /* リテラルデータ: cmd+1 バイトをそのままコピー */
            int raw_len = cmd + 1;
            if (count + raw_len > size) raw_len = size - count;
            if (api->sys_read(fd, dst + count, raw_len) != raw_len) return -1;
            count += raw_len;
        } else if (cmd > 128) {
            /* ランレングス: 257-cmd 回繰り返し */
            int run_len = 257 - cmd;
            u8 val;
            int j;
            u32 val32;
            if (api->sys_read(fd, &val, 1) != 1) return -1;
            if (count + run_len > size) run_len = size - count;

            /* 32bitアクセスで高速フィル */
            val32 = (u32)val | ((u32)val << 8) | ((u32)val << 16) | ((u32)val << 24);
            j = 0;
            while (j + 4 <= run_len) {
                *(u32 *)(dst + count + j) = val32;
                j += 4;
            }
            while (j < run_len) {
                dst[count + j] = val;
                j++;
            }
            count += run_len;
        }
        /* cmd == 128 は NOP (無視) */
    }
    return 0;
}

/* ======================================================================== */
/*  プレーンデータ読み込み (非圧縮 / PackBits 自動判定)                     */
/* ======================================================================== */
static int read_plane(KernelAPI *api, int fd, u8 *plane_buf,
                      int plane_size, u8 compress)
{
    if (compress == 0x01) {
        return decode_packbits(api, fd, plane_buf, plane_size);
    } else {
        /* 非圧縮 */
        if (api->sys_read(fd, plane_buf, plane_size) != plane_size) {
            return -1;
        }
        return 0;
    }
}

/* ======================================================================== */
/*  パレット適用                                                            */
/* ======================================================================== */
static void apply_palette(KernelAPI *api, const VdpInfo *info)
{
    int i;
    for (i = 0; i < 16; i++) {
        u8 r = info->palette[i * 3 + 0];
        u8 g = info->palette[i * 3 + 1];
        u8 b = info->palette[i * 3 + 2];
        api->gfx_set_palette(i, r, g, b);
    }
}

/* ======================================================================== */
/*  VDP表示: フルスクリーン DUMP/RAW (DataType 0x00 / 0x01)                 */
/*  640x400の場合はプレーンに直接読み込み、                                  */
/*  それ以外のサイズは中央配置で展開する。                                   */
/* ======================================================================== */
static int display_dump(KernelAPI *api, int fd, const VdpInfo *info)
{
    int p;
    int bpl = info->width / 8;       /* ソースのbytes per line */
    int plane_size = bpl * info->height;

    if (info->width == 640 && info->height == 400) {
        /* フルスクリーン: 直接バックバッファに読み込み */
        for (p = 0; p < 4 && p < info->num_planes; p++) {
            if (read_plane(api, fd, gfx_fb.planes[p],
                           VDP_PLANE_SIZE, info->compress) < 0) {
                return -1;
            }
        }
    } else {
        /* 任意サイズ: 一時バッファ経由で中央配置 */
        u8 *tmp;
        int dx, dy;
        int y, copy_w, copy_h;

        tmp = (u8 *)api->mem_alloc(plane_size);
        if (!tmp) return -1;

        /* 表示位置 (中央基点) */
        dx = (640 - (int)info->width) / 2;
        dy = (400 - (int)info->height) / 2;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;

        /* コピーする範囲 (画面外クリップ) */
        copy_w = (int)info->width;
        copy_h = (int)info->height;
        if (dx + copy_w > 640) copy_w = 640 - dx;
        if (dy + copy_h > 400) copy_h = 400 - dy;

        for (p = 0; p < 4 && p < info->num_planes; p++) {
            if (read_plane(api, fd, tmp, plane_size, info->compress) < 0) {
                api->mem_free(tmp);
                return -1;
            }

            /* 一時バッファからバックバッファへ行単位コピー */
            {
                int src_bpl = bpl;
                int dst_bpl = VDP_BPL;  /* 80 bytes */
                int copy_bytes = copy_w / 8;
                int dst_off_x = dx / 8;

                for (y = 0; y < copy_h; y++) {
                    u8 *src = tmp + y * src_bpl;
                    u8 *dst = gfx_fb.planes[p] + (dy + y) * dst_bpl + dst_off_x;
                    memcpy(dst, src, copy_bytes);
                }
            }
        }
        api->mem_free(tmp);
    }
    return 0;
}

/* ======================================================================== */
/*  メインプログラム                                                        */
/* ======================================================================== */
void main(int argc, char **argv, KernelAPI *api)
{
    const char *filename = NULL;
    int keep_vram = 0;
    int i, fd, ret;
    u8 hdr_buf[VDP_HDR_SIZE];
    VdpInfo info;
    int ch;

    /* 引数解析 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keep_vram = 1;
        } else {
            filename = argv[i];
        }
    }

    if (filename == NULL) {
        api->kprintf(ATTR_WHITE, "%s", "VDPView v1.0 - VDP File Viewer for OS32\n");
        api->kprintf(ATTR_WHITE, "%s", "Usage: vdpview FILE.VDP [-k]\n");
        api->kprintf(ATTR_WHITE, "%s", "  -k : keep VRAM on exit\n");
        return;
    }

    /* ファイルオープン */
    fd = api->sys_open(filename, O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_WHITE, "Error: cannot open %s\n", filename);
        return;
    }

    /* ヘッダ読み込み */
    if (api->sys_read(fd, hdr_buf, VDP_HDR_SIZE) != VDP_HDR_SIZE) {
        api->kprintf(ATTR_WHITE, "%s", "Error: read header failed\n");
        api->sys_close(fd);
        return;
    }

    /* ヘッダ解析 */
    if (parse_vdp_header(hdr_buf, &info) < 0) {
        api->kprintf(ATTR_WHITE, "%s", "Error: not a VDP file\n");
        api->sys_close(fd);
        return;
    }

    /* DataType確認 */
    if (info.data_type != 0x00 && info.data_type != 0x01) {
        api->kprintf(ATTR_WHITE, "%s", "Error: unsupported DataType\n");
        api->sys_close(fd);
        return;
    }

    /* GFX初期化 + バックバッファ取得 */
    libos32gfx_init(api);

    /* 背景クリア */
    gfx_clear(0);

    /* パレット設定 */
    apply_palette(api, &info);

    /* VDP表示 */
    ret = display_dump(api, fd, &info);
    api->sys_close(fd);

    if (ret < 0) {
        libos32gfx_shutdown();
        api->tvram_clear();
        api->kprintf(ATTR_WHITE, "%s", "Error: decode failed\n");
        return;
    }

    /* VRAM転送 */
    gfx_present();
    api->gfx_present_dirty();

    /* キー入力待ち */
    while ((ch = api->kbd_trygetchar()) < 0) {
        api->sys_halt();
    }

    /* 後片付け */
    if (!keep_vram) {
        gfx_clear(0);
        gfx_present();
        api->gfx_present_dirty();
    }

    libos32gfx_shutdown();
    api->tvram_clear();
}
