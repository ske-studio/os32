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
/*    合計BSS: ~36KB (アライメント用パディング含む)                         */
/*                                                                          */
/*  保護マップ:                                                             */
/*    0x00000 - 0x00FFF : Read-Only   (IVT + BIOSデータ)                   */
/*    0x01000 - 0x06FFF : Read-Only   (BIOS周辺)                           */
/*    0x07000 - 0x07FFF : Read-Write  (BIOSトランポリン)                    */
/*    0x08000 - 0x08FFF : Read-Only   (loader+pm32, 使用済み)              */
/*    0x09000 - 0x3FFFF : Read-Write  (カーネル)                           */
/*    0x40000 - 0x6FFFF : Read-Write  (kmallocヒープ)                      */
/*    0x70000 - 0x8EFFF : Read-Write  (VRAMバックバッファ)                  */
/*    0x8F000 - 0x8FFFF : Not-Present (★スタックガードページ)              */
/*    0x90000 - 0x9FFFF : Read-Write  (スタック)                           */
/*    0xA0000 - 0xEFFFF : Read-Write  (テキスト/グラフィックVRAM)           */
/*    0xF0000 - 0xFFFFF : Read-Only   (BIOS ROM)                           */
/*    0x100000-0xFFFFFF : Read-Write  (拡張メモリ)                         */
/* ======================================================================== */

#include "paging.h"

#include "io.h"
#include "pc98.h"
#include "memmap.h"

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
                // コンベンショナルメモリ(0-1MB)またはプローブ範囲内
                page_tables[i][j] = phys | PAGE_RW;
            } else {
                // 未実装領域
                page_tables[i][j] = PAGE_NOT_PRESENT;
            }
        }
        /* ページディレクトリにテーブルを登録 */
        page_directory[i] = (u32)page_tables[i] | PAGE_RW;
    }

    /* ========================================================
     *  保護属性の設定
     * ======================================================== */

    /* IVT/BIOSデータ領域: Read-Only
     * ページ0 (0x0-0xFFF) にBIOSトランポリン パラメータブロック(0x600)が
     * あり、FDD I/O時に書き込みが発生するためR/Wのまま維持する。
     * ページ6 (0x6000-0x6FFF) もトランポリン隣接のためR/Wに。 */
    paging_set_readonly(MEM_IVT_PROT_START, MEM_IVT_PROT_END);

    /* BIOSトランポリン: Read-Write (0x07000 - 0x07FFF) — そのまま */

    /* loader.bin + pm32.bin (使用済み): Read-Only */
    paging_set_readonly(MEM_LOADER_START, MEM_LOADER_END);

    /* スタックガードページ: Not-Present */
    paging_set_not_present(MEM_STACK_GUARD, MEM_STACK_GUARD_END);

    /* ====== 外部プログラム保護 (VMMにより不要となったため削除) ====== */

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
/*  paging_set_page — 1ページの属性を変更                                   */
/* ======================================================================== */
void paging_set_page(u32 virt_addr, u32 phys_addr, u32 flags)
{
    u32 pdi = virt_addr >> 22;
    u32 pti = (virt_addr >> 12) & 0x3FF;

    /* マッピング範囲外なら無視 */
    if (pdi >= PAGING_PT_COUNT) return;

    page_tables[pdi][pti] = (phys_addr & 0xFFFFF000UL) | flags;

    if (pg_enabled) tlb_flush_all();
}

/* ======================================================================== */
/*  paging_set_readonly — 範囲内の全ページをRead-Onlyに                     */
/* ======================================================================== */
void paging_set_readonly(u32 start, u32 end)
{
    u32 addr;
    start &= ~(PAGE_SIZE - 1);     /* ページ境界に切り下げ */
    end = (end + PAGE_SIZE) & ~(PAGE_SIZE - 1);  /* 切り上げ */

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        if (pdi >= PAGING_PT_COUNT) break;
        page_tables[pdi][pti] = (addr & 0xFFFFF000UL) | PAGE_RO;
    }

    if (pg_enabled) tlb_flush_all();
}

/* ======================================================================== */
/*  paging_set_not_present — 範囲内の全ページをアクセス不可に               */
/* ======================================================================== */
void paging_set_not_present(u32 start, u32 end)
{
    u32 addr;
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE) & ~(PAGE_SIZE - 1);

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        if (pdi >= PAGING_PT_COUNT) break;
        page_tables[pdi][pti] = PAGE_NOT_PRESENT;
    }

    if (pg_enabled) tlb_flush_all();
}

/* ======================================================================== */
/*  paging_enabled — ページングが有効かどうか                               */
/* ======================================================================== */
int paging_enabled(void) { return pg_enabled; }


