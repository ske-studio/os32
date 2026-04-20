/* ======================================================================== */
/*  RASTER.C — PC-98 ラスタパレットデモ                                      */
/*                                                                          */
/*  H-Sync (水平帰線期間) を利用してパレットを書き換え、                      */
/*  擬似的に16色を超える発色を行うテクニックのデモンストレーション。           */
/*                                                                          */
/*  技術概要:                                                                */
/*    1. VSYNC (port 0xA0 bit5) で画面描画開始を検出                         */
/*    2. HBLANK (port 0xA0 bit6) をカウントして分割ラインを特定               */
/*    3. 分割位置でパレットレジスタ (0xA8/AA/AC/AE) を高速書き換え            */
/*                                                                          */
/*  描画・初期化にはlibos32gfxを使用。ラスタ分割中のパレット書き換えのみ      */
/*  I/O直接操作を行う。                                                      */
/*                                                                          */
/*  参照: PC9800Bible §2-7, §1-4                                            */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ======== I/Oポート定数 ======== */
#define GDC_STATUS      0xA0    /* GDCステータスリード */
#define VSYNC_BIT       0x20    /* bit5: 垂直同期中 */
#define HBLANK_BIT      0x40    /* bit6: 水平帰線中 */

#define PAL_IDX         0xA8    /* パレット番号 */
#define PAL_GREEN       0xAA    /* 緑輝度 (0-15) */
#define PAL_RED         0xAC    /* 赤輝度 (0-15) */
#define PAL_BLUE        0xAE    /* 青輝度 (0-15) */

/* ======== 画面定数 ======== */
#define SCREEN_W        640
#define SCREEN_H        400
#define BAND_COUNT      16     /* パレット帯の数 (16色分) */
#define BAND_HEIGHT     (SCREEN_H / BAND_COUNT)  /* 25ライン/帯 */

/* ======== ラスタ分割定数 ======== */
#define MAX_SPLITS      8

/* ======== デモモード ======== */
#define DEMO_2SPLIT     0
#define DEMO_8SPLIT     1
#define DEMO_ANIMATE    2
#define DEMO_GRADIENT   3      /* 1ラインごとのグラデーション (400色) */
#define DEMO_COUNT      4

/* ======== グラデーション用パレット番号 ======== */
#define GRAD_PAL_IDX    1      /* パレット1をグラデーション用に使う */

/* ======== パレットセット構造体 ======== */
typedef struct {
    u8 r[16];
    u8 g[16];
    u8 b[16];
} PaletteSet;

/* ======== グラデーションテーブル (100エントリ: 4ライン単位) ======== */
#define GRAD_STEPS      (SCREEN_H / 4)  /* 100段階 */
static u8 grad_r[GRAD_STEPS];
static u8 grad_g[GRAD_STEPS];
static u8 grad_b[GRAD_STEPS];

/* ======== 画面モード ======== */
#define SCREEN_BANDS    0      /* 16色カラーバンド */
#define SCREEN_SOLID    1      /* 単色塗りつぶし (グラデーション用) */
static int current_screen;

/* ======== インラインアセンブリ (ラスタ分割専用) ======== */
/* ラスタ分割中のタイミングクリティカルなI/O操作に使用。
 * 通常の描画・パレット操作にはlibos32gfx / KAPIを使用する。 */

static inline void r_outb(unsigned int port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"((unsigned short)port));
}

static inline unsigned char r_inb(unsigned int port)
{
    unsigned char ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"((unsigned short)port));
    return ret;
}

static inline void r_cli(void)
{
    __asm__ volatile("cli");
}

static inline void r_sti(void)
{
    __asm__ volatile("sti");
}

/* ======== ラスタ分割用パレット操作 (I/O直接) ======== */
/* HBLANK中に高速でパレット16色を書き換えるため、
 * KAPI経由では間に合わない。直接outbで書き込む。 */

static void pal_set_all_hw(const PaletteSet *ps)
{
    int i;
    for (i = 0; i < 16; i++) {
        r_outb(PAL_IDX, (u8)i);
        r_outb(PAL_GREEN, ps->g[i] & 0x0F);
        r_outb(PAL_RED, ps->r[i] & 0x0F);
        r_outb(PAL_BLUE, ps->b[i] & 0x0F);
    }
}

/* ======== 同期関数 ======== */

/* VSYNC終了→開始の立ち上がりを待つ */
static void wait_vsync(void)
{
    /* まず VSYNC 中なら終了を待つ */
    while (r_inb(GDC_STATUS) & VSYNC_BIT);
    /* VSYNC 開始を待つ */
    while (!(r_inb(GDC_STATUS) & VSYNC_BIT));
}

/* HBLANKの1回の立ち上がりを待つ (=1ライン分) */
static void wait_hblank(void)
{
    /* HBLANK 中なら終了を待つ */
    while (r_inb(GDC_STATUS) & HBLANK_BIT);
    /* HBLANK 開始を待つ */
    while (!(r_inb(GDC_STATUS) & HBLANK_BIT));
}

/* n ライン分のHBLANKをカウント */
static void wait_lines(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        wait_hblank();
    }
}

/* ======== 描画 (libos32gfx 使用) ======== */

/* 画面全体に16色の横帯を描く (パレット番号 = band) */
static void draw_color_bands(void)
{
    int band;
    for (band = 0; band < BAND_COUNT; band++) {
        gfx_fill_rect(0, band * BAND_HEIGHT, SCREEN_W, BAND_HEIGHT, (u8)band);
    }
    gfx_present();
    gfx_api->gfx_present_dirty();
    current_screen = SCREEN_BANDS;
}

/* 画面全体を単一パレット番号で塗りつぶす (グラデーション用) */
static void draw_solid(u8 color)
{
    gfx_clear(0);
    gfx_fill_rect(0, 0, SCREEN_W, SCREEN_H, color);
    gfx_present();
    gfx_api->gfx_present_dirty();
    current_screen = SCREEN_SOLID;
}

/* ======== パレットデータ生成 ======== */

/* HSV→RGB変換 (H: 0-360, S/V: 0-15) → R/G/B: 0-15 */
static void hsv_to_rgb(int h, int s, int v, u8 *r, u8 *g, u8 *b)
{
    int hi, f, p, q, t;

    if (s == 0) {
        *r = *g = *b = (u8)v;
        return;
    }

    hi = (h / 60) % 6;
    f = h % 60;
    p = v * (15 - s) / 15;
    q = v * (15 - (s * f / 60)) / 15;
    t = v * (15 - (s * (60 - f) / 60)) / 15;

    switch (hi) {
        case 0: *r = (u8)v; *g = (u8)t; *b = (u8)p; break;
        case 1: *r = (u8)q; *g = (u8)v; *b = (u8)p; break;
        case 2: *r = (u8)p; *g = (u8)v; *b = (u8)t; break;
        case 3: *r = (u8)p; *g = (u8)q; *b = (u8)v; break;
        case 4: *r = (u8)t; *g = (u8)p; *b = (u8)v; break;
        default:*r = (u8)v; *g = (u8)p; *b = (u8)q; break;
    }
}

/* デモ1用: 暖色系パレット (上半分) */
static void make_warm_palette(PaletteSet *ps)
{
    int i;
    for (i = 0; i < 16; i++) {
        /* 赤→橙→黄のグラデーション (H: 0～60) */
        hsv_to_rgb(i * 4, 15, 12 + (i > 8 ? 3 : 0), &ps->r[i], &ps->g[i], &ps->b[i]);
    }
    /* パレット0 は黒 (背景) */
    ps->r[0] = ps->g[0] = ps->b[0] = 0;
}

/* デモ1用: 寒色系パレット (下半分) */
static void make_cool_palette(PaletteSet *ps)
{
    int i;
    for (i = 0; i < 16; i++) {
        /* 青→紫→水色のグラデーション (H: 200～300) */
        hsv_to_rgb(200 + i * 7, 15, 12 + (i > 8 ? 3 : 0), &ps->r[i], &ps->g[i], &ps->b[i]);
    }
    /* パレット0 は黒 (背景) */
    ps->r[0] = ps->g[0] = ps->b[0] = 0;
}

/* デモ2/3用: 虹色グラデーションパレット (h_offset でスタート色を指定) */
static void make_rainbow_palette(PaletteSet *ps, int h_offset)
{
    int i, h;
    for (i = 0; i < 16; i++) {
        h = (h_offset + i * 22) % 360;
        hsv_to_rgb(h, 14, 15, &ps->r[i], &ps->g[i], &ps->b[i]);
    }
}

/* デモ4用: 200段階のグラデーションテーブル生成 (虹色, 2ライン単位) */
static void make_gradient_table(void)
{
    int i, h;
    for (i = 0; i < GRAD_STEPS; i++) {
        h = i * 360 / GRAD_STEPS;
        hsv_to_rgb(h, 15, 15, &grad_r[i], &grad_g[i], &grad_b[i]);
    }
}

/* ======== テキスト表示ヘルパー ======== */

/* テキストVRAMに1行テキストを表示 */
static void show_text(int x, int y, const char *str, u8 attr)
{
    while (*str) {
        gfx_api->tvram_putchar_at(x, y, *str, attr);
        str++;
        x++;
    }
}

/* ======== デモモード表示テキスト更新 ======== */
static void update_demo_text(int demo_mode)
{
    gfx_api->tvram_clear();
    show_text(2, 0, "Raster Palette Demo - ESC:Exit  <->:Switch", ATTR_WHITE);
    switch (demo_mode) {
    case DEMO_2SPLIT:
        show_text(2, 1, "Demo 1: 2-Split (32 colors)", ATTR_CYAN);
        break;
    case DEMO_8SPLIT:
        show_text(2, 1, "Demo 2: 8-Split Rainbow Gradient", ATTR_CYAN);
        break;
    case DEMO_ANIMATE:
        show_text(2, 1, "Demo 3: Animated Rainbow (128 colors)", ATTR_CYAN);
        break;
    case DEMO_GRADIENT:
        show_text(2, 1, "Demo 4: Per-Line Gradient (400 colors!)", ATTR_CYAN);
        break;
    }
}

/* ======== デモモード切替時の画面再描画 ======== */
static void setup_screen_for_demo(int demo_mode)
{
    if (demo_mode == DEMO_GRADIENT) {
        /* グラデーションモード: 画面全体をパレット1で塗る */
        if (current_screen != SCREEN_SOLID) {
            draw_solid(GRAD_PAL_IDX);
        }
    } else {
        /* その他のモード: 16色カラーバンド */
        if (current_screen != SCREEN_BANDS) {
            draw_color_bands();
        }
    }
}

/* ======== メインプログラム ======== */
void main(int argc, char **argv, KernelAPI *api)
{
    int demo_mode;
    int running;
    int anim_frame;
    int splits;
    int lines_per_split;
    int grad_offset;

    PaletteSet pal_top, pal_bot;
    PaletteSet pal_sections[MAX_SPLITS];
    GFX_RasterPalTable raster_table;

    (void)argc;
    (void)argv;

    /* libos32gfx 初期化 (gfx_init + Surface/Spriteプール初期化) */
    libos32gfx_init(api);

    /* バックバッファにカラー帯描画 (libos32gfx経由) */
    draw_color_bands();

    running = 1;
    demo_mode = DEMO_2SPLIT;
    anim_frame = 0;
    grad_offset = 0;

    /* 初期パレットデータ生成 */
    make_warm_palette(&pal_top);
    make_cool_palette(&pal_bot);

    /* グラデーションテーブルを事前計算 */
    make_gradient_table();

    /* テキスト表示 */
    update_demo_text(demo_mode);

    while (running) {
        int ch;

        switch (demo_mode) {
        case DEMO_2SPLIT:
            /* ======== デモ1: 2分割 (上200ライン / 下200ライン) ======== */
            wait_vsync();
            r_cli();

            wait_lines(8);

            /* 上半分パレットセット (暖色) */
            pal_set_all_hw(&pal_top);

            /* ライン200まで待機 */
            wait_lines(192);

            /* 下半分パレットセット (寒色) */
            pal_set_all_hw(&pal_bot);

            r_sti();
            break;

        case DEMO_8SPLIT:
            /* ======== デモ2: 8分割グラデーション ======== */
            splits = 8;
            lines_per_split = SCREEN_H / splits;

            {
                int s;
                for (s = 0; s < splits; s++) {
                    make_rainbow_palette(&pal_sections[s], s * 45);
                }
            }

            wait_vsync();
            r_cli();
            wait_lines(8);

            {
                int s;
                for (s = 0; s < splits; s++) {
                    pal_set_all_hw(&pal_sections[s]);
                    if (s < splits - 1) {
                        wait_lines(lines_per_split);
                    }
                }
            }

            r_sti();
            break;

        case DEMO_ANIMATE:
            /* ======== デモ3: 虹アニメーション (8分割) ======== */
            splits = 8;
            lines_per_split = SCREEN_H / splits;

            {
                int s;
                for (s = 0; s < splits; s++) {
                    make_rainbow_palette(&pal_sections[s],
                                         (s * 45 + anim_frame * 3) % 360);
                }
            }
            anim_frame++;

            wait_vsync();
            r_cli();
            wait_lines(8);

            {
                int s;
                for (s = 0; s < splits; s++) {
                    pal_set_all_hw(&pal_sections[s]);
                    if (s < splits - 1) {
                        wait_lines(lines_per_split);
                    }
                }
            }

            r_sti();
            break;

        case DEMO_GRADIENT:
            /* ======== デモ4: ラスタパレットグラデーション (200色) ======== */
            /* 画面全体がパレット1で塗られている状態で、                     */
            /* KAPI gfx_present_raster() を使い、VRAM転送ループ内で          */
            /* HBLANK同期でパレット1のRGB値を2ライン単位に書き換える。        */
            /* NP21/Wのrasterdraw (2ライン単位) と整合する設計。              */

            {
                int step, idx;
                gfx_raster_clear(&raster_table);
                for (step = 0; step < GRAD_STEPS; step++) {
                    idx = (step + grad_offset) % GRAD_STEPS;
                    gfx_raster_add(&raster_table, step * 4,
                                   GRAD_PAL_IDX,
                                   grad_r[idx], grad_g[idx], grad_b[idx]);
                }
            }
            grad_offset = (grad_offset + 1) % GRAD_STEPS;

            /* KAPI経由: HBLANK同期パレット書き換えのみ (VRAM転送不要) */
            gfx_present_raster_only(&raster_table);
            break;
        }

        /* キー入力チェック */
        while ((ch = gfx_api->kbd_trygetchar()) >= 0) {
            if (ch == 0x1B) {
                /* ESC: 終了 */
                running = 0;
            } else if (ch == 0x1C || ch == 'd' || ch == 'D') {
                /* 右キー or D: 次のデモ */
                demo_mode = (demo_mode + 1) % DEMO_COUNT;
                anim_frame = 0;
                grad_offset = 0;
                setup_screen_for_demo(demo_mode);
                update_demo_text(demo_mode);
            } else if (ch == 0x1D || ch == 'a' || ch == 'A') {
                /* 左キー or A: 前のデモ */
                demo_mode = (demo_mode + DEMO_COUNT - 1) % DEMO_COUNT;
                anim_frame = 0;
                grad_offset = 0;
                setup_screen_for_demo(demo_mode);
                update_demo_text(demo_mode);
            }
        }
    }

    /* ======== クリーンアップ ======== */

    /* パレット復帰 (KAPI経由: カーネルのシャドウパレットと同期) */
    {
        static const u8 def_r[] = {0,0,7,7,0,0,7,7,0,0,15,15,0,0,15,15};
        static const u8 def_g[] = {0,0,0,0,7,7,7,7,0,0, 0, 0,15,15,15,15};
        static const u8 def_b[] = {0,7,0,7,0,7,0,7,0,15,0,15,0,15,0,15};
        int i;
        for (i = 0; i < 16; i++) {
            gfx_api->gfx_set_palette(i, def_r[i], def_g[i], def_b[i]);
        }
    }

    /* バックバッファクリア (libos32gfx経由) → VRAM転送 */
    gfx_clear(0);
    gfx_present();
    gfx_api->gfx_present_dirty();

    /* libos32gfx 終了 (gfx_shutdown含む) */
    libos32gfx_shutdown();
    gfx_api->tvram_clear();
}
