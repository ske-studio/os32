#include "ext2_priv.h"

/*  ext2_format — ディスクにext2ファイルシステムを作成                       */
/*                                                                          */
/*  マルチブロックグループ対応、1KBブロック、128バイトinode。                 */
/*  SPARSE_SUPER対応: Linux mkfs.ext2/e2fsck互換のレイアウトを生成する。     */
/*                                                                          */
/*  スパースグループ規則:                                                    */
/*    グループ 0, 1, 3^n, 5^n, 7^n にのみSBバックアップ+GDTコピーを配置。  */
/*    それ以外のグループはビットマップから開始する。                          */
/*                                                                          */
/*  各グループのレイアウト:                                                  */
/*    スパースグループ:                                                      */
/*      SBバックアップ(1) + GDTコピー(N) + bmap(1) + imap(1) + itable       */
/*    非スパースグループ:                                                    */
/*      bmap(1) + imap(1) + itable                                          */
/* ======================================================================== */

/* グループごとのメタデータ情報 */
typedef struct {
    u32 group_start;    /* グループの最初のブロック番号 (first_data_blockからの絶対値) */
    u32 block_bitmap;   /* ブロックビットマップのブロック番号 */
    u32 inode_bitmap;   /* inodeビットマップのブロック番号 */
    u32 inode_table;    /* inodeテーブル開始ブロック番号 */
    u32 data_start;     /* データブロック開始位置 */
    u32 blocks_in_group;/* このグループのブロック数 */
    u32 inodes_in_group;/* このグループのinode数 */
    u32 inode_tbl_blocks;/* inodeテーブルのブロック数 */
} GroupLayout;

/* SPARSE_SUPER規則: 0, 1, 3^n, 5^n, 7^n のグループにSBバックアップを配置 */
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001

static int is_power_of(u32 n, u32 base)
{
    u32 v = base;
    while (v < n) v *= base;
    return (v == n) ? 1 : 0;
}

static int is_sparse_group(u32 g)
{
    if (g <= 1) return 1;
    return (is_power_of(g, 3) || is_power_of(g, 5) || is_power_of(g, 7)) ? 1 : 0;
}

int ext2_format(int ide_drive, u32 total_sectors)
{
    u32 total_blocks, inodes_count, num_groups;
    u32 inodes_per_group, inode_tbl_blocks_per_group;
    u32 gdt_blocks;
    u32 g, i;
    int ret;
    int saved_drive;
    GroupLayout gl;

    /* 既にマウント中なら先にアンマウント */
    if (ext2_mounted) ext2_unmount();

    if (!ide_drive_present(ide_drive)) return EXT2_ERR_IO;

    saved_drive = ext2_drive_num;
    ext2_drive_num = ide_drive;

    total_blocks = total_sectors / 2;  /* 512B→1KB */
    if (total_blocks < 64) { ext2_drive_num = saved_drive; return EXT2_ERR_NOSPC; }

    /* グループ数を計算 (1KBブロック時、1グループ最大8192ブロック) */
    num_groups = (total_blocks - 1 + EXT2_BLOCKS_PER_GROUP_MAX - 1) / EXT2_BLOCKS_PER_GROUP_MAX;
    if (num_groups == 0) num_groups = 1;
    if (num_groups > EXT2_MAX_GROUPS) { ext2_drive_num = saved_drive; return EXT2_ERR_NOSPC; }

    /* inodeの数: ブロック4個あたり1 inode (mkfs.ext2のデフォルトに近い) */
    inodes_count = total_blocks / 4;
    if (inodes_count < 16) inodes_count = 16;

    /* inodes_per_groupはグループ数で均等割り (8の倍数に切り上げ) */
    inodes_per_group = (inodes_count + num_groups - 1) / num_groups;
    inodes_per_group = (inodes_per_group + 7) & ~7u;  /* 8の倍数 */
    inodes_count = inodes_per_group * num_groups;

    /* グループごとのinodeテーブルブロック数 */
    inode_tbl_blocks_per_group = (inodes_per_group * 128 + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

    /* GDTブロック数 (32B × num_groups) */
    gdt_blocks = (num_groups * 32 + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

    /* ===== Block 0: ブートブロック (ゼロクリア) ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    ret = ext2_write_block(0, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== Block 1: スーパーブロック ===== */
    ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
    *(u32 *)&ext2_g_blk[0]  = inodes_count;          /* s_inodes_count */
    *(u32 *)&ext2_g_blk[4]  = total_blocks;          /* s_blocks_count */
    *(u32 *)&ext2_g_blk[8]  = 0;                     /* s_r_blocks_count */
    /* s_free_blocks_count は後で計算 (仮値) */
    *(u32 *)&ext2_g_blk[12] = 0;
    *(u32 *)&ext2_g_blk[16] = inodes_count - 10;     /* s_free_inodes_count (予約inode 1-10) */
    *(u32 *)&ext2_g_blk[20] = 1;                     /* s_first_data_block (1 for 1KB block) */
    *(u32 *)&ext2_g_blk[24] = 0;                     /* s_log_block_size (0 = 1KB) */
    *(u32 *)&ext2_g_blk[28] = 0;                     /* s_log_frag_size */
    *(u32 *)&ext2_g_blk[32] = EXT2_BLOCKS_PER_GROUP_MAX; /* s_blocks_per_group */
    *(u32 *)&ext2_g_blk[36] = EXT2_BLOCKS_PER_GROUP_MAX; /* s_frags_per_group */
    *(u32 *)&ext2_g_blk[40] = inodes_per_group;      /* s_inodes_per_group */
    *(u32 *)&ext2_g_blk[44] = 0;                     /* s_mtime */
    *(u32 *)&ext2_g_blk[48] = ext2_current_time();   /* s_wtime */
    *(u16 *)&ext2_g_blk[52] = 0;                     /* s_mnt_count */
    *(u16 *)&ext2_g_blk[54] = (u16)0xFFFF;           /* s_max_mnt_count */
    *(u16 *)&ext2_g_blk[56] = EXT2_SUPER_MAGIC;      /* s_magic */
    *(u16 *)&ext2_g_blk[58] = 1;                     /* s_state = VALID_FS */
    *(u16 *)&ext2_g_blk[60] = 1;                     /* s_errors = CONTINUE */
    *(u16 *)&ext2_g_blk[62] = 0;                     /* s_minor_rev_level */
    *(u32 *)&ext2_g_blk[64] = 0;                     /* s_lastcheck */
    *(u32 *)&ext2_g_blk[68] = 0;                     /* s_checkinterval */
    *(u32 *)&ext2_g_blk[72] = 0;                     /* s_creator_os = LINUX */
    *(u32 *)&ext2_g_blk[76] = 1;                     /* s_rev_level = DYNAMIC_REV */
    *(u16 *)&ext2_g_blk[80] = 0;                     /* s_def_resuid */
    *(u16 *)&ext2_g_blk[82] = 0;                     /* s_def_resgid */
    *(u32 *)&ext2_g_blk[84] = 11;                    /* s_first_ino */
    *(u16 *)&ext2_g_blk[88] = 128;                   /* s_inode_size */
    *(u16 *)&ext2_g_blk[90] = 0;                     /* s_block_group_nr */
    *(u32 *)&ext2_g_blk[96] = 0x0002;                 /* s_feature_incompat = FILETYPE */
    *(u32 *)&ext2_g_blk[100] = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
    /* s_volume_name at offset 120 */
    ext2_g_blk[120] = 'O'; ext2_g_blk[121] = 'S'; ext2_g_blk[122] = '3'; ext2_g_blk[123] = '2';
    ext2_g_blk[124] = '_'; ext2_g_blk[125] = 'H'; ext2_g_blk[126] = 'D'; ext2_g_blk[127] = 'D';

    ret = ext2_write_block(1, ext2_g_blk);
    if (ret != 0) goto err;

    /* ===== GDT + 各グループのメタデータ初期化 ===== */
    {
        u32 total_free_blocks = 0;
        u32 total_overhead = 0;

        /* まずGDTブロックをゼロクリアして書き込み準備 */
        ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);

        /* 各グループのレイアウトを計算してGDTに書き込み */
        for (g = 0; g < num_groups; g++) {
            u32 gd_offset = (g * 32) % EXT2_BLOCK_SIZE;
            u32 overhead;

            /* このグループのブロック範囲 */
            gl.group_start = 1 + g * EXT2_BLOCKS_PER_GROUP_MAX; /* first_data_block=1 */
            gl.blocks_in_group = EXT2_BLOCKS_PER_GROUP_MAX;
            if (g == num_groups - 1) {
                /* 最終グループ: 残りブロック */
                gl.blocks_in_group = total_blocks - gl.group_start;
            }
            gl.inodes_in_group = inodes_per_group;
            gl.inode_tbl_blocks = inode_tbl_blocks_per_group;

            if (is_sparse_group(g)) {
                /* スパースグループ: SB(1) + GDT(gdt_blocks) + bmap + imap + itable */
                gl.block_bitmap = gl.group_start + 1 + gdt_blocks;
                gl.inode_bitmap = gl.block_bitmap + 1;
                gl.inode_table  = gl.inode_bitmap + 1;
                gl.data_start   = gl.inode_table + gl.inode_tbl_blocks;
                overhead = gl.data_start - gl.group_start;
            } else {
                /* 非スパースグループ: bmap + imap + itable のみ */
                gl.block_bitmap = gl.group_start;
                gl.inode_bitmap = gl.block_bitmap + 1;
                gl.inode_table  = gl.inode_bitmap + 1;
                gl.data_start   = gl.inode_table + gl.inode_tbl_blocks;
                overhead = gl.data_start - gl.group_start;
            }

            total_overhead += overhead;

            /* GDTブロックの該当オフセットに書き込み */
            if (gd_offset == 0 && g > 0) {
                /* 新しいGDTブロックの先頭 → 前ブロックを書き込み済み → ゼロクリア */
                ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
            }

            *(u32 *)&ext2_g_blk[gd_offset + 0]  = gl.block_bitmap;
            *(u32 *)&ext2_g_blk[gd_offset + 4]  = gl.inode_bitmap;
            *(u32 *)&ext2_g_blk[gd_offset + 8]  = gl.inode_table;
            /* free_blocks, free_inodes は後で設定 */
            /* ルートディレクトリはグループ0のデータブロック1つを使用 */
            if (g == 0) {
                *(u16 *)&ext2_g_blk[gd_offset + 12] = (u16)(gl.blocks_in_group - overhead - 1);
                *(u16 *)&ext2_g_blk[gd_offset + 14] = (u16)(gl.inodes_in_group - 10);
                *(u16 *)&ext2_g_blk[gd_offset + 16] = 1;  /* used_dirs (root) */
                total_free_blocks += gl.blocks_in_group - overhead - 1;
            } else {
                *(u16 *)&ext2_g_blk[gd_offset + 12] = (u16)(gl.blocks_in_group - overhead);
                *(u16 *)&ext2_g_blk[gd_offset + 14] = (u16)gl.inodes_in_group;
                *(u16 *)&ext2_g_blk[gd_offset + 16] = 0;
                total_free_blocks += gl.blocks_in_group - overhead;
            }

            /* GDTブロック末尾 or 最後のグループ → 書き込み */
            if (gd_offset + 32 >= EXT2_BLOCK_SIZE || g == num_groups - 1) {
                u32 gd_block = 2 + (g * 32) / EXT2_BLOCK_SIZE;
                ret = ext2_write_block(gd_block, ext2_g_blk);
                if (ret != 0) goto err;
            }
        }

        /* スーパーブロックのfree_blocks_countを更新 */
        ret = ext2_read_block(1, ext2_g_blk);
        if (ret != 0) goto err;
        *(u32 *)&ext2_g_blk[12] = total_free_blocks;
        ret = ext2_write_block(1, ext2_g_blk);
        if (ret != 0) goto err;

        /* ===== スパースグループへのSBバックアップ + GDTコピー ===== */
        for (g = 1; g < num_groups; g++) {
            u32 gs;
            if (!is_sparse_group(g)) continue;
            gs = 1 + g * EXT2_BLOCKS_PER_GROUP_MAX;

            /* SBバックアップ: プライマリSBを読み、s_block_group_nrを変更 */
            ret = ext2_read_block(1, ext2_g_blk);
            if (ret != 0) goto err;
            *(u16 *)&ext2_g_blk[90] = (u16)g;  /* s_block_group_nr = g */
            ret = ext2_write_block(gs, ext2_g_blk);
            if (ret != 0) goto err;

            /* GDTコピー: プライマリGDTを各ブロックごとにコピー */
            for (i = 0; i < gdt_blocks; i++) {
                ret = ext2_read_block(2 + i, ext2_g_blk);
                if (ret != 0) goto err;
                ret = ext2_write_block(gs + 1 + i, ext2_g_blk);
                if (ret != 0) goto err;
            }
        }

        /* ===== 各グループのビットマップとinodeテーブルを初期化 ===== */
        for (g = 0; g < num_groups; g++) {
            u32 overhead, bmap_blk, imap_blk, itable_blk;

            /* レイアウトを再計算 */
            gl.group_start = 1 + g * EXT2_BLOCKS_PER_GROUP_MAX;
            gl.blocks_in_group = EXT2_BLOCKS_PER_GROUP_MAX;
            if (g == num_groups - 1) {
                gl.blocks_in_group = total_blocks - gl.group_start;
            }

            if (is_sparse_group(g)) {
                /* スパースグループ: SB + GDT + bmap + imap + itable */
                bmap_blk   = gl.group_start + 1 + gdt_blocks;
                imap_blk   = bmap_blk + 1;
                itable_blk = imap_blk + 1;
                overhead = (itable_blk + inode_tbl_blocks_per_group) - gl.group_start;
            } else {
                /* 非スパースグループ: bmap + imap + itable のみ */
                bmap_blk  = gl.group_start;
                imap_blk  = bmap_blk + 1;
                itable_blk = imap_blk + 1;
                overhead = 1 + 1 + inode_tbl_blocks_per_group;
            }

            /* ブロックビットマップ */
            ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
            /* オーバーヘッドブロックをマーク */
            for (i = 0; i < overhead; i++) {
                ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
            }
            /* グループ0: ルートディレクトリ用データブロック1つ */
            if (g == 0) {
                ext2_g_blk[overhead / 8] |= (u8)(1 << (overhead % 8));
            }
            /* グループ内のブロック数以降のビットを全て使用不可にマーク
             * (ext2仕様: ビットマップのパディング部分は1) */
            for (i = gl.blocks_in_group; i < EXT2_BLOCKS_PER_GROUP_MAX; i++) {
                ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
            }
            ret = ext2_write_block(bmap_blk, ext2_g_blk);
            if (ret != 0) goto err;

            /* inodeビットマップ */
            ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
            if (g == 0) {
                /* inode 1-10は予約 (ビット0-9) */
                for (i = 0; i < 10; i++) {
                    ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
                }
            }
            /* inodes_per_group以降のビットを全て使用不可にマーク
             * (ext2仕様: ビットマップのパディング部分は1) */
            for (i = inodes_per_group; i < EXT2_BLOCKS_PER_GROUP_MAX; i++) {
                ext2_g_blk[i / 8] |= (u8)(1 << (i % 8));
            }
            ret = ext2_write_block(imap_blk, ext2_g_blk);
            if (ret != 0) goto err;

            /* inodeテーブル (ゼロクリア) */
            ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
            for (i = 0; i < inode_tbl_blocks_per_group; i++) {
                ret = ext2_write_block(itable_blk + i, ext2_g_blk);
                if (ret != 0) goto err;
            }
        }

        /* ===== グループ0: inode 2 (ルートディレクトリ) ===== */
        {
            u32 itable_g0, ino_block, ino_offset, root_data_blk;

            itable_g0 = 1 + 1 + gdt_blocks + 1 + 1; /* boot+SB+GDT+bmap+imap */
            ino_block = itable_g0 + (1 * 128) / EXT2_BLOCK_SIZE;  /* inode 2 = index 1 */
            ino_offset = (1 * 128) % EXT2_BLOCK_SIZE;
            root_data_blk = itable_g0 + inode_tbl_blocks_per_group; /* オーバーヘッド直後 */

            ret = ext2_read_block(ino_block, ext2_g_blk);
            if (ret != 0) goto err;

            *(u16 *)&ext2_g_blk[ino_offset + 0] = (u16)(EXT2_S_IFDIR | 0755);
            *(u16 *)&ext2_g_blk[ino_offset + 2] = 0;
            *(u32 *)&ext2_g_blk[ino_offset + 4] = EXT2_BLOCK_SIZE;
            {
                u32 now = ext2_current_time();
                *(u32 *)&ext2_g_blk[ino_offset + 8]  = now;
                *(u32 *)&ext2_g_blk[ino_offset + 12] = now;
                *(u32 *)&ext2_g_blk[ino_offset + 16] = now;
            }
            *(u16 *)&ext2_g_blk[ino_offset + 26] = 2;    /* links_count */
            *(u32 *)&ext2_g_blk[ino_offset + 28] = 2;    /* blocks (512B単位) */
            *(u32 *)&ext2_g_blk[ino_offset + 40] = root_data_blk; /* block[0] */

            ret = ext2_write_block(ino_block, ext2_g_blk);
            if (ret != 0) goto err;

            /* ルートディレクトリデータブロック */
            ext2_mem_zero(ext2_g_blk, EXT2_BLOCK_SIZE);
            *(u32 *)&ext2_g_blk[0]  = 2;     /* "." → inode 2 */
            *(u16 *)&ext2_g_blk[4]  = 12;
            ext2_g_blk[6] = 1;
            ext2_g_blk[7] = EXT2_FT_DIR;
            ext2_g_blk[8] = '.';
            *(u32 *)&ext2_g_blk[12] = 2;     /* ".." → inode 2 */
            *(u16 *)&ext2_g_blk[16] = (u16)(EXT2_BLOCK_SIZE - 12);
            ext2_g_blk[18] = 2;
            ext2_g_blk[19] = EXT2_FT_DIR;
            ext2_g_blk[20] = '.'; ext2_g_blk[21] = '.';

            ret = ext2_write_block(root_data_blk, ext2_g_blk);
            if (ret != 0) goto err;
        }
    }

    ext2_drive_num = saved_drive;
    return EXT2_OK;

err:
    ext2_drive_num = saved_drive;
    return EXT2_ERR_IO;
}
