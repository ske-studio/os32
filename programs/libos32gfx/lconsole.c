/*
 * lconsole.c - OS32 Logical Console (差分仮想VRAMレイヤー)
 * C89 compatible
 */

#include "lconsole.h"
#include "libos32gfx.h"

/* 1セル = 32bit: [31:16] 属性, [15:0] 文字コードまたはJIS */
static unsigned int logical_vram[LCONS_H][LCONS_W];
static unsigned int physical_vram[LCONS_H][LCONS_W];

void lcons_init(void) {
    lcons_clear();
}

void lcons_clear(void) {
    int i, j;
    for (i = 0; i < LCONS_H; i++) {
        for (j = 0; j < LCONS_W; j++) {
            /* 属性 0x07 (標準色)、文字 ' ' */
            logical_vram[i][j] = ((unsigned int)0x07 << 16) | ' ';
            physical_vram[i][j] = 0xFFFFFFFF; /* Force sync at next time */
        }
    }
}

void lcons_putc(int x, int y, char ch, unsigned char attr) {
    if (x >= 0 && x < LCONS_W && y >= 0 && y < LCONS_H) {
        logical_vram[y][x] = ((unsigned int)attr << 16) | (unsigned char)ch;
    }
}

void lcons_putkanji(int x, int y, unsigned short jis, unsigned char attr) {
    if (x >= 0 && x < LCONS_W - 1 && y >= 0 && y < LCONS_H) {
        logical_vram[y][x] = ((unsigned int)attr << 16) | jis;
        /* 漢字の右半分は 0xFFFF を入れて連結を示す */
        logical_vram[y][x+1] = ((unsigned int)attr << 16) | 0xFFFF;
    }
}

void lcons_fill_rect(int start_x, int start_y, int w, int h, char ch, unsigned char attr) {
    int x, y;
    for (y = start_y; y < start_y + h; y++) {
        for (x = start_x; x < start_x + w; x++) {
            lcons_putc(x, y, ch, attr);
        }
    }
}

void lcons_sync_vram(void) {
    int y, x;

    for (y = 0; y < LCONS_H; y++) {
        int is_diff = 0;
        /* 行単位で差分チェック */
        for (x = 0; x < LCONS_W; x++) {
            if (logical_vram[y][x] != physical_vram[y][x]) {
                is_diff = 1;
                break;
            }
        }
        if (!is_diff) {
            continue;
        }

        /* 差分セルのグループ化 (連続する変更セルの背景を一括で塗り、文字を描画する) */
        {
            int start_x = -1;
            for (x = 0; x <= LCONS_W; x++) {
                if (x < LCONS_W && logical_vram[y][x] != physical_vram[y][x]) {
                    /* 漢字のテイル (0xFFFF) で差分が検出された場合、ヘッド(x-1)も含める */
                    unsigned int code = logical_vram[y][x] & 0xFFFF;
                    if (code == 0xFFFF) {
                        if (start_x == -1 && x > 0) start_x = x - 1; /* Expand block to left */
                    } else {
                        if (start_x == -1) start_x = x;
                    }
                } else {
                    if (start_x != -1) {
                        /* [start_x, x-1] の範囲が連続して変更されている */
                        int end_x = x;
                        int bx;
                        int run_start = start_x;
                        unsigned char cur_attr = (logical_vram[y][start_x] >> 16) & 0xFF;

                        /* 1. まずこの差分ブロックの背景を属性ランレングスで塗る */
                        for (bx = start_x; bx <= end_x; bx++) {
                            unsigned char attr = (bx < end_x) ? ((logical_vram[y][bx] >> 16) & 0xFF) : 0;
                            if (bx == end_x || attr != cur_attr) {
                                /* 互換性のため: VZの属性 0xE5 は反転(背景色 1, 前景 15)とみなす。
                                   それ以外は通常(背景 0) */
                                unsigned char bg_color = (cur_attr == 0xE5) ? 0x01 : 0x00;
                                gfx_fill_rect(run_start * 8, y * 16, (bx - run_start) * 8, 16, bg_color);
                                if (bx < end_x) {
                                    cur_attr = attr;
                                    run_start = bx;
                                }
                            }
                        }

                        /* 2. 背景を透過にして文字を描画 */
                        for (bx = start_x; bx < end_x; bx++) {
                            unsigned int val = logical_vram[y][bx];
                            unsigned char attr = (val >> 16) & 0xFF;
                            unsigned int code = val & 0xFFFF;
                            /* 前景色判定: 0xE5 は 0x0F(白), それ以外は 0x07(グレー) */
                            unsigned char fg_color = (attr == 0xE5) ? 0x0F : 0x07;

                            if (code == 0xFFFF) continue; /* 漢字の右側セルはスキップ */
                            if (code == 0x0020) continue; /* スペースは背景塗りのみで描画なし */

                            if (code > 0xFF) {
                                kcg_draw_kanji(bx * 8, y * 16, code, fg_color, 0xFF); /* bg_color=0xFF は透過の意 */
                                bx++; /* 漢字は2セル占有 */
                            } else if (code > 0) {
                                kcg_draw_ank(bx * 8, y * 16, code, fg_color, 0xFF);
                            }
                        }

                        /* 3. 状態の同期 */
                        for (bx = start_x; bx < end_x; bx++) {
                            physical_vram[y][bx] = logical_vram[y][bx];
                        }

                        start_x = -1;
                    }
                }
            }
        }
    }
}
