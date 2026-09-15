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
    u32 dev;            /* open 時のマウント (vfs_path_dev)。fstat の st_dev */
    int protect;        /* 1=カーネル常駐FD (exec_exitの自動クローズ対象外) */
    int owner;          /* GENERIC: current owner; SQLITE: explicit owner */
    int lifetime;
    VfsSqliteCookie cookie;
    u32 generation;
    int sqlite_flags;
    int quarantined;
} VfsFile;

static VfsFile open_files[VFS_MAX_OPEN_FILES];

static int vfs_open_internal(const char *path, int mode, int owner,
                             const VfsSqliteCookie *cookie, int sqlite_flags)
{
    int i, fd = -1;
    char resolved[VFS_MAX_PATH], rel_path[VFS_MAX_PATH];
    u32 file_size = 0;
    int rc;
    int kind;
    void *fs_ctx;
    VfsOps *ops;

    /* Preflight before even path probes. FD0-2 are reserved. A slot whose
     * generation reached MAX is retired on close, including GENERIC use.
     * Like the existing VFS, this path requires non-reentrant callers. */
    for (i = 3; i < VFS_MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use &&
            open_files[i].generation < VFS_FD_GENERATION_MAX) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return VFS_ERR_NOSPC; /* FD上限 */

    vfs_resolve_path(path, resolved, VFS_MAX_PATH);
    ops = vfs_route(resolved, rel_path, VFS_MAX_PATH, &fs_ctx);
    if (!ops) return VFS_ERR_NOMOUNT;

    /* ディレクトリは open できない。以前は get_file_size がディレクトリの
     * inode サイズを返すため open が通り、`cat /etc` が生のディレクトリ
     * ブロックを吐き、`mv dir x` が dir の生データを x に書いていた。
     *
     * **種別が確定しないときも open しない** (Codex 実装レビュー 往復 4 の B7)。
     * 「DIR に一致したときだけ弾く」作りだと、stat が読めずに kind が負値へ
     * なった経路が拒否をすり抜け、その先の get_file_size (ディレクトリでも
     * 成功する) が FD を発行してしまう — 上の不具合が開き直る。
     * 「エラー」を「ディレクトリではない」と読み替えない。
     *
     * NOTFOUND だけは続行する。下に O_CREAT の作成経路があるため。 */
    kind = vfs_path_kind(resolved);
    if (kind == VFS_KIND_DIR) return VFS_ERR_ISDIR;
    if (kind < 0 && kind != VFS_ERR_NOTFOUND) return kind;

    /* サイズ取得・存在確認 */
    rc = -1;
    if (ops->get_file_size) {
        rc = ops->get_file_size(fs_ctx, rel_path, &file_size);
    } else {
        /* get_file_size非対応の場合、安全のためエラー */
        return VFS_ERR_INVAL;
    }

    if (rc != VFS_OK) {
        /* **作成へ進めるのは「本当に無い」と分かったときだけ** (票 B8 の ②)。
         * 以前はサイズ取得の**あらゆる失敗**でここへ来て空ファイルを書いて
         * いたので、「通常ファイルと確認済み → サイズ取得だけ一度 I/O 失敗
         * → 書き込みは成功」で、**O_TRUNC を付けていなくても既存の中身が
         * 黙って消えた**。O_CREAT あり・O_TRUNC なしは「無ければ作る、
         * あれば開く」という最も普通の書き込み用途なので被害が大きい。
         * 「読めなかった」を「無い」と読み替えない。 */
        if (rc != VFS_ERR_NOTFOUND) return rc;
        if (mode & O_CREAT) {
            /* 作成処理 (サイズ0の空ファイルを作成してからサイズ取得等) */
            /* 今回は簡易的に0バイトでwriteして作らせる */
            if (!ops->write_file) return VFS_ERR_INVAL;
            rc = ops->write_file(fs_ctx, rel_path, "", 0);
            if (rc < 0) return rc;
            file_size = 0;
        } else {
            return VFS_ERR_NOTFOUND;
        }
    } else {
        if (mode & O_TRUNC) {
            if (mode & O_WRONLY || mode & O_RDWR) {
                /* 切り詰め：空ファイルで上書き。
                 * **戻り値を見る** — 以前は捨てていたので、ext2 の一括書き込みが
                 * 持つディレクトリ拒否 (fs/ext2_file.c) のような失敗も消え、
                 * 「切り詰まった」ことになっているサイズ 0 の FD が出ていた。 */
                if (!ops->write_file) return VFS_ERR_INVAL;
                rc = ops->write_file(fs_ctx, rel_path, "", 0);
                if (rc < 0) return rc;
                file_size = 0;
            }
        }
    }

    open_files[fd].generation++;
    str_cpy(open_files[fd].path, rel_path, VFS_MAX_PATH);
    open_files[fd].offset = 0;
    open_files[fd].size = file_size;
    open_files[fd].mode = mode;
    open_files[fd].ops = ops;
    open_files[fd].fs_ctx = fs_ctx;
    open_files[fd].dev = vfs_path_dev(resolved);
    open_files[fd].protect = 0;
    open_files[fd].owner = owner;
    open_files[fd].lifetime = cookie ? VFS_FD_SQLITE : VFS_FD_GENERIC;
    open_files[fd].cookie.group_index = cookie ? cookie->group_index : 0;
    open_files[fd].cookie.generation = cookie ? cookie->generation : 0;
    open_files[fd].sqlite_flags = sqlite_flags;
    open_files[fd].quarantined = 0;
    open_files[fd].in_use = 1;

    return fd;
}

int vfs_open(const char *path, int mode)
{
    return vfs_open_internal(path, mode, res_owner_get(),
                             (const VfsSqliteCookie *)0, 0);
}

int vfs_open_sqlite(const char *path, int mode, int owner,
                    const VfsSqliteCookie *cookie, int sqlite_flags,
                    VfsSqliteLease *out)
{
    int fd;
    if (!path || !out || !cookie || cookie->group_index < 0 ||
        cookie->generation == 0 || owner < 0) return VFS_ERR_INVAL;
    fd = vfs_open_internal(path, mode, owner, cookie, sqlite_flags);
    if (fd < 0) return fd;
    out->fd = fd;
    out->cookie = *cookie;
    out->generation = open_files[fd].generation;
    return VFS_OK;
}

static int sqlite_cookie_equal(const VfsSqliteCookie *a,
                               const VfsSqliteCookie *b)
{
    return a->group_index == b->group_index && a->generation == b->generation;
}

int vfs_count_sqlite(const VfsSqliteCookie *cookie)
{
    int fd, count = 0;
    if (!cookie) return VFS_ERR_INVAL;
    for (fd = 3; fd < VFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].in_use && open_files[fd].lifetime == VFS_FD_SQLITE &&
            sqlite_cookie_equal(&open_files[fd].cookie, cookie)) count++;
    }
    return count;
}

int vfs_quarantine_sqlite(const VfsSqliteCookie *cookie)
{
    int fd;
    if (!cookie) return VFS_ERR_INVAL;
    for (fd = 3; fd < VFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].in_use && open_files[fd].lifetime == VFS_FD_SQLITE &&
            sqlite_cookie_equal(&open_files[fd].cookie, cookie))
            open_files[fd].quarantined = 1;
    }
    return VFS_OK;
}

int vfs_validate_sqlite(const VfsSqliteLease *lease)
{
    const VfsFile *f;
    if (!lease || lease->fd < 3 || lease->fd >= VFS_MAX_OPEN_FILES)
        return VFS_ERR_INVAL;
    f = &open_files[lease->fd];
    if (!f->in_use || f->lifetime != VFS_FD_SQLITE || f->quarantined ||
        f->generation != lease->generation ||
        !sqlite_cookie_equal(&f->cookie, &lease->cookie)) return VFS_ERR_INVAL;
    return VFS_OK;
}

int vfs_close_sqlite(const VfsSqliteLease *lease)
{
    VfsFile *f;
    if (vfs_validate_sqlite(lease) != VFS_OK) return VFS_ERR_INVAL;
    f = &open_files[lease->fd];
    f->in_use = 0;
    f->fs_ctx = (void *)0;
    f->protect = 0;
    f->owner = 0;
    return VFS_OK;
}

void vfs_close(int fd)
{
    if (fd >= 0 && fd < VFS_MAX_OPEN_FILES) {
        if (open_files[fd].lifetime == VFS_FD_SQLITE) return;
        open_files[fd].in_use = 0;
        open_files[fd].fs_ctx = (void *)0;
        open_files[fd].protect = 0;
        open_files[fd].owner = 0;
    }
}

void vfs_close_owned(int owner)
{
    int fd;
    for (fd = 3; fd < VFS_MAX_OPEN_FILES; fd++) {
        if (!open_files[fd].in_use) continue;
        if (open_files[fd].lifetime == VFS_FD_SQLITE) continue;
        if (open_files[fd].protect) continue;
        if (open_files[fd].owner != owner) continue;
        vfs_close(fd);
    }
}

/* カーネル常駐FDの保護フラグ設定 (FEP辞書など、exec_exit の
 * FD一括クローズから除外したいFDに使用する) */
int vfs_fd_set_protect(int fd, int on)
{
    if (fd < 3 || fd >= VFS_MAX_OPEN_FILES) return VFS_ERR_INVAL;
    if (!open_files[fd].in_use) return VFS_ERR_INVAL;
    if (open_files[fd].lifetime == VFS_FD_SQLITE) return VFS_ERR_INVAL;
    open_files[fd].protect = on ? 1 : 0;
    return VFS_OK;
}

int vfs_fd_is_protected(int fd)
{
    if (fd < 3 || fd >= VFS_MAX_OPEN_FILES) return 0;
    if (!open_files[fd].in_use) return 0;
    return open_files[fd].protect;
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
        /* デフォルト: キーボードから行単位で読む (TTY の canonical 相当)。
         * 以前は size バイト溜まるまで返らなかったので、`head` のように
         * 64KB を要求するプログラムが TTY から読むと永久にブロックした。
         * Enter (CR) は '\n' に正規化して行末で返す */
        {
            u8 *p = (u8 *)buf;
            u32 i = 0;
            while (i < size) {
                int c = kbd_getchar();
                if (c == '\r') c = '\n';
                p[i++] = (u8)c;
                if (c == '\n') break;
            }
            return (int)i;
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

    {
        /* vfs_stat と同じ規則で st_dev を上書きする (FS 側は 0 固定)。
         * stat と fstat で値が食い違うと同一ファイル判定が壊れる */
        int rc = open_files[fd].ops->stat(open_files[fd].fs_ctx,
                                          open_files[fd].path, buf);
        if (rc == VFS_OK) buf->st_dev = open_files[fd].dev;
        return rc;
    }
}

/* レガシー shell_print 互換ラッパー (Phase 2) */
void vfs_sys_compat_shell_print(const char *s, u8 attr)
{
    /* 互換機能として常に FD=1(標準出力) へ流し込む。色指定は無視される */
    if (!s) return;
    vfs_write_fd(1, s, str_len(s));
}
