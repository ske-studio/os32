/* ======================================================================== */
/*  BOOT_DEFS.H — ブートローダー共通定義                                     */
/*  カーネルヘッダに依存せず、ローダー内で完結する型と定数を定義。            */
/* ======================================================================== */

#ifndef BOOT_DEFS_H
#define BOOT_DEFS_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define NULL ((void *)0)

/* ext2 定数 */
#define EXT2_BLOCK_SIZE    1024
#define EXT2_SUPER_MAGIC   0xEF53
#define EXT2_ROOT_INO      2
#define EXT2_FT_DIR        2
#define EXT2_FT_REG_FILE   1
#define EXT2_NDIR_BLOCKS   12
#define EXT2_IND_BLOCK     12
#define EXT2_DIND_BLOCK    13

/* VK32 ヘッダ */
#define VK32_MAGIC         0x32334B56UL
#define VK32_MAX_ENTRIES   4

typedef struct {
    u32 load_addr;
    u32 raw_size;
    u32 data_offset;
    u32 compressed_size;
} BootVK32Entry;

typedef struct {
    u32 magic;
    u32 header_size;
    u32 version;
    u32 entry_count;
    BootVK32Entry entries[VK32_MAX_ENTRIES];
} BootVK32Header;

/* メモリアドレス */
#define LOAD_BUF           0x10000UL   /* vmkernel.lz4 一時読み込み先 */
#define MAX_IMAGE_SIZE     (508*1024)  /* 0x10000-0x8EFFF */

/* ASMから呼ばれるセクタ読み込み関数 (loader_hdd_new.asm で定義) */
extern void boot_read_sector_asm(u32 lba, u8 *buf);

/* ASMのTVRAM表示関数 */
extern void boot_print_asm(u32 tvram_addr, const char *msg);

/* ASMのジオメトリパラメータ */
extern u8 param_da;
extern u8 param_heads;
extern u8 param_spt;

/* ext2 ミニリーダー API */
int  ext2m_init(u32 part_lba);
u32  ext2m_lookup(const char *path);
int  ext2m_read_file(u32 ino, u8 *buf, u32 max_size);

/* LZ4 デコーダ */
int  boot_lz4_decode(const u8 *src, int compressed_size,
                     u8 *dst, int dst_capacity);

#endif /* BOOT_DEFS_H */
