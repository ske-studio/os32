/* ======================================================================== */
/*  VFS_FD.C — 仮想ファイルシステム (ファイルディスクリプタ管理)                 */
/*                                                                          */
/*  オープンされたファイルのテーブル(open_files)とシーク状態を管理し、       */
/*  FDベースの入出力(read, write)およびコンソール(TTY)への仮想化を行う。     */
/* ======================================================================== */

#include "vfs.h"
#include "fd_redirect.h"
#include "os32_kapi_shared.h" /* O_RDONLY, SEEK_SET 等 */
#include "console.h"
#include "kbd.h"

#ifndef ATTR_WHITE
#define ATTR_WHITE   TATTR_WHITE
#endif

/* ======== 内部ユーティリティ ======== */

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_cpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ======================================================================== */
/*  ストリーム操作 (ファイルディスクリプタ)                                  */
/* ======================================================================== */

typedef struct {
    int in_use;
    char path[VFS_MAX_PATH];
    u32 offset;
    u32 size;
    int mode;
    VfsOps *ops;
    void *fs_ctx;       /* FSドライバ固有のインスタンスコンテキスト */
} VfsFile;

static VfsFile open_files[VFS_MAX_OPEN_FILES];

int vfs_open(const char *path, int mode)
{
    int i, fd = -1;
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    u32 file_size = 0;
    int rc;
    void *fs_ctx;
    VfsOps *ops;

    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops) return VFS_ERR_NOMOUNT;

    /* 空きスロットを探す (FD0, 1, 2 は標準入出力用に予約) */
    for (i = 3; i < VFS_MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return VFS_ERR_NOSPC; /* FD上限 */

    /* サイズ取得・存在確認 */
    rc = -1;
    if (ops->get_file_size) {
        rc = ops->get_file_size(fs_ctx, rel_path, &file_size);
    } else {
        /* get_file_size非対応の場合、安全のためエラー */
        return VFS_ERR_INVAL;
    }

    if (rc != VFS_OK) {
        /* ファイルが存在しない場合 */
        if (mode & O_CREAT) {
            /* 作成処理 (サイズ0の空ファイルを作成してからサイズ取得等) */
            /* 今回は簡易的に0バイトでwriteして作らせる */
            rc = ops->write_file(fs_ctx, rel_path, "", 0);
            if (rc != VFS_OK) return rc;
            file_size = 0;
        } else {
            return VFS_ERR_NOTFOUND;
        }
    } else {
        if (mode & O_TRUNC) {
            if (mode & O_WRONLY || mode & O_RDWR) {
                /* 切り詰め：空ファイルで上書き */
                ops->write_file(fs_ctx, rel_path, "", 0);
                file_size = 0;
            }
        }
    }

    open_files[fd].in_use = 1;
    str_cpy(open_files[fd].path, rel_path, VFS_MAX_PATH);
    open_files[fd].offset = 0;
    open_files[fd].size = file_size;
    open_files[fd].mode = mode;
    open_files[fd].ops = ops;
    open_files[fd].fs_ctx = fs_ctx;

    return fd;
}

void vfs_close(int fd)
{
    if (fd >= 0 && fd < VFS_MAX_OPEN_FILES) {
        open_files[fd].in_use = 0;
        open_files[fd].fs_ctx = (void *)0;
    }
}

int vfs_read_fd(int fd, void *buf, u32 size)
{
    int rc;
    VfsFile *f;

    if (fd == 0) {
        /* リダイレクト中ならリダイレクト先から読む */
        if (fd_is_redirected(0)) {
            return fd_redirect_read(0, buf, size);
        }
        /* デフォルト: キーボードから1文字ずつ読む */
        {
            u8 *p = (u8 *)buf;
            u32 i;
            for (i = 0; i < size; i++) p[i] = (u8)kbd_getchar();
            return size;
        }
    }
    if (fd == 1 || fd == 2) return VFS_ERR_INVAL;

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    f = &open_files[fd];
    if (!f->in_use) return VFS_ERR_INVAL;
    if ((f->mode & 3) == O_WRONLY) return VFS_ERR_INVAL; /* 書き込み専用 */

    if (f->offset >= f->size) return 0; /* EOF */
    if (f->offset + size > f->size) {
        size = f->size - f->offset;
    }

    if (f->ops->read_stream) {
        rc = f->ops->read_stream(f->fs_ctx, f->path, buf, size, f->offset);
        if (rc > 0) {
            f->offset += rc;
            return rc;
        }
        return rc;
    }
    return VFS_ERR_INVAL;
}

int vfs_write_fd(int fd, const void *buf, u32 size)
{
    int rc;
    VfsFile *f;

    if (fd == 1 || fd == 2) {
        /* リダイレクト中ならリダイレクト先へ書く */
        if (fd_is_redirected(fd)) {
            return fd_redirect_write(fd, buf, size);
        }
        /* デフォルト: コンソール出力 */
        console_write((const char *)buf, size, ATTR_WHITE);
        return size;
    }
    if (fd == 0) return VFS_ERR_INVAL;

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    f = &open_files[fd];
    if (!f->in_use) return VFS_ERR_INVAL;
    if ((f->mode & 3) == O_RDONLY) return VFS_ERR_INVAL; /* 読み込み専用 */

    if (f->ops->write_stream) {
        rc = f->ops->write_stream(f->fs_ctx, f->path, buf, size, f->offset);
        if (rc > 0) {
            f->offset += rc;
            if (f->offset > f->size) {
                f->size = f->offset; /* サイズ拡張 */
            }
            return rc;
        }
        return rc;
    }
    return VFS_ERR_INVAL;
}

int vfs_seek(int fd, int offset, int whence)
{
    VfsFile *f;
    int new_pos;

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    f = &open_files[fd];
    if (!f->in_use) return VFS_ERR_INVAL;

    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = (int)f->offset + offset;
    } else if (whence == SEEK_END) {
        new_pos = (int)f->size + offset;
    } else {
        return VFS_ERR_INVAL;
    }

    if (new_pos < 0) return VFS_ERR_INVAL;
    f->offset = (u32)new_pos;
    return new_pos;
}

int vfs_tell(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    if (!open_files[fd].in_use) return VFS_ERR_INVAL;
    return (int)open_files[fd].offset;
}

u32 vfs_get_size(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return 0;
    if (!open_files[fd].in_use) return 0;
    return open_files[fd].size;
}

int vfs_isatty(int fd)
{
    if (fd == 0 || fd == 1 || fd == 2) {
        /* リダイレクト中はTTYではない */
        if (fd_is_redirected(fd)) return 0;
        return 1;
    }
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    if (!open_files[fd].in_use) return VFS_ERR_INVAL;
    
    return 0;
}

int vfs_fstat(int fd, OS32_Stat *buf)
{
    if (!buf) return VFS_ERR_INVAL;

    if (fd == 0 || fd == 1 || fd == 2) {
        /* Standard I/O / TTY */
        int i;
        u8 *p = (u8 *)buf;
        for (i = 0; i < sizeof(OS32_Stat); i++) p[i] = 0;
        
        buf->st_dev = 0;
        buf->st_ino = fd + 1; /* Dummy inode */
        buf->st_mode = OS_S_IFCHR | OS_S_IRUSR | OS_S_IWUSR | OS_S_IRGRP | OS_S_IWGRP | OS_S_IROTH | OS_S_IWOTH; /* 0666 */
        buf->st_nlink = 1;
        
        /* Times are 0 (Unix epoch start) for dummy TTY */
        return VFS_OK;
    }

    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    if (!open_files[fd].in_use) return VFS_ERR_INVAL;

    if (!open_files[fd].ops || !open_files[fd].ops->stat) {
        return VFS_ERR_NOMOUNT;
    }
    
    return open_files[fd].ops->stat(open_files[fd].fs_ctx, open_files[fd].path, buf);
}

/* レガシー shell_print 互換ラッパー (Phase 2) */
void vfs_sys_compat_shell_print(const char *s, u8 attr)
{
    /* 互換機能として常に FD=1(標準出力) へ流し込む。色指定は無視される */
    if (!s) return;
    vfs_write_fd(1, s, str_len(s));
}
