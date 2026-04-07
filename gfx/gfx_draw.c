#include "gfx_internal.h"

/* ======================================================================== */
/*  描画プリミティブ                                                        */
/* ======================================================================== */

extern void __cdecl asm_gfx_clear(u8 color, u8 **bb_array);

void gfx_clear(u8 color)
{
    asm_gfx_clear(color, bb);
    dirty_min_y = 0;
    dirty_max_y = GFX_HEIGHT - 1;
}

void gfx_clear_rect(int rx, int ry, int rw, int rh, u8 color)
{
    int p, row;
    int byte_x, byte_w;

    byte_x = rx >> 3;
    byte_w = ((rx + rw + 7) >> 3) - byte_x;
    if (byte_x < 0) byte_x = 0;
    if (byte_x + byte_w > GFX_BPL) byte_w = GFX_BPL - byte_x;
    if (ry < 0) ry = 0;
    if (ry + rh > GFX_HEIGHT) rh = GFX_HEIGHT - ry;
    if (byte_w <= 0 || rh <= 0) return;

    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        for (row = ry; row < ry + rh; row++) {
            int off = row * GFX_BPL + byte_x;
            int bx;
            for (bx = 0; bx < byte_w; bx++) {
                bb[p][off + bx] = fill;
            }
        }
    }
    gfx_dirty_mark(ry, ry + rh - 1);
}

void gfx_pixel(int x, int y, u8 color)
{
    int offset;
    u8 bit;
    int p;

    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;

    offset = y * GFX_BPL + (x >> 3);
    bit = 0x80 >> (x & 7);

    for (p = 0; p < 4; p++) {
        if (color & (1 << p))
            bb[p][offset] |= bit;
        else
            bb[p][offset] &= ~bit;
    }
    gfx_dirty_mark(y, y);
}

u8 gfx_get_pixel(int x, int y)
{
    int offset;
    u8 bit, color;
    int p;

    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return 0;

    offset = y * GFX_BPL + (x >> 3);
    bit = 0x80 >> (x & 7);
    color = 0;

    for (p = 0; p < 4; p++) {
        if (bb[p][offset] & bit)
            color |= (1 << p);
    }
    return color;
}

void gfx_hline(int x, int y, int w, u8 color)
{
    int x2, p;
    int bx1, bx2;

    if (y < 0 || y >= GFX_HEIGHT || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    gfx_dirty_mark(y, y);
    x2 = x + w - 1;
    if (x2 >= GFX_WIDTH) x2 = GFX_WIDTH - 1;
    if (x > x2) return;

    if ((x >> 3) == (x2 >> 3)) {
        int i;
        for (i = x; i <= x2; i++) gfx_pixel(i, y, color);
        return;
    }

    if (x & 7) {
        int i;
        bx1 = (x | 7) + 1;
        for (i = x; i < bx1; i++) gfx_pixel(i, y, color);
    } else {
        bx1 = x;
    }

    if ((x2 & 7) != 7) {
        int i;
        bx2 = x2 & ~7;
        for (i = bx2; i <= x2; i++) gfx_pixel(i, y, color);
    } else {
        bx2 = x2 + 1;
    }

    if (bx1 < bx2) {
        int start_byte = y * GFX_BPL + (bx1 >> 3);
        int count = (bx2 - bx1) >> 3;
        for (p = 0; p < 4; p++) {
            u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
            int i;
            for (i = 0; i < count; i++) {
                bb[p][start_byte + i] = fill;
            }
        }
    }
}

void gfx_vline(int x, int y, int h, u8 color)
{
    int i;
    if (x < 0 || x >= GFX_WIDTH) return;
    gfx_dirty_mark(y, y + h - 1);
    for (i = 0; i < h; i++) {
        gfx_pixel(x, y + i, color);
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
    int x2, p, row;
    int bx1, bx2, count, start_byte;

    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x2 = x + w - 1;
    if (x2 >= GFX_WIDTH) x2 = GFX_WIDTH - 1;
    if (y + h > GFX_HEIGHT) h = GFX_HEIGHT - y;
    if (x > x2 || h <= 0) return;

    gfx_dirty_mark(y, y + h - 1);

    bx1 = (x + 7) & ~7;
    bx2 = (x2 + 1) & ~7;
    if (bx1 > x2 + 1) bx1 = x2 + 1;
    if (bx2 < x) bx2 = x;

    for (row = y; row < y + h; row++) {
        int base = row * GFX_BPL;

        if (x < bx1) {
            int off = base + (x >> 3);
            u8 mask_start = (u8)(0xFF >> (x & 7));
            u8 mask;
            if ((x >> 3) == (x2 >> 3)) {
                mask = mask_start & (u8)(0xFF << (7 - (x2 & 7)));
            } else {
                mask = mask_start;
            }
            for (p = 0; p < 4; p++) {
                if (color & (1 << p))
                    bb[p][off] |= mask;
                else
                    bb[p][off] &= ~mask;
            }
        }

        if (bx1 < bx2) {
            start_byte = base + (bx1 >> 3);
            count = (bx2 - bx1) >> 3;
            for (p = 0; p < 4; p++) {
                u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
                int i;
                if (count >= 4 && (count & 3) == 0) {
                    u32 fill32 = fill | ((u32)fill << 8) | ((u32)fill << 16) | ((u32)fill << 24);
                    _memset_d(bb[p] + start_byte, fill32, count / 4);
                } else {
                    for (i = 0; i < count; i++) {
                        bb[p][start_byte + i] = fill;
                    }
                }
            }
        }

        if (bx2 <= x2 && (x >> 3) != (x2 >> 3)) {
            int off = base + (bx2 >> 3);
            u8 mask = (u8)(0xFF << (7 - (x2 & 7)));
            for (p = 0; p < 4; p++) {
                if (color & (1 << p))
                    bb[p][off] |= mask;
                else
                    bb[p][off] &= ~mask;
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
        asm_kcg_draw_font(x, y, pat, w_bytes, h_lines, fg, bb);
        gfx_dirty_mark(y, y + h_lines - 1);
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
