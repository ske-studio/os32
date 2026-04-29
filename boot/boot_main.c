/* ======================================================================== */
/*  BOOT_MAIN.C — HDD新ローダー メインロジック (C言語)                      */
/*                                                                          */
/*  ASM (loader_hdd_new.asm) から呼び出される。                              */
/*  1. PC-98パーティションテーブルからext2パーティションを特定               */
/*  2. ext2から /boot/vmkernel.lz4 を読み込み                               */
/*  3. VK32ヘッダ解析 + LZ4展開                                             */
/*  4. 正常終了時は呼び出し元ASMに戻り、ASMがカーネルにジャンプ              */
/* ======================================================================== */

#include "boot_defs.h"

/* ================================================================ */
/*  パーティション検索 (PC-98 パーティションテーブル: LBA 1)         */
/* ================================================================ */
static u32 find_partition(void)
{
    u8 pt_buf[512];
    int i;
    u32 lba = 1088;  /* フォールバック (シリンダ8) */

    boot_read_sector_asm(1, pt_buf);

    for (i = 0; i < 16; i++) {
        u8 *ent = &pt_buf[i * 32];
        u8 bootable = ent[0];
        u8 sys_type = ent[1];

        if (sys_type == 0x00) continue;

        if (bootable & 0x80) {
            u16 start_c = (u16)ent[8] | ((u16)ent[9] << 8);
            u8  start_h = ent[7];
            u8  start_s = ent[6];
            lba = ((u32)start_c * param_heads + start_h)
                  * param_spt + start_s;
            break;
        }
    }

    if (lba == 0) lba = 1088;
    return lba;
}

/* ================================================================ */
/*  boot_main — ローダーメイン (ASMから呼ばれる)                     */
/*  戻り値: 0=成功, 負数=エラー                                     */
/* ================================================================ */
int boot_main(void)
{
    u32 part_lba;
    u32 ino;
    int file_size;
    BootVK32Header *hdr;
    u8 *file_base;
    int i;

    /* 1. パーティション検索 */
    boot_print_asm(0xA0000 + 160, "Finding partition...");
    part_lba = find_partition();

    /* 2. ext2初期化 */
    if (ext2m_init(part_lba) != 0) {
        boot_print_asm(0xA0000 + 320, "ext2 mount FAIL!");
        return -1;
    }

    /* 3. /boot/vmkernel.lz4 を検索 */
    boot_print_asm(0xA0000 + 320, "Loading vmkernel.lz4...");
    ino = ext2m_lookup("/boot/vmkernel.lz4");
    if (ino == 0) {
        boot_print_asm(0xA0000 + 480, "/boot/vmkernel.lz4 NOT FOUND!");
        return -2;
    }

    /* 4. ファイルを LOAD_BUF (0x10000) に読み込み */
    file_base = (u8 *)LOAD_BUF;
    file_size = ext2m_read_file(ino, file_base, MAX_IMAGE_SIZE);
    if (file_size <= 0) {
        boot_print_asm(0xA0000 + 480, "File read FAIL!");
        return -3;
    }

    /* 5. VK32ヘッダ検証 */
    hdr = (BootVK32Header *)file_base;
    if (hdr->magic != VK32_MAGIC) {
        boot_print_asm(0xA0000 + 480, "Bad VK32 magic!");
        return -4;
    }

    /* 6. 各エントリをLZ4展開 */
    boot_print_asm(0xA0000 + 480, "Decompressing...");
    for (i = 0; i < (int)hdr->entry_count && i < VK32_MAX_ENTRIES; i++) {
        u8 *src     = file_base + hdr->entries[i].data_offset;
        u32 csz     = hdr->entries[i].compressed_size;
        u32 raw_sz  = hdr->entries[i].raw_size;
        u8 *dst     = (u8 *)hdr->entries[i].load_addr;
        int decoded;

        decoded = boot_lz4_decode(src, (int)csz, dst, (int)raw_sz);
        if (decoded < 0) {
            boot_print_asm(0xA0000 + 640, "LZ4 decode FAIL!");
            return -5;
        }
    }

    boot_print_asm(0xA0000 + 640, "Kernel loaded. Booting...");
    return 0;
}
