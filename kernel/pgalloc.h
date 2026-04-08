/* ======================================================================== */
/*  PGALLOC.H — 物理ページフレームアロケータ                                 */
/*                                                                          */
/*  プログラム空間 (0x400000〜mem_end) の物理ページを                        */
/*  ビットマップ方式で管理する。                                              */
/*  exec_run() のネスト呼び出し時に、子プロセス用の物理ページを              */
/*  動的に確保・解放するために使用する。                                      */
/* ======================================================================== */

#ifndef __PGALLOC_H
#define __PGALLOC_H

#include "types.h"
#include "paging.h"

/* 管理対象の開始アドレス (プログラム空間) */
#define PGALLOC_BASE    0x400000UL

/* ======== API ======== */

/* 初期化 (paging_init の後に呼ぶ) */
void pgalloc_init(u32 mem_kb);

/* 1ページ(4KB)確保。戻り値: 物理アドレス, 0=失敗 */
u32  pgalloc_alloc_page(void);

/* 1ページ解放 */
void pgalloc_free_page(u32 phys_addr);

/* 連続nページ確保。戻り値: 先頭物理アドレス, 0=失敗 */
u32  pgalloc_alloc_n(int n);

/* 連続nページ解放 */
void pgalloc_free_n(u32 phys_addr, int n);

/* 指定範囲を「使用中」としてマーキング (Level 0のシェル領域等) */
void pgalloc_mark_used(u32 phys_start, int page_count);

/* 統計情報 */
u32  pgalloc_total_pages(void);
u32  pgalloc_free_pages(void);

#endif /* __PGALLOC_H */
