#include "gfx_internal.h"
#include "os32_kapi_shared.h"

static void _flush_dirty_queue(void);

/* ======================================================================== */
/*  KAPI: ダーティレクタングルの追加 (OS管理)                              */
/* ======================================================================== */
void __cdecl gfx_add_dirty_rect(int x, int y, int w, int h)
{
    int aligned_x, aligned_w;
    int right = x + w;
    int bottom = y + h;
    int i;
    GFX_Rect *r;

    if (w <= 0 || h <= 0) return;

    /* 32ピクセル境界 (4バイト) にアライメント:
     * xは切り捨て、幅は切り上げ
     */
    aligned_x = x & ~31;
    aligned_w = ((right + 31) & ~31) - aligned_x;

    /* 画面クリッピング */
    if (aligned_x < 0) {
        aligned_w += aligned_x;
        aligned_x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (aligned_x + aligned_w > GFX_WIDTH) aligned_w = GFX_WIDTH - aligned_x;
    if (y + h > GFX_HEIGHT) h = GFX_HEIGHT - y;

    if (aligned_w <= 0 || h <= 0) return;

    /* キューがいっぱいの場合は、最も広い領域にマージするなど単純化（全画面フルフラッシュフォールバック） */
    if (dirty_queue.count >= MAX_DIRTY_RECTS) {
        /* キューを圧縮 (0番目に全体を囲むBounding Boxを作成) */
        int min_x = GFX_WIDTH, min_y = GFX_HEIGHT, max_x = 0, max_y = 0;
        for (i = 0; i < dirty_queue.count; i++) {
            if (dirty_queue.rects[i].x < min_x) min_x = dirty_queue.rects[i].x;
            if (dirty_queue.rects[i].y < min_y) min_y = dirty_queue.rects[i].y;
            if (dirty_queue.rects[i].x + dirty_queue.rects[i].w > max_x) max_x = dirty_queue.rects[i].x + dirty_queue.rects[i].w;
            if (dirty_queue.rects[i].y + dirty_queue.rects[i].h > max_y) max_y = dirty_queue.rects[i].y + dirty_queue.rects[i].h;
        }
        dirty_queue.count = 1;
        dirty_queue.rects[0].x = min_x;
        dirty_queue.rects[0].y = min_y;
        dirty_queue.rects[0].w = max_x - min_x;
        dirty_queue.rects[0].h = max_y - min_y;
    }

    r = &dirty_queue.rects[dirty_queue.count++];
    r->x = aligned_x;
    r->y = y;
    r->w = aligned_w;
    r->h = h;
}

/* ======================================================================== */
/*  ダーティレクタングルのVRAM転送 (VSYNC同期内等で呼ばれる)              */
/* ======================================================================== */
static void _flush_dirty_queue(void)
{
    int i, row;
    u8 *vb_base = (u8 *)VRAM_PLANE_B;
    u8 *vr_base = (u8 *)VRAM_PLANE_R;
    u8 *vg_base = (u8 *)VRAM_PLANE_G;
    u8 *vi_base = (u8 *)VRAM_PLANE_I;

    for (i = 0; i < dirty_queue.count; i++) {
        GFX_Rect *r = &dirty_queue.rects[i];
        int byte_x = r->x >> 3;           /* 横は何バイト目か */
        int byte_w = r->w >> 3;           /* 横幅のバイト数 */
        int words  = byte_w >> 1;         /* 16bit転送するワード数 */
        int physical_y;
        unsigned long phys_off, base_off;

        if (words <= 0) continue;

        physical_y = (r->y + vram_scroll_y) % GFX_HEIGHT;
        phys_off = (unsigned long)physical_y * GFX_BPL + byte_x;
        base_off = (unsigned long)r->y * GFX_BPL + byte_x;

        for (row = 0; row < r->h; row++) {
            _memcpy_w(vb_base + phys_off, bb_b + base_off, words);
            _memcpy_w(vr_base + phys_off, bb_r + base_off, words);
            _memcpy_w(vg_base + phys_off, bb_g + base_off, words);
            _memcpy_w(vi_base + phys_off, bb_i + base_off, words);

            base_off += GFX_BPL;
            phys_off += GFX_BPL;
            physical_y++;
            if (physical_y >= GFX_HEIGHT) {
                physical_y = 0;
                phys_off = byte_x;
            }
        }
    }
    dirty_queue.count = 0;
}

/* ======================================================================== */
/*  KAPI: 強制でダーティ領域をVSYNC同期転送する                           */
/* ======================================================================== */
void __cdecl gfx_present_dirty(void)
{
    if (dirty_queue.count == 0) return;

    /* VSYNC期間になるまで待つ (CRTC I/O 0x60 bit5) 
     * ※エミュレータ・実機互換のため単純なポーリングループ */
    /* HINT/NOTE: 古いプログラムはI/O 0x60 を監視する */
    while ((_in(0x60) & 0x20) == 0) { }
    
    _flush_dirty_queue();
}

/* ======================================================================== */
/*  互換KAPI実装 (OS管理に寄せる)                                          */
/* ======================================================================== */
void __cdecl gfx_present(void)
{
    gfx_add_dirty_rect(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_present_dirty();
}

void __cdecl gfx_present_rect(int rx, int ry, int rw, int rh)
{
    gfx_add_dirty_rect(rx, ry, rw, rh);
    gfx_present_dirty();
}
