/* ======================================================================== */
/*  PYXEL_TEST — libpyxel Phase 2 包括テスト                                */
/*                                                                          */
/*  libpyxel の全APIを視覚的に検証するデモプログラム。                       */
/*  複数のテスト画面を用意し、キーで切り替えて確認する。                     */
/*                                                                          */
/*  テスト画面:                                                              */
/*    1: プリミティブ描画 (rect, rectb, circ, circb, tri, trib, line)       */
/*    2: 入力テスト (btn, btnp, btnr の動作確認)                            */
/*    3: パレットスワップ (pyxel_pal / pyxel_pal_reset)                     */
/*    4: カメラ・クリッピング (pyxel_camera / pyxel_clip)                   */
/*                                                                          */
/*  操作:                                                                   */
/*    1-4:    テスト画面切替                                                */
/*    矢印:  カーソル移動 / カメラ移動                                      */
/*    Z:     アクション (色切替 / パレットスワップ)                          */
/*    X:     サブ機能トグル                                                  */
/*    SPACE: SE再生テスト                                                    */
/*    ESC:   終了                                                           */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "pyxel.h"
#include "libos32gfx.h"

static KernelAPI *kapi = NULL;

/* ======================================================================== */
/*  共通状態                                                                 */
/* ======================================================================== */

static int test_page = 0;          /* 現在のテスト画面 (0-3) */
static int cursor_x, cursor_y;    /* カーソル位置 */
static u8  draw_color = 10;

/* UI領域テキスト描画ヘルパー */
static int ui_line;
static void ui_reset(void) { ui_line = 4; }
static void ui_text(const char *s, u8 col)
{
    kcg_draw_utf8(PYXEL_UI_X + 4, ui_line, s, col, 0);
    ui_line += 16;
}
static void ui_text_f(u8 col, const char *fmt, int val)
{
    char buf[48];
    sprintf(buf, fmt, val);
    ui_text(buf, col);
}

/* ======================================================================== */
/*  テスト1: プリミティブ描画                                                */
/* ======================================================================== */

static void test1_update(void)
{
    int speed = 2;
    if (pyxel_btn(PYXEL_KEY_UP)    && cursor_y > 0)        cursor_y -= speed;
    if (pyxel_btn(PYXEL_KEY_DOWN)  && cursor_y < PYXEL_HEIGHT - 1) cursor_y += speed;
    if (pyxel_btn(PYXEL_KEY_LEFT)  && cursor_x > 0)        cursor_x -= speed;
    if (pyxel_btn(PYXEL_KEY_RIGHT) && cursor_x < PYXEL_WIDTH - 1)  cursor_x += speed;

    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        draw_color = (draw_color + 1) % 16;
        if (draw_color == 0) draw_color = 1;
    }
}

static void test1_draw(void)
{
    int i;

    pyxel_cls(0);

    /* グリッド */
    for (i = 0; i < PYXEL_WIDTH; i += 32)
        pyxel_line(i, 0, i, PYXEL_HEIGHT - 1, 1);
    for (i = 0; i < PYXEL_HEIGHT; i += 32)
        pyxel_line(0, i, PYXEL_WIDTH - 1, i, 1);

    /* 塗りつぶし矩形 */
    pyxel_rect(8, 8, 28, 18, 8);
    pyxel_text(10, 30, "rect", 7);

    /* 枠矩形 */
    pyxel_rectb(48, 8, 28, 18, 10);
    pyxel_text(50, 30, "rectb", 7);

    /* 塗り円 */
    pyxel_circ(110, 20, 14, 11);
    pyxel_text(98, 38, "circ", 7);

    /* 枠円 */
    pyxel_circb(155, 20, 14, 6);
    pyxel_text(143, 38, "circb", 7);

    /* 塗り三角形 */
    pyxel_tri(200, 8, 230, 35, 185, 30, 14);
    pyxel_text(195, 38, "tri", 7);

    /* 枠三角形 */
    pyxel_trib(240, 8, 255, 35, 225, 30, 9);
    pyxel_text(230, 38, "trib", 7);

    /* 線 */
    pyxel_line(8, 56, 120, 80, 3);
    pyxel_line(8, 80, 120, 56, 12);
    pyxel_text(8, 84, "line", 7);

    /* テキスト表示テスト */
    pyxel_text(8, 100, "pyxel_text test!", 7);
    pyxel_text(8, 116, "ABCDEFGHIJ 0123", 6);

    /* pset でドット描画 (小さい星型) */
    for (i = 0; i < 16; i++) {
        pyxel_pset(140 + i * 4, 60, (u8)(i % 16));
    }
    pyxel_text(140, 68, "pset 16 colors", 7);

    /* カーソル */
    pyxel_rect(cursor_x - 1, cursor_y - 1, 3, 3, draw_color);

    /* UI */
    ui_reset();
    ui_text("TEST 1", 7);
    ui_text("Primitives", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_text_f(13, "X:%d Y:%d", cursor_x);
    ui_text_f(draw_color, "COL: %d", draw_color);
    ui_line += 8;
    ui_text("Arrow:Move", 7);
    ui_text("Z:Color", 7);
    ui_text("1-4:Page", 7);
    ui_text("ESC:Quit", 7);
}

/* ======================================================================== */
/*  テスト2: 入力テスト (btn / btnp / btnr)                                  */
/* ======================================================================== */

static int btnp_count = 0;
static int btnr_count = 0;
static int btn_held = 0;
static int last_btnp_key = -1;
static int last_btnr_key = -1;
static int repeat_count = 0;

static void test2_update(void)
{
    /* btnp テスト (Zキー: 即時トリガー) */
    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        btnp_count++;
        last_btnp_key = PYXEL_KEY_Z;
    }
    /* btnp リピートテスト (Xキー: 15フレームhold, 5フレームrepeat) */
    if (pyxel_btnp(PYXEL_KEY_X, 15, 5)) {
        repeat_count++;
    }
    /* btnr テスト (SPACEキー: リリース検出) */
    if (pyxel_btnr(PYXEL_KEY_SPACE)) {
        btnr_count++;
        last_btnr_key = PYXEL_KEY_SPACE;
    }
    /* btn テスト (上キー: 押下中カウント) */
    if (pyxel_btn(PYXEL_KEY_UP)) {
        btn_held++;
    }
    /* 矢印キーでカーソル移動 */
    if (pyxel_btn(PYXEL_KEY_LEFT)  && cursor_x > 0)        cursor_x -= 2;
    if (pyxel_btn(PYXEL_KEY_RIGHT) && cursor_x < PYXEL_WIDTH - 1)  cursor_x += 2;
    if (pyxel_btn(PYXEL_KEY_DOWN)  && cursor_y < PYXEL_HEIGHT - 1) cursor_y += 2;
    if (pyxel_btn(PYXEL_KEY_UP)    && cursor_y > 0)        cursor_y -= 2;
}

static void test2_draw(void)
{
    int y;
    char buf[64];

    pyxel_cls(0);

    y = 8;
    pyxel_text(8, y, "=== INPUT TEST ===", 7);
    y += 20;

    /* btn 状態表示 */
    sprintf(buf, "btn(UP) held: %d frames", btn_held);
    pyxel_text(8, y, buf, pyxel_btn(PYXEL_KEY_UP) ? 10 : 13);
    y += 14;

    /* btnp 状態表示 */
    sprintf(buf, "btnp(Z) count: %d", btnp_count);
    pyxel_text(8, y, buf, pyxel_btnp(PYXEL_KEY_Z, 0, 0) ? 10 : 6);
    y += 14;

    /* btnp リピート */
    sprintf(buf, "btnp(X,15,5) rpt: %d", repeat_count);
    pyxel_text(8, y, buf, pyxel_btn(PYXEL_KEY_X) ? 14 : 6);
    y += 14;

    /* btnr 状態表示 */
    sprintf(buf, "btnr(SPC) count: %d", btnr_count);
    pyxel_text(8, y, buf, 12);
    y += 20;

    /* 現在のキー状態をビジュアル表示 */
    pyxel_text(8, y, "Key state:", 7);
    y += 14;

    /* 矢印キー表示 */
    pyxel_rectb(48, y, 12, 12, 13);
    pyxel_rectb(32, y + 14, 12, 12, 13);
    pyxel_rectb(48, y + 14, 12, 12, 13);
    pyxel_rectb(64, y + 14, 12, 12, 13);

    if (pyxel_btn(PYXEL_KEY_UP))    pyxel_rect(49, y + 1, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_LEFT))  pyxel_rect(33, y + 15, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_DOWN))  pyxel_rect(49, y + 15, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_RIGHT)) pyxel_rect(65, y + 15, 10, 10, 10);

    /* Z/X/SPACE */
    pyxel_rectb(100, y, 14, 12, 13);
    pyxel_rectb(118, y, 14, 12, 13);
    pyxel_rectb(100, y + 14, 32, 12, 13);
    pyxel_text(103, y + 2, "Z", 7);
    pyxel_text(121, y + 2, "X", 7);
    pyxel_text(104, y + 16, "SPC", 7);

    if (pyxel_btn(PYXEL_KEY_Z))     pyxel_rect(101, y + 1, 12, 10, 8);
    if (pyxel_btn(PYXEL_KEY_X))     pyxel_rect(119, y + 1, 12, 10, 8);
    if (pyxel_btn(PYXEL_KEY_SPACE)) pyxel_rect(101, y + 15, 30, 10, 8);

    /* カーソル */
    pyxel_circ(cursor_x, cursor_y, 3, draw_color);

    /* UI */
    ui_reset();
    ui_text("TEST 2", 7);
    ui_text("Input", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text("Z:btnp", 7);
    ui_text("X:repeat", 7);
    ui_text("SPC:btnr", 7);
    ui_text("UP:held", 7);
}

/* ======================================================================== */
/*  テスト3: パレットスワップ                                                */
/* ======================================================================== */

static int pal_swapped = 0;

static void test3_update(void)
{
    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        pal_swapped = !pal_swapped;
        if (pal_swapped) {
            /* 赤(8) ↔ 青緑(3), 黄(10) ↔ ピンク(14) */
            pyxel_pal(8, 3);
            pyxel_pal(3, 8);
            pyxel_pal(10, 14);
            pyxel_pal(14, 10);
        } else {
            pyxel_pal_reset();
        }
    }
}

static void test3_draw(void)
{
    int i, px, py;

    pyxel_cls(0);

    pyxel_text(8, 8, "=== PALETTE SWAP ===", 7);
    pyxel_text(8, 26, pal_swapped ? "SWAPPED (Z to reset)" : "DEFAULT (Z to swap)", 7);

    /* 16色パレット表示 */
    for (i = 0; i < 16; i++) {
        px = 16 + (i % 8) * 28;
        py = 50 + (i / 8) * 36;
        pyxel_rect(px, py, 24, 24, i);
        pyxel_rectb(px, py, 24, 24, 7);
    }

    /* カラーバリエーション矩形 */
    pyxel_rect(16,  130, 50, 20, 8);    /* 赤 (スワップ対象) */
    pyxel_text(20, 132, "RED=8", 7);

    pyxel_rect(80,  130, 50, 20, 3);    /* 青緑 (スワップ対象) */
    pyxel_text(84, 132, "GRN=3", 7);

    pyxel_rect(144, 130, 50, 20, 10);   /* 黄 (スワップ対象) */
    pyxel_text(148, 132, "YLW=10", 7);

    pyxel_rect(16,  160, 50, 20, 14);   /* ピンク (スワップ対象) */
    pyxel_text(20, 162, "PNK=14", 7);

    /* UI */
    ui_reset();
    ui_text("TEST 3", 7);
    ui_text("Palette", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text(pal_swapped ? "SWAPPED" : "DEFAULT", pal_swapped ? 8 : 11);
    ui_text("Z:Toggle", 7);
}

/* ======================================================================== */
/*  テスト4: カメラ・クリッピング                                            */
/* ======================================================================== */

static int cam_x = 0, cam_y = 0;
static int clip_on = 0;

static void test4_update(void)
{
    /* 矢印キーでカメラ移動 */
    if (pyxel_btn(PYXEL_KEY_UP))    cam_y -= 2;
    if (pyxel_btn(PYXEL_KEY_DOWN))  cam_y += 2;
    if (pyxel_btn(PYXEL_KEY_LEFT))  cam_x -= 2;
    if (pyxel_btn(PYXEL_KEY_RIGHT)) cam_x += 2;

    /* Zキーでクリッピング切替 */
    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        clip_on = !clip_on;
        if (clip_on) {
            pyxel_clip(64, 48, 128, 96);
        } else {
            pyxel_clip_reset();
        }
    }

    /* Xキーでカメラリセット */
    if (pyxel_btnp(PYXEL_KEY_X, 0, 0)) {
        cam_x = 0;
        cam_y = 0;
    }

    pyxel_camera(cam_x, cam_y);
}

static void test4_draw(void)
{
    int i;
    char buf[48];

    pyxel_cls(0);

    /* グリッド (カメラ移動で動く) */
    for (i = 0; i < 512; i += 32) {
        pyxel_line(i, 0, i, 384, 1);
    }
    for (i = 0; i < 384; i += 32) {
        pyxel_line(0, i, 512, i, 1);
    }

    /* 固定位置のオブジェクト (カメラの影響を受ける) */
    pyxel_rect(40, 40, 30, 20, 8);
    pyxel_text(42, 44, "A", 7);

    pyxel_circ(128, 96, 20, 11);
    pyxel_text(120, 92, "B", 7);

    pyxel_tri(200, 50, 230, 90, 180, 80, 9);
    pyxel_text(198, 68, "C", 7);

    pyxel_rectb(60, 100, 60, 40, 6);
    pyxel_text(62, 104, "D", 7);

    /* クリッピング領域の可視化 (カメラリセットして描画) */
    pyxel_camera(0, 0);
    if (clip_on) {
        pyxel_rectb(64, 48, 128, 96, 14);
        pyxel_text(66, 50, "CLIP", 14);
    }

    /* カメラ情報テキスト */
    sprintf(buf, "CAM:(%d,%d)", cam_x, cam_y);
    pyxel_text(4, 4, buf, 7);

    /* カメラを元に戻す */
    pyxel_camera(cam_x, cam_y);

    /* UI */
    pyxel_camera(0, 0); /* UI描画はカメラなし */
    ui_reset();
    ui_text("TEST 4", 7);
    ui_text("Camera", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_text_f(13, "CX:%d", cam_x);
    ui_text_f(13, "CY:%d", cam_y);
    ui_text(clip_on ? "CLIP:ON" : "CLIP:OFF", clip_on ? 14 : 13);
    ui_line += 8;
    ui_text("Arrow:Cam", 7);
    ui_text("Z:Clip", 7);
    ui_text("X:Reset", 7);
    pyxel_camera(cam_x, cam_y); /* カメラを戻す */
}

/* ======================================================================== */
/*  メインのupdate/draw — テスト画面をディスパッチ                           */
/* ======================================================================== */

static void update(void)
{
    /* Xキーでページ送り (pyxel_btnpエッジ検出) */
    if (pyxel_btnp(PYXEL_KEY_X, 0, 0)) {
        test_page = (test_page + 1) % 4;
        pyxel_pal_reset();
        pyxel_camera(0, 0);
        pyxel_clip_reset();
        cam_x = 0;
        cam_y = 0;
        clip_on = 0;
        pal_swapped = 0;
    }

    /* SPACE: SE再生 */
    if (pyxel_btnp(PYXEL_KEY_SPACE, 0, 0) && test_page != 1) {
        if (kapi) kapi->snd_se_play_raw(36, 5, 1);
    }

    /* テスト画面ごとのupdate */
    switch (test_page) {
    case 0: test1_update(); break;
    case 1: test2_update(); break;
    case 2: test3_update(); break;
    case 3: test4_update(); break;
    }
}

static void draw(void)
{
    int i;

    /* === UI領域のクリア (テスト画面描画の前に行う) === */
    gfx_fill_rect(PYXEL_UI_X, PYXEL_UI_Y, PYXEL_UI_W, PYXEL_UI_H, 0);

    /* テスト画面ごとのdraw (ゲーム領域 + UI領域テキスト) */
    switch (test_page) {
    case 0: test1_draw(); break;
    case 1: test2_draw(); break;
    case 2: test3_draw(); break;
    case 3: test4_draw(); break;
    }

    /* === UI領域の共通部分 === */

    /* 16色パレットバー (UI領域下部) */
    for (i = 0; i < 16; i++) {
        gfx_fill_rect(PYXEL_UI_X + 4 + i * 7, 340, 6, 12, (u8)i);
    }

    /* ページインジケーター */
    for (i = 0; i < 4; i++) {
        gfx_fill_rect(PYXEL_UI_X + 20 + i * 24, 360, 18, 14,
                       (u8)(i == test_page ? 10 : 1));
        {
            char num[2];
            num[0] = '1' + i;
            num[1] = '\0';
            kcg_draw_utf8(PYXEL_UI_X + 25 + i * 24, 362, num,
                          (u8)(i == test_page ? 0 : 7), (u8)(i == test_page ? 10 : 1));
        }
    }

    kapi->gfx_add_dirty_rect(PYXEL_UI_X, PYXEL_UI_Y, PYXEL_UI_W, PYXEL_UI_H);

    /* === ステータスバー === */
    gfx_fill_rect(0, PYXEL_STATUS_Y, 640, PYXEL_STATUS_H, 0);
    {
        char status[64];
        sprintf(status, "libpyxel Phase 2 Test [Page %d/4]  F:%d",
                test_page + 1, pyxel_frame_count);
        kcg_draw_utf8(4, PYXEL_STATUS_Y + 2, status, 13, 0);
    }
    kapi->gfx_add_dirty_rect(0, PYXEL_STATUS_Y, 640, PYXEL_STATUS_H);
}

/* ======================================================================== */
/*  main                                                                     */
/* ======================================================================== */

int main(int argc, char *argv[], KernelAPI *api)
{
    (void)argc; (void)argv;

    if (!api) return -1;
    kapi = api;

    cursor_x = PYXEL_WIDTH / 2;
    cursor_y = PYXEL_HEIGHT / 2;

    pyxel_init(PYXEL_WIDTH, PYXEL_HEIGHT, api);
    pyxel_run(update, draw);
    pyxel_quit();

    return 0;
}
