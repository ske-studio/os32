/* ======================================================================== */
/*  FD_REDIRECT.C -- FD 0/1/2 リダイレクト管理                               */
/*                                                                          */
/*  標準入出力 (stdin/stdout/stderr) をファイルやメモリバッファに             */
/*  リダイレクトするためのカーネルモジュール。                                */
/*                                                                          */
/*  vfs_fd.c の vfs_read_fd()/vfs_write_fd() から呼び出され、                */
/*  リダイレクト先への透過的な入出力切り替えを提供する。                      */
/* ======================================================================== */

#include "fd_redirect.h"
#include "vfs.h"
#include "os32_kapi_shared.h"

/* FD 0/1/2 のリダイレクト状態テーブル */
static FdRedirect redir_table[3];

/* ======================================================================== */
/*  初期化                                                                  */
/* ======================================================================== */

void fd_redirect_init(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        redir_table[i].target_type = FD_TARGET_CONSOLE;
        redir_table[i].file_fd = -1;
        redir_table[i].buffer = (u8 *)0;
        redir_table[i].buf_capacity = 0;
        redir_table[i].buf_pos = 0;
        redir_table[i].buf_len = 0;
    }
}

/* ======================================================================== */
/*  リダイレクト設定                                                        */
/* ======================================================================== */

int fd_redirect_to_file(int fd, const char *path, int mode)
{
    int file_fd;
    int open_mode;

    if (fd < 0 || fd > 2) return -1;
    if (!path) return -1;

    /* 既存のリダイレクトを解除 */
    fd_redirect_reset(fd);

    /* オープンモードを決定 */
    if (mode == FD_REDIR_READ) {
        open_mode = O_RDONLY;
    } else if (mode == FD_REDIR_APPEND) {
        open_mode = O_WRONLY | O_CREAT;
    } else {
        /* FD_REDIR_WRITE: 上書き */
        open_mode = O_WRONLY | O_CREAT | O_TRUNC;
    }

    file_fd = vfs_open(path, open_mode);
    if (file_fd < 0) return file_fd;

    /* 追記モードの場合、末尾にシーク */
    if (mode == FD_REDIR_APPEND) {
        vfs_seek(file_fd, 0, SEEK_END);
    }

    redir_table[fd].target_type = FD_TARGET_FILE;
    redir_table[fd].file_fd = file_fd;
    redir_table[fd].buffer = (u8 *)0;
    redir_table[fd].buf_capacity = 0;
    redir_table[fd].buf_pos = 0;
    redir_table[fd].buf_len = 0;

    return 0;
}

int fd_redirect_to_buffer(int fd, u8 *buf, u32 size, u32 len)
{
    if (fd < 0 || fd > 2) return -1;
    if (!buf || size == 0) return -1;

    /* 既存のリダイレクトを解除 */
    fd_redirect_reset(fd);

    redir_table[fd].target_type = FD_TARGET_BUFFER;
    redir_table[fd].file_fd = -1;
    redir_table[fd].buffer = buf;
    redir_table[fd].buf_capacity = size;
    redir_table[fd].buf_pos = 0;
    redir_table[fd].buf_len = len;

    return 0;
}

/* ======================================================================== */
/*  リダイレクト解除                                                        */
/* ======================================================================== */

void fd_redirect_reset(int fd)
{
    if (fd < 0 || fd > 2) return;

    /* ファイルリダイレクト中ならクローズ */
    if (redir_table[fd].target_type == FD_TARGET_FILE) {
        if (redir_table[fd].file_fd >= 0) {
            vfs_close(redir_table[fd].file_fd);
        }
    }

    redir_table[fd].target_type = FD_TARGET_CONSOLE;
    redir_table[fd].file_fd = -1;
    redir_table[fd].buffer = (u8 *)0;
    redir_table[fd].buf_capacity = 0;
    redir_table[fd].buf_pos = 0;
    redir_table[fd].buf_len = 0;
}

/* ======================================================================== */
/*  状態問い合わせ                                                          */
/* ======================================================================== */

int fd_is_redirected(int fd)
{
    if (fd < 0 || fd > 2) return 0;
    return (redir_table[fd].target_type != FD_TARGET_CONSOLE);
}

/* ======================================================================== */
/*  リダイレクト先への読み書き                                              */
/* ======================================================================== */

int fd_redirect_read(int fd, void *buf, u32 size)
{
    FdRedirect *r;

    if (fd < 0 || fd > 2) return -1;
    r = &redir_table[fd];

    if (r->target_type == FD_TARGET_FILE) {
        /* ファイルからの読み込み */
        return vfs_read_fd(r->file_fd, buf, size);
    }

    if (r->target_type == FD_TARGET_BUFFER) {
        /* バッファからの読み込み */
        u32 avail = r->buf_len - r->buf_pos;
        u32 to_read;
        u32 i;
        u8 *dst = (u8 *)buf;

        if (avail == 0) return 0; /* EOF */
        to_read = (size < avail) ? size : avail;
        for (i = 0; i < to_read; i++) {
            dst[i] = r->buffer[r->buf_pos + i];
        }
        r->buf_pos += to_read;
        return (int)to_read;
    }

    return -1; /* コンソールモードでは呼ばれないはず */
}

int fd_redirect_write(int fd, const void *buf, u32 size)
{
    FdRedirect *r;

    if (fd < 0 || fd > 2) return -1;
    r = &redir_table[fd];

    if (r->target_type == FD_TARGET_FILE) {
        /* ファイルへの書き込み */
        return vfs_write_fd(r->file_fd, buf, size);
    }

    if (r->target_type == FD_TARGET_BUFFER) {
        /* バッファへの書き込み */
        u32 space = r->buf_capacity - r->buf_len;
        u32 to_write;
        u32 i;
        const u8 *src = (const u8 *)buf;

        to_write = (size < space) ? size : space;
        for (i = 0; i < to_write; i++) {
            r->buffer[r->buf_len + i] = src[i];
        }
        r->buf_len += to_write;
        return (int)to_write;
    }

    return -1;
}

/* ======================================================================== */
/*  パイプバッファ情報取得                                                  */
/* ======================================================================== */

u32 fd_redirect_get_buf_len(int fd)
{
    if (fd < 0 || fd > 2) return 0;
    if (redir_table[fd].target_type != FD_TARGET_BUFFER) return 0;
    return redir_table[fd].buf_len;
}
