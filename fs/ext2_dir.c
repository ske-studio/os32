#include "ext2_priv.h"

/*  ディレクトリ操作 — g_aux使用                                            */
/* ======================================================================== */

int ext2_list_dir(Ext2Ctx *ctx, u32 dir_ino, ext2_dir_callback cb, void *user_ctx)
{
    Ext2Inode inode;
    int ret;
    u32 bi, pos, phys;
    /* 走査中のディレクトリブロックはローカルに写す。コールバックの中で
     * 呼び出し側がファイルへ書く (例: `ls > file` — printf がリダイレクト先の
     * ext2_write_stream を呼ぶ) と、その経路の ext2_bmap / alloc_block が
     * 共有バッファ ext2_g_aux を上書きし、2 件目以降のエントリがビットマップ
     * の中身 (0xFF...) に化けていた (2026-09-03 実測)。 */
    u8 blk[EXT2_BLOCK_SIZE];

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;
    if (!(inode.mode & EXT2_S_IFDIR)) return EXT2_ERR_NOTDIR;

    for (bi = 0; ; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, blk);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&blk[pos];
            u16 de_reclen = *(u16 *)&blk[pos + 4];
            u8  de_namelen = blk[pos + 6];
            u8  de_type    = blk[pos + 7];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen > 0) {
                Ext2DirEntry entry;
                int j;
                entry.inode = de_inode;
                entry.rec_len = de_reclen;
                entry.name_len = de_namelen;
                entry.file_type = de_type;
                for (j = 0; j < (int)de_namelen && j < EXT2_NAME_LEN; j++) {
                    entry.name[j] = (char)blk[pos + 8 + j];
                }
                entry.name[j] = '\0';
                cb(&entry, user_ctx);
            }
            pos += de_reclen;
        }
    }
    return EXT2_OK;
}

int ext2_find_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 *out_ino, u8 *out_type)
{
    Ext2Inode inode;
    int ret, name_len;
    u32 bi, pos, phys;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    name_len = ext2_str_len(name);

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            u8  de_type    = ext2_g_aux[pos + 7];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen == (u8)name_len) {
                if (ext2_str_ncmp(name, (const char *)&ext2_g_aux[pos + 8], name_len) == 0) {
                    if (out_ino) *out_ino = de_inode;
                    if (out_type) *out_type = de_type;
                    return EXT2_OK;
                }
            }
            pos += de_reclen;
        }
    }
    return EXT2_ERR_NOTFOUND;
}

int ext2_add_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 ino, u8 file_type)
{
    Ext2Inode dir_inode;
    int ret, name_len;
    u32 bi, pos, phys;
    u16 new_rec_len;
    u32 now;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    name_len = ext2_str_len(name);
    new_rec_len = (u16)((8 + name_len + 3) & ~3);

    ret = ext2_read_inode(ctx, dir_ino, &dir_inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        phys = ext2_bmap(ctx, &dir_inode, bi);
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            u16 de_actual;

            if (de_reclen == 0) break;

            de_actual = (de_inode != 0) ? (u16)((8 + de_namelen + 3) & ~3) : 0;

            if (de_reclen - de_actual >= new_rec_len) {
                if (de_inode != 0) {
                    *(u16 *)&ext2_g_aux[pos + 4] = de_actual;
                    pos += de_actual;
                    *(u32 *)&ext2_g_aux[pos]     = ino;
                    *(u16 *)&ext2_g_aux[pos + 4] = de_reclen - de_actual;
                    ext2_g_aux[pos + 6] = (u8)name_len;
                    ext2_g_aux[pos + 7] = file_type;
                    ext2_mem_copy(&ext2_g_aux[pos + 8], name, (u32)name_len);
                } else {
                    *(u32 *)&ext2_g_aux[pos]     = ino;
                    ext2_g_aux[pos + 6] = (u8)name_len;
                    ext2_g_aux[pos + 7] = file_type;
                    ext2_mem_copy(&ext2_g_aux[pos + 8], name, (u32)name_len);
                }
                ret = ext2_write_block(ctx, phys, ext2_g_aux);
                if (ret != 0) return EXT2_ERR_IO;
                now = ext2_current_time();
                dir_inode.mtime = now;
                ext2_write_inode(ctx, dir_ino, &dir_inode);
                return EXT2_OK;
            }
            pos += de_reclen;
        }
    }

    /* 新ブロック割り当て */
    {
        int new_blk = ext2_alloc_block(ctx);
        if (new_blk < 0) return EXT2_ERR_NOSPC;

        ret = ext2_bmap_set(ctx, &dir_inode, bi, (u32)new_blk);
        if (ret != 0) { ext2_free_block(ctx, (u32)new_blk); return ret; }

        ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
        *(u32 *)&ext2_g_aux[0]     = ino;
        *(u16 *)&ext2_g_aux[4]     = (u16)EXT2_BLOCK_SIZE;
        ext2_g_aux[6] = (u8)name_len;
        ext2_g_aux[7] = file_type;
        ext2_mem_copy(&ext2_g_aux[8], name, (u32)name_len);

        ret = ext2_write_block(ctx, (u32)new_blk, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        now = ext2_current_time();
        dir_inode.size += EXT2_BLOCK_SIZE;
        dir_inode.blocks += 2;
        dir_inode.mtime = now;
        ext2_write_inode(ctx, dir_ino, &dir_inode);
    }
    return EXT2_OK;
}

int ext2_delete_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name)
{
    Ext2Inode dir_inode;
    int ret, name_len;
    u32 bi, pos, prev_pos, phys;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    name_len = ext2_str_len(name);

    ret = ext2_read_inode(ctx, dir_ino, &dir_inode);
    if (ret != 0) return ret;

    for (bi = 0; ; bi++) {
        phys = ext2_bmap(ctx, &dir_inode, bi);
        if (phys == 0) break;

        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return EXT2_ERR_IO;

        pos = 0; prev_pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];

            if (de_reclen == 0) break;

            if (de_inode != 0 && de_namelen == (u8)name_len) {
                if (ext2_str_ncmp(name, (const char *)&ext2_g_aux[pos + 8], name_len) == 0) {
                    if (pos == prev_pos) {
                        *(u32 *)&ext2_g_aux[pos] = 0;
                    } else {
                        u16 prev_reclen = *(u16 *)&ext2_g_aux[prev_pos + 4];
                        *(u16 *)&ext2_g_aux[prev_pos + 4] = prev_reclen + de_reclen;
                    }
                    ret = ext2_write_block(ctx, phys, ext2_g_aux);
                    if (ret != 0) return EXT2_ERR_IO;
                    dir_inode.mtime = ext2_current_time();
                    ext2_write_inode(ctx, dir_ino, &dir_inode);
                    return EXT2_OK;
                }
            }
            prev_pos = pos;
            pos += de_reclen;
        }
    }
    return EXT2_ERR_NOTFOUND;
}

/* ======================================================================== */
/*  mkdir / rmdir                                                            */
/* ======================================================================== */

int ext2_mkdir(Ext2Ctx *ctx, u32 parent_ino, const char *name)
{
    int new_ino, new_blk;
    Ext2Inode inode, parent_inode;
    u32 now, pos;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    { u32 tmp; if (ext2_find_entry(ctx, parent_ino, name, &tmp, (u8 *)0) == EXT2_OK) return EXT2_ERR_EXIST; }

    new_ino = ext2_alloc_inode(ctx);
    if (new_ino < 0) return EXT2_ERR_NOSPC;
    new_blk = ext2_alloc_block(ctx);
    if (new_blk < 0) { ext2_free_inode(ctx, (u32)new_ino); return EXT2_ERR_NOSPC; }

    now = ext2_current_time();
    ext2_mem_zero(&inode, sizeof(inode));
    inode.mode = (u16)(EXT2_S_IFDIR | 0755);
    inode.size = EXT2_BLOCK_SIZE;
    inode.atime = now; inode.ctime = now; inode.mtime = now;
    inode.links_count = 2;
    inode.blocks = 2;
    inode.block[0] = (u32)new_blk;

    /* "." と ".." */
    ext2_mem_zero(ext2_g_aux, EXT2_BLOCK_SIZE);
    pos = 0;
    *(u32 *)&ext2_g_aux[pos] = (u32)new_ino;
    *(u16 *)&ext2_g_aux[pos + 4] = 12;
    ext2_g_aux[pos + 6] = 1; ext2_g_aux[pos + 7] = EXT2_FT_DIR;
    ext2_g_aux[pos + 8] = '.';
    pos = 12;
    *(u32 *)&ext2_g_aux[pos] = parent_ino;
    *(u16 *)&ext2_g_aux[pos + 4] = (u16)(EXT2_BLOCK_SIZE - 12);
    ext2_g_aux[pos + 6] = 2; ext2_g_aux[pos + 7] = EXT2_FT_DIR;
    ext2_g_aux[pos + 8] = '.'; ext2_g_aux[pos + 9] = '.';

    ret = ext2_write_block(ctx, (u32)new_blk, ext2_g_aux);
    if (ret != 0) { ext2_free_block(ctx, (u32)new_blk); ext2_free_inode(ctx, (u32)new_ino); return EXT2_ERR_IO; }

    ext2_write_inode(ctx, (u32)new_ino, &inode);
    ret = ext2_add_entry(ctx, parent_ino, name, (u32)new_ino, EXT2_FT_DIR);
    if (ret != 0) return ret;

    ret = ext2_read_inode(ctx, parent_ino, &parent_inode);
    if (ret == 0) { parent_inode.links_count++; ext2_write_inode(ctx, parent_ino, &parent_inode); }

    {
        u32 dir_group = ((u32)new_ino - 1) / ctx->sb_info.inodes_per_group;
        if (dir_group < ctx->num_groups)
            ctx->gd_table[dir_group].used_dirs++;
    }
    ext2_sync(ctx);
    return EXT2_OK;
}

static int ext2_is_dir_empty(Ext2Ctx *ctx, u32 dir_ino)
{
    Ext2Inode inode;
    int ret;
    u32 bi, pos, phys;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return 0;

    for (bi = 0; ; bi++) {
        phys = ext2_bmap(ctx, &inode, bi);
        if (phys == 0) break;
        ret = ext2_read_block(ctx, phys, ext2_g_aux);
        if (ret != 0) return 0;

        pos = 0;
        while (pos < EXT2_BLOCK_SIZE) {
            u32 de_inode  = *(u32 *)&ext2_g_aux[pos];
            u16 de_reclen = *(u16 *)&ext2_g_aux[pos + 4];
            u8  de_namelen = ext2_g_aux[pos + 6];
            if (de_reclen == 0) break;
            if (de_inode != 0) {
                if (!(de_namelen == 1 && ext2_g_aux[pos + 8] == '.') &&
                    !(de_namelen == 2 && ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.')) {
                    return 0;
                }
            }
            pos += de_reclen;
        }
    }
    return 1;
}

int ext2_rmdir(Ext2Ctx *ctx, u32 parent_ino, const char *name)
{
    u32 ino;
    u8 ftype;
    Ext2Inode inode, parent_inode;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;

    ret = ext2_find_entry(ctx, parent_ino, name, &ino, &ftype);
    if (ret != 0) return ret;
    if (ftype != EXT2_FT_DIR) return EXT2_ERR_NOTDIR;
    if (!ext2_is_dir_empty(ctx, ino)) return EXT2_ERR_NOTEMPTY;

    ret = ext2_read_inode(ctx, ino, &inode);
    if (ret != 0) return ret;

    ret = ext2_delete_entry(ctx, parent_ino, name);
    if (ret != 0) return ret;

    ext2_free_all_blocks(ctx, &inode);
    inode.links_count = 0;
    inode.dtime = ext2_current_time();
    ext2_write_inode(ctx, ino, &inode);
    ext2_free_inode(ctx, ino);

    ret = ext2_read_inode(ctx, parent_ino, &parent_inode);
    if (ret == 0) {
        if (parent_inode.links_count > 0) parent_inode.links_count--;
        parent_inode.mtime = ext2_current_time();
        ext2_write_inode(ctx, parent_ino, &parent_inode);
    }

    {
        u32 dir_group = (ino - 1) / ctx->sb_info.inodes_per_group;
        if (dir_group < ctx->num_groups)
            ctx->gd_table[dir_group].used_dirs--;
    }
    ext2_sync(ctx);
    return EXT2_OK;
}

/* ======================================================================== */
/*  rename                                                                  */
/* ======================================================================== */

/* ディレクトリ dir_ino の ".." が指す inode を返す (失敗時 0) */
static u32 ext2_parent_of(Ext2Ctx *ctx, u32 dir_ino)
{
    Ext2Inode inode;
    u32 phys, pos;

    if (ext2_read_inode(ctx, dir_ino, &inode) != 0) return 0;
    phys = ext2_bmap(ctx, &inode, 0);
    if (phys == 0) return 0;
    if (ext2_read_block(ctx, phys, ext2_g_aux) != 0) return 0;

    pos = 0;
    while (pos < EXT2_BLOCK_SIZE) {
        u32 de_inode   = *(u32 *)&ext2_g_aux[pos];
        u16 de_reclen  = *(u16 *)&ext2_g_aux[pos + 4];
        u8  de_namelen = ext2_g_aux[pos + 6];
        if (de_reclen == 0) break;
        if (de_inode != 0 && de_namelen == 2 &&
            ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.') {
            return de_inode;
        }
        pos += de_reclen;
    }
    return 0;
}

/* ディレクトリ dir_ino の ".." を new_parent に書き換える */
static int ext2_set_dotdot(Ext2Ctx *ctx, u32 dir_ino, u32 new_parent)
{
    Ext2Inode inode;
    u32 phys, pos;
    int ret;

    ret = ext2_read_inode(ctx, dir_ino, &inode);
    if (ret != 0) return ret;
    phys = ext2_bmap(ctx, &inode, 0);
    if (phys == 0) return EXT2_ERR_IO;
    ret = ext2_read_block(ctx, phys, ext2_g_aux);
    if (ret != 0) return EXT2_ERR_IO;

    pos = 0;
    while (pos < EXT2_BLOCK_SIZE) {
        u32 de_inode   = *(u32 *)&ext2_g_aux[pos];
        u16 de_reclen  = *(u16 *)&ext2_g_aux[pos + 4];
        u8  de_namelen = ext2_g_aux[pos + 6];
        if (de_reclen == 0) break;
        if (de_inode != 0 && de_namelen == 2 &&
            ext2_g_aux[pos + 8] == '.' && ext2_g_aux[pos + 9] == '.') {
            *(u32 *)&ext2_g_aux[pos] = new_parent;
            ret = ext2_write_block(ctx, phys, ext2_g_aux);
            return (ret != 0) ? EXT2_ERR_IO : EXT2_OK;
        }
        pos += de_reclen;
    }
    return EXT2_ERR_NOTFOUND;
}

/* new_dir が ino 自身、または ino の子孫か (ディレクトリを自分の中へ移す
 * 循環を防ぐ)。".." を root まで辿る。 */
static int ext2_is_self_or_descendant(Ext2Ctx *ctx, u32 ino, u32 new_dir)
{
    u32 cur = new_dir;
    int hops = 0;
    while (cur != 0 && hops < EXT2_RENAME_MAX_DEPTH) {
        if (cur == ino) return 1;
        if (cur == EXT2_ROOT_INO) return 0;
        {
            u32 parent = ext2_parent_of(ctx, cur);
            if (parent == 0 || parent == cur) return 0;
            cur = parent;
        }
        hops++;
    }
    return 0;
}

int ext2_rename(Ext2Ctx *ctx, u32 old_dir, const char *old_name,
                u32 new_dir, const char *new_name)
{
    u32 ino, dst_ino;
    u8 ftype, dst_type;
    Ext2Inode inode;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    if (!old_name[0] || !new_name[0]) return EXT2_ERR_INVAL;

    ret = ext2_find_entry(ctx, old_dir, old_name, &ino, &ftype);
    if (ret != 0) return ret;

    /* 同一エントリへの rename は no-op */
    if (old_dir == new_dir && ext2_str_ncmp(old_name, new_name,
            ext2_str_len(old_name) + 1) == 0) {
        return EXT2_OK;
    }

    /* 移動先に同名がある場合: ディレクトリは上書きしない。
     * ファイル同士なら POSIX と同じく置き換える */
    if (ext2_find_entry(ctx, new_dir, new_name, &dst_ino, &dst_type) == EXT2_OK) {
        if (dst_ino == ino) return EXT2_OK;   /* ハードリンク同士 */
        if (dst_type == EXT2_FT_DIR) return EXT2_ERR_EXIST;
        if (ftype == EXT2_FT_DIR) return EXT2_ERR_NOTDIR;
        ret = ext2_unlink(ctx, new_dir, new_name);
        if (ret != 0) return ret;
    }

    if (ftype == EXT2_FT_DIR && old_dir != new_dir) {
        if (ext2_is_self_or_descendant(ctx, ino, new_dir)) return EXT2_ERR_INVAL;
    }

    ret = ext2_add_entry(ctx, new_dir, new_name, ino, ftype);
    if (ret != 0) return ret;

    ret = ext2_delete_entry(ctx, old_dir, old_name);
    if (ret != 0) {
        /* 巻き戻し: 追加した方を消す (失敗しても元の名前は残っている) */
        ext2_delete_entry(ctx, new_dir, new_name);
        return ret;
    }

    if (ftype == EXT2_FT_DIR && old_dir != new_dir) {
        Ext2Inode pinode;
        ret = ext2_set_dotdot(ctx, ino, new_dir);
        if (ret != 0) return ret;
        if (ext2_read_inode(ctx, old_dir, &pinode) == 0) {
            if (pinode.links_count > 0) pinode.links_count--;
            ext2_write_inode(ctx, old_dir, &pinode);
        }
        if (ext2_read_inode(ctx, new_dir, &pinode) == 0) {
            pinode.links_count++;
            ext2_write_inode(ctx, new_dir, &pinode);
        }
    }

    if (ext2_read_inode(ctx, ino, &inode) == 0) {
        inode.ctime = ext2_current_time();
        ext2_write_inode(ctx, ino, &inode);
    }

    ext2_sync(ctx);
    return EXT2_OK;
}

/* ======================================================================== */
/*  パス検索                                                                */
/* ======================================================================== */

int ext2_lookup(Ext2Ctx *ctx, const char *path, u32 *out_ino)
{
    u32 current_ino = EXT2_ROOT_INO;
    char component[EXT2_NAME_LEN + 1];
    int i, ci;
    u32 found_ino;
    u8 found_type;
    int ret;

    if (!ctx->mounted) return EXT2_ERR_NOMOUNT;
    if (!path || path[0] == '\0') { *out_ino = EXT2_ROOT_INO; return EXT2_OK; }

    i = 0;
    if (path[0] == '/') i++;

    while (path[i] != '\0') {
        ci = 0;
        while (path[i] != '\0' && path[i] != '/' && ci < EXT2_NAME_LEN) {
            component[ci++] = path[i++];
        }
        component[ci] = '\0';
        if (ci == 0) { if (path[i] == '/') { i++; continue; } break; }

        ret = ext2_find_entry(ctx, current_ino, component, &found_ino, &found_type);
        if (ret != 0) return EXT2_ERR_NOTFOUND;
        current_ino = found_ino;
        if (path[i] == '/') i++;
    }

    *out_ino = current_ino;
    return EXT2_OK;
}

/* ======================================================================== */
