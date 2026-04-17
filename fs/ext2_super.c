/* ======================================================================== */
/*  EXT2_SUPER.C — ext2ファイルシステムドライバ (読み書き対応)                */
/*                                                                          */
/*  マルチインスタンス対応: 全関数が Ext2Ctx* を受け取り、                   */
/*  グローバル状態ではなくコンテキスト経由でインスタンスを識別する。          */
/*                                                                          */
/*  ブロック→セクタ変換:                                                    */
/*    1 ext2ブロック (1024B) = 2 IDEセクタ (512B)                           */
/*    ext2ブロックN → IDEセクタ (N*2)                                      */
/* ======================================================================== */

#include "ext2_priv.h"

/* 共有静的バッファ (スタックオーバーフロー防止)
 * シングルタスクOSのため全インスタンスで共有可能。
 * ext2_g_blk: inode/super/GD読み書き用
 * ext2_g_aux: ディレクトリ走査/ビットマップ/間接ブロック/データ用 */
u8 ext2_g_blk[EXT2_BLOCK_SIZE];
u8 ext2_g_aux[EXT2_BLOCK_SIZE];

/* ======================================================================== */
/*  ブロック読み書き基盤                                                     */
/* ======================================================================== */

int ext2_read_block(Ext2Ctx *ctx, u32 block_num, void *buf)
{
    u32 sector = ctx->base_lba + block_num * 2;
    u8 *dst = (u8 *)buf;
    int ret;

    /* セクタ0 → buf[0..511] */
    ret = ide_read_sector(ctx->drive_num, sector, dst);
    if (ret != 0) return ret;

    /* セクタ1 → buf[512..1023] */
    ret = ide_read_sector(ctx->drive_num, sector + 1, dst + 512);
    return ret;
}

int ext2_write_block(Ext2Ctx *ctx, u32 block_num, const void *buf)
{
    u32 sector = ctx->base_lba + block_num * 2;
    int ret;
    ret = ide_write_sector(ctx->drive_num, sector, buf);
    if (ret != 0) return ret;
    ret = ide_write_sector(ctx->drive_num, sector + 1, (const u8 *)buf + 512);
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

int ext2_write_super_raw(Ext2Ctx *ctx)
{
    int ret;
    ret = ext2_read_block(ctx, 1, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;
    *(u32 *)&ext2_g_blk[12] = ctx->sb_info.free_blocks_count;
    *(u32 *)&ext2_g_blk[16] = ctx->sb_info.free_inodes_count;
    return ext2_write_block(ctx, 1, ext2_g_blk);
}

int ext2_write_gd_raw(Ext2Ctx *ctx)
{
    /* GDTはブロック2から連続配置。1エントリ32バイト。
     * 1KBブロックに32エントリ収まる → MAX_GROUPS=32なら1ブロックで足りる */
    int ret;
    u32 g, gd_block, offset;

    /* GDTが占めるブロック数を計算 (32B × num_groups) */
    for (g = 0; g < ctx->num_groups; g++) {
        gd_block = 2 + (g * 32) / EXT2_BLOCK_SIZE;
        offset = (g * 32) % EXT2_BLOCK_SIZE;

        /* ブロックの先頭エントリの場合のみ読み込み */
        if (offset == 0) {
            ret = ext2_read_block(ctx, gd_block, ext2_g_blk);
            if (ret != 0) return EXT2_ERR_IO;
        }

        *(u32 *)&ext2_g_blk[offset + 0]  = ctx->gd_table[g].block_bitmap;
        *(u32 *)&ext2_g_blk[offset + 4]  = ctx->gd_table[g].inode_bitmap;
        *(u32 *)&ext2_g_blk[offset + 8]  = ctx->gd_table[g].inode_table;
        *(u16 *)&ext2_g_blk[offset + 12] = ctx->gd_table[g].free_blocks;
        *(u16 *)&ext2_g_blk[offset + 14] = ctx->gd_table[g].free_inodes;
        *(u16 *)&ext2_g_blk[offset + 16] = ctx->gd_table[g].used_dirs;

        /* ブロック末尾のエントリ or 最後のグループの場合に書き込み */
        if (offset + 32 >= EXT2_BLOCK_SIZE || g == ctx->num_groups - 1) {
            ret = ext2_write_block(ctx, gd_block, ext2_g_blk);
            if (ret != 0) return EXT2_ERR_IO;
        }
    }
    return EXT2_OK;
}

/* ======================================================================== */
/*  パーティションテーブル解析                                               */
/* ======================================================================== */

u32 ext2_find_partition(int ide_drive)
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

int ext2_mount(Ext2Ctx *ctx, int ide_drive)
{
    int ret, i;

    if (ctx->mounted) ext2_unmount(ctx);
    if (!ide_drive_present(ide_drive)) return EXT2_ERR_IO;
    ctx->drive_num = ide_drive;

    /* パーティションテーブルを解析してbase_lbaを設定 */
    ctx->base_lba = ext2_find_partition(ide_drive);

    /* スーパーブロック読み込み: ローカルバッファに読んでからg_blkへコピー */
    {
        u8 sb_sect[512];
        int j;
        ret = ide_read_sector(ctx->drive_num, ctx->base_lba + 2, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        for (j = 0; j < 512; j++) ext2_g_blk[j] = sb_sect[j];
        
        ret = ide_read_sector(ctx->drive_num, ctx->base_lba + 3, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        for (j = 0; j < 512; j++) ext2_g_blk[512 + j] = sb_sect[j];
    }

    {
        u16 magic = (u16)ext2_g_blk[56] | ((u16)ext2_g_blk[57] << 8);
        if (magic != EXT2_SUPER_MAGIC) return EXT2_ERR_MAGIC;
    }

    ctx->sb_info.total_inodes     = *(u32 *)&ext2_g_blk[0];
    ctx->sb_info.total_blocks     = *(u32 *)&ext2_g_blk[4];
    ctx->sb_info.free_blocks_count = *(u32 *)&ext2_g_blk[12];
    ctx->sb_info.free_inodes_count = *(u32 *)&ext2_g_blk[16];
    ctx->sb_info.first_data_block = *(u32 *)&ext2_g_blk[20];
    ctx->sb_info.block_size       = 1024U << (*(u32 *)&ext2_g_blk[24]);
    ctx->sb_info.blocks_per_group = *(u32 *)&ext2_g_blk[32];
    ctx->sb_info.inodes_per_group = *(u32 *)&ext2_g_blk[40];
    ctx->sb_info.magic            = *(u16 *)&ext2_g_blk[56];
    ctx->sb_info.first_ino        = *(u32 *)&ext2_g_blk[84];
    ctx->sb_info.inode_size       = *(u16 *)&ext2_g_blk[88];
    if (ctx->sb_info.inode_size == 0) ctx->sb_info.inode_size = 128;

    for (i = 0; i < 16; i++) {
        ctx->sb_info.volume_name[i] = (char)ext2_g_blk[120 + i];
    }
    ctx->sb_info.volume_name[16] = '\0';

    /* グループ数を計算 */
    ctx->num_groups = (ctx->sb_info.total_blocks - ctx->sb_info.first_data_block
                       + ctx->sb_info.blocks_per_group - 1)
                      / ctx->sb_info.blocks_per_group;
    if (ctx->num_groups == 0) ctx->num_groups = 1;
    if (ctx->num_groups > EXT2_MAX_GROUPS) return EXT2_ERR_IO;

    /* グループディスクリプタテーブル全体を読み込み — g_blk再利用 */
    {
        u32 g, gd_block, offset;
        for (g = 0; g < ctx->num_groups; g++) {
            gd_block = 2 + (g * 32) / EXT2_BLOCK_SIZE;
            offset = (g * 32) % EXT2_BLOCK_SIZE;

            /* ブロックの先頭エントリの場合のみ読み込み */
            if (offset == 0) {
                ret = ext2_read_block(ctx, gd_block, ext2_g_blk);
                if (ret != 0) return EXT2_ERR_IO;
            }

            ctx->gd_table[g].block_bitmap = *(u32 *)&ext2_g_blk[offset + 0];
            ctx->gd_table[g].inode_bitmap = *(u32 *)&ext2_g_blk[offset + 4];
            ctx->gd_table[g].inode_table  = *(u32 *)&ext2_g_blk[offset + 8];
            ctx->gd_table[g].free_blocks  = *(u16 *)&ext2_g_blk[offset + 12];
            ctx->gd_table[g].free_inodes  = *(u16 *)&ext2_g_blk[offset + 14];
            ctx->gd_table[g].used_dirs    = *(u16 *)&ext2_g_blk[offset + 16];
        }
    }

    ctx->mounted = 1;
    return EXT2_OK;
}

void ext2_unmount(Ext2Ctx *ctx)
{
    if (ctx->mounted) ext2_sync(ctx);
    ctx->mounted = 0;
}

int ext2_is_mounted_ctx(Ext2Ctx *ctx) { return ctx->mounted; }

const Ext2Super *ext2_get_super_ctx(Ext2Ctx *ctx)
{
    return ctx->mounted ? &ctx->sb_info : (const Ext2Super *)0;
}

int ext2_sync(Ext2Ctx *ctx)
{
    int ret;
    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_write_super_raw(ctx);
    if (ret != 0) return ret;
    return ext2_write_gd_raw(ctx);
}

/* ======================================================================== */
