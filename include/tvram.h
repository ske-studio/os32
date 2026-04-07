/* ======================================================================== */
/*  TVRAM.H — テキストVRAM操作関数宣言                                      */
/* ======================================================================== */

#ifndef __TVRAM_H
#define __TVRAM_H

#include "types.h"

/* テキストVRAM定数 (PC-98) */
#define TVRAM_COLS    80
#define TVRAM_ROWS    25
#define TVRAM_BASE    0xA0000UL
#define TVRAM_ATTR    0xA2000UL

/* 画面クリア */
void tvram_clear(void);

/* 1文字表示 */
void tvram_putchar_at(int x, int y, char ch, u8 color);

/* 漢字表示 (JISコード) */
void tvram_putkanji_at(int x, int y, u16 jis, u8 color);

/* 1行スクロール */
void tvram_scroll(void);

#endif /* __TVRAM_H */
