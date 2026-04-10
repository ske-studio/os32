#include "ext2_priv.h"

/* ======== ext2 VFSラッパー ======== */

/* パス文字列の分離 (dir_pathとfilename) */
static void ext2_split_path(const char *path, char *dir_path, const char **filename)
{
    int last_slash = -1;
    int i;

    for (i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }

    if (last_slash <= 0) {
        dir_path[0] = '/'; dir_path[1] = '\0';
        *filename = (path[0] == '/') ? path + 1 : path;
    } else {
        for (i = 0; i < last_slash && i < 255; i++) dir_path[i] = path[i];
        dir_path[i] = '\0';
        *filename = path + last_slash + 1;
    }
}

/* パス文字列からinode番号を解決 */
static int ext2_resolve_path(const char *path, u32 *out_ino)
{
    /* ルートまたは "/" */
    if (!path || !path[0] || (path[0] == '/' && !path[1])) {
        *out_ino = EXT2_ROOT_INO;
        return VFS_OK;
    }

    return ext2_lookup(path, out_ino);
}

/* ディレクトリ一覧のコールバック変換 */
typedef struct {
    vfs_dir_cb  user_cb;
    void       *user_ctx;
} Ext2ListCtx;

static void ext2_to_vfs_cb(const Ext2DirEntry *e, void *ctx)
{
    Ext2ListCtx *lc = (Ext2ListCtx *)ctx;
    VfsDirEntry ve;
    int i;

    if (!e->inode) return;

    for (i = 0; i < e->name_len && i < 255; i++) ve.name[i] = e->name[i];
    ve.name[i] = '\0';

    ve.type = (e->file_type == EXT2_FT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;

    /* ext2ディレクトリエントリにはサイズ情報がないためinodeから取得 */
    ve.size = 0;
    if (e->file_type != EXT2_FT_DIR) {
        ext2_get_size_ino(e->inode, &ve.size);
    }

    lc->user_cb(&ve, lc->user_ctx);
}

static int ext2_vfs_list(const char *path, vfs_dir_cb cb, void *ctx)
{
    u32 ino;
    int rc;
    Ext2ListCtx lc;

    rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    lc.user_cb = cb;
    lc.user_ctx = ctx;
    return ext2_list_dir(ino, ext2_to_vfs_cb, &lc);
}

static int ext2_vfs_read(const char *path, void *buf, u32 max_size)
{
    u32 ino;
    int rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_read_file(ino, buf, max_size);
}

static int ext2_vfs_write(const char *path, const void *data, u32 size)
{
    u32 dir_ino, file_ino;
    u8 ftype;
    int rc;
    /* パスからディレクトリ部分とファイル名を分離 */
    char dir_path[256];
    const char *fname;

    ext2_split_path(path, dir_path, &fname);

    rc = ext2_resolve_path(dir_path, &dir_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    /* ファイルが既存なら上書き、なければ作成 */
    rc = ext2_find_entry(dir_ino, fname, &file_ino, &ftype);
    if (rc == 0) {
        /* 既存ファイル → 上書き */
        return ext2_write(file_ino, data, size);
    } else {
        /* 新規作成 */
        return ext2_create(dir_ino, fname, data, size);
    }
}

static int ext2_vfs_unlink(const char *path)
{
    char dir_path[256];
    const char *fname;
    u32 dir_ino;
    int rc;

    ext2_split_path(path, dir_path, &fname);

    rc = ext2_resolve_path(dir_path, &dir_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_unlink(dir_ino, fname);
}

static int ext2_vfs_mkdir(const char *path)
{
    char dir_path[256];
    const char *dname;
    u32 parent_ino;
    int rc;

    ext2_split_path(path, dir_path, &dname);

    rc = ext2_resolve_path(dir_path, &parent_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_mkdir(parent_ino, dname);
}

static int ext2_vfs_rmdir(const char *path)
{
    char dir_path[256];
    const char *dname;
    u32 parent_ino;
    int rc;

    ext2_split_path(path, dir_path, &dname);

    rc = ext2_resolve_path(dir_path, &parent_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_rmdir(parent_ino, dname);
}

static int ext2_vfs_read_stream(const char *path, void *buf, u32 size, u32 offset)
{
    u32 ino;
    int rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_read_stream(ino, buf, size, offset);
}

static int ext2_vfs_write_stream(const char *path, const void *data, u32 size, u32 offset)
{
    u32 ino;
    int rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_write_stream(ino, data, size, offset);
}

static int ext2_vfs_get_size(const char *path, u32 *size)
{
    u32 ino;
    int rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_get_size_ino(ino, size);
}

static int ext2_vfs_stat(const char *path, OS32_Stat *buf)
{
    u32 ino;
    Ext2Inode inode;
    int rc;
    int i;
    u8 *p;
    
    if (!buf) return VFS_ERR_INVAL;

    rc = ext2_resolve_path(path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    rc = ext2_read_inode(ino, &inode);
    if (rc != 0) return VFS_ERR_IO;

    p = (u8 *)buf;
    for (i = 0; i < sizeof(OS32_Stat); i++) p[i] = 0;

    buf->st_dev = 0;
    buf->st_ino = ino;
    /* ext2とOS32_StatのフラグはPOSIX互換のため直接代入可能 */
    buf->st_mode = inode.mode;
    buf->st_nlink = inode.links_count;
    buf->st_uid = inode.uid;
    buf->st_gid = inode.gid;
    buf->st_size = inode.size;
    buf->st_atime = inode.atime;
    buf->st_mtime = inode.mtime;
    buf->st_ctime = inode.ctime;

    return VFS_OK;
}

static int ext2_vfs_mount(int dev_id) { return ext2_mount(dev_id); }
static void ext2_vfs_umount(void) { ext2_unmount(); }

static u32 ext2_vfs_total_blocks(void) { return ext2_get_super()->total_blocks; }
static u32 ext2_vfs_free_blocks(void) { return ext2_get_super()->free_blocks_count; }
static u32 ext2_vfs_block_size(void) { return ext2_get_super()->block_size; }

/* ext2操作テーブル */
static VfsOps ext2_ops = {
    "ext2",
    ext2_vfs_mount, ext2_vfs_umount, ext2_is_mounted,
    ext2_vfs_list, ext2_vfs_mkdir, ext2_vfs_rmdir,
    ext2_vfs_read, ext2_vfs_write, ext2_vfs_unlink,
    (void *)0, /* rename */
    ext2_vfs_get_size, ext2_vfs_read_stream, ext2_vfs_write_stream,
    ext2_sync,
    ext2_vfs_total_blocks, ext2_vfs_free_blocks, ext2_vfs_block_size,
    ext2_vfs_stat
};


/* ======== 初期化・登録 ======== */
void ext2_init(void)
{
    vfs_register_fs(&ext2_ops);
}
