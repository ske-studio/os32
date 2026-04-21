/* ======================================================================== */
/*  PYXEL_TEST — libpyxel Phase 2 包括テスト + パフォーマンスベンチマーク    */
/*                                                                          */
/*  テスト画面:                                                              */
/*    1: プリミティブ描画 (rect, circ, tri, line, text, pset)              */
/*    2: 入力テスト (btn, btnp, btnr)                                      */
/*    3: パレットスワップ                                                    */
/*    4: カメラ・クリッピング                                                */
/*    5: アニメーションベンチマーク (cls有/無 切替)                          */
/*                                                                          */
/*  操作:                                                                   */
/*    X:     ページ送り                                                      */
/*    Z:     各テストのアクション                                            */
/*    矢印:  カーソル/カメラ移動                                             */
/*    SPACE: SE再生                                                          */
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

#define NUM_PAGES  7

static int test_page = 0;
static int cursor_x, cursor_y;
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
static void ui_text_f2(u8 col, const char *fmt, int v1, int v2)
{
    char buf[48];
    sprintf(buf, fmt, v1, v2);
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

    /* プリミティブ */
    pyxel_rect(8, 8, 28, 18, 8);
    pyxel_text(10, 30, "rect", 7);
    pyxel_rectb(48, 8, 28, 18, 10);
    pyxel_text(50, 30, "rectb", 7);
    pyxel_circ(110, 20, 14, 11);
    pyxel_text(98, 38, "circ", 7);
    pyxel_circb(155, 20, 14, 6);
    pyxel_text(143, 38, "circb", 7);
    pyxel_tri(200, 8, 230, 35, 185, 30, 14);
    pyxel_text(195, 38, "tri", 7);
    pyxel_trib(240, 8, 255, 35, 225, 30, 9);
    pyxel_text(230, 38, "trib", 7);
    pyxel_line(8, 56, 120, 80, 3);
    pyxel_line(8, 80, 120, 56, 12);
    pyxel_text(8, 84, "line", 7);
    pyxel_text(8, 100, "pyxel_text test!", 7);
    pyxel_text(8, 116, "ABCDEFGHIJ 0123", 6);
    for (i = 0; i < 16; i++)
        pyxel_pset(140 + i * 4, 60, (u8)(i % 16));
    pyxel_text(140, 68, "pset 16 colors", 7);

    /* カーソル */
    pyxel_rect(cursor_x - 1, cursor_y - 1, 3, 3, draw_color);

    /* UI */
    ui_reset();
    ui_text("TEST 1", 7);
    ui_text("Primitives", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_text_f2(13, "X:%d Y:%d", cursor_x, cursor_y);
    ui_text_f(draw_color, "COL: %d", draw_color);
    ui_line += 8;
    ui_text("Arrow:Move", 7);
    ui_text("Z:Color", 7);
    ui_text("X:Next Page", 7);
    ui_text("ESC:Quit", 7);
}

/* ======================================================================== */
/*  テスト2: 入力テスト                                                      */
/* ======================================================================== */

static int btnp_count = 0;
static int btnr_count = 0;
static int btn_held = 0;
static int repeat_count = 0;

static void test2_update(void)
{
    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0))       btnp_count++;
    if (pyxel_btnp(PYXEL_KEY_Z, 15, 5))      repeat_count++;
    if (pyxel_btnr(PYXEL_KEY_SPACE))          btnr_count++;
    if (pyxel_btn(PYXEL_KEY_UP))              btn_held++;

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
    pyxel_text(8, y, "=== INPUT TEST ===", 7); y += 20;
    sprintf(buf, "btn(UP) held: %d", btn_held);
    pyxel_text(8, y, buf, pyxel_btn(PYXEL_KEY_UP) ? 10 : 13); y += 14;
    sprintf(buf, "btnp(Z) count: %d", btnp_count);
    pyxel_text(8, y, buf, 6); y += 14;
    sprintf(buf, "btnp(Z,15,5) rpt: %d", repeat_count);
    pyxel_text(8, y, buf, 6); y += 14;
    sprintf(buf, "btnr(SPC) count: %d", btnr_count);
    pyxel_text(8, y, buf, 12); y += 20;

    /* キー状態ビジュアル */
    pyxel_text(8, y, "Key state:", 7); y += 14;
    pyxel_rectb(48, y, 12, 12, 13);
    pyxel_rectb(32, y + 14, 12, 12, 13);
    pyxel_rectb(48, y + 14, 12, 12, 13);
    pyxel_rectb(64, y + 14, 12, 12, 13);
    if (pyxel_btn(PYXEL_KEY_UP))    pyxel_rect(49, y + 1, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_LEFT))  pyxel_rect(33, y + 15, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_DOWN))  pyxel_rect(49, y + 15, 10, 10, 10);
    if (pyxel_btn(PYXEL_KEY_RIGHT)) pyxel_rect(65, y + 15, 10, 10, 10);
    pyxel_rectb(100, y, 14, 12, 13);
    pyxel_rectb(118, y, 14, 12, 13);
    pyxel_rectb(100, y + 14, 32, 12, 13);
    pyxel_text(103, y + 2, "Z", 7);
    pyxel_text(121, y + 2, "X", 7);
    pyxel_text(104, y + 16, "SPC", 7);
    if (pyxel_btn(PYXEL_KEY_Z))     pyxel_rect(101, y + 1, 12, 10, 8);
    if (pyxel_btn(PYXEL_KEY_X))     pyxel_rect(119, y + 1, 12, 10, 8);
    if (pyxel_btn(PYXEL_KEY_SPACE)) pyxel_rect(101, y + 15, 30, 10, 8);
    pyxel_circ(cursor_x, cursor_y, 3, draw_color);

    ui_reset();
    ui_text("TEST 2", 7);
    ui_text("Input", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text("Z:btnp", 7);
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
            pyxel_pal(8, 3); pyxel_pal(3, 8);
            pyxel_pal(10, 14); pyxel_pal(14, 10);
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

    for (i = 0; i < 16; i++) {
        px = 16 + (i % 8) * 28;
        py = 50 + (i / 8) * 36;
        pyxel_rect(px, py, 24, 24, i);
        pyxel_rectb(px, py, 24, 24, 7);
    }

    pyxel_rect(16, 130, 50, 20, 8);  pyxel_text(20, 132, "RED=8", 7);
    pyxel_rect(80, 130, 50, 20, 3);  pyxel_text(84, 132, "GRN=3", 7);
    pyxel_rect(144,130, 50, 20, 10); pyxel_text(148,132, "YLW=10", 7);
    pyxel_rect(16, 160, 50, 20, 14); pyxel_text(20, 162, "PNK=14", 7);

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
    if (pyxel_btn(PYXEL_KEY_UP))    cam_y -= 2;
    if (pyxel_btn(PYXEL_KEY_DOWN))  cam_y += 2;
    if (pyxel_btn(PYXEL_KEY_LEFT))  cam_x -= 2;
    if (pyxel_btn(PYXEL_KEY_RIGHT)) cam_x += 2;

    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        clip_on = !clip_on;
        if (clip_on) pyxel_clip(64, 48, 128, 96);
        else         pyxel_clip_reset();
    }
    pyxel_camera(cam_x, cam_y);
}

static void test4_draw(void)
{
    int i;

    pyxel_cls(0);

    for (i = 0; i < 512; i += 32)
        pyxel_line(i, 0, i, 384, 1);
    for (i = 0; i < 384; i += 32)
        pyxel_line(0, i, 512, i, 1);

    pyxel_rect(40, 40, 30, 20, 8);    pyxel_text(42, 44, "A", 7);
    pyxel_circ(128, 96, 20, 11);       pyxel_text(120, 92, "B", 7);
    pyxel_tri(200, 50, 230, 90, 180, 80, 9); pyxel_text(198, 68, "C", 7);
    pyxel_rectb(60, 100, 60, 40, 6);   pyxel_text(62, 104, "D", 7);

    pyxel_camera(0, 0);
    if (clip_on) {
        pyxel_rectb(64, 48, 128, 96, 14);
        pyxel_text(66, 50, "CLIP", 14);
    }
    {
        char buf[48];
        sprintf(buf, "CAM:(%d,%d)", cam_x, cam_y);
        pyxel_text(4, 4, buf, 7);
    }
    pyxel_camera(cam_x, cam_y);

    pyxel_camera(0, 0);
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
    pyxel_camera(cam_x, cam_y);
}

/* ======================================================================== */
/*  テスト5: アニメーションベンチマーク                                      */
/*                                                                          */
/*  cls有/無の2モードを切り替えて FPS差を比較する。                          */
/*  sprite overlay方式: 移動前位置を背景色で消し、新位置に描き直す。         */
/* ======================================================================== */

/* ボール数 */
#define BALL_COUNT 8

/* ボール状態 */
static struct {
    int x, y;       /* 現在位置 */
    int dx, dy;     /* 速度 */
    int r;          /* 半径 */
    int col;        /* 色 */
    int prev_x, prev_y;  /* 前フレーム位置 (overlay消去用) */
} balls[BALL_COUNT];

static int bench_use_cls = 1;      /* 1=cls方式, 0=overlay方式 */
static int bench_initialized = 0;
static int bench_fps_min = 999;
static int bench_fps_max = 0;
static int bench_frame_total = 0;
static int bench_fps_sum = 0;
static int bench_ui_dirty = 1;

static void test5_init(void)
{
    int i;
    /* ボール初期化 — 散らばった位置と異なる速度 */
    int init_data[BALL_COUNT][5] = {
        /* x,   y,  dx, dy, col */
        { 30,  30,  2,  1,  8  },
        { 80,  60,  1,  2,  3  },
        {150,  40, -2,  1, 10  },
        {200, 100,  1, -1, 11  },
        { 60, 130, -1,  2, 14  },
        {120,  90,  2, -2,  9  },
        {180, 150, -2, -1, 12  },
        { 40, 160,  1,  1,  6  }
    };

    for (i = 0; i < BALL_COUNT; i++) {
        balls[i].x  = init_data[i][0];
        balls[i].y  = init_data[i][1];
        balls[i].dx = init_data[i][2];
        balls[i].dy = init_data[i][3];
        balls[i].col = init_data[i][4];
        balls[i].r  = 6 + (i % 3) * 3;   /* 6, 9, 12 のいずれか */
        balls[i].prev_x = balls[i].x;
        balls[i].prev_y = balls[i].y;
    }
    bench_fps_min = 999;
    bench_fps_max = 0;
    bench_frame_total = 0;
    bench_fps_sum = 0;
    bench_initialized = 1;
}

static void test5_update(void)
{
    int i;

    if (!bench_initialized) test5_init();

    /* Zキー: cls/overlay切替 */
    if (pyxel_btnp(PYXEL_KEY_Z, 0, 0)) {
        bench_use_cls = !bench_use_cls;
        /* overlay切替時は画面をクリアして再初期化 */
        if (!bench_use_cls) {
            pyxel_cls(0);
        }
        bench_fps_min = 999;
        bench_fps_max = 0;
        bench_frame_total = 0;
        bench_fps_sum = 0;
    }

    /* ボール物理演算 */
    for (i = 0; i < BALL_COUNT; i++) {
        /* 前位置を保存 */
        balls[i].prev_x = balls[i].x;
        balls[i].prev_y = balls[i].y;

        /* 移動 */
        balls[i].x += balls[i].dx;
        balls[i].y += balls[i].dy;

        /* 壁反射 */
        if (balls[i].x - balls[i].r < 0) {
            balls[i].x = balls[i].r;
            balls[i].dx = -balls[i].dx;
        }
        if (balls[i].x + balls[i].r >= PYXEL_WIDTH) {
            balls[i].x = PYXEL_WIDTH - 1 - balls[i].r;
            balls[i].dx = -balls[i].dx;
        }
        if (balls[i].y - balls[i].r < 0) {
            balls[i].y = balls[i].r;
            balls[i].dy = -balls[i].dy;
        }
        if (balls[i].y + balls[i].r >= PYXEL_HEIGHT) {
            balls[i].y = PYXEL_HEIGHT - 1 - balls[i].r;
            balls[i].dy = -balls[i].dy;
        }
    }

    /* FPS統計 */
    if (pyxel_fps > 0 && pyxel_frame_count > 10) {
        bench_frame_total++;
        bench_fps_sum += pyxel_fps;
        if (pyxel_fps < bench_fps_min) bench_fps_min = pyxel_fps;
        if (pyxel_fps > bench_fps_max) bench_fps_max = pyxel_fps;
    }
}

static int bench_prev_fps = -1;   /* UI更新判定用 */

static void test5_draw(void)
{
    int i;

    if (bench_use_cls) {
        /* === cls 方式: 毎フレーム全画面クリア === */
        pyxel_cls(0);

        /* ボール描画 */
        for (i = 0; i < BALL_COUNT; i++) {
            pyxel_circ(balls[i].x, balls[i].y, balls[i].r, balls[i].col);
            pyxel_circb(balls[i].x, balls[i].y, balls[i].r, 7);
        }

        pyxel_text(4, 4, "BENCHMARK: cls() mode", 7);
        pyxel_text(4, 18, "Z to switch overlay", 13);

    } else {
        /* === overlay 方式: ボールのバウンディングBoxだけ消去+再描画 === */

        /* 前位置を矩形で消去 (最小面積) */
        for (i = 0; i < BALL_COUNT; i++) {
            int er;
            er = balls[i].r + 2;
            pyxel_rect(balls[i].prev_x - er, balls[i].prev_y - er,
                        er * 2 + 1, er * 2 + 1, 0);
        }

        /* 新位置に描画 */
        for (i = 0; i < BALL_COUNT; i++) {
            pyxel_circ(balls[i].x, balls[i].y, balls[i].r, balls[i].col);
            pyxel_circb(balls[i].x, balls[i].y, balls[i].r, 7);
        }

        /* overlay時のラベル — 固定位置に毎フレーム上書き (小面積) */
        pyxel_rect(0, 0, 130, 14, 0);
        pyxel_text(4, 4, "BENCHMARK: overlay", 11);
    }

    /* FPSが変化した時だけUIを更新 */
    if (pyxel_fps != bench_prev_fps) {
        bench_prev_fps = pyxel_fps;
        bench_ui_dirty = 1;
    }
}

/* ベンチマーク用UI描画 (bench_ui_dirty時のみ呼ばれる) */
static void test5_draw_ui(void)
{
    int avg_fps;
    ui_reset();
    ui_text("TEST 5", 7);
    ui_text("Benchmark", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text(bench_use_cls ? "MODE:CLS" : "MODE:OVLY", bench_use_cls ? 8 : 11);
    ui_text_f(13, "Balls: %d", BALL_COUNT);
    ui_line += 8;
    ui_text_f(6, "Min: %d", bench_fps_min < 999 ? bench_fps_min : 0);
    ui_text_f(6, "Max: %d", bench_fps_max);
    avg_fps = bench_frame_total > 0 ? bench_fps_sum / bench_frame_total : 0;
    ui_text_f(6, "Avg: %d", avg_fps);
    ui_line += 8;
    ui_text("Z:Toggle", 7);
}

/* ======================================================================== */
/*  テスト6: ネイティブ解像度ベンチマーク (2xスケーリングなし)                */
/*                                                                          */
/*  libos32gfx を直接使用し、Pyxelの2x拡大処理を完全にバイパスする。         */
/*  save/restore + fill_circle 方式 (bench_scale2x Test7 と同等手法)         */
/* ======================================================================== */

#define T6_AREA_W 512
#define T6_AREA_H 384
#define T6_BG_SIZE(r) (((r)*2+3) / 8 + 1) * ((r)*2+3) * 4

/* 背景退避バッファ (最大半径24: sz=51, orig_wb=最大8, 8*51*4=1632 bytes) */
static u8 t6_bg[BALL_COUNT][2048];

static int t6_initialized = 0;
static int t6_fps_min = 999;
static int t6_fps_max = 0;
static int t6_frame_total = 0;
static int t6_fps_sum = 0;
static int t6_ui_dirty = 1;
static int t6_prev_fps = -1;

static struct {
    int x, y, dx, dy, r, col;
    int prev_x, prev_y;
} t6_balls[BALL_COUNT];

static void test6_init(void)
{
    int i;
    int init_data[BALL_COUNT][5] = {
        { 60,  60,  3,  2,  8  },
        {160, 120,  2,  3,  3  },
        {300,  80, -3,  2, 10  },
        {400, 200,  2, -2, 11  },
        {120, 260, -2,  3, 14  },
        {240, 180,  3, -3,  9  },
        {360, 300, -3, -2, 12  },
        { 80, 320,  2,  2,  6  }
    };

    for (i = 0; i < BALL_COUNT; i++) {
        t6_balls[i].x  = init_data[i][0];
        t6_balls[i].y  = init_data[i][1];
        t6_balls[i].dx = init_data[i][2];
        t6_balls[i].dy = init_data[i][3];
        t6_balls[i].col = init_data[i][4];
        t6_balls[i].r  = 12 + (i % 3) * 6;   /* 12, 18, 24 (ネイティブ解像度) */
        t6_balls[i].prev_x = t6_balls[i].x;
        t6_balls[i].prev_y = t6_balls[i].y;
    }

    /* 背景を描画 (チェッカーパターン) */
    gfx_clear(0);
    {
        int bx, by;
        for (by = 0; by < T6_AREA_H; by += 32) {
            for (bx = 0; bx < T6_AREA_W; bx += 32) {
                if (((bx / 32) + (by / 32)) % 2 == 0)
                    gfx_fill_rect(bx, by, 32, 32, 1);
            }
        }
    }
    kapi->gfx_present_rect(0, 0, T6_AREA_W, T6_AREA_H);

    /* 初回の背景退避 + ボール描画 */
    for (i = 0; i < BALL_COUNT; i++) {
        int sz = t6_balls[i].r * 2 + 3;
        gfx_save_rect(t6_balls[i].x - t6_balls[i].r - 1,
                      t6_balls[i].y - t6_balls[i].r - 1,
                      sz, sz, t6_bg[i]);
        gfx_fill_circle(t6_balls[i].x, t6_balls[i].y,
                        t6_balls[i].r, (u8)t6_balls[i].col);
        gfx_circle(t6_balls[i].x, t6_balls[i].y,
                   t6_balls[i].r, 7);
        kapi->gfx_add_dirty_rect(t6_balls[i].x - t6_balls[i].r - 1,
                                  t6_balls[i].y - t6_balls[i].r - 1,
                                  sz, sz);
    }
    kapi->gfx_present_dirty();

    t6_fps_min = 999;
    t6_fps_max = 0;
    t6_frame_total = 0;
    t6_fps_sum = 0;
    t6_initialized = 1;
}

static void test6_update(void)
{
    int i;

    if (!t6_initialized) test6_init();

    for (i = 0; i < BALL_COUNT; i++) {
        t6_balls[i].prev_x = t6_balls[i].x;
        t6_balls[i].prev_y = t6_balls[i].y;
        t6_balls[i].x += t6_balls[i].dx;
        t6_balls[i].y += t6_balls[i].dy;

        if (t6_balls[i].x - t6_balls[i].r < 0) {
            t6_balls[i].x = t6_balls[i].r;
            t6_balls[i].dx = -t6_balls[i].dx;
        }
        if (t6_balls[i].x + t6_balls[i].r >= T6_AREA_W) {
            t6_balls[i].x = T6_AREA_W - 1 - t6_balls[i].r;
            t6_balls[i].dx = -t6_balls[i].dx;
        }
        if (t6_balls[i].y - t6_balls[i].r < 0) {
            t6_balls[i].y = t6_balls[i].r;
            t6_balls[i].dy = -t6_balls[i].dy;
        }
        if (t6_balls[i].y + t6_balls[i].r >= T6_AREA_H) {
            t6_balls[i].y = T6_AREA_H - 1 - t6_balls[i].r;
            t6_balls[i].dy = -t6_balls[i].dy;
        }
    }

    if (pyxel_fps > 0 && pyxel_frame_count > 10) {
        t6_frame_total++;
        t6_fps_sum += pyxel_fps;
        if (pyxel_fps < t6_fps_min) t6_fps_min = pyxel_fps;
        if (pyxel_fps > t6_fps_max) t6_fps_max = pyxel_fps;
    }
}

static void test6_draw(void)
{
    int i;

    /* 前位置の背景を復元 */
    for (i = 0; i < BALL_COUNT; i++) {
        int sz = t6_balls[i].r * 2 + 3;
        gfx_restore_rect(t6_balls[i].prev_x - t6_balls[i].r - 1,
                         t6_balls[i].prev_y - t6_balls[i].r - 1,
                         sz, sz, t6_bg[i]);
        kapi->gfx_add_dirty_rect(t6_balls[i].prev_x - t6_balls[i].r - 1,
                                  t6_balls[i].prev_y - t6_balls[i].r - 1,
                                  sz, sz);
    }

    /* 新位置の背景退避 + ボール描画 */
    for (i = 0; i < BALL_COUNT; i++) {
        int sz = t6_balls[i].r * 2 + 3;
        gfx_save_rect(t6_balls[i].x - t6_balls[i].r - 1,
                      t6_balls[i].y - t6_balls[i].r - 1,
                      sz, sz, t6_bg[i]);
        gfx_fill_circle(t6_balls[i].x, t6_balls[i].y,
                        t6_balls[i].r, (u8)t6_balls[i].col);
        gfx_circle(t6_balls[i].x, t6_balls[i].y,
                   t6_balls[i].r, 7);
        kapi->gfx_add_dirty_rect(t6_balls[i].x - t6_balls[i].r - 1,
                                  t6_balls[i].y - t6_balls[i].r - 1,
                                  sz, sz);
    }
}

static void test6_draw_ui(void)
{
    int avg_fps;
    ui_reset();
    ui_text("TEST 6", 7);
    ui_text("Native", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text("No Scale", 11);
    ui_text_f(13, "Balls: %d", BALL_COUNT);
    ui_line += 8;
    ui_text_f(6, "Min: %d", t6_fps_min < 999 ? t6_fps_min : 0);
    ui_text_f(6, "Max: %d", t6_fps_max);
    avg_fps = t6_frame_total > 0 ? t6_fps_sum / t6_frame_total : 0;
    ui_text_f(6, "Avg: %d", avg_fps);
}

/* ======================================================================== */
/*  テスト7: 256x192 スプライト方式ベンチマーク                              */
/*                                                                          */
/*  gfx_demo と同じパターン: 事前レンダリング済みスプライトで描画。          */
/*  gfx_sprite_save_bg / restore_bg / draw_sprite を使用。                  */
/* ======================================================================== */

#define T7_AREA_W 256
#define T7_AREA_H 192

static int t7_initialized = 0;
static int t7_fps_min = 999;
static int t7_fps_max = 0;
static int t7_frame_total = 0;
static int t7_fps_sum = 0;
static int t7_prev_fps = -1;

static struct {
    int x, y, dx, dy, r, col;
    int prev_x, prev_y;
    GFX_Sprite *spr;
} t7_balls[BALL_COUNT];

/* 円をサーフェスに描画してスプライト化 (ネイティブ解像度) */
static GFX_Sprite *_t7_make_sprite(int r, int col)
{
    GFX_Surface *surf;
    GFX_Sprite *spr;
    int sz, cx, cy, x, y, d;

    sz = r * 2 + 1;
    surf = gfx_create_surface(sz, sz);
    if (!surf) return NULL;

    gfx_surface_clear(surf, 0);
    cx = r;
    cy = r;

    /* 塗り潰し円 */
    x = 0; y = r; d = 1 - r;
    while (x <= y) {
        { int lx;
          for (lx = cx - y; lx <= cx + y; lx++) {
              if (lx >= 0 && lx < sz) {
                  if (cy + x < sz) gfx_surface_pixel(surf, lx, cy + x, (u8)col);
                  if (cy - x >= 0) gfx_surface_pixel(surf, lx, cy - x, (u8)col);
              }
          }
          for (lx = cx - x; lx <= cx + x; lx++) {
              if (lx >= 0 && lx < sz) {
                  if (cy + y < sz) gfx_surface_pixel(surf, lx, cy + y, (u8)col);
                  if (cy - y >= 0) gfx_surface_pixel(surf, lx, cy - y, (u8)col);
              }
          }
        }
        if (d < 0) { d += 2 * x + 3; }
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }

    /* 輪郭線 (白) */
    x = 0; y = r; d = 1 - r;
    while (x <= y) {
        gfx_surface_pixel(surf, cx+x, cy+y, 7);
        gfx_surface_pixel(surf, cx-x, cy+y, 7);
        gfx_surface_pixel(surf, cx+x, cy-y, 7);
        gfx_surface_pixel(surf, cx-x, cy-y, 7);
        gfx_surface_pixel(surf, cx+y, cy+x, 7);
        gfx_surface_pixel(surf, cx-y, cy+x, 7);
        gfx_surface_pixel(surf, cx+y, cy-x, 7);
        gfx_surface_pixel(surf, cx-y, cy-x, 7);
        if (d < 0) { d += 2 * x + 3; }
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }

    spr = gfx_create_sprite(surf, 0);
    gfx_free_surface(surf);
    return spr;
}

static void test7_init(void)
{
    int i;
    int init_data[BALL_COUNT][5] = {
        { 30,  30,  2,  1,  8  },
        { 80,  60,  1,  2,  3  },
        {150,  40, -2,  1, 10  },
        {200, 100,  1, -1, 11  },
        { 60, 130, -1,  2, 14  },
        {120,  90,  2, -2,  9  },
        {180, 150, -2, -1, 12  },
        { 40, 160,  1,  1,  6  }
    };

    for (i = 0; i < BALL_COUNT; i++) {
        t7_balls[i].x  = init_data[i][0];
        t7_balls[i].y  = init_data[i][1];
        t7_balls[i].dx = init_data[i][2];
        t7_balls[i].dy = init_data[i][3];
        t7_balls[i].col = init_data[i][4];
        t7_balls[i].r  = 6 + (i % 3) * 3;   /* 6, 9, 12 */
        t7_balls[i].prev_x = t7_balls[i].x;
        t7_balls[i].prev_y = t7_balls[i].y;
        t7_balls[i].spr = _t7_make_sprite(t7_balls[i].r, t7_balls[i].col);
    }

    /* 背景を描画 (チェッカーパターン) */
    gfx_clear(0);
    {
        int bx, by;
        for (by = 0; by < T7_AREA_H; by += 16) {
            for (bx = 0; bx < T7_AREA_W; bx += 16) {
                if (((bx / 16) + (by / 16)) % 2 == 0)
                    gfx_fill_rect(bx, by, 16, 16, 1);
            }
        }
    }
    kapi->gfx_present_rect(0, 0, T7_AREA_W, T7_AREA_H);

    /* 初回: 背景退避 + スプライト描画 */
    for (i = 0; i < BALL_COUNT; i++) {
        if (t7_balls[i].spr) {
            int sx = t7_balls[i].x - t7_balls[i].r;
            int sy = t7_balls[i].y - t7_balls[i].r;
            gfx_sprite_save_bg(sx, sy, t7_balls[i].spr);
            gfx_draw_sprite(sx, sy, t7_balls[i].spr);
        }
    }
    kapi->gfx_present_dirty();

    t7_fps_min = 999;
    t7_fps_max = 0;
    t7_frame_total = 0;
    t7_fps_sum = 0;
    t7_initialized = 1;
}

static void test7_update(void)
{
    int i;

    if (!t7_initialized) test7_init();

    for (i = 0; i < BALL_COUNT; i++) {
        t7_balls[i].prev_x = t7_balls[i].x;
        t7_balls[i].prev_y = t7_balls[i].y;
        t7_balls[i].x += t7_balls[i].dx;
        t7_balls[i].y += t7_balls[i].dy;

        if (t7_balls[i].x - t7_balls[i].r < 0) {
            t7_balls[i].x = t7_balls[i].r;
            t7_balls[i].dx = -t7_balls[i].dx;
        }
        if (t7_balls[i].x + t7_balls[i].r >= T7_AREA_W) {
            t7_balls[i].x = T7_AREA_W - 1 - t7_balls[i].r;
            t7_balls[i].dx = -t7_balls[i].dx;
        }
        if (t7_balls[i].y - t7_balls[i].r < 0) {
            t7_balls[i].y = t7_balls[i].r;
            t7_balls[i].dy = -t7_balls[i].dy;
        }
        if (t7_balls[i].y + t7_balls[i].r >= T7_AREA_H) {
            t7_balls[i].y = T7_AREA_H - 1 - t7_balls[i].r;
            t7_balls[i].dy = -t7_balls[i].dy;
        }
    }

    if (pyxel_fps > 0 && pyxel_frame_count > 10) {
        t7_frame_total++;
        t7_fps_sum += pyxel_fps;
        if (pyxel_fps < t7_fps_min) t7_fps_min = pyxel_fps;
        if (pyxel_fps > t7_fps_max) t7_fps_max = pyxel_fps;
    }
}

static void test7_draw(void)
{
    int i;

    /* 全ボールの背景を復元 */
    for (i = 0; i < BALL_COUNT; i++) {
        if (t7_balls[i].spr) {
            int px = t7_balls[i].prev_x - t7_balls[i].r;
            int py = t7_balls[i].prev_y - t7_balls[i].r;
            gfx_sprite_restore_bg(px, py, t7_balls[i].spr);
        }
    }

    /* 新位置の背景退避 + スプライト描画 */
    for (i = 0; i < BALL_COUNT; i++) {
        if (t7_balls[i].spr) {
            int sx = t7_balls[i].x - t7_balls[i].r;
            int sy = t7_balls[i].y - t7_balls[i].r;
            gfx_sprite_save_bg(sx, sy, t7_balls[i].spr);
            gfx_draw_sprite(sx, sy, t7_balls[i].spr);
        }
    }
}

static void test7_draw_ui(void)
{
    int avg_fps;
    ui_reset();
    ui_text("TEST 7", 7);
    ui_text("Sprite", 10);
    ui_line += 8;
    ui_text_f(6, "FPS: %d", pyxel_fps);
    ui_line += 8;
    ui_text("256x192", 11);
    ui_text_f(13, "Balls: %d", BALL_COUNT);
    ui_line += 8;
    ui_text_f(6, "Min: %d", t7_fps_min < 999 ? t7_fps_min : 0);
    ui_text_f(6, "Max: %d", t7_fps_max);
    avg_fps = t7_frame_total > 0 ? t7_fps_sum / t7_frame_total : 0;
    ui_text_f(6, "Avg: %d", avg_fps);
}

/* ======================================================================== */
/*  メインのupdate/draw                                                      */
/* ======================================================================== */

static void update(void)
{
    /* Xキーでページ送り */
    if (pyxel_btnp(PYXEL_KEY_X, 0, 0)) {
        int prev = test_page;
        test_page = (test_page + 1) % NUM_PAGES;
        pyxel_pal_reset();
        pyxel_camera(0, 0);
        pyxel_clip_reset();
        cam_x = 0;
        cam_y = 0;
        clip_on = 0;
        pal_swapped = 0;
        /* ベンチマーク初期化リセット */
        if (test_page == 4 || prev == 4) {
            bench_initialized = 0;
        }
        if (test_page == 5 || prev == 5) {
            t6_initialized = 0;
        }
        if (test_page == 6 || prev == 6) {
            t7_initialized = 0;
        }
    }

    /* SPACE: SE再生 */
    if (pyxel_btnp(PYXEL_KEY_SPACE, 0, 0) && test_page != 1) {
        if (kapi) kapi->snd_se_play_raw(36, 5, 1);
    }

    switch (test_page) {
    case 0: test1_update(); break;
    case 1: test2_update(); break;
    case 2: test3_update(); break;
    case 3: test4_update(); break;
    case 4: test5_update(); break;
    case 5: test6_update(); break;
    case 6: test7_update(); break;
    }
}

static void draw_ui_common(void)
{
    int i;

    /* UI領域クリア + 再描画 */
    gfx_fill_rect(PYXEL_UI_X, PYXEL_UI_Y, PYXEL_UI_W, PYXEL_UI_H, 0);

    /* 16色パレットバー */
    for (i = 0; i < 16; i++) {
        gfx_fill_rect(PYXEL_UI_X + 4 + i * 7, 340, 6, 12, (u8)i);
    }

    /* ページインジケーター */
    for (i = 0; i < NUM_PAGES; i++) {
        int bx;
        bx = PYXEL_UI_X + 4 + i * 22;
        gfx_fill_rect(bx, 360, 18, 14, (u8)(i == test_page ? 10 : 1));
        {
            char num[2];
            num[0] = '1' + i;
            num[1] = '\0';
            kcg_draw_utf8(bx + 5, 362, num,
                          (u8)(i == test_page ? 0 : 7),
                          (u8)(i == test_page ? 10 : 1));
        }
    }

    kapi->gfx_add_dirty_rect(PYXEL_UI_X, PYXEL_UI_Y, PYXEL_UI_W, PYXEL_UI_H);
}

static void draw_status_bar(void)
{
    char status[64];
    gfx_fill_rect(0, PYXEL_STATUS_Y, 640, PYXEL_STATUS_H, 0);
    sprintf(status, "libpyxel Phase 2 Test [Page %d/%d]  F:%d",
            test_page + 1, NUM_PAGES, pyxel_frame_count);
    kcg_draw_utf8(4, PYXEL_STATUS_Y + 2, status, 13, 0);
    kapi->gfx_add_dirty_rect(0, PYXEL_STATUS_Y, 640, PYXEL_STATUS_H);
}

static void draw(void)
{
    /* TEST 5 overlay モード: UI/ステータスバーを毎フレーム更新しない */
    if (test_page == 4 && !bench_use_cls) {
        test5_draw();
        if (bench_ui_dirty) {
            draw_ui_common();
            test5_draw_ui();
            draw_status_bar();
            bench_ui_dirty = 0;
        }
        return;
    }

    /* TEST 6: ネイティブ解像度 — 常にoverlay方式 */
    if (test_page == 5) {
        test6_draw();
        if (pyxel_fps != t6_prev_fps) {
            t6_prev_fps = pyxel_fps;
            draw_ui_common();
            test6_draw_ui();
            draw_status_bar();
        }
        return;
    }

    /* TEST 7: 256x192ビューポート — 常にoverlay方式 */
    if (test_page == 6) {
        test7_draw();
        if (pyxel_fps != t7_prev_fps) {
            t7_prev_fps = pyxel_fps;
            draw_ui_common();
            test7_draw_ui();
            draw_status_bar();
        }
        return;
    }

    /* 通常モード: 毎フレームUI+ステータスバー更新 */
    draw_ui_common();

    switch (test_page) {
    case 0: test1_draw(); break;
    case 1: test2_draw(); break;
    case 2: test3_draw(); break;
    case 3: test4_draw(); break;
    case 4: test5_draw(); test5_draw_ui(); break;
    case 5: break; /* test6 は上の分岐で処理済み */
    case 6: break; /* test7 は上の分岐で処理済み */
    }

    draw_status_bar();
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
