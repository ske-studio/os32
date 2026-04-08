#include "libos32gfx.h"
#include "libgfx_internal.h"
#include "os32api.h"

extern void __cdecl asm_fill_plane_rect(u8 *start, int pitch, int rows, int width_bytes, u8 fill_val);
extern void __cdecl asm_copy_plane_rect(u8 *dst, int dst_pitch,
                                        const u8 *src, int src_pitch,
                                        int rows, int width_bytes);

/* ======================================================================== */
/*  静的サーフェスプール                                                    */
/* ======================================================================== */

static SurfSlot *surf_pool = 0;

void gfx_surface_init(void)
{
    int i;
    if (!surf_pool) {
        surf_pool = (SurfSlot *)gfx_api->mem_alloc(sizeof(SurfSlot) * SURF_POOL_MAX);
    }
    for (i = 0; i < SURF_POOL_MAX; i++) surf_pool[i].used = 0;
}

GFX_Surface *gfx_create_surface(int w, int h)
{
    int i, p;
    int pitch = (w + 7) >> 3;
    int sz = pitch * h;

    if (sz > SURF_DATA_SIZE) return 0;

    for (i = 0; i < SURF_POOL_MAX; i++) {
        if (!surf_pool[i].used) {
            SurfSlot *s = &surf_pool[i];
            s->used = 1;
            s->surf.w = w;
            s->surf.h = h;
            s->surf.pitch = pitch;
            s->surf._pool_idx = i;
            for (p = 0; p < 4; p++) {
                s->surf.planes[p] = s->data[p];
            }
            gfx_surface_clear(&s->surf, 0);
            return &s->surf;
        }
    }
    return 0;
}

void gfx_free_surface(GFX_Surface *surf)
{
    if (surf && surf->_pool_idx >= 0 && surf->_pool_idx < SURF_POOL_MAX) {
        surf_pool[surf->_pool_idx].used = 0;
    }
}

void gfx_surface_clear(GFX_Surface *surf, u8 color)
{
    int p;
    if (!surf) return;
    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        asm_fill_plane_rect(surf->planes[p], surf->pitch, surf->h, surf->pitch, fill);
    }
}

void gfx_surface_pixel(GFX_Surface *surf, int x, int y, u8 color)
{
    int offset;
    u8 bit;
    int p;

    if (!surf || x < 0 || x >= surf->w || y < 0 || y >= surf->h) return;

    offset = y * surf->pitch + (x >> 3);
    bit = 0x80 >> (x & 7);

    for (p = 0; p < 4; p++) {
        if (color & (1 << p))
            surf->planes[p][offset] |= bit;
        else
            surf->planes[p][offset] &= ~bit;
    }
}

/* ======================================================================== */
/*  ブリット                                                                */
/* ======================================================================== */

void gfx_blit(int dx, int dy,
              const GFX_Surface *src, const GFX_Rect *src_rect)
{
    int sx, sy, sw, sh;
    int p;

    if (!src) return;

    if (src_rect) {
        sx = src_rect->x; sy = src_rect->y;
        sw = src_rect->w; sh = src_rect->h;
    } else {
        sx = 0; sy = 0; sw = src->w; sh = src->h;
    }

    /* バイト境界揃い時の高速パス */
    if ((sx & 7) == 0 && (dx & 7) == 0) {
        int wb = (sw + 7) >> 3;
        int dy_clip = dy;
        int sy_clip = sy;
        int sh_clip = sh;

        /* Y軸クリップ */
        if (dy_clip < 0) { sy_clip -= dy_clip; sh_clip += dy_clip; dy_clip = 0; }
        if (dy_clip + sh_clip > gfx_fb.height) sh_clip = gfx_fb.height - dy_clip;
        if (sh_clip <= 0) return;

        /* X軸クリップ (バイト単位) */
        if (dx + wb * 8 > gfx_fb.width) wb = (gfx_fb.width - dx + 7) >> 3;
        if (wb <= 0) return;

        for (p = 0; p < 4; p++) {
            const u8 *s = src->planes[p] + sy_clip * src->pitch + (sx >> 3);
            u8 *d = gfx_fb.planes[p] + dy_clip * gfx_fb.pitch + (dx >> 3);
            asm_copy_plane_rect(d, gfx_fb.pitch, s, src->pitch, sh_clip, wb);
        }
        gfx_api->gfx_add_dirty_rect(dx, dy_clip, sw, sh_clip);
        return;
    }

    /* 非境界揃い: ピクセル単位フォールバック */
    {
        int ix, iy;
        for (iy = 0; iy < sh; iy++) {
            for (ix = 0; ix < sw; ix++) {
                int soff = (sy + iy) * src->pitch + ((sx + ix) >> 3);
                u8 sbit = 0x80 >> ((sx + ix) & 7);
                u8 color = 0;

                for (p = 0; p < 4; p++) {
                    if (src->planes[p][soff] & sbit)
                        color |= (1 << p);
                }
                gfx_pixel(dx + ix, dy + iy, color);
            }
        }
    }
}

void gfx_blit_colorkey(int dx, int dy,
                       const GFX_Surface *src, const GFX_Rect *src_rect,
                       u8 colorkey)
{
    int sx, sy, sw, sh;
    int ix, iy;

    if (!src) return;

    if (src_rect) {
        sx = src_rect->x; sy = src_rect->y;
        sw = src_rect->w; sh = src_rect->h;
    } else {
        sx = 0; sy = 0; sw = src->w; sh = src->h;
    }

    for (iy = 0; iy < sh; iy++) {
        for (ix = 0; ix < sw; ix++) {
            int soff = (sy + iy) * src->pitch + ((sx + ix) >> 3);
            u8 sbit = 0x80 >> ((sx + ix) & 7);
            u8 color = 0;
            int p;

            for (p = 0; p < 4; p++) {
                if (src->planes[p][soff] & sbit)
                    color |= (1 << p);
            }
            if (color != colorkey) {
                gfx_pixel(dx + ix, dy + iy, color);
            }
        }
    }
}

void gfx_surface_fill_rect(GFX_Surface *surf, int x, int y, int w, int h, u8 color)
{
    int x2, p;
    int bx1, bx2, count;
    int same_byte;
    u8 left_mask, right_mask;

    if (!surf) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x2 = x + w - 1;
    if (x2 >= surf->w) x2 = surf->w - 1;
    if (y + h > surf->h) h = surf->h - y;
    if (x > x2 || h <= 0) return;

    same_byte = ((x >> 3) == (x2 >> 3));

    /* 左端マスク */
    left_mask = 0;
    if (x & 7) {
        left_mask = (u8)(0xFF >> (x & 7));
        if (same_byte)
            left_mask &= (u8)(0xFF << (7 - (x2 & 7)));
    } else if (same_byte) {
        left_mask = (u8)(0xFF << (7 - (x2 & 7)));
    }

    /* 右端マスク */
    right_mask = 0;
    if (!same_byte && (x2 & 7) != 7) {
        right_mask = (u8)(0xFF << (7 - (x2 & 7)));
    }

    bx1 = (x + 7) & ~7;
    bx2 = (x2 + 1) & ~7;
    if (same_byte) { bx1 = bx2 = 0; count = 0; }
    else { count = (bx2 - bx1) >> 3; }

    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        u8 *plane = surf->planes[p];
        int row_off = y * surf->pitch;

        if (left_mask) {
            int off = row_off + (x >> 3);
            int r;
            if (fill) {
                for (r = 0; r < h; r++) { plane[off] |= left_mask; off += surf->pitch; }
            } else {
                u8 inv = ~left_mask;
                for (r = 0; r < h; r++) { plane[off] &= inv; off += surf->pitch; }
            }
        }

        if (count > 0) {
            asm_fill_plane_rect(plane + row_off + (bx1 >> 3),
                                surf->pitch, h, count, fill);
        }

        if (right_mask) {
            int off = row_off + (x2 >> 3);
            int r;
            if (fill) {
                for (r = 0; r < h; r++) { plane[off] |= right_mask; off += surf->pitch; }
            } else {
                u8 inv = ~right_mask;
                for (r = 0; r < h; r++) { plane[off] &= inv; off += surf->pitch; }
            }
        }
    }
}
