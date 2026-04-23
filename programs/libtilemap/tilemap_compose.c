#include "tilemap_internal.h"

/* TileDef から一時的な GFX_Surface を作成 */
static void tile_to_surface(GFX_Surface *s, const TileDef *t)
{
    s->w = TILE_W;
    s->h = TILE_H;
    s->pitch = TILE_PITCH;
    s->planes[0] = (u8 *)t->planes[0];
    s->planes[1] = (u8 *)t->planes[1];
    s->planes[2] = (u8 *)t->planes[2];
    s->planes[3] = (u8 *)t->planes[3];
    s->_pool_idx = -1;  /* プール外 */
}

void tilemap_compose_btf(void)
{
    int row, col, bg;
    u8 cell_dirty[TILEMAP_ROWS][TILEMAP_COLS];
    GFX_Surface tmp_surf;

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    /* 1. 全BGを走査してダーティなセルを特定しつつ、フラグをクリア */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            cell_dirty[row][col] = 0;
            for (bg = 0; bg < BG_COUNT; bg++) {
                if (_tilemap.bg_planes[bg].dirty[row][col]) {
                    cell_dirty[row][col] = 1;
                    _tilemap.bg_planes[bg].dirty[row][col] = 0;
                }
            }
        }
    }

    /* 2. ダーティなセルのみ、BG0から順に再描画 */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            if (!cell_dirty[row][col]) continue;

            for (bg = 0; bg < BG_COUNT; bg++) {
                int tile_id;
                TileDef *tile;
                int x, y;

                if (!_tilemap.bg_planes[bg].visible) continue;

                tile_id = TILEMAP_ID(_tilemap.bg_planes[bg].map[row][col]);
                tile = &_tilemap.tiles[tile_id];

                if (tile->opacity == TILE_TRANSPARENT) continue;

                x = _tilemap.origin_x + col * TILE_W;
                y = _tilemap.origin_y + row * TILE_H;

                tile_to_surface(&tmp_surf, tile);

                /* 最背面(BG0) または 不透明タイルなら上書きblit (高速パス) */
                if (tile->opacity == TILE_OPAQUE) {
                    gfx_blit(x, y, &tmp_surf, NULL);
                } else {
                    /* BG0の透過部分の扱いはユーザー依存 (OPAQUEタイル推奨) */
                    gfx_blit_transparent(x, y, &tmp_surf, NULL);
                }
            }
        }
    }
}

void tilemap_compose_ftb(void)
{
    /* Phase 2 で実装 */
}

void tilemap_present(void)
{
    if (_tilemap.kapi) {
        _tilemap.kapi->gfx_present_dirty();
    }
}
