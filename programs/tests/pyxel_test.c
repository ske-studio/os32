/* ======================================================================== */
/*  PYXEL_TEST — libpyxel インフラ動作検証デモ                              */
/*                                                                          */
/*  Pyxel互換の描画基盤 (パレット, 2倍座標, プリミティブ, ポーリング入力,    */
/*  ゲームループ, dirty rect転送) が正しく動作するか視覚的に確認する。       */
/*                                                                          */
/*  操作:                                                                   */
/*    矢印キー: カーソル移動 (Pyxel座標系 256x192)                          */
/*    Zキー:    描画色切替                                                  */
/*    Xキー:    16色パレット表示トグル                                      */
/*    SPACE:    SE再生テスト                                                 */
/*    ESCキー:  終了                                                        */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

static KernelAPI *kapi = NULL;

/* ======================================================================== */
/*  Pyxel互換定数 (04_API_MAPPING.md §2, §3 準拠)                          */
/* ======================================================================== */

/* 画面サイズ (06_IMPLEMENTATION_DETAILS.md §2-1) */
#define PX_WIDTH     256
#define PX_HEIGHT    192
#define PX_SCALE     2
#define PX_DISP_W    (PX_WIDTH * PX_SCALE)     /* 512 */
#define PX_DISP_H    (PX_HEIGHT * PX_SCALE)    /* 384 */

/* UI領域 (03_SCALING.md §1) */
#define UI_X         512
#define UI_Y         0
#define UI_W         128
#define UI_H         384
#define STATUS_Y     384
#define STATUS_H     16

/* キースキャンコード (04_API_MAPPING.md §2) */
#define KEY_ESCAPE   0x00
#define KEY_UP       0x3A
#define KEY_DOWN     0x3D
#define KEY_LEFT     0x3B
#define KEY_RIGHT    0x3C
#define KEY_SPACE    0x34
#define KEY_Z        0x2A
#define KEY_X        0x2B

/* Pyxel 16色パレット → PC-98 4bit RGB (04_API_MAPPING.md §3-1) */
/* pc98_val = (pyxel_val * 15 + 127) / 255 */
static const u8 pyxel_palette[16][3] = {
    { 0,  0,  0},    /* 0  黒 */
    { 3,  3,  6},    /* 1  紺 */
    { 7,  2,  7},    /* 2  紫 */
    { 1,  9,  9},    /* 3  青緑 */
    { 8,  4,  5},    /* 4  茶 */
    { 3,  5,  9},    /* 5  暗青 */
    {10, 11, 15},    /* 6  薄青 */
    {14, 14, 14},    /* 7  白 */
    {12,  1,  6},    /* 8  赤 */
    {12,  8,  4},    /* 9  橙 */
    {14, 11,  5},    /* 10 黄 */
    { 7, 12, 10},    /* 11 薄緑 */
    { 7,  9, 13},    /* 12 シアン */
    {10, 10, 10},    /* 13 灰 */
    {15,  9,  9},    /* 14 ピンク */
    {14, 12, 10}     /* 15 肌色 */
};

/* ======================================================================== */
/*  Pyxelエミュレーション関数群                                              */
/* ======================================================================== */

/* Pyxelパレットを設定 */
static void px_set_palette(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        kapi->gfx_set_palette(i,
            pyxel_palette[i][0],
            pyxel_palette[i][1],
            pyxel_palette[i][2]);
    }
}

/* Pyxel座標 → PC-98実座標変換 pset */
static void px_pset(int x, int y, u8 col)
{
    if (x < 0 || x >= PX_WIDTH || y < 0 || y >= PX_HEIGHT) return;
    gfx_fill_rect(x * PX_SCALE, y * PX_SCALE, PX_SCALE, PX_SCALE, col);
}

/* Pyxel座標系の矩形 (塗りつぶし) */
static void px_rect(int x, int y, int w, int h, u8 col)
{
    gfx_fill_rect(x * PX_SCALE, y * PX_SCALE,
                  w * PX_SCALE, h * PX_SCALE, col);
}

/* Pyxel座標系の矩形 (枠のみ) */
static void px_rectb(int x, int y, int w, int h, u8 col)
{
    gfx_rect(x * PX_SCALE, y * PX_SCALE,
             w * PX_SCALE, h * PX_SCALE, col);
}

/* Pyxel座標系の線 */
static void px_line(int x0, int y0, int x1, int y1, u8 col)
{
    gfx_line(x0 * PX_SCALE, y0 * PX_SCALE,
             x1 * PX_SCALE, y1 * PX_SCALE, col);
}

/* Pyxel座標系の円 (塗りつぶし) */
static void px_circ(int cx, int cy, int r, u8 col)
{
    gfx_fill_circle(cx * PX_SCALE, cy * PX_SCALE, r * PX_SCALE, col);
}

/* Pyxel座標系の円 (枠のみ) */
static void px_circb(int cx, int cy, int r, u8 col)
{
    gfx_circle(cx * PX_SCALE, cy * PX_SCALE, r * PX_SCALE, col);
}

/* ゲーム領域クリア */
static void px_cls(u8 col)
{
    gfx_fill_rect(0, 0, PX_DISP_W, PX_DISP_H, col);
}

/* ======================================================================== */
/*  ゲーム状態                                                              */
/* ======================================================================== */

static int cursor_x = PX_WIDTH / 2;
static int cursor_y = PX_HEIGHT / 2;
static u8  draw_color = 10;  /* 黄色 */
static int show_palette = 0;
static int frame_count = 0;
static int fps = 0;
static int fps_frames = 0;
static u32 fps_tick = 0;

/* 描画軌跡バッファ: 簡易的なピクセルトレイル */
#define TRAIL_MAX 64
static int trail_x[TRAIL_MAX];
static int trail_y[TRAIL_MAX];
static u8  trail_col[TRAIL_MAX];
static int trail_count = 0;
static int trail_head = 0;

/* ======================================================================== */
/*  update() — ゲームロジック更新                                            */
/* ======================================================================== */
static void game_update(void)
{
    int speed = 2;

    /* 矢印キーでカーソル移動 (ポーリング方式: kbd_is_pressed) */
    if (kapi->kbd_is_pressed(KEY_UP)    && cursor_y > 0)
        cursor_y -= speed;
    if (kapi->kbd_is_pressed(KEY_DOWN)  && cursor_y < PX_HEIGHT - 1)
        cursor_y += speed;
    if (kapi->kbd_is_pressed(KEY_LEFT)  && cursor_x > 0)
        cursor_x -= speed;
    if (kapi->kbd_is_pressed(KEY_RIGHT) && cursor_x < PX_WIDTH - 1)
        cursor_x += speed;

    /* Zキー: 描画色切替 (フレーム差分で1回だけ) */
    if (kapi->kbd_is_pressed(KEY_Z)) {
        if (frame_count % 10 == 0) {
            draw_color = (draw_color + 1) % 16;
            if (draw_color == 0) draw_color = 1; /* 色0は背景色なので飛ばす */
        }
    }

    /* Xキー: パレット表示トグル */
    if (kapi->kbd_is_pressed(KEY_X)) {
        if (frame_count % 15 == 0) {
            show_palette = !show_palette;
        }
    }

    /* SPACE: SE再生 */
    if (kapi->kbd_is_pressed(KEY_SPACE)) {
        if (frame_count % 30 == 0) {
            kapi->snd_se_play_raw(36, 5, 1); /* C3, 50ms, 矩形波 */
        }
    }

    /* 軌跡を記録 */
    trail_x[trail_head] = cursor_x;
    trail_y[trail_head] = cursor_y;
    trail_col[trail_head] = draw_color;
    trail_head = (trail_head + 1) % TRAIL_MAX;
    if (trail_count < TRAIL_MAX) trail_count++;

    frame_count++;
}

/* ======================================================================== */
/*  draw() — 描画処理                                                       */
/* ======================================================================== */
static void game_draw(void)
{
    int i, idx;
    char buf[48];

    /* ゲーム領域クリア */
    px_cls(0);

    /* --- デモ描画: プリミティブのショーケース --- */

    /* 格子線 (暗色) */
    for (i = 0; i < PX_WIDTH; i += 32) {
        px_line(i, 0, i, PX_HEIGHT - 1, 1);
    }
    for (i = 0; i < PX_HEIGHT; i += 32) {
        px_line(0, i, PX_WIDTH - 1, i, 1);
    }

    /* 塗り矩形 */
    px_rect(10, 10, 30, 20, 8);   /* 赤 */
    px_rect(50, 10, 30, 20, 3);   /* 青緑 */

    /* 枠矩形 */
    px_rectb(100, 10, 30, 20, 10); /* 黄 */
    px_rectb(140, 10, 30, 20, 14); /* ピンク */

    /* 塗り円 */
    px_circ(40, 80, 15, 11);      /* 薄緑 */
    px_circ(100, 80, 20, 9);      /* 橙 */

    /* 枠円 */
    px_circb(170, 80, 12, 6);     /* 薄青 */
    px_circb(220, 80, 18, 12);    /* シアン */

    /* 斜め線 */
    px_line(0, 0, PX_WIDTH - 1, PX_HEIGHT - 1, 7);
    px_line(PX_WIDTH - 1, 0, 0, PX_HEIGHT - 1, 13);

    /* 16色パレット表示 (Xキーでトグル) */
    if (show_palette) {
        int px, py;
        for (i = 0; i < 16; i++) {
            px = 80 + (i % 8) * 12;
            py = 120 + (i / 8) * 12;
            px_rect(px, py, 10, 10, (u8)i);
            px_rectb(px, py, 10, 10, 7);
        }
    }

    /* 移動軌跡の描画 */
    for (i = 0; i < trail_count; i++) {
        idx = (trail_head - trail_count + i + TRAIL_MAX) % TRAIL_MAX;
        /* 古いほど暗く */
        if (i > trail_count - 10) {
            px_pset(trail_x[idx], trail_y[idx], trail_col[idx]);
        } else {
            px_pset(trail_x[idx], trail_y[idx], 1); /* 暗い色 */
        }
    }

    /* カーソル (十字マーク) */
    px_pset(cursor_x, cursor_y, draw_color);
    if (cursor_x > 0)            px_pset(cursor_x - 1, cursor_y, draw_color);
    if (cursor_x < PX_WIDTH - 1) px_pset(cursor_x + 1, cursor_y, draw_color);
    if (cursor_y > 0)            px_pset(cursor_x, cursor_y - 1, draw_color);
    if (cursor_y < PX_HEIGHT - 1)px_pset(cursor_x, cursor_y + 1, draw_color);

    /* ゲーム領域はdirtyとして登録 */
    kapi->gfx_add_dirty_rect(0, 0, PX_DISP_W, PX_DISP_H);

    /* --- UI領域 (右側 128px) --- */
    gfx_fill_rect(UI_X, UI_Y, UI_W, UI_H, 0);

    /* タイトル */
    kcg_draw_utf8(UI_X + 4, 4, "PYXEL TEST", 7, 0);

    /* FPS表示 */
    sprintf(buf, "FPS: %d", fps);
    kcg_draw_utf8(UI_X + 4, 24, buf, 10, 0);

    /* カーソル座標 */
    sprintf(buf, "X:%3d Y:%3d", cursor_x, cursor_y);
    kcg_draw_utf8(UI_X + 4, 44, buf, 6, 0);

    /* 描画色表示 */
    sprintf(buf, "COL: %d", draw_color);
    kcg_draw_utf8(UI_X + 4, 64, buf, draw_color, 0);
    gfx_fill_rect(UI_X + 90, 64, 16, 16, draw_color);

    /* フレームカウント */
    sprintf(buf, "F:%d", frame_count);
    kcg_draw_utf8(UI_X + 4, 84, buf, 13, 0);

    /* 操作ガイド */
    kcg_draw_utf8(UI_X + 4, 120, "Arrow:Move", 7, 0);
    kcg_draw_utf8(UI_X + 4, 136, "Z:Color", 7, 0);
    kcg_draw_utf8(UI_X + 4, 152, "X:Palette", 7, 0);
    kcg_draw_utf8(UI_X + 4, 168, "SPC:SE", 7, 0);
    kcg_draw_utf8(UI_X + 4, 184, "ESC:Quit", 7, 0);

    /* 16色パレットバー */
    for (i = 0; i < 16; i++) {
        gfx_fill_rect(UI_X + 4 + i * 7, 220, 6, 12, (u8)i);
    }

    kapi->gfx_add_dirty_rect(UI_X, UI_Y, UI_W, UI_H);

    /* --- ステータスバー (下部 16px) --- */
    gfx_fill_rect(0, STATUS_Y, 640, STATUS_H, 0);
    kcg_draw_utf8(4, STATUS_Y + 2,
                  "libpyxel Infrastructure Test - Phase 0 Verification",
                  13, 0);
    kapi->gfx_add_dirty_rect(0, STATUS_Y, 640, STATUS_H);
}

/* ======================================================================== */
/*  メインエントリ — Pyxel互換ゲームループ                                  */
/*  04_API_MAPPING.md §5 のゲームループ構造を模擬                           */
/* ======================================================================== */

int main(int argc, char *argv[], KernelAPI *api)
{
    u32 last_tick;

    if (!api) return -1;
    kapi = api;

    kapi->kprintf(ATTR_WHITE, "Starting pyxel_test...\r\n");

    /* GFX初期化 */
    libos32gfx_init(kapi);
    kapi->kcg_init();
    kcg_set_scale(1);

    /* Pyxel 16色パレット設定 (04_API_MAPPING.md §3) */
    px_set_palette();

    /* 画面クリア + 全画面転送 */
    gfx_clear(0);
    gfx_present();

    /* FPS計測初期化 */
    fps_tick = kapi->get_tick();
    last_tick = fps_tick;

    /* ゲームループ (04_API_MAPPING.md §5 準拠) */
    while (1) {
        /* ESCキーで終了 */
        if (kapi->kbd_is_pressed(KEY_ESCAPE)) break;

        /* ユーザーのupdate関数呼び出し */
        game_update();

        /* ユーザーのdraw関数呼び出し */
        game_draw();

        /* フレームカウンタ更新 */
        fps_frames++;

        /* VRAM転送 (dirty rectのみ) */
        kapi->gfx_present_dirty();

        /* FPS計測 (1秒ごと) */
        if (kapi->get_tick() - fps_tick >= 100) {
            fps = fps_frames;
            fps_frames = 0;
            fps_tick = kapi->get_tick();
        }

        /* フレームレート制御 (VSYNC同期) */
        while (kapi->get_tick() == last_tick) {
            kapi->sys_halt();
        }
        last_tick = kapi->get_tick();
    }

    /* クリーンアップ */
    gfx_clear(0);
    gfx_present();
    libos32gfx_shutdown();
    kapi->tvram_clear();

    kapi->kprintf(ATTR_GREEN, "pyxel_test finished. Frames: %d\r\n",
                  frame_count);

    return 0;
}
