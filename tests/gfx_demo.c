/* ======================================================================== */
/*  GFX_DEMO.C — 320x200ビューポート デモ (FPSカウンタ付き)                  */
/*                                                                          */
/*  640x400画面の中央に320x200のゲーム領域を配置                            */
/*  ゲーム領域のみgfx_present_rectで転送してfps測定                          */
/*  ESCで終了                                                               */
/* ======================================================================== */

#include "gfx.h"
#include "gfx_font.h"
#include "kbd.h"
#include "idt.h"

/* ======== ビューポート定数 (320x200) ======== */
#define VP_W   320
#define VP_H   200
#define VP_X   ((640 - VP_W) / 2)   /* = 160 */
#define VP_Y   ((400 - VP_H) / 2)   /* = 100 */

/* ======== 簡易乱数 ======== */
static unsigned long _seed = 7654321UL;
static int rnd(int max)
{
    _seed = _seed * 1103515245UL + 12345UL;
    return (int)((_seed >> 16) % (unsigned long)max);
}

/* ======== バウンスブロック (SFC風16x16) ======== */
#define NBOX 8
static int bx[NBOX], by[NBOX], bdx[NBOX], bdy[NBOX];
static u8  bc[NBOX];
static int obx[NBOX], oby[NBOX];

static void init_boxes(void)
{
    int i;
    static const u8 colors[] = {9, 10, 12, 14, 11, 13, 9, 10};
    for (i = 0; i < NBOX; i++) {
        bx[i] = VP_X + 10 + rnd(VP_W - 36);
        by[i] = VP_Y + 10 + rnd(VP_H - 36);
        bdx[i] = (rnd(2) ? 2 : -2);
        bdy[i] = (rnd(2) ? 2 : -2);
        bc[i] = colors[i];
        obx[i] = bx[i]; oby[i] = by[i];
    }
}

static void update_boxes(void)
{
    int i;
    for (i = 0; i < NBOX; i++) {
        obx[i] = bx[i]; oby[i] = by[i];
        bx[i] += bdx[i];
        by[i] += bdy[i];
        if (bx[i] < VP_X + 1) { bx[i] = VP_X + 1; bdx[i] = -bdx[i]; }
        if (bx[i] + 16 >= VP_X + VP_W - 1) { bx[i] = VP_X + VP_W - 18; bdx[i] = -bdx[i]; }
        if (by[i] < VP_Y + 1) { by[i] = VP_Y + 1; bdy[i] = -bdy[i]; }
        if (by[i] + 16 >= VP_Y + VP_H - 1) { by[i] = VP_Y + VP_H - 18; bdy[i] = -bdy[i]; }
    }
}

static void erase_boxes(void)
{
    int i;
    for (i = 0; i < NBOX; i++) {
        gfx_fill_rect(obx[i], oby[i], 16, 16, 0);
    }
}

static void draw_boxes(void)
{
    int i;
    for (i = 0; i < NBOX; i++) {
        gfx_fill_rect(bx[i], by[i], 16, 16, bc[i]);
        gfx_rect(bx[i], by[i], 16, 16, 15);
        /* 十字マーク */
        gfx_hline(bx[i] + 4, by[i] + 7, 8, 15);
        gfx_vline(bx[i] + 7, by[i] + 4, 8, 15);
    }
}

/* ======== パーティクル ======== */
#define NPART 16
static int px[NPART], py[NPART], pdx[NPART], pdy[NPART];
static u8  pc[NPART];
static int plife[NPART];
static int opx[NPART], opy[NPART];

static void spawn_particle(int i)
{
    px[i] = VP_X + VP_W / 2 + rnd(80) - 40;
    py[i] = VP_Y + VP_H - 10;
    pdx[i] = rnd(5) - 2;
    pdy[i] = -(rnd(3) + 2);
    pc[i] = (u8)(rnd(6) + 9);
    plife[i] = 15 + rnd(15);
    opx[i] = px[i]; opy[i] = py[i];
}

static void init_particles(void)
{
    int i;
    for (i = 0; i < NPART; i++) spawn_particle(i);
}

static void update_particles(void)
{
    int i;
    for (i = 0; i < NPART; i++) {
        opx[i] = px[i]; opy[i] = py[i];
        px[i] += pdx[i];
        py[i] += pdy[i];
        pdy[i] += 1;
        plife[i]--;
        if (plife[i] <= 0 ||
            px[i] < VP_X + 1 || px[i] >= VP_X + VP_W - 2 ||
            py[i] < VP_Y + 1 || py[i] >= VP_Y + VP_H - 2) {
            spawn_particle(i);
        }
    }
}

static void erase_particles(void)
{
    int i;
    for (i = 0; i < NPART; i++) {
        if (opx[i] > VP_X && opx[i]+1 < VP_X+VP_W &&
            opy[i] > VP_Y && opy[i]+1 < VP_Y+VP_H) {
            gfx_pixel(opx[i], opy[i], 0);
            gfx_pixel(opx[i]+1, opy[i], 0);
            gfx_pixel(opx[i], opy[i]+1, 0);
            gfx_pixel(opx[i]+1, opy[i]+1, 0);
        }
    }
}

static void draw_particles(void)
{
    int i;
    for (i = 0; i < NPART; i++) {
        if (px[i] > VP_X && px[i]+1 < VP_X+VP_W &&
            py[i] > VP_Y && py[i]+1 < VP_Y+VP_H) {
            u8 c = (plife[i] > 8) ? pc[i] : 8;
            gfx_pixel(px[i], py[i], c);
            gfx_pixel(px[i]+1, py[i], c);
            gfx_pixel(px[i], py[i]+1, c);
            gfx_pixel(px[i]+1, py[i]+1, c);
        }
    }
}

/* ======== ビューポート枠 (初回のみ) ======== */
static void draw_frame(void)
{
    /* 二重枠 */
    gfx_rect(VP_X - 2, VP_Y - 2, VP_W + 4, VP_H + 4, 7);
    gfx_rect(VP_X - 1, VP_Y - 1, VP_W + 2, VP_H + 2, 15);

    /* 四隅マーカー */
    gfx_fill_rect(VP_X - 4, VP_Y - 4, 3, 3, 14);
    gfx_fill_rect(VP_X + VP_W + 1, VP_Y - 4, 3, 3, 14);
    gfx_fill_rect(VP_X - 4, VP_Y + VP_H + 1, 3, 3, 14);
    gfx_fill_rect(VP_X + VP_W + 1, VP_Y + VP_H + 1, 3, 3, 14);

    /* FPSバー枠 (ビューポート下) */
    gfx_rect(VP_X, VP_Y + VP_H + 6, 302, 10, 7);
}

/* ======== FPS バー更新 ======== */
static void draw_fps_bar(int fps)
{
    int bw;
    u8 c;

    /* バー背景クリア */
    gfx_fill_rect(VP_X + 1, VP_Y + VP_H + 7, 300, 8, 0);

    /* FPSバー */
    bw = fps * 5;
    if (bw > 300) bw = 300;
    if (bw > 0) {
        if (fps >= 55) c = 10;       /* 緑 */
        else if (fps >= 30) c = 14;  /* 黄 */
        else c = 12;                  /* 赤 */
        gfx_fill_rect(VP_X + 1, VP_Y + VP_H + 7, bw, 8, c);
    }

    /* 30fps/60fps目盛り */
    gfx_vline(VP_X + 150, VP_Y + VP_H + 7, 8, 8);
    gfx_vline(VP_X + 300, VP_Y + VP_H + 7, 8, 7);

    /* FPSバー領域のみ転送 */
    gfx_present_rect(VP_X, VP_Y + VP_H + 6, 304, 12);
}

/* ======================================================================== */
/*  メインデモループ                                                        */
/* ======================================================================== */

void gfx_run_demo(void)
{
    int frame = 0;
    int fps = 0;
    int fps_frames = 0;
    u32 last_tick, fps_tick;

    gfx_init();
    gfx_clear(0);

    /* 初回: 枠を描画して全画面転送 */
    draw_frame();
    /* タイトルテキスト (フォント描画デモ) */
    gfx_puts(VP_X, VP_Y - 12, "OS32 GFX Demo - 320x200 Viewport", 14);
    gfx_puts(VP_X + VP_W + 8, VP_Y, "Sprites: 8", 10);
    gfx_puts(VP_X + VP_W + 8, VP_Y + 10, "Particles: 16", 10);
    gfx_puts(VP_X + VP_W + 8, VP_Y + 24, "ESC to quit", 7);
    init_boxes();
    init_particles();
    draw_boxes();
    draw_particles();
    gfx_present();

    last_tick = tick_count;
    fps_tick = tick_count;

    for (;;) {
        int ch;

        /* ESCチェック */
        while ((ch = kbd_trygetchar()) >= 0) {
            if (ch == 0x1B) goto done;
        }

        /* 差分描画 */
        erase_boxes();
        erase_particles();

        update_boxes();
        update_particles();

        draw_boxes();
        draw_particles();

        /* ゲーム領域のみ転送 (320x200 = 32KB) */
        gfx_present_rect(VP_X, VP_Y, VP_W, VP_H);

        frame++;
        fps_frames++;

        /* FPS計測 (1秒ごと) */
        if (tick_count - fps_tick >= 100) {
            fps = fps_frames;
            fps_frames = 0;
            fps_tick = tick_count;
            draw_fps_bar(fps);
            /* FPS数値テキスト */
            gfx_fill_rect(VP_X + 310, VP_Y + VP_H + 7, 48, 10, 0);
            gfx_printf(VP_X + 310, VP_Y + VP_H + 7, 15, "%d fps", fps);
            gfx_present_rect(VP_X + 310, VP_Y + VP_H + 6, 50, 12);
        }

        /* 次フレームまで待機 (タイマ 100Hz = 10ms刻み → 1tick待ち) */
        while (tick_count == last_tick) {
            __asm__ volatile("hlt");
        }
        last_tick = tick_count;
    }

done:
    gfx_shutdown();
}
