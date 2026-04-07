/* ======================================================================== */
/*  GFX_TEST.C — グラフィック描画 総合テスト (OS32移植版)                   */
/*                                                                          */
/*  glib_openwatcom/test/GTEST_GFX.C をOS32の gfx.h API用に移植            */
/*  レイアウト: 640x400 に収まる左右2列構成                                 */
/*                                                                          */
/*  スペースキーで一時停止 (スクリーンショット用)                           */
/*  ESCで終了                                                               */
/* ======================================================================== */

#include "gfx.h"
#include "kbd.h"

/* ======== 定数 ======== */
#define LABEL_X  8

/* ======== ユーティリティ ======== */

/* 一時停止: スペースで停止→任意キーで再開、ESC=-1で終了 */
static int wait_or_pause(void)
{
    int ch;
    for (;;) {
        ch = kbd_trygetchar();
        if (ch < 0) return 0;       /* キーなし → 続行 */
        if (ch == 0x1B) return -1;  /* ESC → 終了 */
        if (ch == ' ') {
            /* 一時停止: 次のキー入力まで待つ */
            while ((ch = kbd_trygetchar()) < 0) {
                __asm__ volatile("hlt");
            }
            if (ch == 0x1B) return -1;
            return 0;
        }
    }
}

/* 簡易文字描画 (8x8ビットマップフォント、ASCII限定) */
static const u8 mini_font[][8] = {
    /* 32: スペース */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33: ! */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 34: " */ {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35: # */ {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},
    /* 36: $ */ {0x18,0x7E,0x58,0x7E,0x1A,0x7E,0x18,0x00},
    /* 37: % */ {0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00},
    /* 38: & */ {0x38,0x44,0x38,0x3A,0x44,0x3A,0x00,0x00},
    /* 39: ' */ {0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40: ( */ {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    /* 41: ) */ {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    /* 42: * */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 43: + */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 44: , */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    /* 45: - */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 46: . */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 47: / */ {0x02,0x04,0x08,0x10,0x20,0x40,0x00,0x00},
    /* 48: 0 */ {0x3C,0x46,0x4A,0x52,0x62,0x3C,0x00,0x00},
    /* 49: 1 */ {0x18,0x38,0x18,0x18,0x18,0x7E,0x00,0x00},
    /* 50: 2 */ {0x3C,0x42,0x02,0x3C,0x40,0x7E,0x00,0x00},
    /* 51: 3 */ {0x3C,0x42,0x0C,0x02,0x42,0x3C,0x00,0x00},
    /* 52: 4 */ {0x08,0x18,0x28,0x48,0x7E,0x08,0x00,0x00},
    /* 53: 5 */ {0x7E,0x40,0x7C,0x02,0x42,0x3C,0x00,0x00},
    /* 54: 6 */ {0x3C,0x40,0x7C,0x42,0x42,0x3C,0x00,0x00},
    /* 55: 7 */ {0x7E,0x02,0x04,0x08,0x10,0x10,0x00,0x00},
    /* 56: 8 */ {0x3C,0x42,0x3C,0x42,0x42,0x3C,0x00,0x00},
    /* 57: 9 */ {0x3C,0x42,0x42,0x3E,0x02,0x3C,0x00,0x00},
    /* 58: : */ {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    /* 59: ; */ {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    /* 60: < */ {0x06,0x18,0x60,0x18,0x06,0x00,0x00,0x00},
    /* 61: = */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    /* 62: > */ {0x60,0x18,0x06,0x18,0x60,0x00,0x00,0x00},
    /* 63: ? */ {0x3C,0x42,0x04,0x08,0x00,0x08,0x00,0x00},
    /* 64: @ */ {0x3C,0x42,0x5E,0x56,0x5E,0x40,0x3C,0x00},
    /* 65: A */ {0x3C,0x42,0x42,0x7E,0x42,0x42,0x00,0x00},
    /* 66: B */ {0x7C,0x42,0x7C,0x42,0x42,0x7C,0x00,0x00},
    /* 67: C */ {0x3C,0x42,0x40,0x40,0x42,0x3C,0x00,0x00},
    /* 68: D */ {0x78,0x44,0x42,0x42,0x44,0x78,0x00,0x00},
    /* 69: E */ {0x7E,0x40,0x7C,0x40,0x40,0x7E,0x00,0x00},
    /* 70: F */ {0x7E,0x40,0x7C,0x40,0x40,0x40,0x00,0x00},
    /* 71: G */ {0x3C,0x42,0x40,0x4E,0x42,0x3C,0x00,0x00},
    /* 72: H */ {0x42,0x42,0x7E,0x42,0x42,0x42,0x00,0x00},
    /* 73: I */ {0x7E,0x18,0x18,0x18,0x18,0x7E,0x00,0x00},
    /* 74: J */ {0x1E,0x02,0x02,0x42,0x42,0x3C,0x00,0x00},
    /* 75: K */ {0x44,0x48,0x70,0x48,0x44,0x42,0x00,0x00},
    /* 76: L */ {0x40,0x40,0x40,0x40,0x40,0x7E,0x00,0x00},
    /* 77: M */ {0x42,0x66,0x5A,0x42,0x42,0x42,0x00,0x00},
    /* 78: N */ {0x42,0x62,0x52,0x4A,0x46,0x42,0x00,0x00},
    /* 79: O */ {0x3C,0x42,0x42,0x42,0x42,0x3C,0x00,0x00},
    /* 80: P */ {0x7C,0x42,0x42,0x7C,0x40,0x40,0x00,0x00},
    /* 81: Q */ {0x3C,0x42,0x42,0x4A,0x44,0x3A,0x00,0x00},
    /* 82: R */ {0x7C,0x42,0x42,0x7C,0x44,0x42,0x00,0x00},
    /* 83: S */ {0x3C,0x40,0x3C,0x02,0x42,0x3C,0x00,0x00},
    /* 84: T */ {0x7E,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
    /* 85: U */ {0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00},
    /* 86: V */ {0x42,0x42,0x42,0x42,0x24,0x18,0x00,0x00},
    /* 87: W */ {0x42,0x42,0x42,0x5A,0x66,0x42,0x00,0x00},
    /* 88: X */ {0x42,0x24,0x18,0x18,0x24,0x42,0x00,0x00},
    /* 89: Y */ {0x42,0x42,0x24,0x18,0x18,0x18,0x00,0x00},
    /* 90: Z */ {0x7E,0x04,0x08,0x10,0x20,0x7E,0x00,0x00},
};

/* 8x8フォントで1文字描画 */
static void draw_char(int x, int y, char ch, u8 color)
{
    int idx, row, col;
    const u8 *glyph;

    if (ch >= 'a' && ch <= 'z') ch -= 32;  /* 小文字→大文字 */
    if (ch < 32 || ch > 90) return;
    idx = ch - 32;
    if (idx >= (int)(sizeof(mini_font) / sizeof(mini_font[0]))) return;

    glyph = mini_font[idx];
    for (row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx_pixel(x + col, y + row, color);
            }
        }
    }
}

/* 文字列描画 (gfx_demo.cからも使用) */
void draw_text(int x, int y, const char *str, u8 color)
{
    while (*str) {
        draw_char(x, y, *str, color);
        x += 8;
        str++;
    }
}

/* ======================================================================== */
/*  テスト関数群 — 左列 (x=0-310), 右列 (x=320-640)                       */
/*  各テストは y 範囲を明確に管理して 400ライン内に収める                   */
/* ======================================================================== */

/* ======== T1: 基本プリミティブ (左上, y=0-44) ======== */
static void test_primitives(void)
{
    int i;

    draw_text(LABEL_X, 0, "T1: PRIMITIVES", 15);

    /* ドット */
    for (i = 0; i < 32; i++)
        gfx_pixel(20 + i * 2, 12 + (i & 7), 15);

    /* 直線 */
    gfx_line(20, 24, 100, 24, 14);
    gfx_line(20, 24, 20, 42, 14);
    gfx_line(20, 24, 100, 42, 14);

    /* 塗り矩形 */
    gfx_fill_rect(120, 12, 28, 30, 9);
    gfx_fill_rect(152, 12, 28, 30, 11);
    gfx_fill_rect(184, 12, 28, 30, 12);
}

/* ======== T2: H/Vライン (左, y=48-78) ======== */
static void test_lines(void)
{
    int i;

    draw_text(LABEL_X, 48, "T2: H/VLINE", 15);

    for (i = 0; i < 8; i++) {
        gfx_hline(20 + i * 28, 60 + i * 2, 22, (u8)(i + 1));
    }

    for (i = 0; i < 8; i++) {
        gfx_vline(268 + i * 6, 56, 22, (u8)(i + 8));
    }
}

/* ======== T3: パレット (左, y=82-108) ======== */
static void test_palette(void)
{
    int i;

    draw_text(LABEL_X, 82, "T3: PALETTE", 15);

    for (i = 0; i < 16; i++) {
        gfx_fill_rect(20 + i * 19, 94, 17, 14, (u8)i);
    }
}

/* ======== T4: 矩形 (左, y=112-158) ======== */
static void test_rects(void)
{
    draw_text(LABEL_X, 112, "T4: RECT", 15);

    gfx_rect(20, 124, 54, 32, 15);
    gfx_rect(22, 126, 50, 28, 9);
    gfx_fill_rect(28, 130, 18, 14, 10);
    gfx_fill_rect(50, 130, 18, 14, 12);

    gfx_rect(90, 124, 70, 32, 14);
    gfx_fill_rect(96, 130, 58, 20, 1);
    gfx_rect(102, 136, 46, 8, 15);
}

/* ======== T5: 直線パターン (右上, y=0-78) ======== */
static void test_line_pattern(void)
{
    int i;

    draw_text(330, 0, "T5: LINES", 15);

    /* 放射状 */
    for (i = 0; i < 16; i++) {
        gfx_line(480, 68, 480 + i * 8 - 64, 12, (u8)(i % 16));
    }

    /* 交差 */
    for (i = 0; i < 10; i++) {
        gfx_line(340, 12 + i * 5, 440, 62 - i * 5, (u8)((i + 1) % 16));
    }
}

/* ======== T6: スプライト (右中, y=82-140) ======== */

static const u8 test_sprite[16][16] = {
    {0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0},
    {0,0,0,0,2,2,2,2,2,2,2,2,0,0,0,0},
    {0,0,0,0,4,4,4,6,6,0,6,0,0,0,0,0},
    {0,0,0,4,6,4,6,6,6,0,6,6,6,0,0,0},
    {0,0,0,4,6,4,4,6,6,6,0,6,6,0,0,0},
    {0,0,0,0,4,6,6,6,4,4,4,0,0,0,0,0},
    {0,0,0,0,0,4,4,4,4,4,0,0,0,0,0,0},
    {0,0,0,0,2,2,1,2,2,2,0,0,0,0,0,0},
    {0,0,0,2,2,2,1,2,2,1,2,2,0,0,0,0},
    {0,0,2,2,2,2,1,1,1,1,2,2,2,0,0,0},
    {0,0,4,4,2,1,4,1,1,4,1,2,4,0,0,0},
    {0,0,4,4,4,1,1,1,1,1,1,4,4,0,0,0},
    {0,0,4,4,1,1,1,1,1,1,1,1,4,0,0,0},
    {0,0,0,0,1,1,1,0,0,1,1,1,0,0,0,0},
    {0,0,0,4,4,4,0,0,0,0,4,4,4,0,0,0},
    {0,0,4,4,4,4,0,0,0,0,4,4,4,4,0,0}
};

static void test_sprite_draw(void)
{
    GFX_Surface *surf;
    GFX_Sprite *spr;
    int i, x, y;

    draw_text(330, 82, "T6: SPRITE", 15);

    surf = gfx_create_surface(16, 16);
    if (!surf) return;

    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            gfx_surface_pixel(surf, x, y, test_sprite[y][x]);
        }
    }

    spr = gfx_create_sprite(surf, 0);

    /* ブリット (不透過) */
    draw_text(340, 96, "BLIT:", 7);
    for (i = 0; i < 4; i++) {
        gfx_blit(392 + i * 20, 96, surf, 0);
    }

    /* スプライト (透過) */
    if (spr) {
        draw_text(340, 118, "SPR:", 7);
        gfx_fill_rect(392, 118, 96, 16, 1);
        for (i = 0; i < 4; i++) {
            gfx_draw_sprite(392 + i * 20, 118, spr);
        }
        gfx_free_sprite(spr);
    }

    gfx_free_surface(surf);
}

/* ======== T7: 全画面直線 (下半分, y=166-380) ======== */
static void test_diagonals(void)
{
    int i;

    draw_text(LABEL_X, 166, "T7: DIAGONAL", 15);

    gfx_line(0, 180, 639, 374, 14);     /* 黄 */
    gfx_line(639, 180, 0, 374, 13);     /* 水色 */

    /* 放射状パターン */
    for (i = 0; i < 16; i++) {
        gfx_line(320, 374, i * 42, 180, (u8)(i % 16));
    }
}

/* ======== ステータスバー (y=384-399) ======== */
static void draw_status(void)
{
    gfx_fill_rect(0, 384, 640, 16, 1);
    draw_text(8, 388, "SP:PAUSE  ESC:EXIT  OS32 GFX TEST", 15);
}

/* ======================================================================== */
/*  メイン                                                                  */
/* ======================================================================== */

void gfx_run_test(void)
{
    gfx_init();

    test_primitives();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_lines();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_palette();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_rects();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_line_pattern();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_sprite_draw();
    gfx_present();
    if (wait_or_pause() < 0) goto done;

    test_diagonals();
    draw_status();
    gfx_present();

    /* 最終待機 */
    for (;;) {
        int ch = kbd_trygetchar();
        if (ch == 0x1B || (ch >= 0 && ch != ' ')) break;
        if (ch == ' ') {
            while (kbd_trygetchar() < 0) { __asm__ volatile("hlt"); }
        }
        __asm__ volatile("hlt");
    }

done:
    gfx_shutdown();
}
