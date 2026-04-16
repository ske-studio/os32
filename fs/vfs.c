/* ======================================================================== */
/*  VFS.C — 仮想ファイルシステム ディスパッチャ                                */
/*                                                                          */
/*  各FSドライバ(ext2, fat12)への呼び出しを仲介し、                          */
/*  パス文字列ベースの統一APIを提供する。                                    */
/* ======================================================================== */

#include "vfs.h"
#include "fat12.h"
#include "os32_kapi_shared.h" /* O_RDONLY, SEEK_SET 等 */
#include "kstring.h"
#include "console.h"
#include "kbd.h"

#ifndef ATTR_WHITE
#define ATTR_WHITE   TATTR_WHITE
#endif

/* ======== 内部ユーティリティ ======== */
/* 文字列関数は kstring.h (kstrcmp, kstrlen, kstrncpy) に統一 */

/* ======== カレントディレクトリ ======== */

static char cwd[VFS_MAX_PATH] = "/";

const char *vfs_cwd(void) { return cwd; }

int vfs_chdir(const char *path)
{
    char resolved[VFS_MAX_PATH];
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    kstrncpy(cwd, resolved, VFS_MAX_PATH);
    /* 末尾に / を保証 */
    {
        int len = (int)kstrlen(cwd);
        if (len > 0 && cwd[len - 1] != '/') {
            if (len < VFS_MAX_PATH - 2) { cwd[len] = '/'; cwd[len + 1] = '\0'; }
        }
    }
    return VFS_OK;
}

/* パスの正規化: 相対パス→絶対パス */
void vfs_resolve_path(const char *input, char *output, int out_size)
{
    char tmp[VFS_MAX_PATH];
    const char *parts[VFS_MAX_PATH_DEPTH];
    int num_parts = 0;
    int i, p, o;

    if (!input || !input[0]) {
        kstrncpy(output, cwd, (u32)out_size);
        return;
    }

    if (input[0] == '/') {
        kstrncpy(tmp, input, VFS_MAX_PATH);
    } else {
        o = 0;
        for (i = 0; cwd[i] && o < VFS_MAX_PATH - 2; i++) tmp[o++] = cwd[i];
        if (o > 0 && tmp[o - 1] != '/') tmp[o++] = '/';
        for (i = 0; input[i] && o < VFS_MAX_PATH - 1; i++) tmp[o++] = input[i];
        tmp[o] = '\0';
    }

    p = 0;
    while (tmp[p] != '\0') {
        if (tmp[p] == '/') { p++; continue; }
        
        int start = p;
        while (tmp[p] != '/' && tmp[p] != '\0') p++;
        
        int len = p - start;
        char c = tmp[p];
        tmp[p] = '\0';
        
        if (len == 1 && tmp[start] == '.') {
            /* nop */
        } else if (len == 2 && tmp[start] == '.' && tmp[start+1] == '.') {
            if (num_parts > 0) num_parts--;
        } else {
            if (num_parts < VFS_MAX_PATH_DEPTH) parts[num_parts++] = &tmp[start];
        }
        
        if (c == '\0') break;
        p++;
    }

    output[0] = '/';
    o = 1;
    for (i = 0; i < num_parts; i++) {
        int j = 0;
        while (parts[i][j] && o < out_size - 1) {
            output[o++] = parts[i][j++];
        }
        if (i < num_parts - 1 && o < out_size - 1) {
            output[o++] = '/';
        }
    }
    output[o] = '\0';
}

/* ======== FSプラグイン登録 ======== */

static VfsOps *fs_registry[VFS_MAX_FS];
static int num_fs = 0;

void vfs_register_fs(VfsOps *ops)
{
    if (num_fs < VFS_MAX_FS) {
        fs_registry[num_fs++] = ops;
    }
}

/* ======== VFSグローバル状態 ======== */

#define VFS_DEV_HD     0
#define VFS_DEV_FD     1
#define VFS_DEV_SERIAL 2

typedef struct {
    int in_use;
    char prefix[VFS_MAX_PATH];
    VfsOps *ops;
    int dev_type;
    int dev_id;
    char dev_name[VFS_MAX_DEVNAME];
} MountPoint;

static MountPoint mounts[VFS_MAX_FS];

/* ======== マウントの検索とパスの分離 ======== */

static MountPoint *vfs_find_mount(const char *path, const char **out_relpath)
{
    int i, match_len;
    int best_idx = -1;
    int best_len = -1;

    for (i = 0; i < VFS_MAX_FS; i++) {
        if (!mounts[i].in_use) continue;
        
        match_len = (int)kstrlen(mounts[i].prefix);
        if (match_len == 1 && mounts[i].prefix[0] == '/') {
            if (best_len < 1) { best_len = 1; best_idx = i; }
        } else {
            int j = 0;
            while (j < match_len && path[j] == mounts[i].prefix[j]) j++;
            if (j == match_len && (path[j] == '/' || path[j] == '\0')) {
                if (match_len > best_len) { best_len = match_len; best_idx = i; }
            }
        }
    }

    if (best_idx >= 0) {
        if (out_relpath) {
            *out_relpath = path + best_len;
            if (**out_relpath == '/') (*out_relpath)++; 
        }
        return &mounts[best_idx];
    }
    return (MountPoint *)0;
}

VfsOps *vfs_route(const char *path, char *rel_out, int max_rel)
{
    const char *rel_ptr;
    MountPoint *mnt = vfs_find_mount(path, &rel_ptr);
    if (!mnt) return (VfsOps *)0;

    if (*rel_ptr == '\0') {
        kstrncpy(rel_out, "/", (u32)max_rel);
    } else {
        if (*rel_ptr != '/') {
            rel_out[0] = '/';
            kstrncpy(rel_out + 1, rel_ptr, (u32)(max_rel - 1));
        } else {
            kstrncpy(rel_out, rel_ptr, (u32)max_rel);
        }
    }
    return mnt->ops;
}

/* ======== デバイス名パース ======== */

int vfs_dev_parse(const char *name, int *dev_type, int *dev_id)
{
    /* "hd0", "hd1", "fd0", "fd1" */
    if (name[0] == 'h' && name[1] == 'd' && name[2] >= '0' && name[2] <= '3') {
        *dev_type = VFS_DEV_HD;
        *dev_id = name[2] - '0';
        return VFS_OK;
    }
    if (name[0] == 'f' && name[1] == 'd' && name[2] >= '0' && name[2] <= '3') {
        *dev_type = VFS_DEV_FD;
        *dev_id = name[2] - '0';
        return VFS_OK;
    }
    /* "COM1" etc */
    if (name[0] == 'C' && name[1] == 'O' && name[2] == 'M') {
        *dev_type = VFS_DEV_SERIAL;
        *dev_id = 1;
        return VFS_OK;
    }
    return VFS_ERR_INVAL;
}

/* ======== VFS 公開API ======== */

int vfs_mount(const char *prefix, const char *dev_name, const char *fstype)
{
    int dev_type, dev_id, rc, i, slot;
    VfsOps *ops = (VfsOps *)0;

    rc = vfs_dev_parse(dev_name, &dev_type, &dev_id);
    if (rc != VFS_OK) return VFS_ERR_INVAL;

    for (i = 0; i < num_fs; i++) {
        if (kstrcmp(fstype, fs_registry[i]->name) == 0) {
            ops = fs_registry[i];
            break;
        }
    }
    if (!ops) return VFS_ERR_INVAL;

    rc = ops->mount(dev_id);
    if (rc != 0) return rc;

    slot = -1;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (!mounts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        ops->umount();
        return VFS_ERR_NOSPC;
    }

    mounts[slot].in_use = 1;
    kstrncpy(mounts[slot].prefix, prefix, VFS_MAX_PATH);
    mounts[slot].ops = ops;
    mounts[slot].dev_type = dev_type;
    mounts[slot].dev_id = dev_id;
    kstrncpy(mounts[slot].dev_name, dev_name, VFS_MAX_DEVNAME);

    kstrncpy(cwd, "/", VFS_MAX_PATH);
    return VFS_OK;
}

void vfs_umount(const char *prefix)
{
    int i;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && kstrcmp(mounts[i].prefix, prefix) == 0) {
            mounts[i].ops->umount();
            mounts[i].in_use = 0;
            break;
        }
    }
}

int vfs_is_mounted(const char *prefix)
{
    int i;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && kstrcmp(mounts[i].prefix, prefix) == 0) return mounts[i].ops->is_mounted();
    }
    return 0;
}

const char *vfs_fstype(const char *prefix)
{
    int i;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && kstrcmp(mounts[i].prefix, prefix) == 0) return mounts[i].ops->name;
    }
    return "";
}

const char *vfs_devname(const char *prefix)
{
    int i;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && kstrcmp(mounts[i].prefix, prefix) == 0) return mounts[i].dev_name;
    }
    return "";
}

int vfs_ls(const char *path, vfs_dir_cb cb, void *ctx)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->list_dir) return VFS_ERR_NOMOUNT;
    return ops->list_dir(rel_path, cb, ctx);
}

int vfs_read(const char *path, void *buf, u32 max_size)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->read_file) return VFS_ERR_NOMOUNT;
    return ops->read_file(rel_path, buf, max_size);
}

int vfs_write(const char *path, const void *data, u32 size)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->write_file) return VFS_ERR_NOMOUNT;
    return ops->write_file(rel_path, data, size);
}

int vfs_rm(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->unlink) return VFS_ERR_NOMOUNT;
    return ops->unlink(rel_path);
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    char old_abs[VFS_MAX_PATH], old_rel[VFS_MAX_PATH];
    char new_abs[VFS_MAX_PATH], new_rel[VFS_MAX_PATH];
    VfsOps *old_ops, *new_ops;

    vfs_resolve_path(oldpath, old_abs, VFS_MAX_PATH);
    vfs_resolve_path(newpath, new_abs, VFS_MAX_PATH);

    old_ops = vfs_route(old_abs, old_rel, VFS_MAX_PATH);
    new_ops = vfs_route(new_abs, new_rel, VFS_MAX_PATH);

    if (!old_ops || !new_ops) return VFS_ERR_NOMOUNT;

    /* アプローチ2: 異なるファイルシステム(例: /host から /hd0)などへの移動はVFS層では許容しない
     * (上層のシェル側で cp + rm へフォールバックさせるため INVAL を返す) */
    if (old_ops != new_ops) return VFS_ERR_INVAL;

    if (!old_ops->rename) return VFS_ERR_INVAL;
    return old_ops->rename(old_rel, new_rel);
}

int vfs_mkdir(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->mkdir) return VFS_ERR_NOMOUNT;
    return ops->mkdir(rel_path);
}

int vfs_rmdir(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    if (!ops || !ops->rmdir) return VFS_ERR_NOMOUNT;
    return ops->rmdir(rel_path);
}

int vfs_sync(void)
{
    int i, rc = VFS_OK, cur_rc;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && mounts[i].ops->sync) {
            cur_rc = mounts[i].ops->sync();
            if (cur_rc != VFS_OK) rc = cur_rc;
        }
    }
    return rc;
}

u32 vfs_total_blocks(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->total_blocks) ? mnt->ops->total_blocks() : 0;
}
u32 vfs_free_blocks(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->free_blocks) ? mnt->ops->free_blocks() : 0;
}
u32 vfs_block_size(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->block_size) ? mnt->ops->block_size() : 0;
}

int vfs_stat(const char *path, OS32_Stat *buf)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    VfsOps *ops;
    
    if (!buf) return VFS_ERR_INVAL;

    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH);
    
    if (!ops || !ops->stat) return VFS_ERR_NOMOUNT;
    return ops->stat(rel_path, buf);
}

/* end of vfs.c */
