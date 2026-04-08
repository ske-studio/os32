#include "gfx_internal.h"
#include "os32_kapi_shared.h"

/* ======================================================================== */
/*  バックバッファ (拡張メモリ固定アドレス, 128KB)                          */
/* ======================================================================== */
u8 *bb_b = (u8 *)MEM_GFX_BB_BASE;
u8 *bb_r = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ);
u8 *bb_g = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 2);
u8 *bb_i = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 3);

u8 *bb[4];

DirtyRectQueue dirty_queue = {0};

/* ======================================================================== */
/*  KAPI: フレームバッファ取得                                              */
/* ======================================================================== */
void __cdecl gfx_get_framebuffer(GFX_Framebuffer *fb)
{
    if (!fb) return;
    fb->width = GFX_WIDTH;
    fb->height = GFX_HEIGHT;
    fb->pitch = GFX_BPL;
    fb->planes[0] = bb[0];
    fb->planes[1] = bb[1];
    fb->planes[2] = bb[2];
    fb->planes[3] = bb[3];
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
    dirty_queue.count = 0;

    /* テキストVRAMクリア */
    for (i = 0; i < 2000; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }

    /* ゼロクリア (バックバッファ) */
    for (i = 0; i < GFX_PLANE_SZ / 4; i++) {
        ((u32*)bb_b)[i] = 0;
        ((u32*)bb_r)[i] = 0;
        ((u32*)bb_g)[i] = 0;
        ((u32*)bb_i)[i] = 0;
    }

    _out(MODE_FF2_PORT, MFF2_16COLOR);

    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x00);
    _out(MODE_FF1_PORT, MFF1_HIRES);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x00);

    palette_init();
    gfx_scroll_init();
}

void gfx_shutdown(void)
{
    _out(GDC_GFX_CMD, GDC_CMD_STOP);
}
