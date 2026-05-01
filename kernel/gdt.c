#include "types.h"
#include "kstring.h"
#include "tss.h"

/* GDTエントリ構造体 */
struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_middle;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed));

/* GDTポインタ構造体 */
struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

/* GDTは4エントリ (NULL, コード, データ, TSS) */
#define GDT_ENTRIES 4
#define GDT_TSS_INDEX 3
#define GDT_TSS_SELECTOR (GDT_TSS_INDEX * 8)  /* 0x18 */

struct gdt_entry gdt[GDT_ENTRIES];
struct gdt_ptr gp;

/* アセンブラの lgdt ラッパー (kentry.asmなどに置くかインラインで) */
static void gdt_flush(u32 pointer)
{
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : "r"(pointer) : "memory", "eax"
    );
}

/* LTR命令でTSSをロード */
static void tss_flush(u16 selector)
{
    __asm__ volatile ("ltr %0" : : "r"(selector));
}

/* GDTエントリ設定 */
static void gdt_set_gate(int num, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access = access;
}

/* カーネルGDTの初期化 */
void gdt_init(void)
{
    u32 tss_base;
    u32 tss_limit;

    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base = (u32)&gdt;

    /* NULLディスクリプタ */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* コードセグメント: ベース=0, リミット=4GB, 実行/読み込み可能, 32ビット, 4KBグラニュラリティ */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* データセグメント: ベース=0, リミット=4GB, 読み書き可能, 32ビット, 4KBグラニュラリティ */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* TSSディスクリプタ:
     *   Access: 0x89 = Present(1) | DPL(00) | 0(0) | Type(1001) = 32bit TSS (Available)
     *   Granularity: 0x00 = バイトグラニュラリティ, 16bitリミット
     */
    tss_base = (u32)&kernel_tss;
    tss_limit = sizeof(struct tss_entry) - 1;
    gdt_set_gate(GDT_TSS_INDEX, tss_base, tss_limit, 0x89, 0x00);

    /* GDTのロードとセグメントレジスタの再設定 */
    gdt_flush((u32)&gp);

    /* TSSの初期化 (カーネルスタックトップ = 0x9FFF0) */
    tss_init(0x9FFF0);

    /* LTR命令でTSSレジスタにロード */
    tss_flush(GDT_TSS_SELECTOR);
}
