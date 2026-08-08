/* ======================================================================== */
/*  V86_MEM.H — V86 ゲスト用アドレス空間                                    */
/*                                                                          */
/*  ゲストには 16bit 実行環境らしい低位 1MB を見せる必要があるが、OS32 は    */
/*  そこを既に使っている (フォントキャッシュ 292KB / Unicode 表 128KB /      */
/*  GFX バックバッファ 128KB / カーネルスタック 64KB)。                     */
/*                                                                          */
/*  そこで pgalloc から連続物理メモリを別途確保し、セッション中だけ         */
/*  低位アドレスをそこへリマップする。OS32 のデータは物理的には無傷のまま   */
/*  で、teardown でアイデンティティマッピングに戻せば元通りになる。         */
/*                                                                          */
/*  VRAM (0xA0000-0xEFFFF) はリマップせず実物理をそのまま見せる。           */
/*  実測で Ys は GVRAM へ CPU で直接書き込んで描画しており、GDC/GRCG/EGC の  */
/*  コマンドポートを使っていなかった。VRAM を直マップすれば描画は           */
/*  メモリアクセスのままなので #GP が一切発生しない — V86 方式が            */
/*  386DX20 で成立する最大の理由 (docs/tasks/v86v2/03_ys_profile.md §3.3)。  */
/* ======================================================================== */

#ifndef __V86_MEM_H
#define __V86_MEM_H

#include "types.h"
#include "memmap.h"

/* リマップ範囲は 0x00000 から カーネルスタックガードの直前まで。
 *
 * カーネルスタック (0x90000-0x9FFFF) は絶対にリマップしてはならない。
 * V86 から #GP が入ると CPU はそのスタックにフレームを積むので、
 * 実行中に張り替えたらその瞬間に全てが壊れる。
 * 結果としてゲストから見える連続 RAM は 572KB になる。 */
#define V86_REMAP_START     0x00000UL
#define V86_REMAP_END       MEM_STACK_GUARD          /* 0x8F000 (含まず) */
#define V86_BACKING_PAGES   (V86_REMAP_END / PAGE_SIZE)   /* 143 ページ */

/* VRAM / ROM 帯 (リマップせず実物理を見せる) */
#define V86_VRAM_START      0xA0000UL
#define V86_VRAM_END        0xC0000UL   /* TVRAM/CG/GVRAM (含まず) */
#define V86_EXTROM_START    0xC0000UL
#define V86_EXTROM_END      0xE0000UL   /* 拡張 ROM (含まず) */
#define V86_GVRAM_E_START   0xE0000UL
#define V86_GVRAM_E_END     0xE8000UL   /* GVRAM プレーン E (含まず) */
#define V86_SOUNDROM_START  0xE8000UL
#define V86_SOUNDROM_END    0xF0000UL   /* サウンド BIOS ROM (含まず) */

/* ======== API ======== */

/* バッキング RAM を確保して低位アドレスをリマップする。
 * 0 で成功、負でエラー。 */
int  v86_mem_setup(void);

/* アイデンティティマッピングに戻し、バッキング RAM を解放する。 */
void v86_mem_teardown(void);

/* 確保済みバッキング RAM の物理先頭 (0 = 未確保)。デバッグ用。 */
u32  v86_mem_backing_phys(void);

#endif /* __V86_MEM_H */
