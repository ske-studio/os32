#include "gfx_internal.h"

/* ======================================================================== */
/*  静的サーフェスプール                                                    */
/* ======================================================================== */

static SurfSlot *surf_pool = (SurfSlot *)MEM_GFX_SURF_POOL;

void gfx_surface_init(void)
{
    int i;
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
    int p, sz;
    if (!surf) return;
    sz = surf->pitch * surf->h;
    for (p = 0; p < 4; p++) {
        u8 fill = (color & (1 << p)) ? 0xFF : 0x00;
        int i;
        for (i = 0; i < sz; i++) surf->planes[p][i] = fill;
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
            gfx_pixel(dx + ix, dy + iy, color);
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
