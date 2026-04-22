/* ======================================================================== */
/*  VDPVIEW.C — OS32 高解像度・VQスクロール対応統合 VDP ビューワ             */
/*                                                                          */
/*  DUMP(0x00), RAW(0x01), VQ(0x03) に対応する統合版。                      */
/*  - DUMP/RAW: 静止画表示 (PackBits圧縮対応)                               */
/*  - VQ: マップ/辞書展開、差分描画による加速スクロール、等倍/サムネイル切替   */
/*                                                                          */
/*  使い方: exec vdpview FILENAME.VDP [-k]                                  */
/*    -k : 終了時にVRAMをクリアしない                                        */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ---- キーボードコード ---- */
#define KEY_ESC     0x1B
#define KEY_Q       'q'
#define KEY_SPACE   ' '
#define KEY_UP      0x1E
#define KEY_DOWN    0x1F
#define KEY_LEFT    0x1D
#define KEY_RIGHT   0x1C

/* ---- VDP定数 ---- */
#define VDP_HDR_SIZE     256
#define VDP_TYPE_DUMP    0x00
#define VDP_TYPE_RAW     0x01
#define VDP_TYPE_VQ      0x03

#define VQF_16BIT_INDEX  0x04
#define VQF_UNIFIED_DICT 0x08

#define VDP_BPL          80
#define VDP_PLANE_SIZE   32000

/* ---- 表示モード (VQ用) ---- */
#define MODE_THUMB  0
#define MODE_FULL   1

/* ---- VRAM定数 (バックバッファ) ---- */
#define SCREEN_BX    80
#define SCREEN_BY    50
#define SCREEN_H     400

/* ---- LE読み取りマクロ ---- */
#define LE16(p) ((u16)(p)[0] | ((u16)(p)[1] << 8))
#define LE32(p) ((u32)(p)[0] | ((u32)(p)[1] << 8) | ((u32)(p)[2] << 16) | ((u32)(p)[3] << 24))

/* ---- VDPヘッダ ---- */
typedef struct {
    u16 width;
    u16 height;
    u8  num_planes;
    u8  data_type;
    u16 orig_width;
    u16 orig_height;
    u8  palette[48];
    u8  compress;          /* 0x00=RAW, 0x01=PackBits (DUMP/RAW用) */
    u8  vq_flags;          /* VQ用 */
    u32 vq_map_offset;
    u32 dict_offset;
    u16 dict_patterns;
    u16 thumb_width;
    u16 thumb_height;
    u32 thumb_map_offset;
    int is_v2;
} VdpHeader;

static KernelAPI *api;
static VdpHeader hdr;

/* ---- VQ状態変数 ---- */
static int view_mode = MODE_THUMB;
static int scroll_x = 0;
static int scroll_y = 0;
static int max_scroll_x;
static int max_scroll_y;

static u8 *full_map[4]  = {NULL, NULL, NULL, NULL};
static u8 *thumb_map[4] = {NULL, NULL, NULL, NULL};
static u8 *vq_dict      = NULL;

static int use_16bit;
static int use_unified;
static u32 full_bx, full_by;
static u32 thumb_bx, thumb_by;

/* ------------------------------------------------------------------------
 *  VDPヘッダ解析 (VDP1/VDP2 自動判定・全フィールド対応)
 * ------------------------------------------------------------------------ */
static int load_vdp_header(int fd, VdpHeader *h)
{
    u8 buf[VDP_HDR_SIZE];
    int i;
    int read_bytes;

    read_bytes = api->sys_read(fd, buf, VDP_HDR_SIZE);
    if (read_bytes != VDP_HDR_SIZE) {
        api->kprintf(ATTR_RED, "Header sys_read failed: expected %d, got %d\n", VDP_HDR_SIZE, read_bytes);
        return -1;
    }

    if (buf[0] != 'V' || buf[1] != 'D' || buf[2] != 'P') {
        api->kprintf(ATTR_RED, "Header magic mismatch: %02X %02X %02X %02X\n", buf[0], buf[1], buf[2], buf[3]);
        return -1;
    }

    if (buf[3] != '1' && buf[3] != '2') {
        return -1;
    }

    h->is_v2 = (buf[3] == '2');

    h->width      = LE16(buf + 0x04);
    h->height     = LE16(buf + 0x06);
    h->num_planes = buf[0x08];
    h->data_type  = buf[0x09];

    if (h->is_v2) {
        h->orig_width  = LE16(buf + 0x0A);
        h->orig_height = LE16(buf + 0x0C);
        for (i = 0; i < 48; i++) h->palette[i] = buf[0x10 + i];
        
        h->compress         = buf[0x50];
        h->vq_flags         = buf[0x51];
        h->vq_map_offset    = LE32(buf + 0x52);
        h->dict_offset      = LE32(buf + 0x56);
        h->dict_patterns    = LE16(buf + 0x5A);
        h->thumb_width      = LE16(buf + 0x60);
        h->thumb_height     = LE16(buf + 0x62);
        h->thumb_map_offset = LE32(buf + 0x64);
    } else {
        for (i = 0; i < 48; i++) h->palette[i] = buf[0x0A + i];
        
        h->compress      = buf[0x5C];
        h->vq_flags      = buf[0x41];
        h->vq_map_offset = LE32(buf + 0x42);
        h->dict_offset   = 0;
        h->dict_patterns = LE16(buf + 0x4A);
    }
    return 0;
}

/* ======================================================================== */
/*  DUMP / RAW 表示機能                                                     */
/* ======================================================================== */

static int decode_packbits(KernelAPI *api, int fd, u8 *dst, int size)
{
    int count = 0;
    while (count < size) {
        u8 cmd;
        if (api->sys_read(fd, &cmd, 1) != 1) return -1;

        if (cmd < 128) {
            int raw_len = cmd + 1;
            if (count + raw_len > size) raw_len = size - count;
            if (api->sys_read(fd, dst + count, raw_len) != raw_len) return -1;
            count += raw_len;
        } else if (cmd > 128) {
            int run_len = 257 - cmd;
            u8 val;
            int j;
            u32 val32;
            if (api->sys_read(fd, &val, 1) != 1) return -1;
            if (count + run_len > size) run_len = size - count;

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
    }
    return 0;
}

static int read_plane(KernelAPI *api, int fd, u8 *plane_buf, int plane_size, u8 compress)
{
    if (compress == 0x01) {
        return decode_packbits(api, fd, plane_buf, plane_size);
    } else {
        if (api->sys_read(fd, plane_buf, plane_size) != plane_size) {
            return -1;
        }
        return 0;
    }
}

static int display_dump(KernelAPI *api, int fd, const VdpHeader *info)
{
    int p;
    int bpl = info->width / 8;
    int plane_size = bpl * info->height;

    if (info->width == 640 && info->height == 400) {
        for (p = 0; p < 4 && p < info->num_planes; p++) {
            if (read_plane(api, fd, gfx_fb.planes[p], VDP_PLANE_SIZE, info->compress) < 0) {
                return -1;
            }
        }
    } else {
        u8 *tmp;
        int dx, dy;
        int y, copy_w, copy_h;

        tmp = (u8 *)api->mem_alloc(plane_size);
        if (!tmp) return -1;

        dx = (640 - (int)info->width) / 2;
        dy = (400 - (int)info->height) / 2;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;

        copy_w = (int)info->width;
        copy_h = (int)info->height;
        if (dx + copy_w > 640) copy_w = 640 - dx;
        if (dy + copy_h > 400) copy_h = 400 - dy;

        for (p = 0; p < 4 && p < info->num_planes; p++) {
            if (read_plane(api, fd, tmp, plane_size, info->compress) < 0) {
                api->mem_free(tmp);
                return -1;
            }
            {
                int src_bpl = bpl;
                int dst_bpl = VDP_BPL;
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
/*  VQ 表示・スクロール機能                                                  */
/* ======================================================================== */

static void free_maps(u8 *m[4])
{
    if (use_unified) {
        if (m[0]) api->mem_free(m[0]);
    } else {
        int i;
        for (i=0; i<4; i++) if (m[i]) api->mem_free(m[i]);
    }
    m[0]=m[1]=m[2]=m[3]=NULL;
}

static void free_all(void)
{
    free_maps(full_map);
    free_maps(thumb_map);
    if (vq_dict) api->mem_free(vq_dict);
    vq_dict = NULL;
}

static int load_dict_and_maps(int fd)
{
    u32 dict_size;
    u32 map_size, bcount;
    int p;

    use_16bit = (hdr.vq_flags & VQF_16BIT_INDEX) ? 1 : 0;
    use_unified = (hdr.vq_flags & VQF_UNIFIED_DICT) ? 1 : 0;

    if (hdr.dict_offset > 0 && hdr.dict_patterns > 0) {
        dict_size = hdr.dict_patterns * 32;
        if (!use_unified) dict_size = hdr.dict_patterns * 8 * 4;
        vq_dict = (u8*)api->mem_alloc(dict_size);
        if (!vq_dict) return -1;

        api->sys_lseek(fd, hdr.dict_offset, SEEK_SET);
        if (api->sys_read(fd, vq_dict, dict_size) != (int)dict_size) return -1;
    } else {
        return -1;
    }

    full_bx = hdr.width / 8;
    full_by = hdr.height / 8;
    bcount = full_bx * full_by;
    map_size = bcount * (use_16bit ? 2 : 1);

    api->sys_lseek(fd, hdr.vq_map_offset, SEEK_SET);
    if (use_unified) {
        full_map[0] = (u8*)api->mem_alloc(map_size);
        if (!full_map[0]) return -1;
        if (api->sys_read(fd, full_map[0], map_size) != (int)map_size) return -1;
        for (p=1; p<4; p++) full_map[p] = full_map[0];
    } else {
        for (p=0; p<4; p++) {
            full_map[p] = (u8*)api->mem_alloc(map_size);
            if (!full_map[p]) return -1;
            if (api->sys_read(fd, full_map[p], map_size) != (int)map_size) return -1;
        }
    }

    if (hdr.thumb_width > 0 && hdr.thumb_map_offset > 0) {
        thumb_bx = hdr.thumb_width / 8;
        thumb_by = hdr.thumb_height / 8;
        bcount = thumb_bx * thumb_by;
        map_size = bcount * (use_16bit ? 2 : 1);

        api->sys_lseek(fd, hdr.thumb_map_offset, SEEK_SET);
        if (use_unified) {
            thumb_map[0] = (u8*)api->mem_alloc(map_size);
            if (thumb_map[0] && api->sys_read(fd, thumb_map[0], map_size) == (int)map_size) {
                for(p=1; p<4; p++) thumb_map[p] = thumb_map[0];
            }
        } else {
            for (p=0; p<4; p++) {
                thumb_map[p] = (u8*)api->mem_alloc(map_size);
                if (thumb_map[p]) api->sys_read(fd, thumb_map[p], map_size);
            }
        }
    }

    max_scroll_x = (int)full_bx - SCREEN_BX;
    max_scroll_y = (int)full_by - SCREEN_BY;
    if (max_scroll_x < 0) max_scroll_x = 0;
    if (max_scroll_y < 0) max_scroll_y = 0;

    return 0;
}

static u32 get_map_idx(const u8 *pmap, u32 block)
{
    if (use_16bit) {
        const u8 *m = pmap + block * 2;
        return m[0] | ((u32)m[1] << 8);
    }
    return pmap[block];
}

static void draw_block_fb(u32 dst_bx, u32 dst_by, u32 idx)
{
    u8 *entry;
    u8 *dst;
    int p, row;
    int pitch = gfx_fb.pitch;

    if (use_unified) {
        entry = vq_dict + idx * 32;
        for (p = 0; p < 4; p++) {
            dst = gfx_fb.planes[p] + dst_by * 8 * pitch + dst_bx;
            for (row = 0; row < 8; row++) {
                *dst = entry[p * 8 + row];
                dst += pitch;
            }
        }
    }
}

static void draw_column(u32 src_bx, u32 dst_bx)
{
    u32 by;
    for (by = 0; by < SCREEN_BY; by++) {
        int src_by = by + scroll_y;
        if (src_by < (int)full_by) {
            u32 block = src_by * full_bx + src_bx;
            u32 idx = get_map_idx(full_map[0], block);
            draw_block_fb(dst_bx, by, idx);
        }
    }
}

static void draw_row(u32 src_by, u32 dst_by)
{
    u32 bx;
    for (bx = 0; bx < SCREEN_BX; bx++) {
        int src_bx = bx + scroll_x;
        if (src_bx < (int)full_bx) {
            u32 block = src_by * full_bx + src_bx;
            u32 idx = get_map_idx(full_map[0], block);
            draw_block_fb(bx, dst_by, idx);
        }
    }
}

static void buffer_shift_h(int dx)
{
    int p, y;
    int pitch = gfx_fb.pitch;
    int copy_bytes = SCREEN_BX - (dx > 0 ? dx : -dx);

    if (copy_bytes <= 0) return;

    for (p = 0; p < 4; p++) {
        u8 *plane = gfx_fb.planes[p];
        for (y = 0; y < SCREEN_H; y++) {
            u8 *line = plane + y * pitch;
            if (dx > 0) {
                memmove(line, line + dx, copy_bytes);
            } else {
                memmove(line - dx, line, copy_bytes);
            }
        }
    }
}

static void buffer_shift_v(int dy_px)
{
    int p;
    int pitch = gfx_fb.pitch;
    int copy_lines = SCREEN_H - (dy_px > 0 ? dy_px : -dy_px);

    if (copy_lines <= 0) return;

    for (p = 0; p < 4; p++) {
        u8 *plane = gfx_fb.planes[p];
        if (dy_px > 0) {
            memmove(plane, plane + dy_px * pitch, copy_lines * pitch);
        } else {
            memmove(plane - dy_px * pitch, plane, copy_lines * pitch);
        }
    }
}

static void scroll_inc_h(int delta)
{
    int new_x = scroll_x + delta;
    if (new_x < 0) new_x = 0;
    if (new_x > max_scroll_x) new_x = max_scroll_x;
    delta = new_x - scroll_x;
    if (delta == 0) return;

    scroll_x = new_x;
    buffer_shift_h(delta);

    if (delta > 0) {
        int col;
        for (col = 0; col < delta; col++) {
            int dst_bx = SCREEN_BX - delta + col;
            int src_bx = scroll_x + dst_bx;
            if (dst_bx >= 0 && src_bx < (int)full_bx)
                draw_column((u32)src_bx, (u32)dst_bx);
        }
    } else {
        int abs_d = -delta;
        int col;
        for (col = 0; col < abs_d; col++) {
            int src_bx = scroll_x + col;
            if (src_bx >= 0 && src_bx < (int)full_bx)
                draw_column((u32)src_bx, (u32)col);
        }
    }
}

static void scroll_inc_v(int delta)
{
    int new_y = scroll_y + delta;
    if (new_y < 0) new_y = 0;
    if (new_y > max_scroll_y) new_y = max_scroll_y;
    delta = new_y - scroll_y;
    if (delta == 0) return;

    scroll_y = new_y;

    if (delta > 0) {
        buffer_shift_v(delta * 8);
        {
            int row;
            for (row = 0; row < delta; row++) {
                int dst = SCREEN_BY - delta + row;
                int src = scroll_y + dst;
                if (dst >= 0 && src < (int)full_by)
                    draw_row((u32)src, (u32)dst);
            }
        }
    } else {
        int abs_d = -delta;
        buffer_shift_v(-(abs_d * 8));
        {
            int row;
            for (row = 0; row < abs_d; row++) {
                int src = scroll_y + row;
                if (src >= 0 && src < (int)full_by)
                    draw_row((u32)src, (u32)row);
            }
        }
    }
}

static void draw_thumb(void)
{
    u32 center_bx, center_by;
    u32 bx, by;

    if (!thumb_map[0]) return;

    center_bx = (SCREEN_BX - thumb_bx) / 2;
    center_by = (SCREEN_BY - thumb_by) / 2;

    gfx_clear(0);

    for (by = 0; by < thumb_by; by++) {
        for (bx = 0; bx < thumb_bx; bx++) {
            u32 block = by * thumb_bx + bx;
            u32 idx = get_map_idx(thumb_map[0], block);
            if (center_bx + bx < SCREEN_BX && center_by + by < SCREEN_BY) {
                draw_block_fb(center_bx + bx, center_by + by, idx);
            }
        }
    }
}

static void draw_full(void)
{
    u32 bx;
    gfx_clear(0);

    for (bx = 0; bx < SCREEN_BX; bx++) {
        int src_bx = bx + scroll_x;
        if (src_bx < (int)full_bx)
            draw_column((u32)src_bx, bx);
    }
}

static int view_file_vq(int fd)
{
    int need_full_redraw = 1;
    int hold_x = 0, hold_y = 0;

    view_mode = MODE_THUMB;
    scroll_x = scroll_y = 0;

    if (load_dict_and_maps(fd) < 0) {
        api->kprintf(ATTR_RED, "Error: Map/Dict load failed\n");
        free_all();
        return -1;
    }

    while (1) {
        int ch;
        int d_x = 0, d_y = 0;

        if (need_full_redraw) {
            if (view_mode == MODE_THUMB && thumb_map[0] != NULL) draw_thumb();
            else draw_full();
            api->gfx_add_dirty_rect(0, 0, gfx_fb.width, gfx_fb.height);
            need_full_redraw = 0;
        }

        api->gfx_present_dirty();

        ch = api->kbd_trygetchar();
        if (ch >= 0) {
            if (ch == KEY_ESC || ch == KEY_Q) break;
            if (ch == KEY_SPACE) {
                view_mode = (view_mode == MODE_THUMB) ? MODE_FULL : MODE_THUMB;
                scroll_x = scroll_y = 0;
                need_full_redraw = 1;
            }
            if (view_mode == MODE_FULL) {
                if (ch == KEY_LEFT)  d_x = -1;
                if (ch == KEY_RIGHT) d_x = 1;
                if (ch == KEY_UP)    d_y = -1;
                if (ch == KEY_DOWN)  d_y = 1;
            }
        }

        if (d_x != 0 || d_y != 0) {
            if (d_x != 0) {
                hold_x++;
                scroll_inc_h(d_x * (hold_x > 5 ? 2 : 1));
            }
            if (d_y != 0) {
                hold_y++;
                scroll_inc_v(d_y * (hold_y > 5 ? 2 : 1));
            }
            api->gfx_add_dirty_rect(0, 0, gfx_fb.width, gfx_fb.height);
        } else {
            hold_x = hold_y = 0;
            api->sys_halt();
        }
    }

    free_all();
    return 0;
}


/* ======================================================================== */
/*  メインプログラム                                                        */
/* ======================================================================== */
void main(int argc, char **argv, KernelAPI *sys_api)
{
    const char *filename = NULL;
    int keep_vram = 0;
    int i, fd, ret;

    api = sys_api;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keep_vram = 1;
        } else {
            filename = argv[i];
        }
    }

    if (filename == NULL) {
        api->kprintf(ATTR_WHITE, "%s", "VDPView v2.0 - Unified VDP Viewer for OS32\n");
        api->kprintf(ATTR_WHITE, "%s", "Usage: vdpview FILE.VDP [-k]\n");
        api->kprintf(ATTR_WHITE, "%s", "  -k : keep VRAM on exit\n");
        return;
    }

    fd = api->sys_open(filename, O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_WHITE, "Error: cannot open %s\n", filename);
        return;
    }

    if (load_vdp_header(fd, &hdr) < 0) {
        api->kprintf(ATTR_WHITE, "%s", "Error: not a valid VDP file\n");
        api->sys_close(fd);
        return;
    }

    if (hdr.data_type != VDP_TYPE_DUMP && hdr.data_type != VDP_TYPE_RAW && hdr.data_type != VDP_TYPE_VQ) {
        api->kprintf(ATTR_WHITE, "%s", "Error: unsupported DataType\n");
        api->sys_close(fd);
        return;
    }

    /* GFX初期化とパレット設定 */
    libos32gfx_init(api);
    for (i = 0; i < 16; i++) {
        api->gfx_set_palette(i, hdr.palette[i*3], hdr.palette[i*3+1], hdr.palette[i*3+2]);
    }

    if (hdr.data_type == VDP_TYPE_DUMP || hdr.data_type == VDP_TYPE_RAW) {
        /* DUMP / RAW 表示 */
        int ch;
        gfx_clear(0);
        ret = display_dump(api, fd, &hdr);
        api->sys_close(fd);

        if (ret < 0) {
            libos32gfx_shutdown();
            api->tvram_clear();
            api->kprintf(ATTR_WHITE, "%s", "Error: decode failed\n");
            return;
        }

        gfx_present();
        api->gfx_present_dirty();

        while ((ch = api->kbd_trygetchar()) < 0) {
            api->sys_halt();
        }
    } else if (hdr.data_type == VDP_TYPE_VQ) {
        /* VQ 表示 */
        ret = view_file_vq(fd);
        api->sys_close(fd);
        
        if (ret < 0) {
            api->kprintf(ATTR_WHITE, "%s", "Error: VQ decode failed\n");
        }
    }

    /* 後片付けとパレット復元 */
    if (!keep_vram) {
        gfx_clear(0);
        gfx_present();
        api->gfx_present_dirty();
    }
    
    libos32gfx_shutdown();

    /* PC-98デフォルトパレット復元 */
    {
        static const u8 default_pal[16][3] = {
            {0,0,0}, {0,0,7}, {7,0,0}, {7,0,7},
            {0,7,0}, {0,7,7}, {7,7,0}, {7,7,7},
            {0,0,0}, {0,0,15},{15,0,0},{15,0,15},
            {0,15,0},{0,15,15},{15,15,0},{15,15,15}
        };
        int pi;
        for (pi = 0; pi < 16; pi++)
            api->gfx_set_palette(pi, default_pal[pi][0], default_pal[pi][1], default_pal[pi][2]);
    }

    api->tvram_clear();
}
