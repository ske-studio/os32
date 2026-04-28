/* ======================================================================== */
/*  MAP_VIEW.C — カメラ管理 + libtilemap への BG転送                         */
/*                                                                          */
/*  ビューポート計算と、マップタイルデータを libtilemap の BGプレーンに        */
/*  転送する機能を提供する。                                                 */
/*                                                                          */
/*  map_apply_to_tilemap() は libtilemap.h の tilemap_set() を呼ぶため、     */
/*  利用側で libtilemap.h をインクルードしていることが前提。                  */
/* ======================================================================== */

#include "libos32map.h"
#include "libtilemap.h"    /* tilemap_set() */

/* ====================================================================== */
/*  内部状態アクセサ (map_core.c で定義)                                    */
/* ====================================================================== */

extern MapDef   *map__get_mapdef(void);
extern u16     **map__get_tiles(void);
extern i16      *map__get_camera_x(void);
extern i16      *map__get_camera_y(void);

/* ====================================================================== */
/*  ビューポート計算                                                        */
/* ====================================================================== */

/*
 * map_get_viewport — カメラ座標からビューポートの左上座標を計算
 *
 * カメラ座標を中心とした TILEMAP_COLS x TILEMAP_ROWS のウィンドウの
 * 左上タイル座標を返す。マップ端ではクランプされる。
 */
void map_get_viewport(int *out_col, int *out_row)
{
    MapDef *m = map__get_mapdef();
    int cx, cy;
    int vx, vy;

    if (m->id == 0) {
        if (out_col) *out_col = 0;
        if (out_row) *out_row = 0;
        return;
    }

    cx = (int)*map__get_camera_x();
    cy = (int)*map__get_camera_y();

    /* カメラ中心からビューポート左上を算出 */
    vx = cx - TILEMAP_COLS / 2;
    vy = cy - TILEMAP_ROWS / 2;

    /* マップ端クランプ */
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > (int)m->width - TILEMAP_COLS) {
        vx = (int)m->width - TILEMAP_COLS;
    }
    if (vy > (int)m->height - TILEMAP_ROWS) {
        vy = (int)m->height - TILEMAP_ROWS;
    }
    /* マップが TILEMAP_COLS 未満の場合の再クランプ */
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;

    if (out_col) *out_col = vx;
    if (out_row) *out_row = vy;
}

/* ====================================================================== */
/*  libtilemap BG転送                                                       */
/* ====================================================================== */

/*
 * map_apply_to_tilemap — ビューポート内のタイルを libtilemap のBGに転送
 *
 * bg_layer:  libtilemap のBGプレーン番号 (0-3)
 * map_layer: libos32map のマップレイヤー番号 (0 = 地面, 1 = 装飾, 2 = 前景)
 *
 * カメラ位置に基づいてビューポート (TILEMAP_COLS x TILEMAP_ROWS) を
 * 計算し、マップタイルデータを tilemap_set() で BG に設定する。
 * マップ範囲外のセルは タイルID=0 (透過/空白) で埋められる。
 */
void map_apply_to_tilemap(int bg_layer, int map_layer)
{
    MapDef *m = map__get_mapdef();
    u16 **tiles = map__get_tiles();
    int view_col, view_row;
    int row, col;
    int map_col, map_row;
    u16 tile;

    if (m->id == 0) return;
    if (map_layer < 0 || map_layer >= (int)m->layer_count) return;
    if (!tiles[map_layer]) return;

    /* ビューポート左上座標を計算 */
    map_get_viewport(&view_col, &view_row);

    /* TILEMAP_ROWS x TILEMAP_COLS をスキャンして tilemap_set で転送 */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        map_row = view_row + row;
        for (col = 0; col < TILEMAP_COLS; col++) {
            map_col = view_col + col;

            /* マップ範囲チェック */
            if (map_col >= 0 && map_col < (int)m->width &&
                map_row >= 0 && map_row < (int)m->height) {
                tile = tiles[map_layer][map_row * (int)m->width + map_col];
            } else {
                tile = 0;  /* 範囲外は空白 */
            }

            tilemap_set(bg_layer, col, row, tile);
        }
    }
}
