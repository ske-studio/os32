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
#include "ide.h"    /* ide_drive_present, ide_get_info — ジオメトリ情報取得のみ */

/* 共有静的バッファ (スタックオーバーフロー防止)
 * シングルタスクOSのため全インスタンスで共有可能。
 * ext2_g_blk: inode/super/GD読み書き用
 * ext2_g_aux: ディレクトリ走査/ビットマップ/間接ブロック用
 * ext2_g_dat: ext2_write_stream のデータ読み書き専用 (ext2_g_aux と競合回避) */
u8 ext2_g_blk[EXT2_BLOCK_SIZE];
u8 ext2_g_aux[EXT2_BLOCK_SIZE];
u8 ext2_g_dat[EXT2_BLOCK_SIZE];

/* ======================================================================== */
/*  ブロック読み書き基盤                                                     */
/* ======================================================================== */

int ext2_read_block(Ext2Ctx *ctx, u32 block_num, void *buf)
{
    u32 sector = ctx->base_lba + block_num * 2;
    u8 *dst = (u8 *)buf;
    int ret;

    /* セクタ0 → buf[0..511] */
    ret = dev_blk_read_lba(ctx->dev, sector, 1, dst);
    if (ret != 0) return ret;

    /* セクタ1 → buf[512..1023] */
    ret = dev_blk_read_lba(ctx->dev, sector + 1, 1, dst + 512);
    return ret;
}

int ext2_write_block(Ext2Ctx *ctx, u32 block_num, const void *buf)
{
    u32 sector = ctx->base_lba + block_num * 2;
    int ret;
    ret = dev_blk_write_lba(ctx->dev, sector, 1, buf);
    if (ret != 0) return ret;
    ret = dev_blk_write_lba(ctx->dev, sector + 1, 1, (const u8 *)buf + 512);
    return ret;
}

/* ======================================================================== */
/*  ヘルパー                                                                */
/* ======================================================================== */

/* ext2_mem_copy / ext2_mem_zero / ext2_str_len / ext2_str_ncmp は
 * ext2_priv.h のマクロで kstring 関数 (ASM最適化済み) に転送済み */

/* ======================================================================== */
/*  メタデータの汚れ / 解決済み経路の記憶                                    */
/* ======================================================================== */

/* スーパーブロックかグループ記述子を動かした側が呼ぶ。
 * 次の ext2_sync() が実際にディスクへ書く。 */
void ext2_meta_touch(Ext2Ctx *ctx)
{
    ctx->meta_dirty = 1;
}

/* 名前空間を動かした側が呼ぶ (ext2_add_entry / ext2_delete_entry)。
 * 世代が進むと記憶は全部まとめて無効になる。 */
void ext2_ns_touch(Ext2Ctx *ctx)
{
    ctx->ns_gen++;
    if (ctx->ns_gen == 0) {
        /* 一周した。同じ世代番号の古い記憶と衝突しないよう全部捨てる */
        ext2_path_memo_reset(ctx);
        ctx->ns_gen = 1;
    }
}

void ext2_path_memo_reset(Ext2Ctx *ctx)
{
    int i;
    for (i = 0; i < EXT2_PATH_MEMO_N; i++) ctx->memo[i].gen = 0;
    ctx->memo_next = 0;
}

int ext2_path_memo_get(Ext2Ctx *ctx, const char *path, u32 *out_ino)
{
    int i;
    for (i = 0; i < EXT2_PATH_MEMO_N; i++) {
        if (ctx->memo[i].gen == ctx->ns_gen &&
            kstrcmp(ctx->memo[i].path, path) == 0) {
            *out_ino = ctx->memo[i].ino;
            return EXT2_OK;
        }
    }
    return EXT2_ERR_NOTFOUND;
}

void ext2_path_memo_put(Ext2Ctx *ctx, const char *path, u32 ino)
{
    int i;

    /* 収まらない経路は覚えない。kstrncpy は黙って切り詰めるので、
     * 覚えてしまうと別の経路に化けて当たる */
    if (kstrlen(path) >= OS32_MAX_PATH) return;

    for (i = 0; i < EXT2_PATH_MEMO_N; i++) {
        if (ctx->memo[i].gen == ctx->ns_gen &&
            kstrcmp(ctx->memo[i].path, path) == 0) {
            ctx->memo[i].ino = ino;
            return;
        }
    }

    i = ctx->memo_next;
    ctx->memo_next = (i + 1) % EXT2_PATH_MEMO_N;
    kstrncpy(ctx->memo[i].path, path, OS32_MAX_PATH);
    ctx->memo[i].ino = ino;
    ctx->memo[i].gen = ctx->ns_gen;
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

Device *ext2_dev_for(int ide_drive)
{
    char devname[8];
    devname[0] = 'h';
    devname[1] = 'd';
    devname[2] = (char)('0' + (ide_drive & 0xFF));
    devname[3] = '\0';
    return dev_find(devname);
}

u32 ext2_find_partition(int ide_drive)
{
    u8 pt_sect[512];
    int ret, i;
    u32 lba = 1088; /* デフォルトフォールバック (シリンダー8) */
    IdeInfo info;
    Device *dev;

    if (ide_get_info(ide_drive, &info) != IDE_OK) return lba;

    /* Device API 経由でパーティションテーブルを読み込み */
    dev = ext2_dev_for(ide_drive);
    if (!dev) return lba;

    /* LBA 1 (PC-98パーティションテーブル) を読み込む */
    ret = dev_blk_read_lba(dev, 1, 1, pt_sect);
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
    if (lba == 0) lba = 1088;
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

    /* Device API ポインタを取得 */
    ctx->dev = ext2_dev_for(ide_drive);
    if (!ctx->dev) return EXT2_ERR_IO;

    /* パーティションテーブルを解析してbase_lbaを設定 */
    ctx->base_lba = ext2_find_partition(ide_drive);

    /* スーパーブロック読み込み: Device API 経由 */
    {
        u8 sb_sect[512];
        ret = dev_blk_read_lba(ctx->dev, ctx->base_lba + 2, 1, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        kmemcpy(ext2_g_blk, sb_sect, 512);
        
        ret = dev_blk_read_lba(ctx->dev, ctx->base_lba + 3, 1, sb_sect);
        if (ret != 0) return EXT2_ERR_IO;
        kmemcpy(ext2_g_blk + 512, sb_sect, 512);
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
    /* 読み込んだばかり = ディスクと一致。経路の記憶も持ち越さない
     * (ctx は使い回されることがある) */
    ctx->meta_dirty = 0;
    ctx->ns_gen = 1;
    ext2_path_memo_reset(ctx);

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

    /* 空きブロック数・空き inode 数・グループ記述子が前回の書き戻しから
     * 一つも動いていなければ、書き戻すものは無い。write-through の契約は
     * 変わらない (戻った時点でディスクは正しい) — 同じ中身を read-modify-
     * write し直す 4 ブロック (= 8 セクタ) を出さないだけ。票 S6-P: 追記
     * 1 回あたり 26 セクタのうち 8 セクタがこれだった。 */
    if (!ctx->meta_dirty) return EXT2_OK;

    ret = ext2_write_super_raw(ctx);
    if (ret != 0) return ret;
    ret = ext2_write_gd_raw(ctx);
    if (ret != 0) return ret;
    ctx->meta_dirty = 0;
    return EXT2_OK;
}

/* ======================================================================== */
