#include "libos32gfx.h"
#include "libgfx_internal.h"

extern void __cdecl asm_copy_plane_rect(u8 *dst, int dst_pitch,
                                        const u8 *src, int src_pitch,
                                        int rows, int width_bytes);

/* ======================================================================== */
/*  バッキングストア・キャッシュ操作                                         */
/* ======================================================================== */

void __cdecl gfx_save_rect(int x, int y, int w, int h, void *buf)
{
    int i;
    int sx, ex, wb;
    u8 *dst_base = (u8 *)buf;

    int orig_wb = ((x + w + 7) / 8) - (x / 8);
    if (orig_wb <= 0 || h <= 0) return;

    /* ビューポートでクリップ */
    if (x < 0) { w -= (0 - x); x = 0; }
    if (y < 0) { h -= (0 - y); y = 0; }
    if (x + w > gfx_fb.width) w = gfx_fb.width - x;
    if (y + h > gfx_fb.height) h = gfx_fb.height - y;
    if (w <= 0 || h <= 0) return;

    sx = x / 8;
    ex = (x + w + 7) / 8;
    wb = ex - sx;

    for (i = 0; i < 4; i++) {
        u8 *dst_plane = dst_base + (i * orig_wb * h);
        const u8 *src_start = gfx_fb.planes[i] + y * gfx_fb.pitch + sx;
        asm_copy_plane_rect(dst_plane, orig_wb,
                            src_start, gfx_fb.pitch,
                            h, wb);
    }
}

void __cdecl gfx_restore_rect(int x, int y, int w, int h, const void *buf)
{
    int i;
    int sx, ex, wb;
    const u8 *src_base = (const u8 *)buf;

    int orig_wb = ((x + w + 7) / 8) - (x / 8);
    if (orig_wb <= 0 || h <= 0) return;

    if (x < 0) { w -= (0 - x); x = 0; }
    if (y < 0) { h -= (0 - y); y = 0; }
    if (x + w > gfx_fb.width) w = gfx_fb.width - x;
    if (y + h > gfx_fb.height) h = gfx_fb.height - y;
    if (w <= 0 || h <= 0) return;

    sx = x / 8;
    ex = (x + w + 7) / 8;
    wb = ex - sx;

    for (i = 0; i < 4; i++) {
        const u8 *src_plane = src_base + (i * orig_wb * h);
        u8 *dst_start = gfx_fb.planes[i] + y * gfx_fb.pitch + sx;
        asm_copy_plane_rect(dst_start, gfx_fb.pitch,
                            src_plane, orig_wb,
                            h, wb);
    }
}
