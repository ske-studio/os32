#include "ext2_priv.h"

/*  ファイル読み込み — g_aux使用                                            */
/* ======================================================================== */

int ext2_read_file(Ext2Ctx *ctx, u32 ino, void *buf, u32 max_size)
{
    Ext2Inode inode;
    int ret;
    u32 bi, remaining, to_copy, total_read, phys;
    u8 *dst;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    remaining = inode.size;
    if (remaining > max_size) remaining = max_size;
    total_read = 0;
    dst = (u8 *)buf;

    for (bi = 0; remaining > 0; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) break;

        to_copy = remaining;
        if (to_copy > EXT2_BLOCK_SIZE) to_copy = EXT2_BLOCK_SIZE;

        /* 宛先バッファに直接読み込み — ext2_g_auxを経由しない。
         * ext2_bmapが間接ブロック参照でext2_g_auxを使うため、
         * ここでext2_g_auxに読むとバッファ競合が発生する。 */
        ret = ext2_read_block(ctx, phys, &dst[total_read]);
        if (ret != 0) return EXT2_ERR_IO;

        total_read += to_copy;
        remaining -= to_copy;
    }
    return (int)total_read;
}

int ext2_read_stream(Ext2Ctx *ctx, u32 ino, void *buf, u32 size, u32 offset)
{
    Ext2Inode inode;
    int ret;
    u32 bi, remaining, to_copy, total_read, phys;
    u32 byte_in_blk;
    u8 *dst;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    if (offset >= inode.size) return 0;
    remaining = inode.size - offset;
    if (remaining > size) remaining = size;
    
    total_read = 0;
    dst = (u8 *)buf;

    bi = offset / EXT2_BLOCK_SIZE;
    byte_in_blk = offset % EXT2_BLOCK_SIZE;

    for (; remaining > 0; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) break;

        to_copy = EXT2_BLOCK_SIZE - byte_in_blk;
        if (to_copy > remaining) to_copy = remaining;

        if (byte_in_blk == 0 && to_copy == EXT2_BLOCK_SIZE) {
            /* ブロック全体: 直接宛先に読み込み */
            ret = ext2_read_block(ctx, phys, &dst[total_read]);
        } else {
            /* 部分ブロック: ext2_g_blkを中間バッファとして使用
             * (ext2_g_auxはext2_bmapと競合するため使えない) */
            ret = ext2_read_block(ctx, phys, ext2_g_blk);
            if (ret == 0) {
                ext2_mem_copy(&dst[total_read], &ext2_g_blk[byte_in_blk], to_copy);
            }
        }
        if (ret != 0) return EXT2_ERR_IO;

        total_read += to_copy;
        remaining -= to_copy;
        byte_in_blk = 0;
    }
    return (int)total_read;
}

/* ======================================================================== */
/*  ファイル作成 / 書き込み / 削除                                          */
/* ======================================================================== */

int ext2_create(Ext2Ctx *ctx, u32 dir_ino, const char *name, const void *data, u32 size)
{
    int new_ino;
    Ext2Inode inode;
    u32 blocks_needed, bi, remaining, to_write, now;
    const u8 *src;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    { u32 tmp; if (ext2_find_entry(ctx, dir_ino, name, &tmp, (u8 *)0) == EXT2_OK) return EXT2_ERR_EXIST; }

    new_ino = ext2_alloc_inode(ctx);
    if (new_ino < 0) return EXT2_ERR_NOSPC;

    now = ext2_current_time();
    ext2_mem_zero(&inode, sizeof(inode));
    inode.mode = (u16)(EXT2_S_IFREG | 0644);
    inode.size = size;
    inode.atime = now; inode.ctime = now; inode.mtime = now;
    inode.links_count = 1;

    blocks_needed = (size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
    src = (const u8 *)data;
    remaining = size;

    for (bi = 0; bi < blocks_needed; bi++) {
        int blk = ext2_alloc_block(ctx);
        if (blk < 0) { ext2_free_all_blocks(ctx, &inode); ext2_free_inode(ctx, (u32)new_ino); return EXT2_ERR_NOSPC; }

        ret = ext2_bmap_set(ctx, &inode, bi, (u32)blk);
        if (ret != 0) { ext2_free_block(ctx, (u32)blk); ext2_free_all_blocks(ctx, &inode); ext2_free_inode(ctx, (u32)new_ino); return ret; }

        ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
        to_write = remaining;
        if (to_write > EXT2_BLOCK_SIZE) to_write = EXT2_BLOCK_SIZE;
        ext2_mem_copy(ext2_g_aux, &src[bi * EXT2_BLOCK_SIZE], to_write);

        ret = ext2_write_block(ctx, (u32)blk, ext2_g_aux);
        if (ret != 0) { ext2_free_all_blocks(ctx, &inode); ext2_free_inode(ctx, (u32)new_ino); return EXT2_ERR_IO; }

        inode.blocks += 2;
        remaining -= to_write;
    }

    ret = ext2_write_inode(ctx, (u32)new_ino, &inode);
    if (ret != 0) return ret;

    ret = ext2_add_entry(ctx, dir_ino, name, (u32)new_ino, EXT2_FT_REG_FILE);
    if (ret != 0) { ext2_free_all_blocks(ctx, &inode); ext2_free_inode(ctx, (u32)new_ino); return ret; }

    ext2_sync(ctx);
    return EXT2_OK;
}

int ext2_write(Ext2Ctx *ctx, u32 ino, const void *data, u32 size)
{
    Ext2Inode inode;
    u32 blocks_needed, bi, remaining, to_write, now;
    const u8 *src;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFREG) return EXT2_ERR_ISDIR;

    ext2_free_all_blocks(ctx, &inode);

    now = ext2_current_time();
    inode.size = size;
    inode.mtime = now; inode.ctime = now;

    blocks_needed = (size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
    src = (const u8 *)data;
    remaining = size;

    for (bi = 0; bi < blocks_needed; bi++) {
        int blk = ext2_alloc_block(ctx);
        if (blk < 0) { ext2_free_all_blocks(ctx, &inode); ext2_write_inode(ctx, ino, &inode); return EXT2_ERR_NOSPC; }

        ret = ext2_bmap_set(ctx, &inode, bi, (u32)blk);
        if (ret != 0) { ext2_free_block(ctx, (u32)blk); return ret; }

        ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
        to_write = remaining;
        if (to_write > EXT2_BLOCK_SIZE) to_write = EXT2_BLOCK_SIZE;
        ext2_mem_copy(ext2_g_aux, &src[bi * EXT2_BLOCK_SIZE], to_write);

        ret = ext2_write_block(ctx, (u32)blk, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        inode.blocks += 2;
        remaining -= to_write;
    }

    ret = ext2_write_inode(ctx, ino, &inode);
    if (ret != 0) return ret;
    ext2_sync(ctx);
    return EXT2_OK;
}

int ext2_write_stream(Ext2Ctx *ctx, u32 ino, const void *buf, u32 size, u32 offset)
{
    Ext2Inode inode;
    int ret;
    u32 bi, remaining, to_write, phys;
    u32 byte_in_blk, now;
    const u8 *src;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    now = ext2_current_time();
    inode.mtime = now;

    src = (const u8 *)buf;
    remaining = size;

    bi = offset / EXT2_BLOCK_SIZE;
    byte_in_blk = offset % EXT2_BLOCK_SIZE;

    for (; remaining > 0; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) {
            int new_blk = ext2_alloc_block(ctx);
            if (new_blk < 0) break;
            ret = ext2_bmap_set(ctx, &inode, bi, (u32)new_blk);
            if (ret != 0) { ext2_free_block(ctx, (u32)new_blk); break; }
            inode.blocks += 2;
            phys = (u32)new_blk;
            if (byte_in_blk > 0 || remaining < EXT2_BLOCK_SIZE) {
                ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
            }
        } else {
            if (byte_in_blk > 0 || remaining < EXT2_BLOCK_SIZE) {
                ret = ext2_read_block(ctx, phys, ext2_g_aux);
                if (ret != 0) break;
            }
        }

        to_write = EXT2_BLOCK_SIZE - byte_in_blk;
        if (to_write > remaining) to_write = remaining;

        ext2_mem_copy(&ext2_g_aux[byte_in_blk], &src[size - remaining], to_write);

        ret = ext2_write_block(ctx, phys, ext2_g_aux);
        if (ret != 0) break;

        remaining -= to_write;
        byte_in_blk = 0;

        if (offset + size - remaining > inode.size) {
            inode.size = offset + size - remaining;
        }
    }
    
    ext2_write_inode(ctx, ino, &inode);
    ext2_sync(ctx);
    return (int)(size - remaining);
}

int ext2_get_size_ino(Ext2Ctx *ctx, u32 ino, u32 *size)
{
    Ext2Inode inode;
    int ret;
    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;
    if (size) *size = inode.size;
    return EXT2_OK;
}

int ext2_unlink(Ext2Ctx *ctx, u32 dir_ino, const char *name)
{
    u32 ino;
    u8 ftype;
    Ext2Inode inode;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_find_entry(ctx, dir_ino, name, &ino, &ftype);
    if (ret != 0) return ret;
    if (ftype == EXT2_FT_DIR) return EXT2_ERR_ISDIR;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    ret = ext2_delete_entry(ctx, dir_ino, name);
    if (ret != 0) return ret;

    inode.links_count--;
    if (inode.links_count == 0) {
        ext2_free_all_blocks(ctx, &inode);
        inode.dtime = ext2_current_time();
        ext2_write_inode(ctx, ino, &inode);
        ext2_free_inode(ctx, ino);
    } else {
        inode.ctime = ext2_current_time();
        ext2_write_inode(ctx, ino, &inode);
    }

    ext2_sync(ctx);
    return EXT2_OK;
}

/* ======================================================================== */
