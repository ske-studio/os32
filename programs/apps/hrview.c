/* ======================================================================== */
/*  HRVIEW.C — OS32 高解像度・VQスクロール対応 VDP ビューワ                */
/*                                                                          */
/*  DOS版 HRVIEW の移植版。VQ辞書およびマップデータをフラットヒープに展開し、 */
/*  libos32gfx のバックバッファ(gfx_fb) 内シフトと差分描画によりスクロール。  */
/*                                                                          */
/*  操作: Space=縮小⇔等倍, 矢印=スクロール(加速), ESC/q=終了               */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ---- キーボードコード (OS32 kbd_trygetchar が返すASCIIコード) ---- */
#define KEY_ESC     0x1B
#define KEY_Q       'q'
#define KEY_SPACE   ' '
#define KEY_UP      0x1E    /* PC-98 scancode 0x3A → ASCII 0x1E */
#define KEY_DOWN    0x1F    /* PC-98 scancode 0x3D → ASCII 0x1F */
#define KEY_LEFT    0x1D    /* PC-98 scancode 0x3B → ASCII 0x1D */
#define KEY_RIGHT   0x1C    /* PC-98 scancode 0x3C → ASCII 0x1C */


/* ---- VDP定数 ---- */
#define VDP_HDR_SIZE     256
#define VDP_TYPE_VQ      0x03
#define VQF_16BIT_INDEX  0x04
#define VQF_UNIFIED_DICT 0x08

/* ---- 表示モード ---- */
#define MODE_THUMB  0
#define MODE_FULL   1

/* ---- VRAM定数 (バックバッファ) ---- */
#define SCREEN_BX    80     /* 640/8ブロック列数 */
#define SCREEN_BY    50     /* 400/8ブロック行数 */
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
    u8  vq_flags;
    u32 vq_map_offset;
    u32 dict_offset;
    u16 dict_patterns;     /* LE16 (DOS版と一致) */
    u16 thumb_width;
    u16 thumb_height;
    u32 thumb_map_offset;
    int is_v2;
} VdpHeader;

static KernelAPI *api;
static VdpHeader hdr;

/* ---- 状態変数 ---- */
static int view_mode = MODE_THUMB;
static int scroll_x = 0;
static int scroll_y = 0;
static int max_scroll_x;
static int max_scroll_y;

/* マップ(プレーン別)と辞書(共有) */
static u8 *full_map[4]  = {NULL, NULL, NULL, NULL};
static u8 *thumb_map[4] = {NULL, NULL, NULL, NULL};
static u8 *vq_dict      = NULL; /* [patterns * 32バイト] 統合または4プレーン順次保持 */

static int use_16bit;
static int use_unified;
static u32 full_bx, full_by;
static u32 thumb_bx, thumb_by;

/* ------------------------------------------------------------------------
 *  VDPヘッダ解析 (VDP2 優先)
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

    h->is_v2 = (buf[3] == '2');
    if (!h->is_v2) {
        api->kprintf(ATTR_WHITE, "Warning: VDP1 file might not have VQ support\n");
    }

    /* 共通フィールド */
    h->width      = LE16(buf + 0x04);
    h->height     = LE16(buf + 0x06);
    h->num_planes = buf[0x08];
    h->data_type  = buf[0x09];

    if (h->is_v2) {
        /* VDP2: オフセットはDOS版 GVDP.C の parse_header() と完全に一致 */
        h->orig_width  = LE16(buf + 0x0A);
        h->orig_height = LE16(buf + 0x0C);
        for (i = 0; i < 48; i++) h->palette[i] = buf[0x10 + i];

        h->vq_flags         = buf[0x51];
        h->vq_map_offset    = LE32(buf + 0x52);
        h->dict_offset      = LE32(buf + 0x56);
        h->dict_patterns    = LE16(buf + 0x5A);
        h->thumb_width      = LE16(buf + 0x60);
        h->thumb_height     = LE16(buf + 0x62);
        h->thumb_map_offset = LE32(buf + 0x64);
    } else {
        /* VDP1 互換オフセット */
        for (i = 0; i < 48; i++) h->palette[i] = buf[0x0A + i];
        h->vq_flags      = buf[0x41];
        h->vq_map_offset = LE32(buf + 0x42);
        h->dict_offset   = 0;
        h->dict_patterns = LE16(buf + 0x4A);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 *  解放とロード
 * ------------------------------------------------------------------------ */
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

    /* 辞書のロード (Unifiedならパターンの32Bエントリが連続、個別面なら8B*4面) */
    if (hdr.dict_offset > 0 && hdr.dict_patterns > 0) {
        dict_size = hdr.dict_patterns * 32; /* Unified想定で常に32B扱い */
        if (!use_unified) {
            dict_size = hdr.dict_patterns * 8 * 4; /* 個別面の場合も等価 */
        }
        vq_dict = (u8*)api->mem_alloc(dict_size);
        if (!vq_dict) return -1;

        api->sys_lseek(fd, hdr.dict_offset, SEEK_SET);
        if (api->sys_read(fd, vq_dict, dict_size) != (int)dict_size) return -1;
    } else {
        return -1; /* 外部SHARED.VQD は今回未サポート (引数ファイルに辞書がある前提) */
    }

    /* マップのロード (Full) */
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

    /* マップのロード (Thumb) */
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

/* ------------------------------------------------------------------------
 *  ブロック描画機能 (バックバッファへ)
 * ------------------------------------------------------------------------ */
static void draw_block_fb(u32 dst_bx, u32 dst_by, u32 idx)
{
    u8 *entry;
    u8 *dst;
    int p, row;
    int pitch = gfx_fb.pitch;

    if (use_unified) {
        /* Entryは 32バイト (B8, R8, G8, E8) */
        entry = vq_dict + idx * 32;
        for (p = 0; p < 4; p++) {
            dst = gfx_fb.planes[p] + dst_by * 8 * pitch + dst_bx;
            for (row = 0; row < 8; row++) {
                *dst = entry[p * 8 + row];
                dst += pitch;
            }
        }
    } else {
        /* 個別面の場合は4面分の別々のマップインデックスを取る必要があるが、
         * 面ごとに辞書を引く... ここでは簡略化のため unified前提とする */
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

/* ------------------------------------------------------------------------
 *  VRAM/バックバッファ シフト機能
 * ------------------------------------------------------------------------ */
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

/* 差分スクロール */
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

/* ------------------------------------------------------------------------
 *  全体描画
 * ------------------------------------------------------------------------ */
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

/* ======================================================================== */
/*  ファイラ — カレントディレクトリの .vdp ファイルを一覧表示し選択            */
/* ======================================================================== */
#define FILER_MAX_FILES  64
#define FILER_NAME_LEN   32
#define KEY_ENTER   0x0D

static char filer_names[FILER_MAX_FILES][FILER_NAME_LEN];
static u32  filer_sizes[FILER_MAX_FILES];
static int  filer_count = 0;

/* 拡張子チェック — .vdp または .VDP のみ許可 */
static int str_ends_with_vdp(const char *name)
{
    int len = 0;
    const char *p = name;
    while (*p) { len++; p++; }
    if (len < 4) return 0;
    p = name + len - 4;
    if (p[0] != '.') return 0;
    /* .vdp (小文字) または .VDP (大文字) のみ */
    if (p[1] == 'v' && p[2] == 'd' && p[3] == 'p') return 1;
    if (p[1] == 'V' && p[2] == 'D' && p[3] == 'P') return 1;
    return 0;
}


/* sys_ls コールバック: .vdp ファイルを収集 */
static void filer_collect_cb(const void *entry_raw, void *ctx)
{
    /* VfsDirEntry: name[256], size(u32), type(u8) */
    const char *name = (const char *)entry_raw;
    const u8 *base = (const u8 *)entry_raw;
    u32 size;
    u8 type;
    int i;

    (void)ctx;
    /* name は先頭 OS32_MAX_PATH(256) バイト、その後に size(4), type(1) */
    size = *(const u32 *)(base + 256);
    type = base[260];

    if (type != 1) return;  /* ファイルのみ (VFS_TYPE_FILE=1) */
    if (!str_ends_with_vdp(name)) return;
    if (filer_count >= FILER_MAX_FILES) return;

    /* 名前コピー */
    for (i = 0; i < FILER_NAME_LEN - 1 && name[i]; i++)
        filer_names[filer_count][i] = name[i];
    filer_names[filer_count][i] = '\0';
    filer_sizes[filer_count] = size;
    filer_count++;
}

/* ファイラ画面描画 (TVRAM) */
static void filer_draw(int cursor, int top)
{
    int row, idx, x;
    api->tvram_clear();

    /* タイトル行 */
    {
        const char *title = "HRVIEW - VDP File Selector   [Enter:Open  ESC:Quit]";
        x = 0;
        while (*title && x < 80) {
            api->tvram_putchar_at(x, 0, *title, 0xE1);
            title++; x++;
        }
    }

    /* 区切り線 */
    for (x = 0; x < 80; x++)
        api->tvram_putchar_at(x, 1, '-', 0x21);

    /* ファイル一覧 (最大23行) */
    for (row = 0; row < 23 && (top + row) < filer_count; row++) {
        u8 attr;
        const char *name;
        char sizebuf[16];
        int slen, si;
        idx = top + row;
        attr = (idx == cursor) ? 0xE5 : 0xE1; /* 選択=反転 */

        /* 行の背景を埋める (選択行のハイライト) */
        if (idx == cursor) {
            for (x = 0; x < 80; x++)
                api->tvram_putchar_at(x, row + 2, ' ', attr);
        }

        /* ファイル名 */
        name = filer_names[idx];
        x = 2;
        while (*name && x < 40) {
            api->tvram_putchar_at(x, row + 2, *name, attr);
            name++; x++;
        }

        /* ファイルサイズ (右寄せ) */
        {
            u32 sz = filer_sizes[idx];
            slen = 0;
            if (sz == 0) { sizebuf[0] = '0'; slen = 1; }
            else {
                u32 tmp = sz;
                while (tmp > 0) { sizebuf[slen++] = '0' + (tmp % 10); tmp /= 10; }
            }
            /* sizebuf は逆順なので右側から出力 */
            x = 60;
            for (si = slen - 1; si >= 0; si--) {
                api->tvram_putchar_at(x, row + 2, sizebuf[si], attr);
                x++;
            }
        }
    }

    /* ステータス行 */
    {
        char msg[40];
        int mi = 0;
        int n;
        n = cursor + 1;
        if (n >= 100) msg[mi++] = '0' + n / 100;
        if (n >= 10)  msg[mi++] = '0' + (n / 10) % 10;
        msg[mi++] = '0' + n % 10;
        msg[mi++] = '/';
        n = filer_count;
        if (n >= 100) msg[mi++] = '0' + n / 100;
        if (n >= 10)  msg[mi++] = '0' + (n / 10) % 10;
        msg[mi++] = '0' + n % 10;
        msg[mi] = '\0';
        x = 0;
        for (x = 0; msg[x]; x++)
            api->tvram_putchar_at(x, 24, msg[x], 0x21);
    }
}

/* ファイラのメインループ。選択されたファイル名を返す (NULL=キャンセル) */
static const char *filer_run(void)
{
    int cursor = 0;
    int top = 0;

    filer_count = 0;
    api->sys_ls(".", (void*)filer_collect_cb, NULL);

    if (filer_count == 0) {
        api->kprintf(ATTR_RED, "No .vdp files found.\n");
        return NULL;
    }

    filer_draw(cursor, top);

    for (;;) {
        int ch = api->kbd_getchar();

        if (ch == KEY_ESC) return NULL;
        if (ch == KEY_ENTER) return filer_names[cursor];

        if (ch == KEY_UP && cursor > 0) {
            cursor--;
            if (cursor < top) top = cursor;
        }
        if (ch == KEY_DOWN && cursor < filer_count - 1) {
            cursor++;
            if (cursor >= top + 23) top = cursor - 22;
        }

        filer_draw(cursor, top);
    }
}

/* ======================================================================== */
/*  ビューワ本体 — ファイルを開いて表示・スクロール                          */
/* ======================================================================== */
static int view_file_vdp(const char *filename)
{
    int fd, i;
    int need_full_redraw = 1;
    int hold_x = 0, hold_y = 0;

    /* 状態リセット */
    view_mode = MODE_THUMB;
    scroll_x = scroll_y = 0;

    fd = api->sys_open(filename, O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_RED, "Error: Cannot open %s\n", filename);
        return -1;
    }

    if (load_vdp_header(fd, &hdr) < 0) {
        api->kprintf(ATTR_RED, "Error: Invalid VDP header\n");
        api->sys_close(fd);
        return -1;
    }

    if (hdr.data_type != VDP_TYPE_VQ) {
        api->kprintf(ATTR_RED, "Error: Only DataType 0x03 (VQ) is supported\n");
        api->sys_close(fd);
        return -1;
    }

    if (load_dict_and_maps(fd) < 0) {
        api->kprintf(ATTR_RED, "Error: Map/Dict load failed\n");
        free_all();
        api->sys_close(fd);
        return -1;
    }
    api->sys_close(fd);

    libos32gfx_init(api);

    for (i = 0; i < 16; i++) {
        api->gfx_set_palette(i, hdr.palette[i*3], hdr.palette[i*3+1], hdr.palette[i*3+2]);
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

    libos32gfx_shutdown();

    /* PC-98デフォルトパレット復元 (テキストモードに戻った時の色崩れ防止) */
    {
        /* PC-98標準16色パレット (BIOS初期値) */
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

    free_all();
    return 0;

}

/* ------------------------------------------------------------------------
 *  メイン
 * ------------------------------------------------------------------------ */
void main(int argc, char **argv, KernelAPI *sys_api)
{
    const char *filename = NULL;
    int i;

    api = sys_api;

    for (i = 1; i < argc; i++) filename = argv[i];

    if (filename) {
        /* 引数指定: 直接表示 */
        view_file_vdp(filename);
        api->tvram_clear();
        return;
    }

    /* 引数なし: ファイラモード */
    for (;;) {
        const char *selected = filer_run();
        if (!selected) break;  /* ESCで終了 */

        api->tvram_clear();
        if (view_file_vdp(selected) < 0) {
            /* エラー時はファイラに戻る */
            api->kbd_getchar();
        }
        api->tvram_clear();
    }

    api->tvram_clear();
}
