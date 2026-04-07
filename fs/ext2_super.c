/* ======================================================================== */
/*  EXT2.C — ext2ファイルシステムドライバ (読み書き対応)                      */
/*                                                                          */
/*  Linux kernel 2.4 (Plamo Linux) ext2実装を参考にしたフルスペック版。      */
/*  IDEドライバ経由でHDD上のext2構造を読み書きする。                         */
/*                                                                          */
/*  ブロック→セクタ変換:                                                    */
/*    1 ext2ブロック (1024B) = 2 IDEセクタ (512B)                           */
/*    ext2ブロックN → IDEセクタ (N*2)                                      */
/*                                                                          */
/*  参照: fs/ext2/balloc.c, ialloc.c, dir.c, namei.c, inode.c              */
/* ======================================================================== */

#include "ext2_priv.h"

/* マウント状態 */
int ext2_mounted = 0;
int ext2_drive_num = 0;
Ext2Super ext2_sb_info;
Ext2GroupDesc ext2_gd_info;

/* 共有静的バッファ (スタックオーバーフロー防止)
 * BSS節約のため2つに統合。バッファの用途は呼び出し元コメント参照。
 * ext2_g_blk: inode/super/GD読み書き用
 * ext2_g_aux: ディレクトリ走査/ビットマップ/間接ブロック/データ用 */
u8 ext2_g_blk[EXT2_BLOCK_SIZE];
u8 ext2_g_aux[EXT2_BLOCK_SIZE];

/* PC-98パーティションテーブル仕様: シリンダ0はIPL/パーティション情報用に予約。
 * ext2パーティションは通常シリンダ1から開始されますが、BTNPART等の情報を元に動的に計算します。 */
u32 ext2_base_lba = 272; /* 初期値: シリンダ2から開始 (8H*17S*2cyl) */

/* ======================================================================== */
/*  ブロック読み書き基盤                                                     */
/* ======================================================================== */

int ext2_read_block(u32 block_num, void *buf)
{
    u32 sector = ext2_base_lba + block_num * 2;
    u8 *dst = (u8 *)buf;
    int ret;

    /* セクタ0 → buf[0..511] */
    ret = ide_read_sector(ext2_drive_num, sector, dst);
    if (ret != 0) return ret;

    /* セクタ1 → buf[512..1023] */
    ret = ide_read_sector(ext2_drive_num, sector + 1, dst + 512);
    return ret;
}

int ext2_write_block(u32 block_num, const void *buf)
{
    u32 sector = ext2_base_lba + block_num * 2;
    int ret;
    ret = ide_write_sector(ext2_drive_num, sector, buf);
    if (ret != 0) return ret;
    ret = ide_write_sector(ext2_drive_num, sector + 1, (const u8 *)buf + 512);
    return ret;
}

/* ======================================================================== */
/*  ヘルパー                                                                */
/* ======================================================================== */

void ext2_mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++) d[i] = s[i];
}

void ext2_mem_zero(void *dst, u32 len)
{
    u8 *d = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++) d[i] = 0;
}

int ext2_str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

int ext2_str_ncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(u8)a[i] - (int)(u8)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

/* ======================================================================== */
/*  タイムスタンプ                                                           */
/* ======================================================================== */

u32 ext2_current_time(void)
{
    return 0x67E8E800UL;  /* 2025-01-01 00:00:00 UTC 近似 */
}

/* ======================================================================== */
/*  スーパーブロック / グループディスクリプタ 書き戻し                       */
/* ======================================================================== */

int ext2_write_super_raw(void)
{
    int ret;
    ret = ext2_read_block(1, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;
    *(u32 *)&ext2_g_blk[12] = ext2_sb_info.free_blocks_count;
    *(u32 *)&ext2_g_blk[16] = ext2_sb_info.free_inodes_count;
    return ext2_write_block(1, ext2_g_blk);
}

int ext2_write_gd_raw(void)
{
    int ret;
    ret = ext2_read_block(2, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;
    *(u16 *)&ext2_g_blk[12] = ext2_gd_info.free_blocks;
    *(u16 *)&ext2_g_blk[14] = ext2_gd_info.free_inodes;
    *(u16 *)&ext2_g_blk[16] = ext2_gd_info.used_dirs;
    return ext2_write_block(2, ext2_g_blk);
}

/* ======================================================================== */
/*  パーティションテーブル解析                                               */
/* ======================================================================== */

static u32 ext2_find_partition(int ide_drive)
{
    u8 pt_sect[512];
    int ret, i;
    u32 lba = 272; /* デフォルトフォールバック */
    IdeInfo info;

    if (ide_get_info(ide_drive, &info) != IDE_OK) return lba;

    /* LBA 1 (PC-98パーティションテーブル) を読み込む */
    ret = ide_read_sector(ide_drive, 1, pt_sect);
    if (ret != 0) return lba;

    for (i = 0; i < 16; i++) {
        u8 *ent = &pt_sect[i * 32];
        u8 bootable = ent[0];
        u8 sys_type = ent[1];
        
        if (sys_type == 0x00) continue;

        /* アクティブなパーティションエントリから開始LBAを計算 */
        /* bootable = bit7 (0x80 または 0xA0 等) */
        if (bootable & 0x80) {
            u16 start_c = (u16)ent[8] | ((u16)ent[9] << 8);
            u8  start_h = ent[7];
            u8  start_s = ent[6];
            /* HDD BIOSのセクタ番号は0開始 (FDDの1開始とは異なる) */
            lba = ((u32)start_c * info.heads + start_h) * info.sectors + start_s;
            break;
        }
    }

    /* 念のためLBAが0ならフォールバック */
    if (lba == 0) lba = 272;
    return lba;
}

/* ======================================================================== */
/*  マウント / アンマウント                                                  */
/* ======================================================================== */

int ext2_mount(int ide_drive)
{
    int ret, i;

    if (ext2_mounted) ext2_unmount();
    if (!ide_drive_present(ide_drive)) return EXT2_ERR_IO;
    ext2_drive_num = ide_drive;

    /* パーティションテーブルを解析してbase_lbaを設定 */
    ext2_base_lba = ext2_find_partition(ide_drive);

    /* スーパーブロック読み込み: ローカルバッファに読んでからg_blkへコピー */
    {
        u8 sb_sect[512];
        int j;
        ret = ide_read_sector(ext2_drive_num, ext2_base_lba + 2, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        for (j = 0; j < 512; j++) ext2_g_blk[j] = sb_sect[j];
        
        ret = ide_read_sector(ext2_drive_num, ext2_base_lba + 3, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        for (j = 0; j < 512; j++) ext2_g_blk[512 + j] = sb_sect[j];
    }

    {
        u16 magic = (u16)ext2_g_blk[56] | ((u16)ext2_g_blk[57] << 8);
        if (magic != EXT2_SUPER_MAGIC) return EXT2_ERR_MAGIC;
    }

    ext2_sb_info.total_inodes     = *(u32 *)&ext2_g_blk[0];
    ext2_sb_info.total_blocks     = *(u32 *)&ext2_g_blk[4];
    ext2_sb_info.free_blocks_count = *(u32 *)&ext2_g_blk[12];
    ext2_sb_info.free_inodes_count = *(u32 *)&ext2_g_blk[16];
    ext2_sb_info.first_data_block = *(u32 *)&ext2_g_blk[20];
    ext2_sb_info.block_size       = 1024U << (*(u32 *)&ext2_g_blk[24]);
    ext2_sb_info.blocks_per_group = *(u32 *)&ext2_g_blk[32];
    ext2_sb_info.inodes_per_group = *(u32 *)&ext2_g_blk[40];
    ext2_sb_info.magic            = *(u16 *)&ext2_g_blk[56];
    ext2_sb_info.first_ino        = *(u32 *)&ext2_g_blk[84];
    ext2_sb_info.inode_size       = *(u16 *)&ext2_g_blk[88];
    if (ext2_sb_info.inode_size == 0) ext2_sb_info.inode_size = 128;

    for (i = 0; i < 16; i++) {
        ext2_sb_info.volume_name[i] = (char)ext2_g_blk[120 + i];
    }
    ext2_sb_info.volume_name[16] = '\0';

    /* グループディスクリプタ (ブロック2) — g_blk再利用 */
    ret = ext2_read_block(2, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;

    ext2_gd_info.block_bitmap = *(u32 *)&ext2_g_blk[0];
    ext2_gd_info.inode_bitmap = *(u32 *)&ext2_g_blk[4];
    ext2_gd_info.inode_table  = *(u32 *)&ext2_g_blk[8];
    ext2_gd_info.free_blocks  = *(u16 *)&ext2_g_blk[12];
    ext2_gd_info.free_inodes  = *(u16 *)&ext2_g_blk[14];
    ext2_gd_info.used_dirs    = *(u16 *)&ext2_g_blk[16];

    ext2_mounted = 1;
    return EXT2_OK;
}

void ext2_unmount(void)
{
    if (ext2_mounted) ext2_sync();
    ext2_mounted = 0;
}

int ext2_is_mounted(void) { return ext2_mounted; }

const Ext2Super *ext2_get_super(void)
{
    return ext2_mounted ? &ext2_sb_info : (const Ext2Super *)0;
}

int ext2_sync(void)
{
    int ret;
    if (!ext2_mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_write_super_raw();
    if (ret != 0) return ret;
    return ext2_write_gd_raw();
}

/* ======================================================================== */
