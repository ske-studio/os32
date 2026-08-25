/* ======================================================================== */
/*  MAP_TILE.C — タイルアクセス・通行判定・化学属性取得                       */
/*                                                                          */
/*  マップ上の個別タイルに対する高速な読み取り・判定機能。                    */
/*  全てRAMキャッシュに基づくため、ホットパスで安全に使用可能。               */
/* ======================================================================== */

#include "libos32map.h"

/* ====================================================================== */
/*  内部状態アクセサ (map_core.c で定義)                                    */
/* ====================================================================== */

extern MapDef      *map__get_mapdef(void);
extern u16        **map__get_tiles(void);
extern TileProp    *map__get_tile_props(void);
extern int          map__tile_prop_count(void);

/* ====================================================================== */
/*  内部ヘルパー: タイルIDからTilePropを検索                                */
/* ====================================================================== */

static const TileProp *find_prop(u16 tile_id)
{
    TileProp *props = map__get_tile_props();
    int count = map__tile_prop_count();
    int i;

    /* タイルID == TileProp.tile_id で線形検索 */
    /* 256件以内なので十分高速 */
    for (i = 0; i < count; i++) {
        if (props[i].tile_id == tile_id) {
            return &props[i];
        }
    }
    return (const TileProp *)0;
}

/* ====================================================================== */
/*  公開API: タイルアクセス                                                 */
/* ====================================================================== */

u16 map_get_tile(int layer, int col, int row)
{
    MapDef *m = map__get_mapdef();
    u16 **tiles = map__get_tiles();

    /* マップ未ロード */
    if (m->id == 0) return 0;

    /* 範囲チェック */
    if (layer < 0 || layer >= (int)m->layer_count) return 0;
    if (col < 0 || col >= (int)m->width) return 0;
    if (row < 0 || row >= (int)m->height) return 0;

    /* タイルデータ未割当 */
    if (!tiles[layer]) return 0;

    return tiles[layer][row * (int)m->width + col];
}

int map_is_passable(int col, int row)
{
    u16 tile;
    const TileProp *prop;
    MapDef *m = map__get_mapdef();

    if (m->id == 0) return 0;

    /* 範囲外は通行不可 */
    if (col < 0 || col >= (int)m->width) return 0;
    if (row < 0 || row >= (int)m->height) return 0;

    /* レイヤー0 (地面) のタイルで判定 */
    tile = map_get_tile(0, col, row);

    /* タイルIDのみ抽出 (bit 9-0) */
    tile &= 0x3FF;

    prop = find_prop(tile);
    if (!prop) {
        /* プロパティ未定義 = デフォルトで通行可能 */
        return 1;
    }

    return (prop->passable != 0) ? 1 : 0;
}

const TileProp *map_get_prop(int col, int row)
{
    u16 tile;
    MapDef *m = map__get_mapdef();

    if (m->id == 0) return (const TileProp *)0;
    if (col < 0 || col >= (int)m->width) return (const TileProp *)0;
    if (row < 0 || row >= (int)m->height) return (const TileProp *)0;

    /* レイヤー0 (地面) のタイルで判定 */
    tile = map_get_tile(0, col, row);
    tile &= 0x3FF;

    return find_prop(tile);
}

u32 map_get_chem_elements(int col, int row)
{
    const TileProp *prop;

    prop = map_get_prop(col, row);
    if (!prop) return 0;

    return prop->chem_elements;
}

/* ====================================================================== */
/*  公開API: 化学エンジン連携                                               */
/* ====================================================================== */

void map_set_tile_element(int col, int row, u32 elements)
{
    u16 tile;
    TileProp *props;
    int count;
    int i;
    MapDef *m = map__get_mapdef();

    if (m->id == 0) return;
    if (col < 0 || col >= (int)m->width) return;
    if (row < 0 || row >= (int)m->height) return;

    tile = map_get_tile(0, col, row);
    tile &= 0x3FF;

    props = map__get_tile_props();
    count = map__tile_prop_count();
    for (i = 0; i < count; i++) {
        if (props[i].tile_id == tile) {
            props[i].chem_elements = elements;
            return;
        }
    }
}

void map_replace_tile(int layer, int col, int row, u16 new_tile)
{
    MapDef *m = map__get_mapdef();
    u16 **tiles = map__get_tiles();

    if (m->id == 0) return;
    if (layer < 0 || layer >= (int)m->layer_count) return;
    if (col < 0 || col >= (int)m->width) return;
    if (row < 0 || row >= (int)m->height) return;
    if (!tiles[layer]) return;

    tiles[layer][row * (int)m->width + col] = new_tile;
}

/* ====================================================================== */
/*  公開API: 矩形通行判定                                                   */
/* ====================================================================== */

int map_rect_passable(int x, int y, int w, int h)
{
    int cx, cy;

    for (cy = y; cy < y + h; cy++) {
        for (cx = x; cx < x + w; cx++) {
            if (!map_is_passable(cx, cy)) {
                return 0;
            }
        }
    }
    return 1;
}
