/* ======================================================================== */
/*  PAGING.C — x86ページング (メモリ保護)                                   */
/*                                                                          */
/*  アイデンティティマッピング方式:                                         */
/*    仮想アドレス = 物理アドレス の1:1対応を維持し、                        */
/*    ページ属性(R/W, Present)でメモリ保護のみを追加する。                   */
/*    既存コード(VRAMアクセス等)を一切変更せずに保護が得られる。             */
/*                                                                          */
/*  構造:                                                                   */
/*    page_directory[1024]  — ページディレクトリ (4KB)                      */
/*    page_tables[4][1024]  — ページテーブル4枚 = 16MBカバー (16KB)         */
/*    合計BSS: ~40KB (アライメント用パディング含む)                         */
/*                                                                          */
/*  保護マップ (ブートアーキテクチャ改善後):                               */
/*                                                                          */
/*  [コンベンショナルメモリ]                                                */
/*    0x00000 - 0x00FFF : NP   (NULLポインタ検出, paging_reclaim後)          */
/*    0x01000 - 0x9FFFF : R/W  (フォント/Unicode/GFX, ブート後に再利用)      */
/*    0xA0000 - 0xEFFFF : R/W  (テキスト/グラフィックVRAM)                  */
/*    0xF0000 - 0xFFFFF : R/O  (BIOS ROM)                                   */
/*                                                                          */
/*  [拡張メモリ]                                                            */
/*    0x100000 - 0x1FAFFF : R/W  (カーネル帯域: code+heap+KAPI+SHM)       */
/*    0x1FB000 - 0x1FBFFF : NP   (カーネルスタックガード)                    */
/*    0x1FC000 - 0x1FFFFF : R/W  (カーネルスタック, 16KB)                    */
/*    0x200000 - 0x23FFFF : R/W  (SQLite帯域: code+BSS+代替スタック)      */
/*    0x240000 - 0x2FFFFF : NP   (カーネル予約)                              */
/*    0x300000 - 0x3FFFFF : R/W  (シェル常駐帯域, ガード付き)             */
/*    0x400000 - mem_end  : R/W  (プログラム空間, ガードページ付き)         */
/*    mem_end  - 0xFFFFFF : NP   (未実装メモリ)                             */
/* ======================================================================== */

#include "paging.h"

#include "io.h"
#include "pc98.h"
#include "memmap.h"

/* カーネルスタック帯のレイアウト不変条件。
 * ガードページはスタック直下に隣接し、スタックは 2MB 境界 (SQLite 帯域)
 * の手前で終わる。ずれると paging_init の R/W 強制やガード設定が
 * 意図しないページに掛かる。 */
STATIC_ASSERT(MEM_STACK_GUARD_END + 1 == MEM_KSTACK_BASE,
              kstack_guard_adjacent);
STATIC_ASSERT(MEM_KSTACK_TOP < 0x200000UL, kstack_below_sqlite_band);
STATIC_ASSERT((MEM_STACK_GUARD & (PAGE_SIZE - 1)) == 0,
              kstack_guard_page_aligned);

/* ======== ページテーブル (BSS配置, 4096バイトアライン必須) ======== */
/* Open Watcomでは __declspec(align(4096)) が使えないため、
 * 手動でアライメントを確保する。
 * 実際のテーブルサイズ + 4095バイトのパディングを確保し、
 * 4096境界に切り上げたアドレスを使用する。 */

static u8 pd_raw[4096 + 4095];      /* ページディレクトリ用生バッファ */
static u8 pt_raw[PAGING_PT_COUNT][4096 + 4095];  /* ページテーブル用生バッファ */

static u32 *page_directory;          /* アライン済みポインタ */
static u32 *page_tables[PAGING_PT_COUNT];

static int pg_enabled = 0;

/* 4096バイト境界に切り上げ */
static u32 *align4096(void *p)
{
    u32 addr = (u32)p;
    addr = (addr + 4095) & ~4095UL;
    return (u32 *)addr;
}

/* TLBフラッシュ (i386互換: CR3リロード方式)
 * invlpgはi486+なので使えない */
static void tlb_flush_all(void)
{
    u32 cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

/* ======================================================================== */
/*  paging_init — ページテーブル構築 + ページング有効化                      */
/* ======================================================================== */
void paging_init(u32 mem_kb)
{
    int i, j;
    u32 phys;
    u32 pd_phys;
    u32 max_mem_bytes = mem_kb * 1024; /* プローブされた実メモリ上限 */

    /* アライン済みポインタを取得 */
    page_directory = align4096(pd_raw);
    for (i = 0; i < PAGING_PT_COUNT; i++) {
        page_tables[i] = align4096(pt_raw[i]);
    }

    /* ページディレクトリ初期化: 全エントリをNot-Presentに */
    for (i = 0; i < PDE_COUNT; i++) {
        page_directory[i] = 0;
    }

    /* ページテーブル構築: 実装されている範囲のみR/W、超えた範囲はNot-Present */
    for (i = 0; i < PAGING_PT_COUNT; i++) {
        for (j = 0; j < PTE_COUNT; j++) {
            phys = (u32)(i * PTE_COUNT + j) * PAGE_SIZE;
            if (phys < max_mem_bytes || phys < MEM_1MB) {
                /* コンベンショナルメモリ(0-1MB)またはプローブ範囲内 */
                page_tables[i][j] = phys | PAGE_RW;
            } else {
                /* 未実装領域 */
                page_tables[i][j] = PAGE_NOT_PRESENT;
            }
        }
        /* ページディレクトリにテーブルを登録 */
        page_directory[i] = (u32)page_tables[i] | PAGE_RW;
    }

    /* ========================================================
     *  保護属性の設定
     * ======================================================== */

    /* カーネルスタック: R/W を強制する。
     *
     * 上のループはプローブしたメモリ量 (max_mem_bytes) を超える範囲を
     * Not-Present にするので、極端に小さいメモリ量が報告された場合でも
     * **自分が今立っているスタックだけは必ず生かしておく**。
     * ここを NP にした瞬間に次の push で三重フォルトになる。 */
    {
        u32 sp_addr;
        for (sp_addr = MEM_KSTACK_BASE; sp_addr <= MEM_KSTACK_TOP;
             sp_addr += PAGE_SIZE) {
            u32 pdi = sp_addr >> 22;
            u32 pti = (sp_addr >> 12) & 0x3FF;
            if (pdi < PAGING_PT_COUNT) {
                page_tables[pdi][pti] = (sp_addr & ~(PAGE_SIZE - 1)) | PAGE_RW;
            }
        }
    }

    /* スタックガードページ: Not-Present */
    paging_set_not_present(MEM_STACK_GUARD, MEM_STACK_GUARD_END);

    /* カーネル帯域内SHM後方予約: Not-Present (スタックガードの手前まで) */
    paging_set_not_present(MEM_SHM_RESV_START, MEM_SHM_RESV_END);

    /* カーネル予約域 (SQLite帯域後 〜 シェル帯域前): Not-Present */
    paging_set_not_present(MEM_KERNEL_RESV_START, MEM_KERNEL_RESV_END);

    /* シェルスタックガード: Not-Present */
    paging_set_not_present(MEM_SHELL_GUARD, MEM_SHELL_GUARD + PAGE_SIZE - 1);

    /* シェル帯域後方 (0x380000-0x3FFFFF): Not-Present */
    paging_set_not_present(0x380000UL, MEM_SHELL_BAND_END);

    /* SQLite帯域 + 代替スタック (0x200000〜): 強制R/W
     * ブートローダーが sqlite.bin を 0x200000 にロード済み。
     * 代替スタックも含めメモリプローブ結果に関係なく R/W を保証する。 */
    {
        u32 sq_addr;
        extern u32 __sqlite_start;
        u32 sq_start = (u32)&__sqlite_start & ~(PAGE_SIZE - 1);
        u32 sq_end   = (MEM_SQLITE_STACK_TOP + PAGE_SIZE) & ~(PAGE_SIZE - 1);
        for (sq_addr = sq_start; sq_addr < sq_end; sq_addr += PAGE_SIZE) {
            u32 pdi = sq_addr >> 22;
            u32 pti = (sq_addr >> 12) & 0x3FF;
            if (pdi < PAGING_PT_COUNT) {
                page_tables[pdi][pti] = sq_addr | PAGE_RW;
            }
        }
    }

    /* BIOS ROM: Read-Only */
    paging_set_readonly(MEM_BIOS_ROM_START, MEM_BIOS_ROM_END);

    /* ========================================================
     *  CR3にページディレクトリをセット → CR0.PGビットを立てる
     * ======================================================== */
    pd_phys = (u32)page_directory;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
    {
        u32 cr0_val;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
        cr0_val |= CR0_PG;
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val) : "memory");
    }

    pg_enabled = 1;
}

/* ======================================================================== */
/*  paging_reclaim_conventional — ブート後のコンベンショナルメモリ属性変更    */
/*  ページ0: NOT PRESENT (NULLポインタ検出)                                 */
/*  0x1000-0x9FFFF: R/W (旧R/O/ローダー領域とブート時スタックを解放)         */
/* ======================================================================== */
void paging_reclaim_conventional(void)
{
    u32 addr;

    /* ページ0: Read-Only (BIOS DATA AREA アクセスを許可しつつ書き込み検出)
     * [DEBUG] NOT PRESENT → R/O に変更: LZ4展開中にBDA参照でクラッシュする
     * 問題を調査中。元は paging_set_not_present(0x0, MEM_NULL_GUARD_END); */
    paging_set_readonly(0x0, MEM_NULL_GUARD_END);

    /* 0x1000-0x9FFFF: R/W (フォント/Unicode/GFX用) */
    for (addr = MEM_CONV_RECLAIM_START; addr <= MEM_CONV_RECLAIM_END; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        if (pdi < PAGING_PT_COUNT) {
            page_tables[pdi][pti] = addr | PAGE_RW;
        }
    }

    if (pg_enabled) tlb_flush_all();
}

/* ======================================================================== */
/*  paging_set_page — 1ページの属性を変更                                   */
/* ======================================================================== */
int paging_set_page(u32 virt_addr, u32 phys_addr, u32 flags)
{
    u32 pdi = virt_addr >> 22;
    u32 pti = (virt_addr >> 12) & 0x3FF;

    /* マッピング範囲外。無言 no-op にすると「ガードページを置いたつもり」の
     * まま保護なしで走る (fail-open) ので、必ず失敗を返す。 */
    if (pdi >= PAGING_PT_COUNT) return -1;

    page_tables[pdi][pti] = (phys_addr & 0xFFFFF000UL) | flags;

    /* 実効権限は PDE と PTE の論理積になる (i386 の仕様)。PTE だけ USER に
     * しても、その PDE に USER が無ければユーザ (V86 ゲスト) からは
     * アクセスできず #PF になる。PTE に USER を付けたら PDE にも伝播させる。
     *
     * PDE を USER にしても、同じ PDE 配下の他のページは PTE 側が
     * supervisor のままなので保護は保たれる。カーネル帯を USER に
     * しないこと (docs/tasks/v86v2/02_np21w_paging_analysis.md)。 */
    if (flags & PTE_USER) {
        page_directory[pdi] |= PTE_USER;
    }

    if (pg_enabled) tlb_flush_all();
    return 0;
}

/* 指定範囲を覆う PDE から USER を落とす。
 * paging_set_page() は USER を立てる方向にしか伝播させないので、
 * V86 セッション終了時にカーネル側が明示的に戻すために使う。 */
int paging_pde_clear_user(u32 start, u32 end)
{
    u32 pdi;
    u32 first = start >> 22;
    u32 last  = end >> 22;

    if (first >= PAGING_PT_COUNT) return -1;

    for (pdi = first; pdi <= last && pdi < PAGING_PT_COUNT; pdi++) {
        page_directory[pdi] &= ~(u32)PTE_USER;
    }

    if (pg_enabled) tlb_flush_all();
    return 0;
}

/* ======================================================================== */
/*  paging_set_readonly — 範囲内の全ページをRead-Onlyに                     */
/* ======================================================================== */
int paging_set_readonly(u32 start, u32 end)
{
    u32 addr;
    int rc = 0;
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_DOWN(end) + PAGE_SIZE;      /* end は inclusive 指定 */

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        u32 pte;
        if (pdi >= PAGING_PT_COUNT) { rc = -1; break; }
        /* 既存 PTE の物理フレームを保持する。identity で上書きすると、
         * V86 リマップ中のページを R/O 化した時にマッピング自体が
         * 壊れてしまう (フラグ変更のつもりが張り替えになる)。 */
        pte = page_tables[pdi][pti];
        if (pte & PTE_PRESENT) {
            page_tables[pdi][pti] = (pte & 0xFFFFF000UL) | PAGE_RO;
        } else {
            page_tables[pdi][pti] = (addr & 0xFFFFF000UL) | PAGE_RO;
        }
    }

    if (pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_set_not_present — 範囲内の全ページをアクセス不可に               */
/* ======================================================================== */
int paging_set_not_present(u32 start, u32 end)
{
    u32 addr;
    int rc = 0;
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_DOWN(end) + PAGE_SIZE;      /* end は inclusive 指定 */

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        if (pdi >= PAGING_PT_COUNT) { rc = -1; break; }
        /* フレーム/属性は保持して P ビットだけ落とす。全消去 (=0) だと
         * 解除時に identity 以外のマッピングを復元できない。 */
        page_tables[pdi][pti] &= ~(u32)PTE_PRESENT;
    }

    if (pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_enabled — ページングが有効かどうか                               */
/* ======================================================================== */
int paging_enabled(void) { return pg_enabled; }

/* ======================================================================== */
/*  paging_is_present — 指定アドレスのページがPresentかどうか                */
/*                                                                          */
/*  メモリダンプの安全チェック用。ページング無効時は常に1を返す。            */
/* ======================================================================== */
int paging_is_present(u32 virt_addr)
{
    u32 pdi, pti;
    if (!pg_enabled) return 1;
    pdi = virt_addr >> 22;
    if (pdi >= PAGING_PT_COUNT) return 0; /* 16MB超: マッピング範囲外 */
    pti = (virt_addr >> 12) & 0x3FF;
    return (page_tables[pdi][pti] & PTE_PRESENT) ? 1 : 0;
}

