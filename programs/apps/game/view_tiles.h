/* ======================================================================== */
/*  VIEW_TILES.H — 盤面の地形タイルとパレット                                */
/* ======================================================================== */

#ifndef VIEW_TILES_H
#define VIEW_TILES_H

#include "os32api.h"

/* --- パレット添字 (view_tiles.c の g_palette と対応) ---
   古地図の色。0 は libos32tilemap では透過扱いなので、
   地形タイルの中では使わないこと。 */
#define TC_OUTLINE   0    /* 濃セピア。gfx 描画の輪郭・文字用 */
#define TC_SEA       1    /* 海 (深) */
#define TC_SEA_HI    2    /* 海 (浅) */
#define TC_LAND_SH   3    /* 陸 (陰) セピア */
#define TC_LAND      4    /* 陸 (地) 羊皮紙 */
#define TC_LAND_HI   5    /* 陸 (明) 生成り */
#define TC_ROAD      6    /* 道 (褪せた朱土) */
#define TC_ROCK      7    /* 岩 */
#define TC_FOREST    8    /* 森 */
#define TC_P_BLUE    9
#define TC_P_RED    10
#define TC_MAGIC    11
#define TC_P_GREEN  12
#define TC_SHRINE   13
#define TC_P_YELLOW 14
#define TC_PAPER    15    /* 紙の白 */

/* --- タイルID ---
   0 は透過タイルとして空けておく (libos32tilemap の規約) */
#define TILE_LAND        1    /* +0..3 陸のバリエーション */
#define TILE_LAND_HATCH  5    /* 起伏のハッチング */
#define TILE_SEA         6    /* +0..1 海 */
#define TILE_SHORE       8    /* +0..3 海岸 (0=陸が上, 1=右, 2=下, 3=左) */
#define TILE_FOREST     12
#define TILE_MOUNTAIN   13
#define TILE_ARROW      16    /* +0..3 矢印 (0=上, 1=右, 2=下, 3=左) */

/* --- 施設タイル (32x32 を 2x2 タイルで持つ。+0=左上 +1=右上 +2=左下 +3=右下) --- */
#define TILE_FAC_VILLAGE    32    /* +0..19: 無主/赤/青/緑/黄 の5種 x 4タイル */
#define TILE_FAC_BATTLE     52
#define TILE_FAC_CHEST      56
#define TILE_FAC_GOLDCHEST  60
#define TILE_FAC_ITEM       64
#define TILE_FAC_EQUIP      68
#define TILE_FAC_MAGIC      72
#define TILE_FAC_CHURCH     76
#define TILE_FAC_CIRCLE     80
#define TILE_FAC_GATE       84
#define TILE_FAC_CASTLE     88
#define TILE_FAC_COLLECT    92
#define TILE_FAC_DUNGEON    96

/* 施設タイルを生成して登録する */
void view_tiles_define_facilities(void);

/* 起動時に古地図パレットをハードへ流し込む */
void view_tiles_set_palette(KernelAPI *api);

/* 地形タイルを生成して libos32tilemap に登録する。
   tilemap_init() のあとに1回だけ呼ぶ */
void view_tiles_define(void);

#endif /* VIEW_TILES_H */
