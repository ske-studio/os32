/* ======================================================================== */
/*  PYXEL_CORE.C — libpyxel コアシステム (init / run / quit)                */
/*                                                                          */
/*  04_API_MAPPING.md §5 のゲームループ構造に準拠。                         */
/* ======================================================================== */

#include <string.h>
#include "pyxel_internal.h"

/* ======================================================================== */
/*  グローバル変数                                                           */
/* ======================================================================== */

unsigned int pyxel_frame_count = 0;
int pyxel_fps = 0;

/* 内部状態 */
PyxelState _pyxel;

/* ======================================================================== */
/*  pyxel_init — 初期化                                                      */
/* ======================================================================== */

void pyxel_init(int width, int height, KernelAPI *api)
{
    int i;

    if (!api) return;

    /* 内部状態のゼロクリア */
    memset(&_pyxel, 0, sizeof(_pyxel));

    _pyxel.kapi = api;
    _pyxel.width = width;
    _pyxel.height = height;

    /* パレットスワップテーブル初期化 (1:1マッピング) */
    for (i = 0; i < PYXEL_COLORS; i++) {
        _pyxel.pal_map[i] = (u8)i;
    }

    /* クリッピングを全画面にリセット */
    _pyxel.clip_x = 0;
    _pyxel.clip_y = 0;
    _pyxel.clip_w = width;
    _pyxel.clip_h = height;
    _pyxel.clip_enabled = 0;

    /* GFX初期化 */
    libos32gfx_init(api);
    api->kcg_init();
    kcg_set_scale(1);

    /* Pyxel 16色パレット設定 (04_API_MAPPING.md §3-1) */
    for (i = 0; i < PYXEL_COLORS; i++) {
        api->gfx_set_palette(i,
            pyxel_default_palette[i][0],
            pyxel_default_palette[i][1],
            pyxel_default_palette[i][2]);
    }

    /* 画面クリア + 全画面転送 */
    gfx_clear(0);
    gfx_present();

    /* フレームカウンタとFPS初期化 */
    pyxel_frame_count = 0;
    pyxel_fps = 0;
    _pyxel.fps_tick = api->get_tick();
    _pyxel.fps_frames = 0;

    api->kprintf(ATTR_WHITE, "libpyxel initialized (%dx%d)\r\n", width, height);
}

/* ======================================================================== */
/*  pyxel_run — メインループ                                                */
/* ======================================================================== */

void pyxel_run(void (*update)(void), void (*draw)(void))
{
    u32 last_tick;

    if (!_pyxel.kapi) return;

    last_tick = _pyxel.kapi->get_tick();

    for (;;) {
        /* 入力状態の更新 */
        _pyxel_update_input();

        /* ESCキーで終了 */
        if (pyxel_btn(PYXEL_KEY_ESCAPE)) break;

        /* ユーザーのupdate関数呼び出し */
        if (update) update();

        /* ユーザーのdraw関数呼び出し */
        if (draw) draw();

        /* フレームカウンタ更新 */
        pyxel_frame_count++;
        _pyxel.fps_frames++;

        /* VRAM転送 (dirty rectのみ) */
        _pyxel_present();

        /* FPS計測 (1秒 = 100 ticks ごと) */
        if (_pyxel.kapi->get_tick() - _pyxel.fps_tick >= 100) {
            pyxel_fps = _pyxel.fps_frames;
            _pyxel.fps_frames = 0;
            _pyxel.fps_tick = _pyxel.kapi->get_tick();
        }

        /* フレームレート制御 (VSYNC同期: 100Hz タイマー待ち) */
        while (_pyxel.kapi->get_tick() == last_tick) {
            _pyxel.kapi->sys_halt();
        }
        last_tick = _pyxel.kapi->get_tick();
    }
}

/* ======================================================================== */
/*  pyxel_quit — 終了処理                                                    */
/* ======================================================================== */

void pyxel_quit(void)
{
    if (!_pyxel.kapi) return;

    /* 画面クリア */
    gfx_clear(0);
    gfx_present();

    /* GFXシャットダウン */
    libos32gfx_shutdown();

    /* TVRAM復帰 (テキスト画面に戻す) */
    _pyxel.kapi->tvram_clear();

    _pyxel.kapi->kprintf(ATTR_GREEN,
        "libpyxel shutdown. Frames: %d\r\n", pyxel_frame_count);
}

/* ======================================================================== */
/*  _pyxel_present — VRAM転送 (dirty rectのみ)                              */
/* ======================================================================== */

void _pyxel_present(void)
{
    if (_pyxel.kapi) {
        _pyxel.kapi->gfx_present_dirty();
    }
}
