#include "libos32gfx.h"
#include "libgfx_internal.h"
#include "os32api.h"

/* ======================================================================== */
/*  描画プリミティブ                                                        */
/* ======================================================================== */

extern void __cdecl asm_gfx_clear(u8 color, u8 **bb_array);
extern void __cdecl asm_fill_plane_rect(u8 *start, int pitch, int rows, int width_bytes, u8 fill_val);

void gfx_clear(u8 color)
{
    asm_gfx_clear(color, gfx_fb.planes);
    gfx_api->gfx_add_dirty_rect(0, 0, gfx_fb.width, gfx_fb.height);
}

void gfx_clear_rect(int rx, int ry, int rw, int rh, u8 color)
{
    int p;
    int byte_x, byte_w;
    int start_off;

    byte_x = rx >> 3;
    byte_w = ((rx + rw + 7) >> 3) - byte_x;
    if (rx < 0) {
        int shift = 0 - rx;
        rw -= shift; rx = 0;
        byte_x = rx >> 3;
        byte_w = ((rx + rw + 7) >> 3) - byte_x;
    }
    if (rx + rw > gfx_fb.width) rw = gfx_fb.width - rx;
    byte_w = ((rx + rw + 7) >> 3) - byte_x;

    if (byte_x < 0) byte_x = 0;
    if (byte_x + byte_w > gfx_fb.pitch) byte_w = gfx_fb.pitch - byte_x;

    if (ry < 0) { rh -= (0 - ry); ry = 0; }
    if (ry + rh > gfx_fb.height) rh = gfx_fb.height - ry;
    if (byte_w <= 0 || rh <= 0) return;

    start_off = ry * gfx_fb.pitch + byte_x;
    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        asm_fill_plane_rect(gfx_fb.planes[p] + start_off,
                            gfx_fb.pitch, rh, byte_w, fill);
    }
    gfx_api->gfx_add_dirty_rect(rx, ry, rw, rh);
}

void gfx_pixel(int x, int y, u8 color)
{
    int offset;
    u8 bit;
    int p;

    if (x < 0 || x >= gfx_fb.width || y < 0 || y >= gfx_fb.height) return;

    offset = y * gfx_fb.pitch + (x >> 3);
    bit = 0x80 >> (x & 7);

    for (p = 0; p < 4; p++) {
        if (color & (1 << p))
            gfx_fb.planes[p][offset] |= bit;
        else
            gfx_fb.planes[p][offset] &= ~bit;
    }
    gfx_api->gfx_add_dirty_rect(x, y, 1, 1);
}

u8 gfx_get_pixel(int x, int y)
{
    int offset;
    u8 bit, color;
    int p;

    if (x < 0 || x >= gfx_fb.width || y < 0 || y >= gfx_fb.height) return 0;

    offset = y * gfx_fb.pitch + (x >> 3);
    bit = 0x80 >> (x & 7);
    color = 0;

    for (p = 0; p < 4; p++) {
        if (gfx_fb.planes[p][offset] & bit)
            color |= (1 << p);
    }
    return color;
}

void gfx_hline(int x, int y, int w, u8 color)
{
    int x2, p;
    int bx1, bx2;
    int base;

    if (y < 0 || y >= gfx_fb.height || w <= 0) return;
    if (x < 0) { w -= (0 - x); x = 0; }
    x2 = x + w - 1;
    if (x2 >= gfx_fb.width) x2 = gfx_fb.width - 1;
    if (x > x2) return;

    gfx_api->gfx_add_dirty_rect(x & ~31, y, ((x2 + 32) & ~31) - (x & ~31), 1);
    base = y * gfx_fb.pitch;

    /* 同一バイト内に収まる場合 */
    if ((x >> 3) == (x2 >> 3)) {
        u8 mask = (u8)((0xFF >> (x & 7)) & (0xFF << (7 - (x2 & 7))));
        for (p = 0; p < 4; p++) {
            if (color & (1 << p))
                gfx_fb.planes[p][base + (x >> 3)] |= mask;
            else
                gfx_fb.planes[p][base + (x >> 3)] &= ~mask;
        }
        return;
    }

    /* 左端パーシャルバイト */
    if (x & 7) {
        u8 mask = (u8)(0xFF >> (x & 7));
        int off = base + (x >> 3);
        for (p = 0; p < 4; p++) {
            if (color & (1 << p))
                gfx_fb.planes[p][off] |= mask;
            else
                gfx_fb.planes[p][off] &= ~mask;
        }
        bx1 = (x | 7) + 1;
    } else {
        bx1 = x;
    }

    /* 右端パーシャルバイト */
    if ((x2 & 7) != 7) {
        u8 mask = (u8)(0xFF << (7 - (x2 & 7)));
        int off = base + (x2 >> 3);
        for (p = 0; p < 4; p++) {
            if (color & (1 << p))
                gfx_fb.planes[p][off] |= mask;
            else
                gfx_fb.planes[p][off] &= ~mask;
        }
        bx2 = x2 & ~7;
    } else {
        bx2 = x2 + 1;
    }

    /* 中間フルバイト: asm_fill_plane_rect (rows=1) */
    if (bx1 < bx2) {
        int start_byte = base + (bx1 >> 3);
        int count = (bx2 - bx1) >> 3;
        for (p = 0; p < 4; p++) {
            u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
            asm_fill_plane_rect(gfx_fb.planes[p] + start_byte,
                                gfx_fb.pitch, 1, count, fill);
        }
    }
}

void gfx_vline(int x, int y, int h, u8 color)
{
    int p, off;
    u8 bit;

    if (x < 0 || x >= gfx_fb.width || h <= 0) return;
    if (y < 0) { h -= (0 - y); y = 0; }
    if (y + h > gfx_fb.height) h = gfx_fb.height - y;
    if (h <= 0) return;

    gfx_api->gfx_add_dirty_rect(x & ~31, y, 32, h);
    off = y * gfx_fb.pitch + (x >> 3);
    bit = 0x80 >> (x & 7);

    for (p = 0; p < 4; p++) {
        int r;
        if (color & (1 << p)) {
            for (r = 0; r < h; r++) {
                gfx_fb.planes[p][off + r * gfx_fb.pitch] |= bit;
            }
        } else {
            u8 inv = ~bit;
            for (r = 0; r < h; r++) {
                gfx_fb.planes[p][off + r * gfx_fb.pitch] &= inv;
            }
        }
    }
}

void gfx_line(int x0, int y0, int x1, int y1, u8 color)
{
    int dx, dy, sx, sy, err, e2;

    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;

    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(int x, int y, int w, int h, u8 color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_fill_rect(int x, int y, int w, int h, u8 color)
{
    int x2, p;
    int bx1, bx2, count;
    int row_off;
    u8 left_mask, right_mask;
    int same_byte;

    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x2 = x + w - 1;
    if (x2 >= gfx_fb.width) x2 = gfx_fb.width - 1;
    if (y + h > gfx_fb.height) h = gfx_fb.height - y;
    if (x > x2 || h <= 0) return;

    gfx_api->gfx_add_dirty_rect(x, y, x2 - x + 1, h);

    same_byte = ((x >> 3) == (x2 >> 3));

    /* 左端マスク計算 */
    left_mask = 0;
    if (x & 7) {
        left_mask = (u8)(0xFF >> (x & 7));
        if (same_byte)
            left_mask &= (u8)(0xFF << (7 - (x2 & 7)));
    } else if (same_byte) {
        left_mask = (u8)(0xFF << (7 - (x2 & 7)));
    }

    /* 右端マスク計算 (別バイトの場合のみ) */
    right_mask = 0;
    if (!same_byte && (x2 & 7) != 7) {
        right_mask = (u8)(0xFF << (7 - (x2 & 7)));
    }

    /* 中間フルバイト範囲 */
    bx1 = (x + 7) & ~7;
    bx2 = (x2 + 1) & ~7;
    if (same_byte) { bx1 = bx2 = 0; count = 0; }
    else { count = (bx2 - bx1) >> 3; }

    row_off = y * gfx_fb.pitch;

    /* プレーンメジャーループ: 各プレーンを独立に処理 */
    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        u8 *plane = gfx_fb.planes[p];

        /* 左端列 */
        if (left_mask) {
            int off = row_off + (x >> 3);
            int r;
            if (fill) {
                for (r = 0; r < h; r++) {
                    plane[off] |= left_mask;
                    off += gfx_fb.pitch;
                }
            } else {
                u8 inv = ~left_mask;
                for (r = 0; r < h; r++) {
                    plane[off] &= inv;
                    off += gfx_fb.pitch;
                }
            }
        }

        /* 中間フルバイト矩形 */
        if (count > 0) {
            asm_fill_plane_rect(plane + row_off + (bx1 >> 3),
                                gfx_fb.pitch, h, count, fill);
        }

        /* 右端列 */
        if (right_mask) {
            int off = row_off + (x2 >> 3);
            int r;
            if (fill) {
                for (r = 0; r < h; r++) {
                    plane[off] |= right_mask;
                    off += gfx_fb.pitch;
                }
            } else {
                u8 inv = ~right_mask;
                for (r = 0; r < h; r++) {
                    plane[off] &= inv;
                    off += gfx_fb.pitch;
                }
            }
        }
    }
}

/* ======================================================================== */
/*  フォントラスタライザ (単色・透過)                                       */
/* ======================================================================== */
extern void __cdecl asm_kcg_draw_font(int x, int y, const u8 *pat, int w_bytes, int h_lines, u8 fg, u8 **bb_array);

void gfx_draw_font(int x, int y, const u8 *pat, int w_bytes, int h_lines, u8 fg)
{
    /* asm_kcg_draw_font はX座標の8ドットアライメント(バイト境界)を前提としている */
    if ((x & 7) == 0) {
        asm_kcg_draw_font(x, y, pat, w_bytes, h_lines, fg, gfx_fb.planes);
        gfx_api->gfx_add_dirty_rect(x, y, w_bytes * 8, h_lines);
    } else {
        /* 非アライメント時のフォールバック (基本ここでは呼ばれないはず) */
        int row, col, b;
        for (row = 0; row < h_lines; row++) {
            for (b = 0; b < w_bytes; b++) {
                u8 bits = pat[row * w_bytes + b];
                for (col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        gfx_pixel(x + b * 8 + col, y + row, fg);
                    }
                }
            }
        }
    }
}
