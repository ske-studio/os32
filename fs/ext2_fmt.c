#include "ext2_priv.h"

/*  ext2_format — ディスクにext2ファイルシステムを作成                       */
/*                                                                          */
/*  単一ブロックグループ、1KBブロック、128バイトinode。                       */
/*  最大 ~200MB のパーティションを想定。                                     */
/*                                                                          */
/*  ディスクレイアウト:                                                      */
/*    Block 0: (空: ブートブロック)                                         */
/*    Block 1: スーパーブロック                                             */
/*    Block 2: グループディスクリプタ                                       */
/*    Block 3: ブロックビットマップ                                         */
/*    Block 4: inodeビットマップ                                            */
/*    Block 5..N: inodeテーブル                                             */
/*    Block N+1..: データブロック                                           */
/* ======================================================================== */
int ext2_format(int ide_drive, u32 total_sectors)
{
    u32 total_blocks, inodes_count, inode_table_blocks;
    u32 first_data_block_num, overhead;
    u32 i, j, sector;
    int ret;
    int saved_drive;

    /* 既にマウント中なら先にアンマウント */
    if (ext2_mounted) ext2_unmount();

    if (!ide_drive_present(ide_drive)) return EXT2_ERR_IO;

    saved_drive = ext2_drive_num;
    ext2_drive_num = ide_drive;

    total_blocks = total_sectors / 2;  /* 512B→1KB */
    if (total_blocks < 64) { ext2_drive_num = saved_drive; return EXT2_ERR_NOSPC; }

    /* inodeの数: ブロック4個あたり1 inode (Linux mkfs.ext2のデフォルトに近い) */
    inodes_count = total_blocks / 4;
    if (inodes_count < 16) inodes_count = 16;
    if (inodes_count > 65536) inodes_count = 65536;

    /* inodeテーブルに必要なブロック数 (128バイト/inode) */
    inode_table_blocks = (inodes_count * 128 + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

    /* オーバーヘッド: boot + SB + GD + blk_bmap + ino_bmap + inode_table */
    overhead = 1 + 1 + 1 + 1 + 1 + inode_table_blocks;
    first_data_block_num = overhead;

    /* ===== Block 0: ブートブロック (ゼロクリア) ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    ret = ext2_write_block(0, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 1: スーパーブロック ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    *(u32 *)&ext2_g_blk[0]  = inodes_count;       /* s_inodes_count */
    *(u32 *)&ext2_g_blk[4]  = total_blocks;       /* s_blocks_count */
    *(u32 *)&ext2_g_blk[8]  = 0;                  /* s_r_blocks_count */
    *(u32 *)&ext2_g_blk[12] = total_blocks - overhead - 1;  /* s_free_blocks_count (-1 for root dir) */
    *(u32 *)&ext2_g_blk[16] = inodes_count - 11;  /* s_free_inodes_count (1-10 reserved + root dir) */
    *(u32 *)&ext2_g_blk[20] = 1;                  /* s_first_data_block (1 for 1KB block) */
    *(u32 *)&ext2_g_blk[24] = 0;                  /* s_log_block_size (0 = 1KB) */
    *(u32 *)&ext2_g_blk[28] = 0;                  /* s_log_frag_size */
    *(u32 *)&ext2_g_blk[32] = total_blocks;       /* s_blocks_per_group */
    *(u32 *)&ext2_g_blk[36] = total_blocks;       /* s_frags_per_group */
    *(u32 *)&ext2_g_blk[40] = inodes_count;       /* s_inodes_per_group */
    *(u32 *)&ext2_g_blk[44] = 0;                  /* s_mtime */
    *(u32 *)&ext2_g_blk[48] = ext2_current_time();/* s_wtime */
    *(u16 *)&ext2_g_blk[52] = 0;                  /* s_mnt_count */
    *(u16 *)&ext2_g_blk[54] = (u16)0xFFFF;        /* s_max_mnt_count */
    *(u16 *)&ext2_g_blk[56] = EXT2_SUPER_MAGIC;   /* s_magic */
    *(u16 *)&ext2_g_blk[58] = 1;                  /* s_state = VALID_FS */
    *(u16 *)&ext2_g_blk[60] = 1;                  /* s_errors = CONTINUE */
    *(u16 *)&ext2_g_blk[62] = 0;                  /* s_minor_rev_level */
    *(u32 *)&ext2_g_blk[64] = 0;                  /* s_lastcheck */
    *(u32 *)&ext2_g_blk[68] = 0;                  /* s_checkinterval */
    *(u32 *)&ext2_g_blk[72] = 0;                  /* s_creator_os = LINUX */
    *(u32 *)&ext2_g_blk[76] = 1;                  /* s_rev_level = DYNAMIC_REV */
    *(u16 *)&ext2_g_blk[80] = 0;                  /* s_def_resuid */
    *(u16 *)&ext2_g_blk[82] = 0;                  /* s_def_resgid */
    *(u32 *)&ext2_g_blk[84] = 11;                 /* s_first_ino */
    *(u16 *)&ext2_g_blk[88] = 128;               /* s_inode_size */
    /* s_volume_name at offset 120 */
    ext2_g_blk[120] = 'O'; ext2_g_blk[121] = 'S'; ext2_g_blk[122] = '3'; ext2_g_blk[123] = '2';
    ext2_g_blk[124] = '_'; ext2_g_blk[125] = 'H'; ext2_g_blk[126] = 'D'; ext2_g_blk[127] = 'D';

    ret = ext2_write_block(1, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 2: グループディスクリプタ ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    *(u32 *)&ext2_g_blk[0]  = 3;                  /* bg_block_bitmap */
    *(u32 *)&ext2_g_blk[4]  = 4;                  /* bg_inode_bitmap */
    *(u32 *)&ext2_g_blk[8]  = 5;                  /* bg_inode_table */
    *(u16 *)&ext2_g_blk[12] = (u16)(total_blocks - overhead - 1); /* bg_free_blocks_count */
    *(u16 *)&ext2_g_blk[14] = (u16)(inodes_count - 11);  /* bg_free_inodes_count */
    *(u16 *)&ext2_g_blk[16] = 1;                  /* bg_used_dirs_count (root dir) */

    ret = ext2_write_block(2, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 3: ブロックビットマップ ===== */
    /* オーバーヘッドブロック + ルートディレクトリの1ブロック分を使用済み */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    /* ブロック0..(overhead-1)とルートディレクトリ用ブロックをマーク */
    for (i = 0; i < overhead; i++) {
        ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
    }
    /* ルートディレクトリ用データブロック (first_data_block_num) */
    ext2_g_blk[first_data_block_num / 8] |= (u8)(1 << (first_data_block_num % 8));
    /* total_blocks以降のビットも1にする (使用不可領域) */
    for (i = total_blocks; i < EXT2_BLOCK_SIZE * 8 && i < total_blocks + 32; i++) {
        ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
    }

    ret = ext2_write_block(3, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 4: inodeビットマップ ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    /* inode 1-10 は予約、inode 2 (root dir) は使用中 */
    /* ビット0=inode1, ビット1=inode2, ... */
    for (i = 0; i < 10; i++) {
        ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
    }
    /* inode 11 (first_ino) は最初の空きinode */
    /* inodes_count以降のビットも1にする */
    for (i = inodes_count; i < EXT2_BLOCK_SIZE * 8 && i < inodes_count + 32; i++) {
        ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
    }

    ret = ext2_write_block(4, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 5..N: inodeテーブル (ゼロクリア) ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    for (i = 0; i < inode_table_blocks; i++) {
        ret = ext2_write_block(5 + i, ext2_g_blk);
        if (ret != 0) goto err;
    }

    /* ===== inode 2: ルートディレクトリ ===== */
    /* inodeテーブルのinode 2 (インデックス1) を書き込み */
    {
        u32 ino_block = 5 + (1 * 128) / EXT2_BLOCK_SIZE;  /* inode 2 = index 1 */
        u32 ino_offset = (1 * 128) % EXT2_BLOCK_SIZE;

        ret = ext2_read_block(ino_block, ext2_g_blk);
        if (ret != 0) goto err;

        /* modeフィールド: directory + 0755 */
        *(u16 *)&ext2_g_blk[ino_offset + 0] = (u16)(EXT2_S_IFDIR | 0755);
        /* uid = 0 */
        *(u16 *)&ext2_g_blk[ino_offset + 2] = 0;
        /* size = 1024 (1ブロック分) */
        *(u32 *)&ext2_g_blk[ino_offset + 4] = EXT2_BLOCK_SIZE;
        /* atime, ctime, mtime */
        {
            u32 now = ext2_current_time();
            *(u32 *)&ext2_g_blk[ino_offset + 8]  = now;
            *(u32 *)&ext2_g_blk[ino_offset + 12] = now;
            *(u32 *)&ext2_g_blk[ino_offset + 16] = now;
        }
        /* links_count = 2 (. と ..) */
        *(u16 *)&ext2_g_blk[ino_offset + 26] = 2;
        /* blocks = 2 (512B単位で2 = 1KB) */
        *(u32 *)&ext2_g_blk[ino_offset + 28] = 2;
        /* block[0] = first_data_block_num */
        *(u32 *)&ext2_g_blk[ino_offset + 40] = first_data_block_num;

        ret = ext2_write_block(ino_block, ext2_g_blk);
        if (ret != 0) goto err;
    }

    /* ===== ルートディレクトリデータブロック ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    /* "." エントリ */
    *(u32 *)&ext2_g_blk[0]  = 2;     /* inode = 2 */
    *(u16 *)&ext2_g_blk[4]  = 12;    /* rec_len = 12 */
    ext2_g_blk[6] = 1;               /* name_len = 1 */
    ext2_g_blk[7] = EXT2_FT_DIR;     /* file_type */
    ext2_g_blk[8] = '.';             /* name */
    /* ".." エントリ */
    *(u32 *)&ext2_g_blk[12] = 2;     /* inode = 2 (root の parent は self) */
    *(u16 *)&ext2_g_blk[16] = (u16)(EXT2_BLOCK_SIZE - 12); /* rec_len = 残り全部 */
    ext2_g_blk[18] = 2;              /* name_len = 2 */
    ext2_g_blk[19] = EXT2_FT_DIR;    /* file_type */
    ext2_g_blk[20] = '.'; ext2_g_blk[21] = '.';

    ret = ext2_write_block(first_data_block_num, ext2_g_blk);
    if (ret != 0) goto err;

    ext2_drive_num = saved_drive;
    return EXT2_OK;

err:
    ext2_drive_num = saved_drive;
    return EXT2_ERR_IO;
}

