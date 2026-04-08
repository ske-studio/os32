#include "libos32gfx.h"
#include "libgfx_internal.h"

/* ======================================================================== */
/*  バッキングストア・キャッシュ操作                                         */
/* ======================================================================== */
/* ※ アライメントは8px(1byte)単位で行われる。x,w が8の倍数でなくても包含するバイトを転送する */

void __cdecl gfx_save_rect(int x, int y, int w, int h, void *buf)
{
    int i, row;
    int sx, ex, wb, py;
    u8 *dst_base = (u8 *)buf;

    /* 初期幅・高さ。元の引数からバッファの大きさを計算するためクリップ前に計算しておく */
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
        u8 *plane = gfx_fb.planes[i];
        u8 *dst_plane = dst_base + (i * orig_wb * h); /* バッファサイズは元の大きさを基準にする */
        for (row = 0; row < h; row++) {
            py = y + row;
            _memcpy_d((void *)(dst_plane + row * orig_wb), (const void *)(plane + py * gfx_fb.pitch + sx), wb / 4);
            int remain = wb % 4;
            int offset = wb - remain;
            while(remain--) {
                *(dst_plane + row * orig_wb + offset) = *(plane + py * gfx_fb.pitch + sx + offset);
                offset++;
            }
        }
    }
}

void __cdecl gfx_restore_rect(int x, int y, int w, int h, const void *buf)
{
    int i, row;
    int sx, ex, wb, py;
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
        u8 *plane = gfx_fb.planes[i];
        const u8 *src_plane = src_base + (i * orig_wb * h);
        for (row = 0; row < h; row++) {
            py = y + row;
            _memcpy_d((void *)(plane + py * gfx_fb.pitch + sx), (const void *)(src_plane + row * orig_wb), wb / 4);
            int remain = wb % 4;
            int offset = wb - remain;
            while(remain--) {
                *(plane + py * gfx_fb.pitch + sx + offset) = *(src_plane + row * orig_wb + offset);
                offset++;
            }
        }
    }
}
