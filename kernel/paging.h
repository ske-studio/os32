/* ======================================================================== */
/*  PAGING.H — x86ページング (メモリ保護)                                   */
/*                                                                          */
/*  アイデンティティマッピング (仮想=物理) でページ保護を実現する。          */
/*  IVT/BIOSデータのRead-Only化、スタックガードページ、NULL保護等。          */
/* ======================================================================== */

#ifndef __PAGING_H
#define __PAGING_H

#include "types.h"

/* ページサイズ */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12

/* ページディレクトリ/テーブルのエントリ数 */
#define PDE_COUNT       1024
#define PTE_COUNT       1024

/* ページ属性ビット */
#define PTE_PRESENT     0x001   /* P   : 存在 */
#define PTE_RW          0x002   /* R/W : 書き込み可 */
#define PTE_USER        0x004   /* U/S : ユーザアクセス可 */
#define PTE_PWT         0x008   /* PWT : ライトスルー */
#define PTE_PCD         0x010   /* PCD : キャッシュ無効 */
#define PTE_ACCESSED    0x020   /* A   : アクセス済み */
#define PTE_DIRTY       0x040   /* D   : 書き込み済み */
#define PTE_PS          0x080   /* PS  : ページサイズ (PDE用, 4MB) */

/* よく使う組み合わせ */
#define PAGE_RW         (PTE_PRESENT | PTE_RW)       /* 読み書き可 */
#define PAGE_RO         (PTE_PRESENT)                 /* 読み取り専用 */
#define PAGE_NOT_PRESENT 0                            /* アクセス不可 */

/* マッピング範囲 (16MB = 4 ページテーブル) */
#define PAGING_MAP_SIZE (16UL * 1024UL * 1024UL)
#define PAGING_PT_COUNT (PAGING_MAP_SIZE / (PTE_COUNT * PAGE_SIZE))

/* ======== API ======== */

/* ページング初期化・有効化 */
void paging_init(u32 mem_kb);

/* 指定ページの属性を変更 */
void paging_set_page(u32 virt_addr, u32 phys_addr, u32 flags);

/* 指定範囲の全ページを Read-Only に */
void paging_set_readonly(u32 start, u32 end);

/* 指定範囲の全ページを Not-Present に */
void paging_set_not_present(u32 start, u32 end);

/* ページング有効かどうか */
int paging_enabled(void);

/* ======== ページディレクトリ切り替えAPI (Phase 2) ======== */

/* 子プロセス用ページディレクトリを構築する
 * phys_pages: マッピングする物理ページアドレスの配列
 * page_count: ページ数
 * virt_start: マッピング先仮想アドレス (通常 0x400000)
 * カーネル空間 (0x000000〜0x3FFFFF) はマスターPDと共有する
 * 戻り値: 構築したPDのポインタ (kmalloc確保), NULL=失敗 */
u32 *paging_create_pd(u32 *phys_pages, int page_count, u32 virt_start);

/* 子プロセス用ページディレクトリを破棄する (kfreeで解放) */
void paging_destroy_pd(u32 *pd);

/* CR3を切り替える (TLB全フラッシュ: CR3リロード方式, i386互換) */
void paging_switch_pd(u32 *pd);

/* マスター(カーネル)ページディレクトリを取得 */
u32 *paging_get_master_pd(void);


#endif /* __PAGING_H */
