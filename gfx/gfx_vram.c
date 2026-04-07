#include "gfx_internal.h"

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
