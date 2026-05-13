/* ======================================================================== */
/*  V86_MEM.H — V86 メモリ空間管理ヘッダ                                    */
/* ======================================================================== */

#ifndef V86_MEM_H
#define V86_MEM_H

#include "types.h"

/* V86バッキングRAM: pgalloc から動的確保される連続 640KB                     */
/* v86_backing_phys は v86_mem_setup() で設定され、teardown() でリセットされる */
extern u32 v86_backing_phys;

#define V86_BACKING_SIZE   0x0A0000UL   /* 640KB */
#define V86_BACKING_PAGES  (V86_BACKING_SIZE / 0x1000UL) /* 160ページ */
#define V86_REMAP_END      0x08F000UL   /* リマップ範囲上限 (カーネルスタック手前) */

/* V86メモリ空間を構築 (ページテーブル + IVT + BDA + I/Oビットマップ) */
void v86_mem_setup(void);

/* V86メモリ空間を解放・復元 */
void v86_mem_teardown(void);

/* V86仮想アドレス (seg:off) をカーネル用リニアアドレスに変換 */
u8 *v86_phys_addr(u32 seg, u32 off);

/* DOS→OS32復帰時の画面リストア (GRCG/EGC OFF、パレット/GDCリセット) */
void v86_restore_screen(void);

/* §1.7 TVRAM (0xA0000-0xA1FFF, 8KB) 退避・復元
 * v86_mem_setup 前に save、v86_restore_screen 後に restore を呼ぶ */
void v86_tvram_save(void);
void v86_tvram_restore(void);

/* ======================================================================== */
/*  A20ラインエミュレーション                                                */
/*                                                                          */
/*  V86モードではCPUのA20が常に有効なため、リアルモードのラップアラウンド     */
/*  (0xFFFFF→0x00000) をページテーブルレベルでエミュレートする。             */
/*                                                                          */
/*  方式: PDE[0]用のPTをコピーし、HMAエリア (PTE[0x100-0x10F]) を            */
/*  バッキングRAMの先頭64KBにマッピングする。                                */
/* ======================================================================== */

/* A20状態を設定する (0=OFF:ラップ, 1=ON:通常) */
void v86_a20_set(int enable);

/* A20状態を取得する */
int  v86_a20_get(void);

#endif /* V86_MEM_H */
