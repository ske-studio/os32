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

/* ページ境界アライメント (切り上げ/切り下げ)。
 * 手書きの (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1) イディオムはこれを使う。 */
#define PAGE_ALIGN_DOWN(x)  ((u32)(x) & ~(u32)(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)    (((u32)(x) + PAGE_SIZE - 1) & ~(u32)(PAGE_SIZE - 1))

/* ======== API ======== */

/* ページング初期化・有効化 */
void paging_init(u32 mem_kb);

/* 指定ページの属性を変更。
 * flags に PTE_USER を含めると PDE 側にも USER を伝播させる
 * (i386 の実効権限は PDE と PTE の論理積のため)。
 * 戻り値: 0=成功, -1=マッピング範囲外 (何も変更されない)。
 * ガードページ設置のような保護目的の呼び出しは必ず戻り値を確認すること
 * (無言 no-op だと保護が入らないまま fail-open になる)。 */
int paging_set_page(u32 virt_addr, u32 phys_addr, u32 flags);

/* 範囲マップ。[virt_start, virt_end) を phys_start からの連続物理へ
 * flags でマップする (end は exclusive)。TLB フラッシュは最後に 1 回。
 * 複数ページの属性変更/張り替えは paging_set_page のループでなく必ず
 * これを使うこと (ページごとの CR3 全リロードを避ける)。
 * 戻り値: 0=成功, -1=範囲の一部がマッピング範囲外 (範囲内分は適用済み) */
int paging_map_range(u32 virt_start, u32 virt_end, u32 phys_start, u32 flags);

/* 指定範囲を覆う PDE から USER を落とす (V86 セッション終了時の後始末)
 * 戻り値: 0=成功, -1=範囲全体がマッピング範囲外 */
int paging_pde_clear_user(u32 start, u32 end);

/* 指定範囲の全ページを Read-Only に。
 * 既存 PTE の物理フレームは保持する (V86 リマップ中でも壊さない)。
 * 戻り値: 0=成功, -1=範囲の一部がマッピング範囲外 (範囲内分は適用済み) */
int paging_set_readonly(u32 start, u32 end);

/* 指定範囲の全ページを Not-Present に。
 * PTE のフレーム/属性ビットは保持し P ビットのみ落とす。
 * 戻り値: 0=成功, -1=範囲の一部がマッピング範囲外 (範囲内分は適用済み) */
int paging_set_not_present(u32 start, u32 end);

/* ブート後のコンベンショナルメモリ再利用 (ページ0: NP, 0x1000-0x9FFFF: R/W) */
void paging_reclaim_conventional(void);

/* ページング有効かどうか */
int paging_enabled(void);

/* 指定アドレスのページがPresentかどうか (メモリダンプ安全チェック用) */
int paging_is_present(u32 virt_addr);

#endif /* __PAGING_H */
