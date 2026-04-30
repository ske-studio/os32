#include "types.h"
#include "kstring.h"

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

/* GDTは3エントリ (NULL, コード, データ) */
struct gdt_entry gdt[3];
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
    gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
    gp.base = (u32)&gdt;

    /* NULLディスクリプタ */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* コードセグメント: ベース=0, リミット=4GB, 実行/読み込み可能, 32ビット, 4KBグラニュラリティ */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* データセグメント: ベース=0, リミット=4GB, 読み書き可能, 32ビット, 4KBグラニュラリティ */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* GDTのロードとセグメントレジスタの再設定 */
    gdt_flush((u32)&gp);
}
