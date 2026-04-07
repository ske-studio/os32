#include "gfx_internal.h"

/* ======================================================================== */
/*  静的スプライトプール                                                    */
/* ======================================================================== */

static SprSlot *spr_pool = (SprSlot *)SPR_POOL_ADDR;

void gfx_sprite_init(void)
{
    int i;
    for (i = 0; i < SPR_POOL_MAX; i++) spr_pool[i].used = 0;
}

GFX_Sprite *gfx_create_sprite(const GFX_Surface *src, u8 trans_color)
{
    int i, p;
    int pitch, sz;
    SprSlot *s;

    if (!src) return 0;

    pitch = src->pitch;
    sz = pitch * src->h;
    if (sz > SPR_DATA_SIZE) return 0;

    for (i = 0; i < SPR_POOL_MAX; i++) {
        if (!spr_pool[i].used) {
            int ix, iy;

            s = &spr_pool[i];
            s->used = 1;
            s->spr.w = src->w;
            s->spr.h = src->h;
            s->spr.pitch = pitch;
            s->spr._pool_idx = i;

            for (p = 0; p < 4; p++) {
                s->spr.planes[p] = s->data[p];
            }
            s->spr.mask = s->mask_data;

            {
                int j;
                for (j = 0; j < sz; j++) {
                    s->mask_data[j] = 0xFF;
                    for (p = 0; p < 4; p++) {
                        s->data[p][j] = 0;
                    }
                }
            }

            for (iy = 0; iy < src->h; iy++) {
                for (ix = 0; ix < src->w; ix++) {
                    int off = iy * pitch + (ix >> 3);
                    u8 bit = 0x80 >> (ix & 7);
                    u8 color = 0;

                    for (p = 0; p < 4; p++) {
                        if (src->planes[p][off] & bit)
                            color |= (1 << p);
                    }

                    if (color != trans_color) {
                        s->mask_data[off] &= ~bit;
                        for (p = 0; p < 4; p++) {
                            if (color & (1 << p))
                                s->data[p][off] |= bit;
                        }
                    }
                }
            }

            return &s->spr;
        }
    }
    return 0;
}

void gfx_free_sprite(GFX_Sprite *spr)
{
    if (spr && spr->_pool_idx >= 0 && spr->_pool_idx < SPR_POOL_MAX) {
        spr_pool[spr->_pool_idx].used = 0;
    }
}

extern void __cdecl asm_gfx_draw_sprite(int x, int y, const GFX_Sprite *spr, u8 **bb_array);

void gfx_draw_sprite(int x, int y, const GFX_Sprite *spr)
{
    if (!spr) return;
    asm_gfx_draw_sprite(x, y, spr, bb);
    gfx_dirty_mark(y, y + spr->h - 1);
}
