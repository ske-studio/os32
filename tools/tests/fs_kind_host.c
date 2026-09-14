/* =========================================================================
 *  FS_KIND_HOST.C — 種別判定を「列挙の成否」で代用していた退行 (B5) を
 *  **実物のソースで** 確かめる
 *
 *  票: H1 (docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md) / Codex 実装レビュー
 *      往復 3 の B5。`d574704` で入った退行。
 *  実行: python3 -B tools/tests/test_fs_kind.py [--target]
 *  記録: tools/tests/h1_tdd.md
 *
 *  userland/shell/cmd_fs_shared.c と userland/shell/cmd_file.c を 1 行も
 *  写さずそのまま #include する (模型ではない)。差し替えるのは KernelAPI と
 *  `shell_print_help` だけ。贋ファイルシステムは
 *    - `sys_stat` は正しく答える
 *    - `sys_ls` は指定したパスで OS32_ERR_FULL / OS32_ERR_IO を返す
 *  という状態を作れる (1000 件超のディレクトリ / 途中で切れた列挙の再現)。
 *
 *  いちばん大事なのは `cp -r` の**宛先の階層**:
 *    cp -r /src /big   ->   /big/src/a.txt   (× /big/a.txt)
 *  列挙がエラーを返しても、宛先がディレクトリである限りここは動かない。
 *
 *  エミュレータ・実配備・make には一切触れない。
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "os32api.h"

static int failures;
static int checks;

static void check(int cond, const char *name)
{
    checks++;
    printf("  %s %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) failures++;
}

/* ------------------------------------------------------------------------ */
/*  贋ファイルシステム                                                        */
/* ------------------------------------------------------------------------ */

#define FSK_MAX_NODES 48
#define FSK_MAX_FDS   8
#define FSK_PATH_CAP  256

typedef struct {
    int  used;
    char path[FSK_PATH_CAP];
    int  is_dir;
    char data[64];
    u32  size;
    int  stat_err;    /* != 0 … sys_stat がこの値を返す */
    int  ls_err;      /* != 0 … sys_ls がこの値を返す (列挙の失敗を注入) */
} FskNode;

typedef struct { int used; int node; u32 pos; } FskFd;

static FskNode fsk[FSK_MAX_NODES];
static FskFd   fsk_fds[FSK_MAX_FDS];
static int     fsk_mkdir_calls;
static char    fsk_log[16384];
static u32     fsk_log_len;

static void fsk_reset(void)
{
    memset(fsk, 0, sizeof(fsk));
    memset(fsk_fds, 0, sizeof(fsk_fds));
    fsk_mkdir_calls = 0;
    fsk_log_len = 0;
    fsk_log[0] = '\0';
}

static int fsk_find(const char *path)
{
    int i;
    for (i = 0; i < FSK_MAX_NODES; i++)
        if (fsk[i].used && strcmp(fsk[i].path, path) == 0) return i;
    return -1;
}

static int fsk_add(const char *path, int is_dir, const char *data)
{
    int i;
    for (i = 0; i < FSK_MAX_NODES; i++) {
        if (fsk[i].used) continue;
        memset(&fsk[i], 0, sizeof(FskNode));
        fsk[i].used = 1;
        fsk[i].is_dir = is_dir;
        strncpy(fsk[i].path, path, FSK_PATH_CAP - 1);
        if (data) {
            strncpy(fsk[i].data, data, sizeof(fsk[i].data) - 1);
            fsk[i].size = (u32)strlen(fsk[i].data);
        }
        return i;
    }
    printf("  (harness) node table full\n");
    exit(2);
}

static void fsk_parent(const char *path, char *out)
{
    int last = -1;
    int i;
    for (i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last <= 0) { strcpy(out, "/"); return; }
    memcpy(out, path, (size_t)last);
    out[last] = '\0';
}

static const char *fsk_base(const char *path)
{
    const char *b = path;
    const char *p;
    for (p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* ------------------------------------------------------------------------ */
/*  贋 KernelAPI                                                              */
/* ------------------------------------------------------------------------ */

static void *fk_mem_alloc(u32 n) { return malloc(n); }
static void  fk_mem_free(void *p) { free(p); }

static void fk_kprintf(u8 attr, const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    int n;
    (void)attr;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((u32)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    if (fsk_log_len + (u32)n + 1 < sizeof(fsk_log)) {
        memcpy(fsk_log + fsk_log_len, line, (size_t)n);
        fsk_log_len += (u32)n;
        fsk_log[fsk_log_len] = '\0';
    }
}

static int fk_sys_stat(const char *path, OS32_Stat *buf)
{
    int n = fsk_find(path);
    memset(buf, 0, sizeof(OS32_Stat));
    if (n < 0) return OS32_ERR_NOTFOUND;
    if (fsk[n].stat_err) return fsk[n].stat_err;
    buf->st_dev = 1;
    buf->st_ino = (u32)(n + 100);
    buf->st_nlink = 1;
    buf->st_mode = (u16)(fsk[n].is_dir ? (OS_S_IFDIR | 0755)
                                       : (OS_S_IFREG | 0644));
    buf->st_size = fsk[n].size;
    return 0;
}

static int fk_sys_ls(const char *path, void *cb, void *ctx)
{
    DirCallback fn = (DirCallback)cb;
    char dir[FSK_PATH_CAP];
    char parent[FSK_PATH_CAP];
    DirEntry_Ext e;
    int i, n;

    strncpy(dir, path && path[0] ? path : "/", FSK_PATH_CAP - 1);
    dir[FSK_PATH_CAP - 1] = '\0';
    n = (int)strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = '\0';

    i = fsk_find(dir);
    if (strcmp(dir, "/") != 0) {
        if (i < 0) return OS32_ERR_NOTFOUND;
        if (!fsk[i].is_dir) return OS32_ERR_NOTDIR;
        /* 注入: 1000 件超 (FULL) / 途中で切れた列挙 (IO) */
        if (fsk[i].ls_err) return fsk[i].ls_err;
    }

    for (i = 0; i < FSK_MAX_NODES; i++) {
        if (!fsk[i].used) continue;
        fsk_parent(fsk[i].path, parent);
        if (strcmp(parent, dir) != 0) continue;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, fsk_base(fsk[i].path), OS32_MAX_PATH - 1);
        e.size = fsk[i].size;
        e.type = fsk[i].is_dir ? OS32_FILE_TYPE_DIR : OS32_FILE_TYPE_FILE;
        fn(&e, ctx);
    }
    return 0;
}

static int fk_sys_mkdir(const char *path)
{
    fsk_mkdir_calls++;
    if (fsk_find(path) >= 0) return OS32_ERR_EXIST;
    fsk_add(path, 1, 0);
    return 0;
}

static int fk_sys_open(const char *path, int mode)
{
    int n = fsk_find(path);
    int f;
    if (n < 0) {
        if (!(mode & KAPI_O_CREAT)) return OS32_ERR_NOTFOUND;
        {   /* 親がディレクトリでなければ作れない (実 FS と同じ) */
            char parent[FSK_PATH_CAP];
            int pi;
            fsk_parent(path, parent);
            pi = fsk_find(parent);
            if (strcmp(parent, "/") != 0 && (pi < 0 || !fsk[pi].is_dir))
                return OS32_ERR_NOTDIR;
        }
        n = fsk_add(path, 0, 0);
    } else if (fsk[n].is_dir) {
        return OS32_ERR_ISDIR;
    }
    if (mode & KAPI_O_TRUNC) { fsk[n].size = 0; fsk[n].data[0] = '\0'; }
    for (f = 0; f < FSK_MAX_FDS; f++) {
        if (fsk_fds[f].used) continue;
        fsk_fds[f].used = 1;
        fsk_fds[f].node = n;
        fsk_fds[f].pos = 0;
        return f + 3;
    }
    return OS32_ERR_NOSPC;
}

static void fk_sys_close(int fd)
{
    if (fd >= 3 && fd - 3 < FSK_MAX_FDS) fsk_fds[fd - 3].used = 0;
}

static int fk_sys_read(int fd, void *buf, u32 size)
{
    FskFd *h;
    FskNode *nd;
    u32 avail;
    if (fd < 3 || fd - 3 >= FSK_MAX_FDS || !fsk_fds[fd - 3].used)
        return OS32_ERR_IO;
    h = &fsk_fds[fd - 3];
    nd = &fsk[h->node];
    if (h->pos >= nd->size) return 0;
    avail = nd->size - h->pos;
    if (avail > size) avail = size;
    memcpy(buf, nd->data + h->pos, avail);
    h->pos += avail;
    return (int)avail;
}

static int fk_sys_write(int fd, const void *buf, u32 size)
{
    FskFd *h;
    FskNode *nd;
    if (fd < 3 || fd - 3 >= FSK_MAX_FDS || !fsk_fds[fd - 3].used)
        return OS32_ERR_IO;
    h = &fsk_fds[fd - 3];
    nd = &fsk[h->node];
    if (h->pos + size >= sizeof(nd->data)) return OS32_ERR_NOSPC;
    memcpy(nd->data + h->pos, buf, size);
    h->pos += size;
    if (h->pos > nd->size) nd->size = h->pos;
    nd->data[nd->size] = '\0';
    return (int)size;
}

static int fk_sys_unlink(const char *path)
{
    int n = fsk_find(path);
    if (n < 0) return OS32_ERR_NOTFOUND;
    fsk[n].used = 0;
    return 0;
}

static int fk_sys_rmdir(const char *path) { return fk_sys_unlink(path); }
static int fk_sys_rename(const char *a, const char *b)
{
    int n = fsk_find(a);
    if (n < 0) return OS32_ERR_NOTFOUND;
    strncpy(fsk[n].path, b, FSK_PATH_CAP - 1);
    return 0;
}
static const char *fk_sys_getcwd(void) { return "/"; }
static int fk_sys_isatty(int fd) { (void)fd; return 0; }

static KernelAPI g_fake;
KernelAPI *g_api = &g_fake;

static void fake_api_init(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.mem_alloc = fk_mem_alloc;
    g_fake.mem_free = fk_mem_free;
    g_fake.kprintf = fk_kprintf;
    g_fake.sys_stat = fk_sys_stat;
    g_fake.sys_ls = fk_sys_ls;
    g_fake.sys_mkdir = fk_sys_mkdir;
    g_fake.sys_open = fk_sys_open;
    g_fake.sys_close = fk_sys_close;
    g_fake.sys_read = fk_sys_read;
    g_fake.sys_write = fk_sys_write;
    g_fake.sys_unlink = fk_sys_unlink;
    g_fake.sys_rmdir = fk_sys_rmdir;
    g_fake.sys_rename = fk_sys_rename;
    g_fake.sys_getcwd = fk_sys_getcwd;
    g_fake.sys_isatty = fk_sys_isatty;
}

/* ------------------------------------------------------------------------ */
/*  実物のソース                                                              */
/* ------------------------------------------------------------------------ */

#include "../../userland/shell/cmd_fs_shared.c"
#include "../../userland/shell/cmd_file.c"

/* shell.c 側の実体。ここでは使わない (コマンド表の登録もしない)。
 * ShellCmd は shell.h (上の #include 経由) で定義される。 */
void shell_print_help(const char *cmd) { (void)cmd; }
void shell_register_cmds(const ShellCmd *cmds) { (void)cmds; }

/* ------------------------------------------------------------------------ */

static void run_cp(const char *a, const char *b, const char *c)
{
    char *av[5];
    int n = 0;
    fsk_log_len = 0;
    fsk_log[0] = '\0';
    av[n++] = (char *)"cp";
    if (a) av[n++] = (char *)a;
    if (b) av[n++] = (char *)b;
    if (c) av[n++] = (char *)c;
    av[n] = 0;
    cmd_cp(n, av);
}

/* /src (ディレクトリ, a.txt を持つ) と /big (ディレクトリ) を作る。
 * /big の列挙は ls_err を返す (1000 件超 / 途中で切れた列挙の再現)。
 * /big/a.txt には既存の内容を置いておく — 取り違えたら壊れる。 */
static void setup_cp_tree(int ls_err)
{
    int n;
    fsk_reset();
    fsk_add("/src", 1, 0);
    fsk_add("/src/a.txt", 0, "NEW");
    n = fsk_add("/big", 1, 0);
    fsk[n].ls_err = ls_err;
    fsk_add("/big/a.txt", 0, "KEEP");
}

int main(void)
{
    int n;

    fake_api_init();
    printf("=== fs_is_dir / cp -r の宛先階層 (票 H1 / 往復 3 の B5) ===\n");

    printf("== fs_path_kind / fs_is_dir は型で答える ==\n");
    fsk_reset();
    fsk_add("/d", 1, 0);
    fsk_add("/f", 0, "x");
    n = fsk_find("/d");
    fsk[n].ls_err = OS32_ERR_FULL;
    check(fs_is_dir("/d") == 1,
          "1000 件超 (列挙 FULL) でもディレクトリと答える");
    check(fs_path_kind("/d") == FS_KIND_DIR, "fs_path_kind も DIR");
    fsk[n].ls_err = OS32_ERR_IO;
    check(fs_is_dir("/d") == 1,
          "途中で切れた列挙 (IO) でもディレクトリと答える");
    check(fs_is_dir("/f") == 0, "通常ファイルは 0");
    check(fs_path_kind("/f") == FS_KIND_FILE, "fs_path_kind は FILE");
    check(fs_path_kind("/nope") == OS32_ERR_NOTFOUND, "不存在は NOTFOUND");
    check(fs_is_dir("/nope") == 0, "不存在は 0");

    printf("== stat が使えない FS のときだけ列挙を代替に使う ==\n");
    fsk_reset();
    n = fsk_add("/d", 1, 0);
    fsk[n].stat_err = OS32_ERR_NOSYS;         /* stat 非対応の FS を模す */
    check(fs_is_dir("/d") == 1, "stat 非対応でも列挙が通ればディレクトリ");
    fsk[n].ls_err = OS32_ERR_FULL;
    check(fs_path_kind("/d") == OS32_ERR_NOSYS,
          "列挙も読めなければ stat のエラーを返す "
          "(「ディレクトリでない」と言い切らない)");
    check(fs_is_dir("/d") == 0, "判定できないときの真偽は 0 (従来の形)");

    printf("== cp -r の宛先階層 ==\n");
    /* (a) 宛先の列挙が FULL — B5 の反例そのもの */
    setup_cp_tree(OS32_ERR_FULL);
    run_cp("-r", "/src", "/big");
    check(fsk_find("/big/src/a.txt") >= 0,
          "列挙 FULL でも /big/src/a.txt へ入る");
    n = fsk_find("/big/a.txt");
    check(n >= 0 && strcmp(fsk[n].data, "KEEP") == 0,
          "**/big/a.txt を上書きしない** (階層の取り違えが無い)");

    /* (b) 宛先の列挙が途中で切れた */
    setup_cp_tree(OS32_ERR_IO);
    run_cp("-r", "/src", "/big");
    check(fsk_find("/big/src/a.txt") >= 0, "列挙 IO でも /big/src/a.txt へ入る");
    n = fsk_find("/big/a.txt");
    check(n >= 0 && strcmp(fsk[n].data, "KEEP") == 0,
          "/big/a.txt を上書きしない");

    /* (c) 列挙が普通に通る場合 — 退行していないこと */
    setup_cp_tree(0);
    run_cp("-r", "/src", "/big");
    check(fsk_find("/big/src/a.txt") >= 0, "通常時も /big/src/a.txt");
    n = fsk_find("/big/a.txt");
    check(n >= 0 && strcmp(fsk[n].data, "KEEP") == 0, "通常時も上書きしない");

    /* (d) 宛先が存在しない = そのものを作る (従来の意味) */
    fsk_reset();
    fsk_add("/src", 1, 0);
    fsk_add("/src/a.txt", 0, "NEW");
    run_cp("-r", "/src", "/newdir");
    check(fsk_find("/newdir/a.txt") >= 0,
          "宛先が無いときは /newdir 直下へ (basename を足さない)");

    printf("== 単一ファイルの cp ==\n");
    fsk_reset();
    fsk_add("/f.txt", 0, "NEW");
    n = fsk_add("/big", 1, 0);
    fsk[n].ls_err = OS32_ERR_FULL;
    fsk_add("/big/f.txt", 0, "KEEP");
    run_cp("/f.txt", "/big", 0);
    n = fsk_find("/big/f.txt");
    check(n >= 0 && strcmp(fsk[n].data, "NEW") == 0,
          "列挙 FULL のディレクトリ宛でも /big/f.txt へ写す");
    check(fsk_find("/big") >= 0 && fsk[fsk_find("/big")].is_dir,
          "/big をファイルで潰さない");

    printf("== 複数入力の cp ==\n");
    fsk_reset();
    fsk_add("/a.txt", 0, "A");
    fsk_add("/b.txt", 0, "B");
    n = fsk_add("/big", 1, 0);
    fsk[n].ls_err = OS32_ERR_FULL;
    run_cp("/a.txt", "/b.txt", "/big");
    check(fsk_find("/big/a.txt") >= 0 && fsk_find("/big/b.txt") >= 0,
          "列挙 FULL でも複数入力を受け付ける");
    check(strstr(fsk_log, "multiple files must be copied into a directory")
              == 0,
          "「ディレクトリでない」と誤って断らない");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
