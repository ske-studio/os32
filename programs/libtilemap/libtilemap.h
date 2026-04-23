#ifndef __LIBTILEMAP_H
#define __LIBTILEMAP_H

#include "os32api.h"
#include "libos32gfx.h"

/* 定数 */
#define TILE_W          16
#define TILE_H          16
#define TILE_PITCH      2       /* 16px / 8 = 2バイト/行/プレーン */
#define TILE_PLANE_SZ   32      /* 2 * 16 = 32バイト/プレーン */
#define TILE_TOTAL_SZ   128     /* 32 * 4 = 128バイト/タイル */
#define MAX_TILES       1024

#define TILEMAP_COLS    24
#define TILEMAP_ROWS    24
#define BG_COUNT        4

/* タイル不透明度 (事前計算) */
#define TILE_TRANSPARENT 0   /* 全ピクセルが色0 → 完全スキップ */
#define TILE_PARTIAL     1   /* 一部が色0 → colorkey付きblit */
#define TILE_OPAQUE      2   /* 色0なし → 無条件blit */

/* タイル属性ビット (16bit)
 * bit 15   : 水平反転 (Phase 3用)
 * bit 14   : 垂直反転 (Phase 3用)
 * bit 13-10: 予約
 * bit 9-0  : タイルID (0-1023)
 */
#define TILEMAP_ID(v)     ((v) & 0x3FF)
#define TILEMAP_HFLIP(v)  ((v) & 0x8000)
#define TILEMAP_VFLIP(v)  ((v) & 0x4000)

/* タイル定義 */
typedef struct {
    u8 planes[4][TILE_PLANE_SZ];  /* プレーン別 (gfx_blit互換) */
    u8 opacity;                   /* 事前計算済み不透明度 */
} TileDef;

/* BGプレーン */
typedef struct {
    u16 map[TILEMAP_ROWS][TILEMAP_COLS]; /* タイル属性 (0-1023) */
    u8  dirty[TILEMAP_ROWS][TILEMAP_COLS]; /* ダーティフラグ */
    i16 scroll_x;     /* ピクセル単位スクロール (Phase 3用) */
    i16 scroll_y;     /* ピクセル単位スクロール (Phase 3用) */
    u8  visible;      /* 表示ON/OFF */
} BGPlane;

/* ===== 初期化・終了 ===== */
void tilemap_init(KernelAPI *api);
void tilemap_shutdown(void);

/* ===== タイル定義 ===== */
/* 4bppパックドデータからタイルを登録 (不透明度も自動計算) */
void tilemap_define(int id, const u8 *data_4bpp);
/* ファイルからタイルセット一括ロード (Phase 3) */
int  tilemap_load(const char *path, int start_id);

/* ===== BGプレーン操作 ===== */
void tilemap_set(int bg, int col, int row, u16 tile_attr);
u16  tilemap_get(int bg, int col, int row);
void tilemap_fill(int bg, u16 tile_attr);
void tilemap_set_visible(int bg, int visible);
void tilemap_scroll(int bg, int sx, int sy); /* Phase 3 */

/* ===== 合成 ===== */
void tilemap_compose_btf(void);  /* Back-to-Front */
void tilemap_compose_ftb(void);  /* Front-to-Back (Phase 2) */

/* ===== 表示 ===== */
/* dirty rectのみVRAM転送 (gfx_present_dirty呼び出し) */
void tilemap_present(void);

/* ===== 画面配置 ===== */
/* 描画オフセット (32px単位、デフォルト0,0) */
void tilemap_set_origin(int ox, int oy);

/* ===== パレット ===== */
void tilemap_set_palette(const u8 pal[16][3]); /* Phase 3 */

#endif /* __LIBTILEMAP_H */
