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


