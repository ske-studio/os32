/* ======================================================================== */
/*  VFS.H — 仮想ファイルシステム抽象化レイヤー                                */
/*                                                                          */
/*  ext2/fat12等を透過的に扱うための共通インターフェース。                    */
/*  Linuxライクなコマンド体系 (ls, cat, rm, mkdir等) を実現する。            */
/* ======================================================================== */

#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "os32_kapi_shared.h"

/* ファイルタイプ */
#define VFS_TYPE_FILE  OS32_FILE_TYPE_FILE
#define VFS_TYPE_DIR   OS32_FILE_TYPE_DIR

/* VFS制限値 */
#define VFS_MAX_PATH       OS32_MAX_PATH
#define VFS_MAX_PATH_DEPTH 32
#define VFS_MAX_DEVNAME    16
#define VFS_MAX_FS         8
#define VFS_MAX_OPEN_FILES 16
#define VFS_MNTPATH_MAX    16

/* エラーコード */
#define VFS_OK          0
#define VFS_ERR_IO     -1
#define VFS_ERR_NOTFOUND -2
#define VFS_ERR_NOMOUNT -3
#define VFS_ERR_NOSPC  -4
#define VFS_ERR_EXIST  -5
#define VFS_ERR_NOTDIR -6
#define VFS_ERR_NOTEMPTY -7
#define VFS_ERR_ISDIR  -8
#define VFS_ERR_INVAL  -9

/* ディレクトリエントリ (FS共通) */
typedef struct {
    char name[VFS_MAX_PATH];
    u32  size;
    u8   type;    /* VFS_TYPE_FILE or VFS_TYPE_DIR */
} VfsDirEntry;

/* ディレクトリ列挙コールバック */
typedef void (*vfs_dir_cb)(const VfsDirEntry *entry, void *ctx);

/* FS操作テーブル (各FSドライバが実装) */
typedef struct {
    const char *name;                /* "ext2", "fat12" */

    /* マウント/アンマウント */
    int  (*mount)(int dev_id);
    void (*umount)(void);
    int  (*is_mounted)(void);

    /* ディレクトリ操作 (パス文字列ベース) */
    int  (*list_dir)(const char *path, vfs_dir_cb cb, void *ctx);
    int  (*mkdir)(const char *path);
    int  (*rmdir)(const char *path);

    /* ファイル操作 (パス文字列ベース) */
    int  (*read_file)(const char *path, void *buf, u32 max_size);
    int  (*write_file)(const char *path, const void *data, u32 size);
    int  (*unlink)(const char *path);
    int  (*rename)(const char *oldpath, const char *newpath);

    /* ストリーム操作 (シーク・部分読み書き対応用) */
    int  (*get_file_size)(const char *path, u32 *size);
    int  (*read_stream)(const char *path, void *buf, u32 size, u32 offset);
    int  (*write_stream)(const char *path, const void *buf, u32 size, u32 offset);

    /* メタデータ */
    int  (*sync)(void);

    /* ファイルシステム情報 */
    u32  (*total_blocks)(void);
    u32  (*free_blocks)(void);
    u32  (*block_size)(void);

    /* 追加: ファイル属性・状態 */
    int  (*stat)(const char *path, OS32_Stat *buf);
} VfsOps;

/* ---- VFS API ---- */

/* ファイルシステム実装の登録 */
void vfs_register_fs(VfsOps *ops);

/* デバイス名 → デバイスID変換 ("hd0"→0, "fd0"→0等) */
int vfs_dev_parse(const char *name, int *dev_type, int *dev_id);

/* マウント/アンマウント */
int  vfs_mount(const char *prefix, const char *dev_name, const char *fstype);
void vfs_umount(const char *prefix);
int  vfs_is_mounted(const char *prefix);
const char *vfs_fstype(const char *prefix);
const char *vfs_devname(const char *prefix);

/* ディレクトリ操作 */
int  vfs_ls(const char *path, vfs_dir_cb cb, void *ctx);
int  vfs_mkdir(const char *path);
int  vfs_rmdir(const char *path);

/* ファイル操作 (パスベース・一括処理) */
int  vfs_read(const char *path, void *buf, u32 max_size);
int  vfs_write(const char *path, const void *data, u32 size);
int  vfs_rm(const char *path);
int  vfs_rename(const char *oldpath, const char *newpath);

/* ストリーム操作 (ファイルディスクリプタ・シーク対応) */
int  vfs_open(const char *path, int mode);
void vfs_close(int fd);
int  vfs_read_fd(int fd, void *buf, u32 size);
int  vfs_write_fd(int fd, const void *buf, u32 size);
int  vfs_seek(int fd, int offset, int whence);
int  vfs_tell(int fd);
u32  vfs_get_size(int fd);
int  vfs_isatty(int fd);

/* ファイル情報 */
int  vfs_stat(const char *path, OS32_Stat *buf);
int  vfs_fstat(int fd, OS32_Stat *buf);
/* メタデータ */
int  vfs_sync(void);

/* ファイルシステム情報 */
u32 vfs_total_blocks(void);
u32 vfs_free_blocks(void);
u32 vfs_block_size(void);

/* カレントディレクトリ */
const char *vfs_cwd(void);
int vfs_chdir(const char *path);

/* パスの正規化 (相対→絶対) */
/* パスの正規化 (相対→絶対) */
void vfs_resolve_path(const char *input, char *output, int out_size);

/* 内部ルーターの公開 (vfs_fd.c向け) */
VfsOps *vfs_route(const char *path, char *rel_out, int max_rel);

/* レガシー互換ラッパー */
void vfs_sys_compat_shell_print(const char *s, u8 attr);

#endif /* VFS_H */
