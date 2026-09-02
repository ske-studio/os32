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

/* ======================================================================== */
/*  リング3 アドレス空間 (M1b: PD 複製)                                     */
/*                                                                          */
/*  CPL=3 プログラムごとに独立したページディレクトリ (PD) を持たせるための   */
/*  基盤。カーネル帯域・SHM・VRAM・ホットデプロイ窓は全 PD で共有し          */
/*  (同一物理を指す)、0x400000 帯 (プログラム code/data/heap/stack) だけ     */
/*  アプリ固有のページテーブルに差し替える (CONTRACTS C2)。                  */
/*                                                                          */
/*  共有/非共有の境界は PDE 単位:                                            */
/*    PDE 0 (0x000000-0x3FFFFF) : 共有 — カーネル/SQLite/シェル/SHM/VRAM     */
/*    PDE 1 (0x400000-0x7FFFFF) : アプリ固有 — プログラム帯 (APP_BAND_PDE)   */
/*    PDE 2 (0x800000-0xBFFFFF) : 共有                                       */
/*    PDE 3 (0xC00000-0xFFFFFF) : 共有 — 物理末尾のホットデプロイ窓を含む     */
/*                                                                          */
/*  M1b の時点ではアプリ PT を master と同一の identity で初期化する          */
/*  (0x400000 帯も present/RW/identity)。これにより CPL=0 のまま CR3 を       */
/*  新 PD に載せてもカーネルは動き続ける (V1)。CPL=3 用の USER マッピングは    */
/*  M1c で overlay する。 */

/* 0x400000 帯を覆う PDE インデックス (= MEM_EXEC_LOAD_ADDR >> 22 = 1) */
#define APP_BAND_PDE   1

struct addrspace {
    u32 pd_phys;       /* 新 PD の物理アドレス (CR3 に載せる値)。0=無効 */
    u32 app_pt_phys;   /* 0x400000 帯アプリ PT の物理アドレス */
    u32 app_pde;       /* アプリ固有にした PDE インデックス */
};

/* カーネル (master) PD の物理アドレス。CR3 を戻すときに使う。 */
u32 paging_kernel_pd_phys(void);

/* 現在の CR3 (= 現在アクティブな PD 物理アドレス) を読む。 */
u32 paging_current_cr3(void);

/* CR3 に PD をロードする (= アドレス空間切り替え + TLB フラッシュ)。 */
void paging_load_cr3(u32 pd_phys);

/* アプリ用アドレス空間を 1 つ作る。
 * master の全 PDE をコピーしてカーネル帯域を共有し、0x400000 帯 (APP_BAND_PDE)
 * だけ新規確保したアプリ PT に差し替える。アプリ PT は M1b では master と
 * 同一の identity で初期化する。
 * PD/PT のバッキングは pgalloc から取る (pgalloc_init 済みが前提)。
 * 戻り値: 0=成功 (as を埋める), -1=物理ページ不足。 */
int paging_addrspace_create(struct addrspace *as);

/* アプリ用アドレス空間を破棄し PD/PT のバッキングページを解放する。
 * 破棄する PD がアクティブ (CR3) であってはならない — 先に
 * paging_load_cr3(paging_kernel_pd_phys()) で master へ戻すこと。 */
void paging_addrspace_destroy(struct addrspace *as);

/* アプリ AS の 1 ページを USER でマップする (M1c)。
 *   - virt が 0x400000 帯 (app_pde) なら、アプリ固有 PT に書く
 *     (このアプリの PD からしか見えない)。
 *   - それ以外の共有帯 (VRAM 0xA8000 / SHM 等、C2 で全 PD 共有 + USER と
 *     定めた領域) なら共有 PT の PTE に USER を立てる。共有 PT は master と
 *     同一だが、master 側の PDE には USER を伝播させないので (このアプリ PD
 *     の PDE コピーにだけ立てる)、カーネル/シェルから見た実効権限は
 *     supervisor のまま保たれる (PDE と PTE の論理積)。
 * flags に PTE_USER を含めること。戻り値 0=成功, -1=範囲外。 */
int paging_addrspace_map_user(struct addrspace *as, u32 virt, u32 phys,
                              u32 flags);

/* [vstart, vend) を identity (phys=virt) で USER マップする (M1c)。
 * end は exclusive。プログラム帯・ユーザスタック・VRAM・SHM に使う。
 * 戻り値 0=成功, -1=範囲の一部が範囲外 (範囲内分は適用済み)。 */
int paging_addrspace_map_user_range(struct addrspace *as, u32 vstart,
                                    u32 vend, u32 flags);

/* PD 複製の自己診断 (V1)。CPL=0 のまま:
 *   1. アプリ AS を作る
 *   2. CR3 を新 PD に載せてもカーネル (コード/スタック/データ) が生存する
 *   3. カーネル帯域の 1 語が master PD と新 PD で同一物理を指す (共有の証明)
 *   4. CR3 を master に戻し、AS を破棄する
 * 戻り値: 0=全通過。非0 はビットフラグで失敗内容を示す。
 * ブート時に kselftest_run() から呼ぶ想定 (pgalloc_init 後)。 */
int paging_pd_clone_selftest(void);

#endif /* __PAGING_H */
