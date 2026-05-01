/* ======================================================================== */
/*  V86_MEM.H — V86 メモリ空間管理ヘッダ                                    */
/* ======================================================================== */

#ifndef V86_MEM_H
#define V86_MEM_H

#include "types.h"

/* V86バッキングRAM物理ベースアドレス */
#define V86_BACKING_PHYS   0x300000UL
#define V86_BACKING_SIZE   0x0A0000UL   /* 640KB */
#define V86_REMAP_END      0x08F000UL   /* リマップ範囲上限 (カーネルスタック手前) */

/* V86メモリ空間を構築 (ページテーブル + IVT + BDA + I/Oビットマップ) */
void v86_mem_setup(void);

/* V86メモリ空間を解放・復元 */
void v86_mem_teardown(void);

/* V86仮想アドレス (seg:off) をカーネル用リニアアドレスに変換 */
u8 *v86_phys_addr(u32 seg, u32 off);

/* DOS→OS32復帰時の画面リストア (GRCG/EGC OFF、パレット/GDCリセット) */
void v86_restore_screen(void);

#endif /* V86_MEM_H */
