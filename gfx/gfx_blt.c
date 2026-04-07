#include "gfx_internal.h"

/* ======================================================================== */
/*  バッキングストア・キャッシュ操作                                         */
/* ======================================================================== */
/* ※ アライメントは8px(1byte)単位で行われる。x,w が8の倍数でなくても包含するバイトを転送する */

void __cdecl gfx_save_rect(int x, int y, int w, int h, void *buf)
{
    int i, row;
    int sx = x / 8;
    int ex = (x + w + 7) / 8;
    int wb = ex - sx;
    u8 *dst_base = (u8 *)buf;
    if (wb <= 0 || h <= 0) return;
    for (i = 0; i < 4; i++) {
        u8 *plane = bb[i];
        u8 *dst_plane = dst_base + (i * GFX_PLANE_SZ); /* 32000バイトごとにプレーンが連続している前提 */
        for (row = 0; row < h; row++) {
            int py = y + row;
            if (py >= 0 && py < GFX_HEIGHT) {
                kmemcpy(dst_plane + py * GFX_BPL + sx, plane + py * GFX_BPL + sx, wb);
            }
        }
    }
}

void __cdecl gfx_restore_rect(int x, int y, int w, int h, const void *buf)
{
    int i, row;
    int sx = x / 8;
    int ex = (x + w + 7) / 8;
    int wb = ex - sx;
    const u8 *src_base = (const u8 *)buf;
    if (wb <= 0 || h <= 0) return;
    for (i = 0; i < 4; i++) {
        u8 *plane = bb[i];
        const u8 *src_plane = src_base + (i * GFX_PLANE_SZ);
        for (row = 0; row < h; row++) {
            int py = y + row;
            if (py >= 0 && py < GFX_HEIGHT) {
                kmemcpy(plane + py * GFX_BPL + sx, src_plane + py * GFX_BPL + sx, wb);
            }
        }
    }
}
