#include "gfx_internal.h"

/* ======================================================================== */
/*  バックバッファ (拡張メモリ固定アドレス, 128KB)                          */
/* ======================================================================== */
u8 *bb_b = (u8 *)MEM_GFX_BB_BASE;
u8 *bb_r = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ);
u8 *bb_g = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 2);
u8 *bb_i = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 3);

u8 *bb[4];

/* ======================================================================== */
/*  ダーティ領域追跡                                                        */
/* ======================================================================== */
int dirty_min_y = 0;
int dirty_max_y = GFX_HEIGHT - 1;

void gfx_dirty_mark(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= GFX_HEIGHT) y1 = GFX_HEIGHT - 1;
    if (y0 < dirty_min_y) dirty_min_y = y0;
    if (y1 > dirty_max_y) dirty_max_y = y1;
}

/* ======================================================================== */
/*  初期化・終了                                                            */
/* ======================================================================== */
void gfx_init(void)
{
    int i;
    volatile u16 *tvram_char = (volatile u16 *)TVRAM_CHAR_BASE;
    volatile u8  *tvram_attr = (volatile u8  *)TVRAM_ATTR_BASE;

    bb[0] = bb_b; bb[1] = bb_r; bb[2] = bb_g; bb[3] = bb_i;

    /* テキストVRAMクリア */
    for (i = 0; i < 2000; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }

    _out(MODE_FF2_PORT, MFF2_16COLOR);

    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x00);
    _out(MODE_FF1_PORT, MFF1_HIRES);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x00);

    gfx_set_default_palette();

    gfx_clear(0);

    gfx_surface_init();
    gfx_sprite_init();
    gfx_scroll_init();
}

void gfx_shutdown(void)
{
    _out(GDC_GFX_CMD, GDC_CMD_STOP);
}

/* ======================================================================== */
/*  画面更新: バックバッファ → VRAM                                        */
/* ======================================================================== */
void gfx_present(void)
{
    int logical_start, logical_end;
    int physical_start, physical_end;
    u8 *vb_base = (u8 *)VRAM_PLANE_B;
    u8 *vr_base = (u8 *)VRAM_PLANE_R;
    u8 *vg_base = (u8 *)VRAM_PLANE_G;
    u8 *vi_base = (u8 *)VRAM_PLANE_I;

    if (dirty_min_y > dirty_max_y) return;

    logical_start = dirty_min_y;
    logical_end = dirty_max_y;
    physical_start = (logical_start + vram_scroll_y) % GFX_HEIGHT;
    physical_end = (logical_end + vram_scroll_y) % GFX_HEIGHT;

    if (physical_start <= physical_end) {
        int sz = (logical_end - logical_start + 1) * GFX_BPL;
        int logical_off = logical_start * GFX_BPL;
        int physical_off = physical_start * GFX_BPL;
        _memcpy_w(vb_base + physical_off, bb_b + logical_off, sz / 2);
        _memcpy_w(vr_base + physical_off, bb_r + logical_off, sz / 2);
        _memcpy_w(vg_base + physical_off, bb_g + logical_off, sz / 2);
        _memcpy_w(vi_base + physical_off, bb_i + logical_off, sz / 2);
    } else {
        int block1_lines = GFX_HEIGHT - physical_start;
        int block2_lines = (logical_end - logical_start + 1) - block1_lines;
        int logical_off = logical_start * GFX_BPL;
        int physical_off = physical_start * GFX_BPL;
        int sz = block1_lines * GFX_BPL;
        _memcpy_w(vb_base + physical_off, bb_b + logical_off, sz / 2);
        _memcpy_w(vr_base + physical_off, bb_r + logical_off, sz / 2);
        _memcpy_w(vg_base + physical_off, bb_g + logical_off, sz / 2);
        _memcpy_w(vi_base + physical_off, bb_i + logical_off, sz / 2);

        logical_off = (logical_start + block1_lines) * GFX_BPL;
        physical_off = 0;
        sz = block2_lines * GFX_BPL;
        _memcpy_w(vb_base + physical_off, bb_b + logical_off, sz / 2);
        _memcpy_w(vr_base + physical_off, bb_r + logical_off, sz / 2);
        _memcpy_w(vg_base + physical_off, bb_g + logical_off, sz / 2);
        _memcpy_w(vi_base + physical_off, bb_i + logical_off, sz / 2);
    }

    dirty_min_y = GFX_HEIGHT;
    dirty_max_y = -1;
}

void gfx_present_rect(int rx, int ry, int rw, int rh)
{
    int row;
    int byte_x, byte_w;
    unsigned long base_off;
    int physical_y;
    unsigned long phys_off;
    volatile u8 *vb_base = (volatile u8 *)VRAM_PLANE_B;
    volatile u8 *vr_base = (volatile u8 *)VRAM_PLANE_R;
    volatile u8 *vg_base = (volatile u8 *)VRAM_PLANE_G;
    volatile u8 *vi_base = (volatile u8 *)VRAM_PLANE_I;

    byte_x = rx >> 3;
    byte_w = ((rx + rw + 7) >> 3) - byte_x;
    if (byte_x < 0) byte_x = 0;
    if (byte_x + byte_w > GFX_BPL) byte_w = GFX_BPL - byte_x;
    if (ry < 0) ry = 0;
    if (ry + rh > GFX_HEIGHT) rh = GFX_HEIGHT - ry;
    if (byte_w <= 0 || rh <= 0) return;

    physical_y = (ry + vram_scroll_y) % GFX_HEIGHT;
    phys_off = (unsigned long)physical_y * GFX_BPL + byte_x;
    base_off = (unsigned long)ry * GFX_BPL + byte_x;

    for (row = ry; row < ry + rh; row++) {
        if ((byte_w & 1) == 0 && (byte_x & 1) == 0) {
            _memcpy_w((void *)(vb_base + phys_off), bb_b + base_off, byte_w / 2);
            _memcpy_w((void *)(vr_base + phys_off), bb_r + base_off, byte_w / 2);
            _memcpy_w((void *)(vg_base + phys_off), bb_g + base_off, byte_w / 2);
            _memcpy_w((void *)(vi_base + phys_off), bb_i + base_off, byte_w / 2);
        } else {
            volatile u8 *vb = vb_base + phys_off;
            volatile u8 *vr = vr_base + phys_off;
            volatile u8 *vg = vg_base + phys_off;
            volatile u8 *vi = vi_base + phys_off;
            int bx;
            for (bx = 0; bx < byte_w; bx++) {
                vb[bx] = bb_b[base_off + bx];
                vr[bx] = bb_r[base_off + bx];
                vg[bx] = bb_g[base_off + bx];
                vi[bx] = bb_i[base_off + bx];
            }
        }
        
        base_off += GFX_BPL;
        phys_off += GFX_BPL;
        physical_y++;
        if (physical_y >= GFX_HEIGHT) {
            physical_y = 0;
            phys_off = byte_x;
        }
    }

    dirty_min_y = GFX_HEIGHT;
    dirty_max_y = -1;
}

/* ======================================================================== */
/*  パレット                                                                */
/* ======================================================================== */
void gfx_set_palette(int idx, u8 r, u8 g, u8 b)
{
    _out(PAL_IDX_PORT, (unsigned)idx);
    _out(PAL_G_PORT, (unsigned)g);
    _out(PAL_R_PORT, (unsigned)r);
    _out(PAL_B_PORT, (unsigned)b);
}

void gfx_set_palette_all(const GFX_Color *pal)
{
    int i;
    for (i = 0; i < PALETTE_COUNT; i++) {
        gfx_set_palette(i, pal[i].r, pal[i].g, pal[i].b);
    }
}

void gfx_set_default_palette(void)
{
    static const GFX_Color default_pal[PALETTE_COUNT] = {
        { 0,  0,  0}, { 0,  0,  7}, { 7,  0,  0}, { 7,  0,  7},
        { 0,  7,  0}, { 0,  7,  7}, { 7,  7,  0}, { 7,  7,  7},
        { 0,  0,  0}, { 0,  0, 15}, {15,  0,  0}, {15,  0, 15},
        { 0, 15,  0}, { 0, 15, 15}, {15, 15,  0}, {15, 15, 15}
    };
    gfx_set_palette_all(default_pal);
}
