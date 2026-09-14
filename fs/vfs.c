/* ======================================================================== */
/*  VFS.C — 仮想ファイルシステム ディスパッチャ                                */
/*                                                                          */
/*  各FSドライバ(ext2, fat等)への呼び出しを仲介し、                           */
/*  パス文字列ベースの統一APIを提供する。                                    */
/*                                                                          */
/*  マルチインスタンス対応: MountPointがfs_ctxを保持し、                     */
/*  各ディスパッチ関数がFSドライバにコンテキストを渡す。                     */
/* ======================================================================== */

#include "vfs.h"
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
    int kind;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);

    /* 実在するディレクトリだけ受け付ける。以前は検証なしに cwd を書き換えて
     * いたので `cd /nonexistent` が成功して見え、以後の相対パスが全部壊れた */
    kind = vfs_path_kind(resolved);
    if (kind < 0) return kind;
    if (kind != VFS_KIND_DIR) return VFS_ERR_NOTDIR;

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

    /* out_size<=0 の u32 キャストは巨大長に化ける */
    if (!output || out_size <= 0) return;

    if (!input || !input[0]) {
        kstrncpy(output, cwd, (u32)out_size);
        return;
    }

    if (input[0] == '/') {
        kstrncpy(tmp, input, VFS_MAX_PATH);
    } else {
        /* cwd + "/" + input を安全に結合 */
        kstrncpy(tmp, cwd, VFS_MAX_PATH);
        {
            int len = (int)kstrlen(tmp);
            if (len > 0 && tmp[len - 1] != '/') {
                kstrncat(tmp, "/", VFS_MAX_PATH);
            }
        }
        kstrncat(tmp, input, VFS_MAX_PATH);
    }

    p = 0;
    while (tmp[p] != '\0') {
        int start, len;
        char c;

        if (tmp[p] == '/') { p++; continue; }

        start = p;
        while (tmp[p] != '/' && tmp[p] != '\0') p++;

        len = p - start;
        c = tmp[p];
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

/* VFS_DEV_* と VFS_MOUNT_DEV_* は fs/vfs.h が正典 (FS ドライバも参照する)。 */

typedef struct {
    int in_use;
    char prefix[VFS_MAX_PATH];
    VfsOps *ops;
    void *fs_ctx;           /* FSドライバ固有のインスタンスコンテキスト */
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

VfsOps *vfs_route(const char *path, char *rel_out, int max_rel, void **ctx_out)
{
    const char *rel_ptr;
    MountPoint *mnt;

    if (!rel_out || max_rel <= 0) return (VfsOps *)0;

    mnt = vfs_find_mount(path, &rel_ptr);
    if (!mnt) {
        if (ctx_out) *ctx_out = (void *)0;
        return (VfsOps *)0;
    }

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
    if (ctx_out) *ctx_out = mnt->fs_ctx;
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
    /* "cd0" - "cd3" */
    if (name[0] == 'c' && name[1] == 'd' && name[2] >= '0' && name[2] <= '3') {
        *dev_type = VFS_DEV_CD;
        *dev_id = name[2] - '0';
        return VFS_OK;
    }
    /* "hostdrv" — 仮想デバイス (HostDrvFS) */
    if (name[0] == 'h' && name[1] == 'o' && name[2] == 's' && name[3] == 't' &&
        name[4] == 'd' && name[5] == 'r' && name[6] == 'v' && name[7] == '\0') {
        *dev_type = VFS_DEV_HOSTDRV;
        *dev_id = 0;
        return VFS_OK;
    }
    return VFS_ERR_INVAL;
}

/* ======== VFS 公開API ======== */

int vfs_mount(const char *prefix, const char *dev_name, const char *fstype)
{
    int dev_type, dev_id, rc, i, slot;
    VfsOps *ops = (VfsOps *)0;
    void *fs_ctx;

    rc = vfs_dev_parse(dev_name, &dev_type, &dev_id);
    if (rc != VFS_OK) return VFS_ERR_INVAL;

    for (i = 0; i < num_fs; i++) {
        if (kstrcmp(fstype, fs_registry[i]->name) == 0) {
            ops = fs_registry[i];
            break;
        }
    }
    if (!ops) return VFS_ERR_INVAL;

    /* 同じ実デバイスを二重マウントさせない。ext2 は 1 デバイスにつき
     * 1 コンテキストしか整合しない — 2 つ目のコンテキストはマウント時の
     * スーパーブロックを抱えたまま vfs_sync() で書き戻し、1 つ目が書いた
     * 正しい空き数を巻き戻す (2026-09-10)。 */
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && mounts[i].ops == ops &&
            mounts[i].dev_type == dev_type && mounts[i].dev_id == dev_id)
            return VFS_ERR_EXIST;
    }

    /* dev_type を dev_id の上位バイトにエンコードして渡す。FS ドライバは
     * VFS_MOUNT_DEV_TYPE() で自分が扱える種別かを必ず確かめること。 */
    fs_ctx = ops->mount(VFS_MOUNT_DEV_ENCODE(dev_type, dev_id));
    if (!fs_ctx) return VFS_ERR_IO;

    slot = -1;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (!mounts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        ops->umount(fs_ctx);
        return VFS_ERR_NOSPC;
    }

    mounts[slot].in_use = 1;
    kstrncpy(mounts[slot].prefix, prefix, VFS_MAX_PATH);
    mounts[slot].ops = ops;
    mounts[slot].fs_ctx = fs_ctx;
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
            mounts[i].ops->umount(mounts[i].fs_ctx);
            mounts[i].in_use = 0;
            mounts[i].fs_ctx = (void *)0;
            break;
        }
    }
}

int vfs_is_mounted(const char *prefix)
{
    int i;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && kstrcmp(mounts[i].prefix, prefix) == 0)
            return mounts[i].ops->is_mounted(mounts[i].fs_ctx);
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
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->list_dir) return VFS_ERR_NOMOUNT;
    return ops->list_dir(fs_ctx, rel_path, cb, ctx);
}

int vfs_read(const char *path, void *buf, u32 max_size)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->read_file) return VFS_ERR_NOMOUNT;
    return ops->read_file(fs_ctx, rel_path, buf, max_size);
}

int vfs_write(const char *path, const void *data, u32 size)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->write_file) return VFS_ERR_NOMOUNT;
    return ops->write_file(fs_ctx, rel_path, data, size);
}

int vfs_rm(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->unlink) return VFS_ERR_NOMOUNT;
    return ops->unlink(fs_ctx, rel_path);
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    char old_abs[VFS_MAX_PATH], old_rel[VFS_MAX_PATH];
    char new_abs[VFS_MAX_PATH], new_rel[VFS_MAX_PATH];
    void *old_ctx, *new_ctx;
    VfsOps *old_ops, *new_ops;

    vfs_resolve_path(oldpath, old_abs, VFS_MAX_PATH);
    vfs_resolve_path(newpath, new_abs, VFS_MAX_PATH);

    old_ops = vfs_route(old_abs, old_rel, VFS_MAX_PATH, &old_ctx);
    new_ops = vfs_route(new_abs, new_rel, VFS_MAX_PATH, &new_ctx);

    if (!old_ops || !new_ops) return VFS_ERR_NOMOUNT;

    /* 異なるファイルシステム間の移動は許容しない */
    if (old_ops != new_ops || old_ctx != new_ctx) return VFS_ERR_INVAL;

    if (!old_ops->rename) return VFS_ERR_INVAL;
    return old_ops->rename(old_ctx, old_rel, new_rel);
}

int vfs_mkdir(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->mkdir) return VFS_ERR_NOMOUNT;
    return ops->mkdir(fs_ctx, rel_path);
}

int vfs_rmdir(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops || !ops->rmdir) return VFS_ERR_NOMOUNT;
    return ops->rmdir(fs_ctx, rel_path);
}

int vfs_sync(void)
{
    int i, rc = VFS_OK, cur_rc;
    for (i = 0; i < VFS_MAX_FS; i++) {
        if (mounts[i].in_use && mounts[i].ops->sync) {
            cur_rc = mounts[i].ops->sync(mounts[i].fs_ctx);
            if (cur_rc != VFS_OK) rc = cur_rc;
        }
    }
    return rc;
}

u32 vfs_total_blocks(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->total_blocks) ? mnt->ops->total_blocks(mnt->fs_ctx) : 0;
}
u32 vfs_free_blocks(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->free_blocks) ? mnt->ops->free_blocks(mnt->fs_ctx) : 0;
}
u32 vfs_block_size(void) {
    MountPoint *mnt = vfs_find_mount("/", (const char **)0);
    return (mnt && mnt->ops->block_size) ? mnt->ops->block_size(mnt->fs_ctx) : 0;
}

/* stat の st_dev をマウント単位で決める。FS ドライバは st_dev を埋めない
 * (ext2/iso9660 は 0 固定、hostdrv/fatfs は kmemset のまま) ので、VFS が
 * 正典として上書きする。値は VFS_MOUNT_DEV_ENCODE() + 1 — +1 は「不明」を
 * 表す 0 と衝突させないため。vfs_mount() が断る二重マウントの鍵は
 * (FS, 種別, unit) で、この値は (種別, unit) だけから作るので、**同じ unit を
 * 別の FS ドライバで**マウントした 2 つ (例: 同じ hd の ext2 と FAT) は同じ
 * st_dev になる。一意なのは「同じ FS ドライバのマウント同士」の間。filer の
 * 同一判定は st_ino != 0 のときだけ st_dev/st_ino を見るので、st_ino を
 * 埋めない FAT/hostdrv とは衝突しない (独立レビュー 2026-09-10 の訂正)。
 *
 * 限界: 同じ物理ディスク上の別パーティションは区別しない。ext2 は
 * ext2_find_partition() が最初のブート可能エントリしか選ばず、同じ
 * hd0 を 2 度マウントすることもできないため、そもそも同時に見えない。 */
static u32 vfs_mount_dev_of(const MountPoint *mnt)
{
    return (u32)VFS_MOUNT_DEV_ENCODE(mnt->dev_type, mnt->dev_id) + 1U;
}

static u32 vfs_dev_of_resolved(const char *resolved)
{
    MountPoint *mnt = vfs_find_mount(resolved, (const char **)0);
    return mnt ? vfs_mount_dev_of(mnt) : 0U;
}

u32 vfs_path_dev(const char *path)
{
    char resolved[VFS_MAX_PATH];

    if (!path) return 0U;
    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    return vfs_dev_of_resolved(resolved);
}

/* マウントルート ("/") の stat 合成。FatFs の f_stat はルートに
 * FR_INVALID_NAME を返すなど、FS ドライバはルートを stat できないことがある */
static void vfs_synth_root_stat(OS32_Stat *buf)
{
    kmemset(buf, 0, sizeof(OS32_Stat));
    buf->st_mode  = OS_S_IFDIR | OS_S_IRWXU;
    buf->st_nlink = 2;
}

static int vfs_rel_is_root(const char *rel_path)
{
    return rel_path[0] == '/' && rel_path[1] == '\0';
}

int vfs_stat(const char *path, OS32_Stat *buf)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    u32 dev;
    int rc;

    if (!buf) return VFS_ERR_INVAL;

    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);

    if (!ops) return VFS_ERR_NOMOUNT;
    dev = vfs_dev_of_resolved(resolved);
    if (!ops->stat) {
        if (vfs_rel_is_root(rel_path)) {
            vfs_synth_root_stat(buf);
            buf->st_dev = dev;
            return VFS_OK;
        }
        return VFS_ERR_NOMOUNT;
    }
    rc = ops->stat(fs_ctx, rel_path, buf);
    if (rc != VFS_OK && vfs_rel_is_root(rel_path)) {
        vfs_synth_root_stat(buf);
        buf->st_dev = dev;
        return VFS_OK;
    }
    /* FS が入れた st_dev (どれも 0) は VFS が上書きする。別デバイスで
     * inode 番号が一致しても (st_dev, st_ino) が衝突しないようにするため */
    if (rc == VFS_OK) buf->st_dev = dev;
    return rc;
}

/* vfs_path_kind の list_dir プローブ用 (何もしない) */
static void vfs_kind_probe_cb(const VfsDirEntry *entry, void *ctx)
{
    (void)entry; (void)ctx;
}

int vfs_path_kind(const char *path)
{
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    void *fs_ctx;
    VfsOps *ops;
    OS32_Stat st;
    int rc;

    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops) return VFS_ERR_NOMOUNT;

    /* マウントルートは常にディレクトリ (FS ドライバに聞かない) */
    if (vfs_rel_is_root(rel_path)) return VFS_KIND_DIR;

    /* stat を**持っている**ドライバの答えは最終判断。失敗してもプローブへ
     * 落とさない (Codex 実装レビュー 往復 3 の B6)。
     * 以前は NOTFOUND 以外の失敗を「stat 未対応」とみなして下へ落としていた
     * ので、stat が一時的に読めなかっただけのディレクトリが
     * get_file_size (ディレクトリでも成功する) でファイルに化け、
     * `cd` が NOTDIR になり `sys_open` のディレクトリ拒否をすり抜けた。
     * 「未対応」はドライバが stat を**持たない**ことで表す。 */
    if (ops->stat) {
        rc = ops->stat(fs_ctx, rel_path, &st);
        if (rc == VFS_OK) {
            return ((st.st_mode & OS_S_IFMT) == OS_S_IFDIR) ? VFS_KIND_DIR : VFS_KIND_FILE;
        }
        return rc;
    }

    /* stat を持たない FS だけのプローブ: list_dir が通ればディレクトリ、
     * get_file_size が通ればファイル。
     * **「読めなかった」エラーはそのまま伝える** — get_file_size は
     * ディレクトリでも成功するので、ここで落とすとファイルに化ける。
     * 次へ進むのは「ディレクトリではない」と分かったときだけ。 */
    if (ops->list_dir) {
        rc = ops->list_dir(fs_ctx, rel_path, vfs_kind_probe_cb, (void *)0);
        if (rc == VFS_OK) return VFS_KIND_DIR;
        if (rc != VFS_ERR_NOTDIR && rc != VFS_ERR_NOTFOUND) return rc;
    }
    if (ops->get_file_size) {
        u32 sz;
        if (ops->get_file_size(fs_ctx, rel_path, &sz) == VFS_OK) return VFS_KIND_FILE;
    }
    return VFS_ERR_NOTFOUND;
}

/* end of vfs.c */
