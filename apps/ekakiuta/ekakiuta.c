/* ======================================================================== */
/*  EKAKIUTA.C — ドラえもん絵描き歌プログラム                               */
/*                                                                          */
/*  絵描き歌の歌詞通りの順序で全身ドラえもんを段階的に描画する。            */
/*                                                                          */
/*  歌詞と描画の対応:                                                       */
/*    1. まるかいて ちょん ×2  → 目 (楕円+黒目)                           */
/*    2. お豆に根が出て        → 鼻 + 鼻下の線                             */
/*    3. うえ木ばち ×2        → 頭の輪郭 + 顔の白い楕円                   */
/*    4. 6月6日に              → 手 (丸い手)                               */
/*    5. UFOが あっちいって…  → 首輪 + 胴体                              */
/*    6. お池が2つできました   → 足 (半円×2)                              */
/*    7. お池にお舟をうかべたら→ ポケット                                  */
/*    8. おそらに三日月のぼって→ 口                                        */
/*    9. ひげをつけたら        → ひげ                                      */
/*   10. ドラえもん            → 仕上げ (色塗り + 完成)                    */
/*                                                                          */
/*  SPACEキーで次のステップ、ESCで終了。                                    */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ======================================================================== */
/*  全身ドラえもんの座標定数                                                */
/*                                                                          */
/*  640x400画面に全身を収める。頭が大きく胴が短いSDプロポーション。          */
/*  頭の中心を画面やや上側に配置する。                                      */
/* ======================================================================== */

/* 頭 */
#define HEAD_CX     320
#define HEAD_CY     140
#define HEAD_R      105     /* 頭の半径 (大きめ) */

/* 目 (中央でピッタリ接する: offset == radius) */
#define EYE_R       36
#define EYE_Y       (HEAD_CY - 40)
#define LEYE_CX     (HEAD_CX - EYE_R)
#define REYE_CX     (HEAD_CX + EYE_R)

/* 目玉 */
#define PUPIL_R     8
#define LPUPIL_X    (HEAD_CX - 16)
#define RPUPIL_X    (HEAD_CX + 16)
#define PUPIL_Y     (EYE_Y)

/* 白い顔面 (楕円) — 大きく丸く */
#define WFACE_CY    (HEAD_CY + 20)
#define WFACE_RX    72
#define WFACE_RY    80

/* 鼻 */
#define NOSE_CY     (EYE_Y + EYE_R)  /* 目の真下 */
#define NOSE_R      12

/* 鼻下の線 (根) */
#define MLINE_TOP   (NOSE_CY + NOSE_R + 1)
#define MLINE_BOT   (NOSE_CY + NOSE_R + 25)

/* 口 — 大きな笑顔 */
#define MOUTH_CY    MLINE_BOT
#define MOUTH_R     55

/* 胴体 — 幅広 */
#define BODY_TOP    (HEAD_CY + HEAD_R - 8)
#define BODY_W      130
#define BODY_H      95
#define BODY_LEFT   (HEAD_CX - BODY_W / 2)
#define BODY_RIGHT  (HEAD_CX + BODY_W / 2)
#define BODY_BOT    (BODY_TOP + BODY_H)

/* 手 (丸い手) — 胴体に密着 */
#define HAND_R      20
#define LHAND_CX    (BODY_LEFT - HAND_R + 8)
#define RHAND_CX    (BODY_RIGHT + HAND_R - 8)
#define HAND_CY     (BODY_TOP + BODY_H / 2 + 5)

/* 足 — 大きく (楕円形の靴) */
#define FOOT_RX     46
#define FOOT_RY     20
#define LFOOT_CX    (HEAD_CX - 46)
#define RFOOT_CX    (HEAD_CX + 46)
#define FOOT_TOP    BODY_BOT

/* ポケット — 大きめ */
#define POCKET_CY   (BODY_TOP + 48)
#define POCKET_RX   38
#define POCKET_RY   28

/* 首輪・鈴 */
#define COLLAR_Y    (BODY_TOP - 3)
#define COLLAR_H    10
#define BELL_CY     (COLLAR_Y + COLLAR_H + 3)
#define BELL_R      10

/* ひげ */
#define WHISK_SX    40
#define WHISK_LEN   65

/* 画面領域 */
#define LYRIC_Y     4
#define GUIDE_Y     382

/* パレット番号 */
#define COL_BG      0
#define COL_WHITE   1
#define COL_BLUE    2
#define COL_RED     3
#define COL_YELLOW  4
#define COL_LTBLUE  5
#define COL_DKBLUE  6
#define COL_LINE    7
#define COL_DKRED   8
#define COL_SKIN    9
#define COL_COLLAR  10
#define COL_BROWN   11
#define COL_GRAY    12
#define COL_LTGREEN 13
#define COL_TEXT    14
#define COL_HITEXT  15

#define NUM_STEPS   11

/* ======================================================================== */
/*  描画補助                                                                */
/* ======================================================================== */

static KernelAPI *api;

static int wait_ms(int ticks)
{
    u32 start = api->get_tick();
    while (api->get_tick() - start < (u32)ticks) {
        int ch = api->kbd_trygetchar();
        if (ch == 0x1B) return -1;
        if (ch == ' ' || ch == 0x0D) return 1;
        api->sys_halt();
    }
    return 0;
}

static int animate_circle(int cx, int cy, int r, u8 color, int delay)
{
    int deg;
    for (deg = 0; deg < 360; deg += 4) {
        gfx_arc(cx, cy, r, deg, deg + 5, color);
        if (deg % 16 == 0) {
            api->gfx_present_dirty();
            if (delay > 0) {
                int ret = wait_ms(delay);
                if (ret != 0) {
                    gfx_circle(cx, cy, r, color);
                    api->gfx_present_dirty();
                    return ret;
                }
            }
        }
    }
    gfx_circle(cx, cy, r, color);
    api->gfx_present_dirty();
    return 0;
}

static int animate_line(int x0, int y0, int x1, int y1, u8 color, int steps)
{
    int i;
    for (i = 1; i <= steps; i++) {
        int mx = x0 + (x1 - x0) * i / steps;
        int my = y0 + (y1 - y0) * i / steps;
        gfx_line(x0, y0, mx, my, color);
        if (i % 3 == 0) api->gfx_present_dirty();
        {
            int ret = wait_ms(1);
            if (ret != 0) {
                gfx_line(x0, y0, x1, y1, color);
                api->gfx_present_dirty();
                return ret;
            }
        }
    }
    api->gfx_present_dirty();
    return 0;
}

/* ======================================================================== */
/*  歌詞表示                                                                */
/* ======================================================================== */

static void show_lyric(const char *text)
{
    gfx_fill_rect(0, LYRIC_Y, 640, 18, COL_BG);
    {
        int len = 0;
        const char *p = text;
        while (*p) {
            if ((u8)*p >= 0x80) {
                len += 16;
                if ((u8)*p >= 0xE0) p += 3;
                else if ((u8)*p >= 0xC0) p += 2;
                else p++;
            } else {
                len += 8;
                p++;
            }
        }
        {
            int x = (640 - len) / 2;
            if (x < 0) x = 0;
            kcg_set_scale(1);
            kcg_draw_utf8(x, LYRIC_Y, text, COL_TEXT, COL_BG);
        }
    }
    api->gfx_present_rect(0, LYRIC_Y, 640, 18);
}

static void show_guide(const char *text)
{
    gfx_fill_rect(0, GUIDE_Y, 640, 18, COL_BG);
    kcg_set_scale(1);
    kcg_draw_utf8(8, GUIDE_Y, text, COL_GRAY, COL_BG);
    api->gfx_present_rect(0, GUIDE_Y, 640, 18);
}

static int wait_key(void)
{
    for (;;) {
        int ch = api->kbd_getchar();
        if (ch == 0x1B) return -1;
        if (ch == ' ' || ch == 0x0D) return 0;
    }
}

/* ======================================================================== */
/*  パレット初期化                                                          */
/* ======================================================================== */

static void setup_palette(void)
{
    api->gfx_set_palette(COL_BG,     15, 14, 11);   /* クリーム色 */
    api->gfx_set_palette(COL_WHITE,  15, 15, 15);
    api->gfx_set_palette(COL_BLUE,    0,  8, 15);
    api->gfx_set_palette(COL_RED,    15,  2,  2);
    api->gfx_set_palette(COL_YELLOW, 15, 14,  0);
    api->gfx_set_palette(COL_LTBLUE,  4, 12, 15);
    api->gfx_set_palette(COL_DKBLUE,  0,  4, 10);
    api->gfx_set_palette(COL_LINE,    2,  2,  2);
    api->gfx_set_palette(COL_DKRED,  10,  0,  0);
    api->gfx_set_palette(COL_SKIN,   15, 10,  7);
    api->gfx_set_palette(COL_COLLAR, 15,  0,  0);
    api->gfx_set_palette(COL_BROWN,   8,  5,  2);
    api->gfx_set_palette(COL_GRAY,    6,  6,  6);
    api->gfx_set_palette(COL_LTGREEN, 0, 15,  4);
    api->gfx_set_palette(COL_TEXT,    0,  3,  8);   /* ダークブルー (明背景用) */
    api->gfx_set_palette(COL_HITEXT,  2,  6,  2);   /* ダークグリーン */
}

/* ======================================================================== */
/*  描画ステップ関数                                                        */
/* ======================================================================== */

/* Step 1: 「まるかいて ちょん まるかいて ちょん」 → 目 */
static int step_eyes(void)
{
    int ret;
    show_lyric("♪ まるかいて ちょん まるかいて ちょん");
    /* 左目 */
    ret = animate_circle(LEYE_CX, EYE_Y, EYE_R, COL_LINE, 1);
    if (ret == -1) return -1;
    /* 左目の黒目 (ちょん) */
    gfx_fill_circle(LPUPIL_X, PUPIL_Y, PUPIL_R, COL_LINE);
    gfx_fill_circle(LPUPIL_X - 2, PUPIL_Y - 2, 2, COL_WHITE);
    api->gfx_present_dirty();

    ret = wait_ms(20);
    if (ret == -1) return -1;

    /* 右目 */
    ret = animate_circle(REYE_CX, EYE_Y, EYE_R, COL_LINE, 1);
    if (ret == -1) return -1;
    /* 右目の黒目 (ちょん) */
    gfx_fill_circle(RPUPIL_X, PUPIL_Y, PUPIL_R, COL_LINE);
    gfx_fill_circle(RPUPIL_X + 2, PUPIL_Y - 2, 2, COL_WHITE);
    /* 中央の仕切り線 */
    /*gfx_vline(HEAD_CX, EYE_Y - EYE_R, (NOSE_CY - NOSE_R) - (EYE_Y - EYE_R), COL_LINE);*/
    api->gfx_present_dirty();
    return 0;
}

/* Step 2: 「お豆に根が出て」 → 鼻 + 鼻下の線 */
static int step_nose(void)
{
    int ret;
    show_lyric("♪ お豆に根が出て");
    /* 鼻 (お豆) */
    gfx_fill_circle(HEAD_CX, NOSE_CY, NOSE_R, COL_RED);
    gfx_fill_circle(HEAD_CX - 2, NOSE_CY - 2, 2, COL_WHITE);
    api->gfx_present_dirty();
    ret = wait_ms(15);
    if (ret == -1) return -1;
    /* 鼻下の線 (根) */
    ret = animate_line(HEAD_CX, MLINE_TOP, HEAD_CX, MLINE_BOT, COL_LINE, 8);
    return ret;
}

/* Step 3: 「うえ木ばち うえ木ばち」 → 頭の輪郭 + 顔の白い楕円 */
static int step_head(void)
{
    int ret;
    show_lyric("♪ うえ木ばち うえ木ばち");
    /* 頭の大きな円 (うえ木ばち1つ目) */
    ret = animate_circle(HEAD_CX, HEAD_CY, HEAD_R, COL_LINE, 1);
    if (ret == -1) return -1;
    ret = wait_ms(15);
    if (ret == -1) return -1;
    /* 顔の白い楕円 (うえ木ばち2つ目) */
    gfx_ellipse(HEAD_CX, WFACE_CY, WFACE_RX, WFACE_RY, COL_LINE);
    api->gfx_present_dirty();
    return 0;
}

/* Step 4: 「6月6日に」 → 手 */
static int step_hands(void)
{
    int ret;
    show_lyric("♪ 6月6日に");
    /* 左手 */
    ret = animate_circle(LHAND_CX, HAND_CY, HAND_R, COL_LINE, 1);
    if (ret == -1) return -1;
    /* 右手 */
    ret = animate_circle(RHAND_CX, HAND_CY, HAND_R, COL_LINE, 1);
    return ret;
}

/* Step 5: 「UFOが あっちいって こっちいって おっこちて」 → 首輪+胴体 */
static int step_body(void)
{
    int ret;
    show_lyric("♪ UFOが あっちいって こっちいって おっこちて");

    /* 首輪 (UFO) */
    gfx_fill_rect(HEAD_CX - 55, COLLAR_Y, 110, COLLAR_H, COL_COLLAR);
    api->gfx_present_dirty();
    ret = wait_ms(10);
    if (ret == -1) return -1;

    /* 鈴 */
    gfx_fill_circle(HEAD_CX, BELL_CY, BELL_R, COL_YELLOW);
    gfx_circle(HEAD_CX, BELL_CY, BELL_R, COL_LINE);
    gfx_hline(HEAD_CX - BELL_R, BELL_CY, BELL_R * 2, COL_LINE);
    gfx_fill_circle(HEAD_CX, BELL_CY + 2, 2, COL_LINE);
    gfx_vline(HEAD_CX, BELL_CY + 4, 3, COL_LINE);
    api->gfx_present_dirty();
    ret = wait_ms(10);
    if (ret == -1) return -1;

    /* 胴体 (あっちいって こっちいって → 左右の辺) */
    ret = animate_line(BODY_LEFT, BODY_TOP, BODY_LEFT, BODY_BOT, COL_LINE, 10);
    if (ret == -1) return -1;
    ret = animate_line(BODY_RIGHT, BODY_TOP, BODY_RIGHT, BODY_BOT, COL_LINE, 10);
    if (ret == -1) return -1;

    /* おっこちて → 底辺 */
    ret = animate_line(BODY_LEFT, BODY_BOT, BODY_RIGHT, BODY_BOT, COL_LINE, 10);
    return ret;
}

/* Step 6: 「お池が2つできました」 → 足 (楕円形の靴) */
static int step_feet(void)
{
    int ret;
    show_lyric("♪ お池が2つできました");
    /* 左足 (下半分の楕円) */
    gfx_ellipse(LFOOT_CX, FOOT_TOP, FOOT_RX, FOOT_RY, COL_LINE);
    gfx_hline(LFOOT_CX - FOOT_RX, FOOT_TOP, FOOT_RX * 2, COL_LINE);
    api->gfx_present_dirty();
    ret = wait_ms(20);
    if (ret == -1) return -1;
    /* 右足 */
    gfx_ellipse(RFOOT_CX, FOOT_TOP, FOOT_RX, FOOT_RY, COL_LINE);
    gfx_hline(RFOOT_CX - FOOT_RX, FOOT_TOP, FOOT_RX * 2, COL_LINE);
    api->gfx_present_dirty();
    return 0;
}

/* Step 7: 「お池にお舟をうかべたら」 → ポケット */
static int step_pocket(void)
{
    show_lyric("♪ お池にお舟を うかべたら");
    /* ポケット (半円) */
    gfx_arc(HEAD_CX, POCKET_CY, POCKET_RX, 180, 360, COL_LINE);
    gfx_arc(HEAD_CX, POCKET_CY, POCKET_RX - 1, 180, 360, COL_LINE);
    gfx_hline(HEAD_CX - POCKET_RX, POCKET_CY, POCKET_RX * 2, COL_LINE);
    api->gfx_present_dirty();
    return 0;
}

/* Step 8: 「おそらに三日月のぼって」 → 口 */
static int step_mouth(void)
{
    show_lyric("♪ おそらに三日月のぼって");
    /* 口 (大きな弧 = 三日月) */
    gfx_arc(HEAD_CX, MOUTH_CY, MOUTH_R, 200, 340, COL_LINE);
    gfx_arc(HEAD_CX, MOUTH_CY, MOUTH_R - 1, 200, 340, COL_LINE);
    /* 口の内側 */
    gfx_fill_ellipse(HEAD_CX, MOUTH_CY + 22, 35, 16, COL_DKRED);
    gfx_fill_ellipse(HEAD_CX, MOUTH_CY + 28, 18, 8, COL_SKIN);
    api->gfx_present_dirty();
    return 0;
}

/* Step 9: 「ひげをつけたら」 → ひげ */
static int step_whiskers(void)
{
    int ret;
    int sx_l = HEAD_CX - WHISK_SX;
    int sx_r = HEAD_CX + WHISK_SX;
    int ex_l = sx_l - WHISK_LEN;
    int ex_r = sx_r + WHISK_LEN;

    show_lyric("♪ ひげをつけたら");

    /* 左ひげ 3本 */
    ret = animate_line(sx_l, HEAD_CY - 2,  ex_l, HEAD_CY - 15, COL_LINE, 10);
    if (ret == -1) return -1;
    ret = animate_line(sx_l, HEAD_CY + 10, ex_l, HEAD_CY + 10, COL_LINE, 10);
    if (ret == -1) return -1;
    ret = animate_line(sx_l, HEAD_CY + 22, ex_l, HEAD_CY + 35, COL_LINE, 10);
    if (ret == -1) return -1;

    /* 右ひげ 3本 */
    ret = animate_line(sx_r, HEAD_CY - 2,  ex_r, HEAD_CY - 15, COL_LINE, 10);
    if (ret == -1) return -1;
    ret = animate_line(sx_r, HEAD_CY + 10, ex_r, HEAD_CY + 10, COL_LINE, 10);
    if (ret == -1) return -1;
    ret = animate_line(sx_r, HEAD_CY + 22, ex_r, HEAD_CY + 35, COL_LINE, 10);
    return ret;
}

/* Step 10: 「ドラえもん」 → 色塗り + 完成 */
static int step_complete(void)
{
    int sx_l = HEAD_CX - WHISK_SX;
    int sx_r = HEAD_CX + WHISK_SX;
    int ex_l = sx_l - WHISK_LEN;
    int ex_r = sx_r + WHISK_LEN;

    show_lyric("♪ ドラえもん！");

    /* === 色塗り === */

    /* 頭: 水色で塗りつぶし → 白い顔面楕円 → 輪郭再描画 */
    gfx_fill_circle(HEAD_CX, HEAD_CY, HEAD_R - 1, COL_BLUE);
    gfx_fill_ellipse(HEAD_CX, WFACE_CY, WFACE_RX - 1, WFACE_RY - 1, COL_WHITE);

    /* 胴体: 水色で塗りつぶし */
    gfx_fill_rect(BODY_LEFT + 1, BODY_TOP, BODY_W - 2, BODY_H, COL_BLUE);

    /* 胴体の白い腹部 (大きく) */
    gfx_fill_ellipse(HEAD_CX, BODY_TOP + BODY_H / 2, BODY_W / 2 - 15, BODY_H / 2 - 5, COL_WHITE);

    /* 手: 白で塗りつぶし */
    gfx_fill_circle(LHAND_CX, HAND_CY, HAND_R - 1, COL_WHITE);
    gfx_fill_circle(RHAND_CX, HAND_CY, HAND_R - 1, COL_WHITE);

    /* 足: 白で塗りつぶし (下半分の楕円) */
    gfx_fill_ellipse(LFOOT_CX, FOOT_TOP, FOOT_RX - 1, FOOT_RY - 1, COL_WHITE);
    gfx_fill_ellipse(RFOOT_CX, FOOT_TOP, FOOT_RX - 1, FOOT_RY - 1, COL_WHITE);

    /* === 全ての輪郭線を再描画 === */

    /* 頭の輪郭 */
    gfx_circle(HEAD_CX, HEAD_CY, HEAD_R, COL_LINE);
    /* 顔面の楕円 */
    gfx_ellipse(HEAD_CX, WFACE_CY, WFACE_RX, WFACE_RY, COL_LINE);

    /* 目の白 */
    gfx_fill_circle(LEYE_CX, EYE_Y, EYE_R - 2, COL_WHITE);
    gfx_fill_circle(REYE_CX, EYE_Y, EYE_R - 2, COL_WHITE);
    gfx_circle(LEYE_CX, EYE_Y, EYE_R, COL_LINE);
    gfx_circle(REYE_CX, EYE_Y, EYE_R, COL_LINE);

    /* 目玉 */
    gfx_fill_circle(LPUPIL_X, PUPIL_Y, PUPIL_R, COL_LINE);
    gfx_fill_circle(LPUPIL_X - 2, PUPIL_Y - 2, 2, COL_WHITE);
    gfx_fill_circle(RPUPIL_X, PUPIL_Y, PUPIL_R, COL_LINE);
    gfx_fill_circle(RPUPIL_X + 2, PUPIL_Y - 2, 2, COL_WHITE);

    /* 鼻 */
    gfx_fill_circle(HEAD_CX, NOSE_CY, NOSE_R, COL_RED);
    gfx_fill_circle(HEAD_CX - 2, NOSE_CY - 2, 2, COL_WHITE);
    gfx_vline(HEAD_CX, MLINE_TOP, MLINE_BOT - MLINE_TOP, COL_LINE);

    /* 口 */
    gfx_arc(HEAD_CX, MOUTH_CY, MOUTH_R, 200, 340, COL_LINE);
    gfx_arc(HEAD_CX, MOUTH_CY, MOUTH_R - 1, 200, 340, COL_LINE);
    gfx_fill_ellipse(HEAD_CX, MOUTH_CY + 22, 35, 16, COL_DKRED);
    gfx_fill_ellipse(HEAD_CX, MOUTH_CY + 28, 18, 8, COL_SKIN);

    /* ひげ (太線) */
    gfx_line(sx_l, HEAD_CY - 2,  ex_l, HEAD_CY - 15, COL_LINE);
    gfx_line(sx_l, HEAD_CY - 3,  ex_l, HEAD_CY - 16, COL_LINE);
    gfx_line(sx_l, HEAD_CY + 10, ex_l, HEAD_CY + 10, COL_LINE);
    gfx_line(sx_l, HEAD_CY + 9,  ex_l, HEAD_CY + 9,  COL_LINE);
    gfx_line(sx_l, HEAD_CY + 22, ex_l, HEAD_CY + 35, COL_LINE);
    gfx_line(sx_l, HEAD_CY + 21, ex_l, HEAD_CY + 34, COL_LINE);

    gfx_line(sx_r, HEAD_CY - 2,  ex_r, HEAD_CY - 15, COL_LINE);
    gfx_line(sx_r, HEAD_CY - 3,  ex_r, HEAD_CY - 16, COL_LINE);
    gfx_line(sx_r, HEAD_CY + 10, ex_r, HEAD_CY + 10, COL_LINE);
    gfx_line(sx_r, HEAD_CY + 9,  ex_r, HEAD_CY + 9,  COL_LINE);
    gfx_line(sx_r, HEAD_CY + 22, ex_r, HEAD_CY + 35, COL_LINE);
    gfx_line(sx_r, HEAD_CY + 21, ex_r, HEAD_CY + 34, COL_LINE);

    /* 首輪 & 鈴 */
    gfx_fill_rect(HEAD_CX - 55, COLLAR_Y, 110, COLLAR_H, COL_COLLAR);
    gfx_fill_circle(HEAD_CX, BELL_CY, BELL_R, COL_YELLOW);
    gfx_circle(HEAD_CX, BELL_CY, BELL_R, COL_LINE);
    gfx_hline(HEAD_CX - BELL_R, BELL_CY, BELL_R * 2, COL_LINE);
    gfx_fill_circle(HEAD_CX, BELL_CY + 2, 2, COL_LINE);
    gfx_vline(HEAD_CX, BELL_CY + 4, 3, COL_LINE);

    /* 胴体の輪郭 */
    gfx_line(BODY_LEFT, BODY_TOP, BODY_LEFT, BODY_BOT, COL_LINE);
    gfx_line(BODY_RIGHT, BODY_TOP, BODY_RIGHT, BODY_BOT, COL_LINE);
    gfx_hline(BODY_LEFT, BODY_BOT, BODY_W, COL_LINE);

    /* 腹部の白いエリア輪郭 */
    gfx_ellipse(HEAD_CX, BODY_TOP + BODY_H / 2, BODY_W / 2 - 15, BODY_H / 2 - 5, COL_LINE);

    /* ポケット */
    gfx_arc(HEAD_CX, POCKET_CY, POCKET_RX, 180, 360, COL_LINE);
    gfx_arc(HEAD_CX, POCKET_CY, POCKET_RX - 1, 180, 360, COL_LINE);
    gfx_hline(HEAD_CX - POCKET_RX, POCKET_CY, POCKET_RX * 2, COL_LINE);

    /* 手の輪郭 */
    gfx_circle(LHAND_CX, HAND_CY, HAND_R, COL_LINE);
    gfx_circle(RHAND_CX, HAND_CY, HAND_R, COL_LINE);

    /* 足の輪郭 (楕円形の靴) */
    gfx_ellipse(LFOOT_CX, FOOT_TOP, FOOT_RX, FOOT_RY, COL_LINE);
    gfx_hline(LFOOT_CX - FOOT_RX, FOOT_TOP, FOOT_RX * 2, COL_LINE);
    gfx_ellipse(RFOOT_CX, FOOT_TOP, FOOT_RX, FOOT_RY, COL_LINE);
    gfx_hline(RFOOT_CX - FOOT_RX, FOOT_TOP, FOOT_RX * 2, COL_LINE);

    api->gfx_present_dirty();
    return 0;
}

/* Step 11: ベジェ曲線デモ — 吹き出し + デコレーション */
static int step_bezier_demo(void)
{
    show_lyric("♪ ベジェ曲線デモ");

    /* === 吹き出し (3次ベジェで角丸四角) === */
    {
        int bx = 470, by = 40;  /* 吹き出しの左上 */
        int bw = 150, bh = 50;  /* 幅、高さ */
        int cr = 12;            /* 角丸半径 */

        /* 背景を白で塗りつぶし */
        gfx_fill_rect(bx + 1, by + 1, bw - 1, bh - 1, COL_WHITE);

        /* 上辺 (左角丸 → 直線 → 右角丸) */
        gfx_bezier3(bx, by + cr,  bx, by,  bx + cr, by,  bx + cr, by, COL_LINE);
        gfx_hline(bx + cr, by, bw - cr * 2, COL_LINE);
        gfx_bezier3(bx + bw - cr, by,  bx + bw, by,  bx + bw, by + cr,  bx + bw, by + cr, COL_LINE);

        /* 右辺 */
        gfx_vline(bx + bw, by + cr, bh - cr * 2, COL_LINE);

        /* 下辺 (右角丸 → 直線 → 左角丸) */
        gfx_bezier3(bx + bw, by + bh - cr,  bx + bw, by + bh,  bx + bw - cr, by + bh,  bx + bw - cr, by + bh, COL_LINE);
        gfx_hline(bx + cr, by + bh, bw - cr * 2, COL_LINE);
        gfx_bezier3(bx + cr, by + bh,  bx, by + bh,  bx, by + bh - cr,  bx, by + bh - cr, COL_LINE);

        /* 左辺 */
        gfx_vline(bx, by + cr, bh - cr * 2, COL_LINE);

        /* 吹き出しの尻尾 (ベジェで曲線的に指す) */
        gfx_bezier3(bx + 20, by + bh,
                    bx + 10, by + bh + 20,
                    bx - 10, by + bh + 15,
                    HEAD_CX + HEAD_R - 20, HEAD_CY - HEAD_R + 10,
                    COL_LINE);

        /* 吹き出し内のテキスト */
        kcg_draw_utf8(bx + 16, by + 8, "ぼくドラえもん!", COL_BLUE, COL_WHITE);
        kcg_draw_utf8(bx + 8, by + 28, "ベジェ曲線テスト", COL_LINE, COL_WHITE);
    }
    api->gfx_present_dirty();

    /* === ハートマーク (2次ベジェ) === */
    {
        int hx = 80, hy = 100;  /* ハート下端 */
        /* 左側 */
        gfx_bezier2(hx, hy,  hx - 30, hy - 40,  hx, hy - 55, COL_RED);
        gfx_bezier2(hx, hy - 55,  hx + 15, hy - 65,  hx + 25, hy - 45, COL_RED);
        /* 右側 */
        gfx_bezier2(hx, hy,  hx + 30, hy - 40,  hx, hy - 55, COL_RED);
        gfx_bezier2(hx, hy - 55,  hx - 15, hy - 65,  hx - 25, hy - 45, COL_RED);
    }
    api->gfx_present_dirty();

    /* === 波線デコレーション (太線3次ベジェ) === */
    gfx_bezier3_thick(30, 370,  180, 340,  460, 390,  610, 355,  3, COL_BLUE);
    gfx_bezier3_thick(30, 378,  180, 348,  460, 398,  610, 363,  2, COL_LTBLUE);
    api->gfx_present_dirty();

    return 0;
}


/*  メインプログラム                                                        */
/* ======================================================================== */

typedef int (*StepFunc)(void);

void main(int argc, char **argv, KernelAPI *kapi)
{
    int step;
    int ret;
    StepFunc steps[NUM_STEPS];

    (void)argc;
    (void)argv;

    api = kapi;

    libos32gfx_init(api);
    setup_palette();

    gfx_clear(COL_BG);
    gfx_present();
    api->gfx_present_dirty();

    kcg_set_scale(1);
    show_lyric("ドラえもんの絵描き歌");
    show_guide("SPACE: 次へ  ESC: 終了");

    steps[0] = step_eyes;
    steps[1] = step_nose;
    steps[2] = step_head;
    steps[3] = step_hands;
    steps[4] = step_body;
    steps[5] = step_feet;
    steps[6] = step_pocket;
    steps[7] = step_mouth;
    steps[8] = step_whiskers;
    steps[9] = step_complete;
    steps[10] = step_bezier_demo;

    if (wait_key() < 0) goto done;

    for (step = 0; step < NUM_STEPS; step++) {
        ret = steps[step]();
        if (ret == -1) goto done;

        if (step < NUM_STEPS - 1) {
            show_guide("SPACE: 次へ  ESC: 終了");
            if (wait_key() < 0) goto done;
        }
    }

    show_guide("完成! SPACEで終了");
    wait_key();

done:
    {
        static const u8 def_r[] = {0,0,7,7,0,0,7,7,0,0,15,15,0,0,15,15};
        static const u8 def_g[] = {0,0,0,0,7,7,7,7,0,0, 0, 0,15,15,15,15};
        static const u8 def_b[] = {0,7,0,7,0,7,0,7,0,15,0,15,0,15,0,15};
        int i;
        for (i = 0; i < 16; i++) {
            api->gfx_set_palette(i, def_r[i], def_g[i], def_b[i]);
        }
    }

    gfx_clear(0);
    gfx_present();
    api->gfx_present_dirty();
    libos32gfx_shutdown();
    api->tvram_clear();
}
