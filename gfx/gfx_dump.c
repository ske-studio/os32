/* ======================================================================== */
/*  GFX_DUMP.C — VDP形式スクリーンショット保存および読み込み (RAW/PackBits)  */
/* ======================================================================== */

#include "gfx.h"
#include "gfx_internal.h"
#include "palette.h"
#include "vfs.h"

/* VDP1 Header Structure (Packed, 256 Bytes) */
typedef struct {
    u8  magic[4];       /* "VDP1" */
    u16 width;
    u16 height;
    u8  num_planes;
    u8  data_type;      /* 0x00: DUMP */
    u8  palette[48];    /* R,G,B */
    u16 trans_x;
    u16 trans_y;
    u8  trans_color;
    u8  color_index[16];
    u8  num_frames;
    u16 anchor_x;
    u16 anchor_y;
    u16 hitbox_x;
    u16 hitbox_y;
    u16 hitbox_w;
    u16 hitbox_h;
    u8  compress;       /* 0x00: RAW, 0x01: PackBits RLE */
    u8  vq_flags;
    u32 vq_map_offset;
    u8  reserved[158];
} __attribute__((packed)) VdpHeader;

/*
 * 連続データ長をカウント (32bitアクセスによる高速化対応)
 */
static int count_run(const u8 *src, int limit) {
    int count = 0;
    u8 val = src[0];
    u32 val32 = val | (val << 8) | (val << 16) | (val << 24);
    
    /* 4バイト単位での高速比較 */
    while (count + 4 <= limit && *(const u32 *)(src + count) == val32) {
        count += 4;
    }
    /* 残りの端数を比較 */
    while (count < limit && src[count] == val) {
        count++;
    }
    return count;
}

/*
 * PackBits エンコーダ (Macintosh 互換 RLE + 出力バッファリング)
 */
static void write_packbits(int fd, const u8 *src, int size) {
    int i = 0;
    u8 out_buf[1024];
    int out_pos = 0;

    /* バッファフラッシュ用マクロ */
#define FLUSH() do { if(out_pos > 0) { vfs_write_fd(fd, out_buf, out_pos); out_pos = 0; } } while(0)

    while (i < size) {
        int max_len = size - i;
        if (max_len > 128) max_len = 128;

        int run_len = count_run(src + i, max_len);

        if (run_len > 1) {
            if (out_pos + 2 > 1024) FLUSH();
            out_buf[out_pos++] = (u8)(257 - run_len);
            out_buf[out_pos++] = src[i];
            i += run_len;
        } else {
            /* 連続していない部分の長さを探す */
            int raw_len = 1;
            while (raw_len < max_len) {
                if (raw_len + 1 < max_len && src[i + raw_len] == src[i + raw_len + 1]) {
                    break;
                }
                raw_len++;
            }

            if (out_pos + 1 + raw_len > 1024) FLUSH();
            out_buf[out_pos++] = (u8)(raw_len - 1);
            
            /* 32bitアクセスで高速コピー可能ですが、ここはバッファへ転送 */
            {
                int k = 0;
                while (k + 4 <= raw_len) {
                    *(u32 *)(out_buf + out_pos + k) = *(const u32 *)(src + i + k);
                    k += 4;
                }
                while (k < raw_len) {
                    out_buf[out_pos + k] = src[i + k];
                    k++;
                }
            }
            out_pos += raw_len;
            i += raw_len;
        }
    }
    FLUSH();
#undef FLUSH
}

/*
 * PackBits デコーダ (読み込み)
 */
static int read_packbits(int fd, u8 *dst, int size) {
    int count = 0;
    while (count < size) {
        u8 cmd;
        if (vfs_read_fd(fd, &cmd, 1) != 1) return -1;

        if (cmd < 128) {
            /* RAW データ */
            int raw_len = cmd + 1;
            if (count + raw_len > size) raw_len = size - count;
            if (vfs_read_fd(fd, dst + count, raw_len) != raw_len) return -1;
            count += raw_len;
        } else if (cmd > 128) {
            /* RUN データ */
            int run_len = 257 - cmd;
            u8 val;
            if (vfs_read_fd(fd, &val, 1) != 1) return -1;
            if (count + run_len > size) run_len = size - count;
            
            /* 32bitアクセスで高速フィル */
            u32 val32 = val | (val << 8) | (val << 16) | (val << 24);
            int i = 0;
            while (i + 4 <= run_len) {
                *(u32 *)(dst + count + i) = val32;
                i += 4;
            }
            while (i < run_len) {
                dst[count + i] = val;
                i++;
            }
            count += run_len;
        }
        /* cmd == 128 は NOP */
    }
    return 0;
}

/* ------------------------------------------------------------------------
 *  gfx_screenshot
 *  バックバッファをVDP形式としてファイルへダンプ (RLE圧縮)
 * ------------------------------------------------------------------------ */
int gfx_screenshot(const char *path) {
    int fd;
    VdpHeader hdr;
    const PaletteEntry *pal;
    int i;
    u8 *ptr;
    extern u8 *bb_b, *bb_r, *bb_g, *bb_i;

    fd = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;

    /* ヘッダ初期化: 32bitアクセスでゼロクリア */
    ptr = (u8*)&hdr;
    for (i = 0; i < sizeof(VdpHeader) / 4; i++) {
        ((u32 *)ptr)[i] = 0;
    }

    hdr.magic[0] = 'V'; hdr.magic[1] = 'D'; hdr.magic[2] = 'P'; hdr.magic[3] = '1';
    hdr.width = GFX_WIDTH;
    hdr.height = GFX_HEIGHT;
    hdr.num_planes = 4;
    hdr.data_type = 0x00; /* DUMP */
    hdr.num_frames = 1;
    hdr.compress = 0x01;  /* PackBits RLE */

    pal = palette_get_all();
    for (i = 0; i < 16; i++) {
        hdr.palette[i * 3 + 0] = pal[i].r;
        hdr.palette[i * 3 + 1] = pal[i].g;
        hdr.palette[i * 3 + 2] = pal[i].b;
    }

    vfs_write_fd(fd, &hdr, sizeof(VdpHeader));

    write_packbits(fd, bb_b, GFX_PLANE_SZ);
    write_packbits(fd, bb_r, GFX_PLANE_SZ);
    write_packbits(fd, bb_g, GFX_PLANE_SZ);
    write_packbits(fd, bb_i, GFX_PLANE_SZ);

    vfs_close(fd);
    return 0;
}

/* ------------------------------------------------------------------------
 *  gfx_load_vdp
 *  指定されたVDPファイルを読み込んでバックバッファへ展開＆パレット設定
 * ------------------------------------------------------------------------ */
int gfx_load_vdp(const char *path) {
    int fd;
    VdpHeader hdr;
    int i;
    extern u8 *bb_b, *bb_r, *bb_g, *bb_i;

    fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return -1;

    if (vfs_read_fd(fd, &hdr, sizeof(VdpHeader)) != sizeof(VdpHeader)) {
        vfs_close(fd);
        return -1;
    }

    /* VDP1/VDP2 & DUMP形式であるかチェック */
    if (hdr.magic[0] != 'V' || hdr.magic[1] != 'D' || hdr.magic[2] != 'P') {
        vfs_close(fd);
        return -1;
    }
    
    if (hdr.data_type != 0x00 && hdr.data_type != 0x01) {
        /* DUMP/RAW形式以外は未対応 */
        vfs_close(fd);
        return -1;
    }

    /* パレット適用 */
    for (i = 0; i < 16; i++) {
        u8 r = hdr.palette[i * 3 + 0];
        u8 g = hdr.palette[i * 3 + 1];
        u8 b = hdr.palette[i * 3 + 2];
        gfx_set_palette(i, r, g, b);
    }

    if (hdr.compress == 0x01) {
        /* PackBits圧縮 */
        if (read_packbits(fd, bb_b, GFX_PLANE_SZ) < 0) goto err;
        if (read_packbits(fd, bb_r, GFX_PLANE_SZ) < 0) goto err;
        if (read_packbits(fd, bb_g, GFX_PLANE_SZ) < 0) goto err;
        if (read_packbits(fd, bb_i, GFX_PLANE_SZ) < 0) goto err;
    } else {
        /* 非圧縮(RAW) */
        if (vfs_read_fd(fd, bb_b, GFX_PLANE_SZ) != GFX_PLANE_SZ) goto err;
        if (vfs_read_fd(fd, bb_r, GFX_PLANE_SZ) != GFX_PLANE_SZ) goto err;
        if (vfs_read_fd(fd, bb_g, GFX_PLANE_SZ) != GFX_PLANE_SZ) goto err;
        if (vfs_read_fd(fd, bb_i, GFX_PLANE_SZ) != GFX_PLANE_SZ) goto err;
    }

    vfs_close(fd);
    
    /* バックバッファ全体を再描画するようにダーティ指定 */
    extern void gfx_dirty_mark(int y0, int y1);
    gfx_dirty_mark(0, GFX_HEIGHT - 1);
    
    return 0;

err:
    vfs_close(fd);
    return -1;
}
