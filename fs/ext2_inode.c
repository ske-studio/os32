#include "ext2_priv.h"

/*  iノード読み書き — g_blk使用                                             */
/* ======================================================================== */

int ext2_read_inode(u32 ino, Ext2Inode *inode)
{
    u32 index, block_num, offset_in_block;
    int ret, i;
    u8 *src;

    if (!ext2_mounted) return EXT2_ERR_NOMOUNT;
    if (ino == 0) return EXT2_ERR_NOTFOUND;

    index = (ino - 1) % ext2_sb_info.inodes_per_group;
    block_num = ext2_gd_info.inode_table + (index * ext2_sb_info.inode_size) / EXT2_BLOCK_SIZE;
    offset_in_block = (index * ext2_sb_info.inode_size) % EXT2_BLOCK_SIZE;

    ret = ext2_read_block(block_num, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;

    src = &ext2_g_blk[offset_in_block];
    inode->mode  = *(u16 *)&src[0];
    inode->uid   = *(u16 *)&src[2];
    inode->size  = *(u32 *)&src[4];
    inode->atime = *(u32 *)&src[8];
    inode->ctime = *(u32 *)&src[12];
    inode->mtime = *(u32 *)&src[16];
    inode->dtime = *(u32 *)&src[20];
    inode->gid   = *(u16 *)&src[24];
    inode->links_count = *(u16 *)&src[26];
    inode->blocks = *(u32 *)&src[28];
    inode->flags  = *(u32 *)&src[32];
    inode->osd1   = *(u32 *)&src[36];
    for (i = 0; i < EXT2_N_BLOCKS; i++) {
        inode->block[i] = *(u32 *)&src[40 + i * 4];
    }
    inode->generation = *(u32 *)&src[100];
    inode->file_acl   = *(u32 *)&src[104];
    inode->dir_acl    = *(u32 *)&src[108];
    inode->faddr      = *(u32 *)&src[112];
    ext2_mem_copy(inode->osd2, &src[116], 12);

    return EXT2_OK;
}

int ext2_write_inode(u32 ino, const Ext2Inode *inode)
{
    u32 index, block_num, offset_in_block;
    int ret, i;
    u8 *dst;

    if (!ext2_mounted) return EXT2_ERR_NOMOUNT;
    if (ino == 0) return EXT2_ERR_NOTFOUND;

    index = (ino - 1) % ext2_sb_info.inodes_per_group;
    block_num = ext2_gd_info.inode_table + (index * ext2_sb_info.inode_size) / EXT2_BLOCK_SIZE;
    offset_in_block = (index * ext2_sb_info.inode_size) % EXT2_BLOCK_SIZE;

    ret = ext2_read_block(block_num, ext2_g_blk);
    if (ret != 0) return EXT2_ERR_IO;

    dst = &ext2_g_blk[offset_in_block];
    *(u16 *)&dst[0]  = inode->mode;
    *(u16 *)&dst[2]  = inode->uid;
    *(u32 *)&dst[4]  = inode->size;
    *(u32 *)&dst[8]  = inode->atime;
    *(u32 *)&dst[12] = inode->ctime;
    *(u32 *)&dst[16] = inode->mtime;
    *(u32 *)&dst[20] = inode->dtime;
    *(u16 *)&dst[24] = inode->gid;
    *(u16 *)&dst[26] = inode->links_count;
    *(u32 *)&dst[28] = inode->blocks;
    *(u32 *)&dst[32] = inode->flags;
    *(u32 *)&dst[36] = inode->osd1;
    for (i = 0; i < EXT2_N_BLOCKS; i++) {
        *(u32 *)&dst[40 + i * 4] = inode->block[i];
    }
    *(u32 *)&dst[100] = inode->generation;
    *(u32 *)&dst[104] = inode->file_acl;
    *(u32 *)&dst[108] = inode->dir_acl;
    *(u32 *)&dst[112] = inode->faddr;
    ext2_mem_copy(&dst[116], inode->osd2, 12);

    return ext2_write_block(block_num, ext2_g_blk);
}

/* ======================================================================== */
/*  ビットマップ管理 — g_aux使用                                            */
/* ======================================================================== */

int ext2_alloc_block(void)
{
    int ret, byte_idx, bit_idx;
    u32 block_num;

    if (!ext2_mounted) return -1;
    if (ext2_gd_info.free_blocks == 0) return -1;

    ret = ext2_read_block(ext2_gd_info.block_bitmap, ext2_g_aux);
    if (ret != 0) return -1;

    for (byte_idx = 0; byte_idx < EXT2_BLOCK_SIZE; byte_idx++) {
        if (ext2_g_aux[byte_idx] == 0xFF) continue;
        for (bit_idx = 0; bit_idx < 8; bit_idx++) {
            if (!(ext2_g_aux[byte_idx] & (1 << bit_idx))) {
                block_num = (u32)(byte_idx * 8 + bit_idx) + ext2_sb_info.first_data_block;
                if (block_num >= ext2_sb_info.total_blocks) return -1;
                ext2_g_aux[byte_idx] |= (u8)(1 << bit_idx);
                ret = ext2_write_block(ext2_gd_info.block_bitmap, ext2_g_aux);
                if (ret != 0) return -1;
                ext2_gd_info.free_blocks--;
                ext2_sb_info.free_blocks_count--;
                return (int)block_num;
            }
        }
    }
    return -1;
}

void ext2_free_block(u32 block_num)
{
    int ret;
    u32 bit, byte_idx, bit_idx;

    if (!ext2_mounted) return;
    bit = block_num - ext2_sb_info.first_data_block;
    byte_idx = bit / 8;
    bit_idx  = bit % 8;

    ret = ext2_read_block(ext2_gd_info.block_bitmap, ext2_g_aux);
    if (ret != 0) return;
    ext2_g_aux[byte_idx] &= (u8)~(1 << bit_idx);
    ext2_write_block(ext2_gd_info.block_bitmap, ext2_g_aux);
    ext2_gd_info.free_blocks++;
    ext2_sb_info.free_blocks_count++;
}

int ext2_alloc_inode(void)
{
    int ret, byte_idx, bit_idx;
    u32 ino;

    if (!ext2_mounted) return -1;
    if (ext2_gd_info.free_inodes == 0) return -1;

    ret = ext2_read_block(ext2_gd_info.inode_bitmap, ext2_g_aux);
    if (ret != 0) return -1;

    for (byte_idx = 0; byte_idx < EXT2_BLOCK_SIZE; byte_idx++) {
        if (ext2_g_aux[byte_idx] == 0xFF) continue;
        for (bit_idx = 0; bit_idx < 8; bit_idx++) {
            if (!(ext2_g_aux[byte_idx] & (1 << bit_idx))) {
                ino = (u32)(byte_idx * 8 + bit_idx) + 1;
                if (ino > ext2_sb_info.total_inodes) return -1;
                ext2_g_aux[byte_idx] |= (u8)(1 << bit_idx);
                ret = ext2_write_block(ext2_gd_info.inode_bitmap, ext2_g_aux);
                if (ret != 0) return -1;
                ext2_gd_info.free_inodes--;
                ext2_sb_info.free_inodes_count--;
                return (int)ino;
            }
        }
    }
    return -1;
}

void ext2_free_inode(u32 ino)
{
    int ret;
    u32 bit, byte_idx, bit_idx;

    if (!ext2_mounted) return;
    bit = ino - 1;
    byte_idx = bit / 8;
    bit_idx  = bit % 8;

    ret = ext2_read_block(ext2_gd_info.inode_bitmap, ext2_g_aux);
    if (ret != 0) return;
    ext2_g_aux[byte_idx] &= (u8)~(1 << bit_idx);
    ext2_write_block(ext2_gd_info.inode_bitmap, ext2_g_aux);
    ext2_gd_info.free_inodes++;
    ext2_sb_info.free_inodes_count++;
}

/* ======================================================================== */
/*  間接ブロック: bmap — g_aux使用                                          */
/* ======================================================================== */

u32 ext2_bmap(const Ext2Inode *inode, u32 file_block)
{
    int ret;

    if (file_block < EXT2_NDIR_BLOCKS) {
        return inode->block[file_block];
    }

    file_block -= EXT2_NDIR_BLOCKS;
    if (file_block < EXT2_ADDR_PER_BLOCK) {
        if (inode->block[EXT2_IND_BLOCK] == 0) return 0;
        ret = ext2_read_block(inode->block[EXT2_IND_BLOCK], ext2_g_aux);
        if (ret != 0) return 0;
        return *(u32 *)&ext2_g_aux[file_block * 4];
    }

    file_block -= EXT2_ADDR_PER_BLOCK;
    if (file_block < EXT2_ADDR_PER_BLOCK * EXT2_ADDR_PER_BLOCK) {
        u32 ind1_idx = file_block / EXT2_ADDR_PER_BLOCK;
        u32 ind2_idx = file_block % EXT2_ADDR_PER_BLOCK;
        u32 ind1_block;

        if (inode->block[EXT2_DIND_BLOCK] == 0) return 0;
        ret = ext2_read_block(inode->block[EXT2_DIND_BLOCK], ext2_g_aux);
        if (ret != 0) return 0;
        ind1_block = *(u32 *)&ext2_g_aux[ind1_idx * 4];
        if (ind1_block == 0) return 0;
        ret = ext2_read_block(ind1_block, ext2_g_aux);
        if (ret != 0) return 0;
        return *(u32 *)&ext2_g_aux[ind2_idx * 4];
    }
    return 0;
}

int ext2_bmap_set(Ext2Inode *inode, u32 file_block, u32 phys_block)
{
    int ret;

    if (file_block < EXT2_NDIR_BLOCKS) {
        inode->block[file_block] = phys_block;
        return EXT2_OK;
    }

    file_block -= EXT2_NDIR_BLOCKS;
    if (file_block < EXT2_ADDR_PER_BLOCK) {
        if (inode->block[EXT2_IND_BLOCK] == 0) {
            int ind_blk = ext2_alloc_block();
            if (ind_blk < 0) return EXT2_ERR_NOSPC;
            inode->block[EXT2_IND_BLOCK] = (u32)ind_blk;
            inode->blocks += 2;
            ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
        } else {
            ret = ext2_read_block(inode->block[EXT2_IND_BLOCK], ext2_g_aux);
            if (ret != 0) return EXT2_ERR_IO;
        }
        *(u32 *)&ext2_g_aux[file_block * 4] = phys_block;
        return ext2_write_block(inode->block[EXT2_IND_BLOCK], ext2_g_aux) == 0
               ? EXT2_OK : EXT2_ERR_IO;
    }
    return EXT2_ERR_NOSPC;
}

/* ======================================================================== */
/*  ブロック解放 (内部)                                                      */
/* ======================================================================== */

void ext2_free_all_blocks(Ext2Inode *inode)
{
    int i;

    for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (inode->block[i] != 0) {
            ext2_free_block(inode->block[i]);
            inode->block[i] = 0;
        }
    }

    if (inode->block[EXT2_IND_BLOCK] != 0) {
        if (ext2_read_block(inode->block[EXT2_IND_BLOCK], ext2_g_aux) == 0) {
            u32 j;
            for (j = 0; j < EXT2_ADDR_PER_BLOCK; j++) {
                u32 blk = *(u32 *)&ext2_g_aux[j * 4];
                if (blk != 0) ext2_free_block(blk);
            }
        }
        ext2_free_block(inode->block[EXT2_IND_BLOCK]);
        inode->block[EXT2_IND_BLOCK] = 0;
    }

    if (inode->block[EXT2_DIND_BLOCK] != 0) {
        /* dindテーブルをg_auxに読む。indテーブルはg_blkに読む。
         * 同じバッファを再利用するとdindのエントリが破壊される。 */
        if (ext2_read_block(inode->block[EXT2_DIND_BLOCK], ext2_g_aux) == 0) {
            u32 j;
            for (j = 0; j < EXT2_ADDR_PER_BLOCK; j++) {
                u32 ind1 = *(u32 *)&ext2_g_aux[j * 4];
                if (ind1 != 0) {
                    if (ext2_read_block(ind1, ext2_g_blk) == 0) {
                        u32 k;
                        for (k = 0; k < EXT2_ADDR_PER_BLOCK; k++) {
                            u32 blk = *(u32 *)&ext2_g_blk[k * 4];
                            if (blk != 0) ext2_free_block(blk);
                        }
                    }
                    ext2_free_block(ind1);
                }
            }
        }
        ext2_free_block(inode->block[EXT2_DIND_BLOCK]);
        inode->block[EXT2_DIND_BLOCK] = 0;
    }

    inode->blocks = 0;
    inode->size = 0;
}

/* ======================================================================== */
