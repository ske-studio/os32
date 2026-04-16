#include "ext2_priv.h"
#include "kstring.h"
#include "kmalloc.h"

/* ======== ext2 VFSラッパー ======== */
/* マルチインスタンス対応: void *ctx を Ext2Ctx* にキャストして全関数に渡す。 */
/* mount() で kmalloc 確保、umount() で kfree 解放。                        */

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
        for (i = 0; i < last_slash && i < VFS_MAX_PATH - 1; i++) dir_path[i] = path[i];
        dir_path[i] = '\0';
        *filename = path + last_slash + 1;
    }
}

/* パス文字列からinode番号を解決 */
static int ext2_resolve_path(Ext2Ctx *ec, const char *path, u32 *out_ino)
{
    /* ルートまたは "/" */
    if (!path || !path[0] || (path[0] == '/' && !path[1])) {
        *out_ino = EXT2_ROOT_INO;
        return VFS_OK;
    }

    return ext2_lookup(ec, path, out_ino);
}

/* ディレクトリ一覧のコールバック変換 */
typedef struct {
    vfs_dir_cb  user_cb;
    void       *user_ctx;
    Ext2Ctx    *ec;
} Ext2ListCtx;

static void ext2_to_vfs_cb(const Ext2DirEntry *e, void *ctx)
{
    Ext2ListCtx *lc = (Ext2ListCtx *)ctx;
    VfsDirEntry ve;
    int i;

    if (!e->inode) return;

    for (i = 0; i < e->name_len && i < VFS_MAX_PATH - 1; i++) ve.name[i] = e->name[i];
    ve.name[i] = '\0';

    ve.type = (e->file_type == EXT2_FT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;

    /* ext2ディレクトリエントリにはサイズ情報がないためinodeから取得 */
    ve.size = 0;
    if (e->file_type != EXT2_FT_DIR) {
        ext2_get_size_ino(lc->ec, e->inode, &ve.size);
    }

    lc->user_cb(&ve, lc->user_ctx);
}

static int ext2_vfs_list(void *ctx, const char *path, vfs_dir_cb cb, void *user_ctx)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    int rc;
    Ext2ListCtx lc;

    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    lc.user_cb = cb;
    lc.user_ctx = user_ctx;
    lc.ec = ec;
    return ext2_list_dir(ec, ino, ext2_to_vfs_cb, &lc);
}

static int ext2_vfs_read(void *ctx, const char *path, void *buf, u32 max_size)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    int rc;
    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_read_file(ec, ino, buf, max_size);
}

static int ext2_vfs_write(void *ctx, const char *path, const void *data, u32 size)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 dir_ino, file_ino;
    u8 ftype;
    int rc;
    /* パスからディレクトリ部分とファイル名を分離 */
    char dir_path[VFS_MAX_PATH];
    const char *fname;

    ext2_split_path(path, dir_path, &fname);

    rc = ext2_resolve_path(ec, dir_path, &dir_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    /* ファイルが既存なら上書き、なければ作成 */
    rc = ext2_find_entry(ec, dir_ino, fname, &file_ino, &ftype);
    if (rc == 0) {
        /* 既存ファイル → 上書き */
        return ext2_write(ec, file_ino, data, size);
    } else {
        /* 新規作成 */
        return ext2_create(ec, dir_ino, fname, data, size);
    }
}

static int ext2_vfs_unlink(void *ctx, const char *path)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    char dir_path[VFS_MAX_PATH];
    const char *fname;
    u32 dir_ino;
    int rc;

    ext2_split_path(path, dir_path, &fname);

    rc = ext2_resolve_path(ec, dir_path, &dir_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_unlink(ec, dir_ino, fname);
}

static int ext2_vfs_mkdir(void *ctx, const char *path)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    char dir_path[VFS_MAX_PATH];
    const char *dname;
    u32 parent_ino;
    int rc;

    ext2_split_path(path, dir_path, &dname);

    rc = ext2_resolve_path(ec, dir_path, &parent_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_mkdir(ec, parent_ino, dname);
}

static int ext2_vfs_rmdir(void *ctx, const char *path)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    char dir_path[VFS_MAX_PATH];
    const char *dname;
    u32 parent_ino;
    int rc;

    ext2_split_path(path, dir_path, &dname);

    rc = ext2_resolve_path(ec, dir_path, &parent_ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_rmdir(ec, parent_ino, dname);
}

static int ext2_vfs_read_stream(void *ctx, const char *path, void *buf, u32 size, u32 offset)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    int rc;
    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_read_stream(ec, ino, buf, size, offset);
}

static int ext2_vfs_write_stream(void *ctx, const char *path, const void *data, u32 size, u32 offset)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    int rc;
    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_write_stream(ec, ino, data, size, offset);
}

static int ext2_vfs_get_size(void *ctx, const char *path, u32 *size)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    int rc;
    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;
    return ext2_get_size_ino(ec, ino, size);
}

static int ext2_vfs_stat(void *ctx, const char *path, OS32_Stat *buf)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    u32 ino;
    Ext2Inode inode;
    int rc;
    
    if (!buf) return VFS_ERR_INVAL;

    rc = ext2_resolve_path(ec, path, &ino);
    if (rc != 0) return VFS_ERR_NOTFOUND;

    rc = ext2_read_inode(ec, ino, &inode);
    if (rc != 0) return VFS_ERR_IO;

    kmemset(buf, 0, sizeof(OS32_Stat));

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

/* ---- マウント/アンマウント (kmalloc/kfree) ---- */

static void *ext2_vfs_mount(int dev_id)
{
    Ext2Ctx *ec = (Ext2Ctx *)kmalloc(sizeof(Ext2Ctx));
    if (!ec) return (void *)0;
    kmemset(ec, 0, sizeof(Ext2Ctx));
    if (ext2_mount(ec, dev_id) != EXT2_OK) {
        kfree(ec);
        return (void *)0;
    }
    return (void *)ec;
}

static void ext2_vfs_umount(void *ctx)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    if (ec) {
        ext2_unmount(ec);
        kfree(ec);
    }
}

static int ext2_vfs_is_mounted(void *ctx)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    return ec ? ext2_is_mounted_ctx(ec) : 0;
}

static int ext2_vfs_sync(void *ctx)
{
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    return ec ? ext2_sync(ec) : EXT2_ERR_NOMOUNT;
}

static u32 ext2_vfs_total_blocks(void *ctx) {
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    return ec ? ec->sb_info.total_blocks : 0;
}
static u32 ext2_vfs_free_blocks(void *ctx) {
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    return ec ? ec->sb_info.free_blocks_count : 0;
}
static u32 ext2_vfs_block_size(void *ctx) {
    Ext2Ctx *ec = (Ext2Ctx *)ctx;
    return ec ? ec->sb_info.block_size : 0;
}

/* ext2操作テーブル */
static VfsOps ext2_ops = {
    "ext2",
    ext2_vfs_mount, ext2_vfs_umount, ext2_vfs_is_mounted,
    ext2_vfs_list, ext2_vfs_mkdir, ext2_vfs_rmdir,
    ext2_vfs_read, ext2_vfs_write, ext2_vfs_unlink,
    (void *)0, /* rename */
    ext2_vfs_get_size, ext2_vfs_read_stream, ext2_vfs_write_stream,
    ext2_vfs_sync,
    ext2_vfs_total_blocks, ext2_vfs_free_blocks, ext2_vfs_block_size,
    ext2_vfs_stat
};


/* ======== 初期化・登録 ======== */
void ext2_init(void)
{
    vfs_register_fs(&ext2_ops);
}
