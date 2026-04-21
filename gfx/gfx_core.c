#include "gfx_internal.h"
#include "os32_kapi_shared.h"
#include "kstring.h"

/* ======================================================================== */
/*  バックバッファ (拡張メモリ固定アドレス, 128KB)                          */
/* ======================================================================== */
u8 *bb_b = (u8 *)MEM_GFX_BB_BASE;
u8 *bb_r = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ);
u8 *bb_g = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 2);
u8 *bb_i = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 3);

int gfx_current_height = GFX_HEIGHT;  /* 200 or 400 */

u8 *bb[4];

DirtyRectQueue dirty_queue = {0};
DirtyRectQueue prev_dirty = {0};   /* 前フレームdirty (ステイルページ対策) */

int gfx_flip_enabled = 0;
int gfx_display_page = 0;

/* ======================================================================== */
/*  KAPI: フレームバッファ取得                                              */
/* ======================================================================== */
void __cdecl gfx_get_framebuffer(GFX_Framebuffer *fb)
{
    if (!fb) return;
    fb->width = GFX_WIDTH;
    fb->height = gfx_current_height;
    fb->pitch = GFX_BPL;
    fb->planes[0] = bb[0];
    fb->planes[1] = bb[1];
    fb->planes[2] = bb[2];
    fb->planes[3] = bb[3];
}

int gfx_get_height(void)
{
    return gfx_current_height;
}

/* ======================================================================== */
/*  初期化・終了                                                            */
/* ======================================================================== */
/* 共通初期化処理 (テキストVRAMクリア + バックバッファゼロクリア) */
static void _gfx_common_init(int plane_sz)
{
    int i;
    volatile u16 *tvram_char = (volatile u16 *)TVRAM_CHAR_BASE;
    volatile u8  *tvram_attr = (volatile u8  *)TVRAM_ATTR_BASE;

    bb[0] = bb_b; bb[1] = bb_r; bb[2] = bb_g; bb[3] = bb_i;
    dirty_queue.count = 0;

    /* テキストVRAMクリア */
    for (i = 0; i < TVRAM_COLS * TVRAM_ROWS; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }

    /* ゼロクリア (バックバッファ) — kmemset (rep stosd) で高速化 */
    kmemset(bb_b, 0, plane_sz);
    kmemset(bb_r, 0, plane_sz);
    kmemset(bb_g, 0, plane_sz);
    kmemset(bb_i, 0, plane_sz);

    _out(MODE_FF2_PORT, MFF2_16COLOR);
}

void gfx_init(void)
{
    gfx_current_height = GFX_HEIGHT;  /* 400ラインモード */
    gfx_display_page = 0;
    prev_dirty.count = 0;

    _gfx_common_init(GFX_PLANE_SZ);

    /* GDC CSRFORM: L/R=0 (400ライン) */
    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x00);
    _out(MODE_FF1_PORT, MFF1_HIRES);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    /* ページフリッピング有効化: 両ページのVRAMをゼロクリア */
    _out(GDC_ACCESS_PAGE, 0x00);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ);

    _out(GDC_ACCESS_PAGE, 0x01);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ);

    /* ページ0を表示、ページ1に描画 */
    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x01);
    gfx_flip_enabled = 1;

    palette_init();
    gfx_scroll_init();
}

void gfx_init_200(void)
{
    gfx_current_height = GFX_HEIGHT_200;  /* 200ラインモード */

    _gfx_common_init(GFX_PLANE_SZ_200);

    /* GDC CSRFORM: L/R=1 (各ライン2倍表示 → 200ライン) */
    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x01);
    _out(MODE_FF1_PORT, MFF1_200LINE);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    /* ページフリッピング有効化: 両ページのVRAMをゼロクリア */
    _out(GDC_ACCESS_PAGE, 0x00);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ_200);

    _out(GDC_ACCESS_PAGE, 0x01);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ_200);

    /* ページ0を表示、ページ1に描画 */
    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x01);
    gfx_flip_enabled = 1;
    gfx_display_page = 0;
    prev_dirty.count = 0;

    palette_init();
    gfx_scroll_init();
}

void gfx_shutdown(void)
{
    /* フリップモード解除: ページ0に復帰 */
    if (gfx_flip_enabled) {
        _out(GDC_DISP_PAGE, 0x00);
        _out(GDC_ACCESS_PAGE, 0x00);
        gfx_flip_enabled = 0;
        gfx_display_page = 0;
    }
    gfx_current_height = GFX_HEIGHT;
    _out(GDC_GFX_CMD, GDC_CMD_STOP);
}
