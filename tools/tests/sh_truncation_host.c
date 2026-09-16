/* ========================================================================
 *  sh_truncation_host.c — シェルの「黙って切り詰める」経路を **実物のソース
 *                         で** 押さえる足場
 *
 *  対象票: docs/tasks/shell/TASK_SH_TRUNCATION.md §5 の段 1「足場」
 *  実行:   python3 -B tools/tests/test_sh_truncation.py
 *  記録:   tools/tests/sh_truncation_tdd.md
 *
 *  1 行も写さずそのまま #include する実物 (模型ではない):
 *    - userland/shell/main.c        (execute_command / execute_single /
 *                                    apply_redirects / split_pipeline、
 *                                    および sh_exec.inc = try_exec /
 *                                    try_exec_from_path / run_cmd_internal)
 *    - userland/shell/cmd_script.c  (source / if / goto / ask)
 *    - userland/shell/cmd_env.c     (env_expand / set / export)
 *    - userland/shell/cmd_base.c    (time / exit)
 *    - userland/shell/cmd_mnt.c     (exec)
 *    - userland/shell/cmd_dir.c / cmd_file.c / cmd_fs_shared.c / cmd_sys.c
 *
 *  つまり **登録表も execute_command も本物**で、`exit` を直接見るような
 *  スタブは 1 つも置いていない (sh_shell_host.c は execute_command を
 *  スタブにしているので、切り詰めの経路はそこでは試験できない)。
 *
 *  main.c の main() は _start から呼ばない (KernelAPI を受け取る形なので)。
 *  代わりに登録関数を並べて呼ぶ sh_boot() を置く — main() が呼ぶ順と同じ。
 *
 *  tools/tests/sh_shell_host.c と同じ様式 — ホスト ILP32 GNU89、libc 無し
 *  (-nostdlib、Linux の int 0x80 で write/exit)。<string.h> / <stdio.h> /
 *  <stdlib.h> は python 側が一時ディレクトリに置く薄いシム。
 * ======================================================================== */

#include "shell.h"
#include "config.h"

/* ---- libc の代わり (str_eq などが引く分だけ) ---------------------------- */

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i = 0;
    while (i < n && a[i] && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

unsigned long strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) ((char *)d)[i] = ((const char *)s)[i];
    return d;
}

void *memset(void *d, int c, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) ((unsigned char *)d)[i] = (unsigned char)c;
    return d;
}

char *strncpy(char *d, const char *s, unsigned long n)
{
    unsigned long i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i++; }
    while (i < n) d[i++] = '\0';
    return d;
}

char *strncat(char *d, const char *s, unsigned long n)
{
    unsigned long i = 0, j = 0;
    while (d[i]) i++;
    while (j < n && s[j]) { d[i + j] = s[j]; j++; }
    d[i + j] = '\0';
    return d;
}

char *strcat(char *d, const char *s)
{
    unsigned long i = 0, j = 0;
    while (d[i]) i++;
    while (s[j]) { d[i + j] = s[j]; j++; }
    d[i + j] = '\0';
    return d;
}

int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/* ---- 最小の報告系 (libc 無し) ------------------------------------------ */

static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}

static void report(const char *text)
{
    unsigned long len = 0;
    while (text[len]) len++;
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(text), "d"(len)
                     : "memory");
}

static int failures;

static void check(int cond, const char *name)
{
    report(cond ? "  ok   " : "  FAIL ");
    report(name);
    report("\n");
    if (!cond) failures++;
}

/* ---- 出力の捕獲 -------------------------------------------------------- */

#define OUT_CAP  2048

static char g_out[OUT_CAP];
static int  g_out_len;

static void out_reset(void) { g_out_len = 0; g_out[0] = '\0'; }

static void out_byte(char c)
{
    if (g_out_len + 1 < OUT_CAP) g_out[g_out_len++] = c;
    g_out[g_out_len] = '\0';
}

static void out_str(const char *s) { while (*s) out_byte(*s++); }

/* 出力に部分列が現れるか */
static int out_has(const char *want)
{
    int i, j;
    int wl = 0;
    while (want[wl]) wl++;
    if (wl == 0) return 1;
    for (i = 0; i + wl <= g_out_len; i++) {
        for (j = 0; j < wl && g_out[i + j] == want[j]; j++) {}
        if (j == wl) return 1;
    }
    return 0;
}

/* 出力に部分列が **何回** 現れるか。断りを PATH 候補の数だけ出していないか
 * (= 断ったら走査ごと止めているか) を見るのに使う。 */
static int out_count(const char *want)
{
    int i, j, n = 0;
    int wl = 0;
    while (want[wl]) wl++;
    if (wl == 0) return 0;
    for (i = 0; i + wl <= g_out_len; i++) {
        for (j = 0; j < wl && g_out[i + j] == want[j]; j++) {}
        if (j == wl) n++;
    }
    return n;
}

/* 書式を 1 つ読んで可変引数を必ず 1 つ消費する (sh_shell_host.c と同じ) */
static void fmt_run(const char **pp, __builtin_va_list *ap)
{
    const char *p = *pp;

    p++;                                   /* '%' の次へ */
    if (*p == '%') { out_byte('%'); *pp = p + 1; return; }
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') p++;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == 'l' || *p == 'h') p++;
    if (*p == 's') {
        out_str(__builtin_va_arg(*ap, const char *));
    } else if (*p) {
        (void)__builtin_va_arg(*ap, int);
        out_byte('#');
    }
    *pp = *p ? p + 1 : p;
}

int printf(const char *fmt, ...)
{
    __builtin_va_list ap;
    const char *p = fmt;

    __builtin_va_start(ap, fmt);
    while (*p) {
        if (*p == '%') fmt_run(&p, &ap);
        else out_byte(*p++);
    }
    __builtin_va_end(ap);
    return 0;
}

/* main.c が <stdio.h> から引くのはこの 3 つだけ */
int   fflush(void *stream)                               { (void)stream; return 0; }
int   setvbuf(void *stream, char *buf, int mode, unsigned long sz)
{
    (void)stream; (void)buf; (void)mode; (void)sz;
    return 0;
}
void *stdout_impl;

/* ---- ごく小さなヒープと疑似ファイル ------------------------------------ */

#define POOL_SIZE  (256 * 1024)

static char g_pool[POOL_SIZE];
static unsigned long g_pool_used;

#define FILE_MAX  8

static struct { const char *path; const char *body; } g_files[FILE_MAX];
static int g_file_count;
static int g_open_fd;
static int g_read_pos;
static int g_open_leak;

static void files_reset(void)
{
    g_file_count = 0;
    g_open_fd = 0;
    g_open_leak = 0;
    g_read_pos = 0;
    g_pool_used = 0;
}

static void file_add(const char *path, const char *body)
{
    if (g_file_count < FILE_MAX) {
        g_files[g_file_count].path = path;
        g_files[g_file_count].body = body;
        g_file_count++;
    }
}

/* ---- 差し替える KernelAPI の中身 --------------------------------------- */

static void __cdecl h_kprintf(u8 attr, const char *fmt, ...)
{
    __builtin_va_list ap;
    const char *p = fmt;
    (void)attr;
    __builtin_va_start(ap, fmt);
    while (*p) {
        if (*p == '%') fmt_run(&p, &ap);
        else out_byte(*p++);
    }
    __builtin_va_end(ap);
}

static void __cdecl h_shell_putchar(char c, u8 attr) { (void)attr; out_byte(c); }
static void __cdecl h_shell_print_utf8(const char *s, u8 attr)
{
    (void)attr;
    out_str(s);
}

/* T6: glob が 1 件ごとに確保する文字列 (dir + name + 1 = たかだか数十
 * バイト) **だけ** を N 回目で失敗させる窓。-1 = 無制限 (既定)。
 * 大きな確保 (スクリプトの読み込みバッファ、パイプの段バッファ) を
 * 区別しないと、行が glob に届く前に予算を使い切って別の理由で失敗し、
 * 「glob の確保失敗を見た」つもりの偽の緑になる。 */
#define GLOB_ALLOC_SIZE_MAX 64
static int g_glob_alloc_budget = -1;
static int g_glob_allocs;
static int g_frees;

static void *__cdecl h_mem_alloc(u32 size)
{
    char *p;
    unsigned long n = (unsigned long)size;

    if (size <= (u32)GLOB_ALLOC_SIZE_MAX) {
        if (g_glob_alloc_budget == 0) return (void *)0;
        if (g_glob_alloc_budget > 0) g_glob_alloc_budget--;
        g_glob_allocs++;
    }

    n = (n + 7UL) & ~7UL;
    if (g_pool_used + n > (unsigned long)POOL_SIZE) return (void *)0;
    p = g_pool + g_pool_used;
    g_pool_used += n;
    return (void *)p;
}

static void __cdecl h_mem_free(void *p) { (void)p; g_frees++; }

static int __cdecl h_sys_open(const char *path, int flags)
{
    int i;
    if (g_open_fd) g_open_leak = 1;
    g_read_pos = 0;
    for (i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            g_open_fd = i + 1;
            return i + 1;
        }
    }
    if (flags != KAPI_O_RDONLY) {
        file_add(path, "");
        g_open_fd = g_file_count;
        return g_file_count;
    }
    return -1;
}

/* fd 1 / 2 への書き込みは出力に残す。`echo` は kprintf ではなく
 * sys_write(1, ...) を使うので、ここを捨てると「その段が走ったか」の
 * 痕跡が取れない (パイプの段の検査が偽の GREEN になる)。
 * 本物はリダイレクト中ならファイルへ行くが、この試験が見るのは
 * 「段が実行されたか」なので宛先は区別しない (リダイレクト先を
 * 開いたかどうかは h_sys_redirect_fd の記録で別に見る)。 */
static int __cdecl h_sys_write(int fd, const void *buf, u32 size)
{
    const char *b = (const char *)buf;
    u32 i;
    if (fd == 1 || fd == 2) {
        for (i = 0; i < size; i++) out_byte(b[i]);
    }
    return (int)size;
}

static int __cdecl h_sys_read(int fd, void *buf, u32 size)
{
    const char *src;
    char *dst = (char *)buf;
    int n = 0;
    if (fd <= 0 || fd > g_file_count) return -1;
    src = g_files[fd - 1].body + g_read_pos;
    while (src[n] && (u32)n < size) { dst[n] = src[n]; n++; }
    g_read_pos += n;
    return n;
}

static void __cdecl h_sys_close(int fd) { (void)fd; g_open_fd = 0; }

static int __cdecl h_sys_stat(const char *path, OS32_Stat *st)
{
    int i;
    if (!st) return -1;
    for (i = 0; i < (int)sizeof(OS32_Stat); i++) ((u8 *)st)[i] = 0;
    for (i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            st->st_size = (u32)strlen(g_files[i].body);
            return 0;
        }
    }
    return OS32_ERR_NOTFOUND;
}

/* ---- 贋ディレクトリ (glob の T6 / T7 に要る) ---------------------------- */
/*  g_ls_calls は「**照合を試みたか**」を見る唯一の窓。T7 (パターン /         */
/*  ディレクトリ部が上限超過) は sys_ls を 1 度も呼ばないこと。               */
#define DIRENT_MAX 8

static const char *g_dir_path;
static const char *g_dir_names[DIRENT_MAX];
static int g_dir_count;
static int g_ls_calls;

static void dir_reset(void)
{
    g_dir_path = (const char *)0;
    g_dir_count = 0;
    g_ls_calls = 0;
}

static void dir_set(const char *path) { g_dir_path = path; g_dir_count = 0; }

static void dir_add(const char *name)
{
    if (g_dir_count < DIRENT_MAX) g_dir_names[g_dir_count++] = name;
}

/* sys_ls: 贋ディレクトリを 1 階層ぶんコールバックへ流す (glob 用) */
static int __cdecl h_sys_ls(const char *path, void *cb, void *ctx)
{
    DirCallback f = (DirCallback)cb;
    DirEntry_Ext e;
    int i, k;

    g_ls_calls++;
    if (!g_dir_path || !f) return 0;
    if (strcmp(g_dir_path, path) != 0) return 0;

    for (i = 0; i < g_dir_count; i++) {
        for (k = 0; k < (int)sizeof(e.name); k++) e.name[k] = '\0';
        for (k = 0; g_dir_names[i][k] && k + 1 < (int)sizeof(e.name); k++)
            e.name[k] = g_dir_names[i][k];
        e.size = 1;
        e.type = OS32_FILE_TYPE_FILE;
        f(&e, ctx);
    }
    return g_dir_count;
}

static int __cdecl h_sys_isatty(int fd) { (void)fd; return 1; }
static const char *__cdecl h_sys_getcwd(void) { return "/cwd"; }
static int __cdecl h_sys_chdir(const char *p) { (void)p; return 0; }
static int __cdecl h_sys_mkdir(const char *p) { (void)p; return 0; }
static int __cdecl h_sys_unlink(const char *p) { (void)p; return 0; }
/* リダイレクトの記録。`> file` は本物では O_TRUNC で開くので、**この呼び出し
 * が起きたこと自体**が「リダイレクト先が空で上書きされた」ことを意味する。
 * 断った段の後ろの段が走らないことを、痕跡の側から見るための窓。 */
#define REDIR_LOG_CAP 512
static char g_redir_log[REDIR_LOG_CAP];
static int  g_redir_log_len;
static int  g_redir_count;

static void redir_log_reset(void)
{
    g_redir_log_len = 0;
    g_redir_log[0] = '\0';
    g_redir_count = 0;
}

/* リダイレクト先として <want> が開かれたか */
static int redir_opened(const char *want)
{
    int i, j;
    int wl = 0;
    while (want[wl]) wl++;
    if (wl == 0) return 1;
    for (i = 0; i + wl <= g_redir_log_len; i++) {
        for (j = 0; j < wl && g_redir_log[i + j] == want[j]; j++) {}
        if (j == wl) return 1;
    }
    return 0;
}

static int __cdecl h_sys_redirect_fd(int fd, const char *p, int mode)
{
    (void)fd; (void)mode;
    g_redir_count++;
    while (p && *p && g_redir_log_len < REDIR_LOG_CAP - 2) {
        g_redir_log[g_redir_log_len++] = *p++;
    }
    if (g_redir_log_len < REDIR_LOG_CAP - 1) g_redir_log[g_redir_log_len++] = '\n';
    g_redir_log[g_redir_log_len] = '\0';
    return 0;
}
static int __cdecl h_sys_redirect_fd_buf(int fd, u8 *b, u32 cap, u32 len)
{
    (void)fd; (void)b; (void)cap; (void)len;
    return 0;
}
static u32 __cdecl h_sys_redirect_get_buf_len(int fd) { (void)fd; return 0; }
static void __cdecl h_sys_reset_redirect(int fd) { (void)fd; }
static u32 __cdecl h_get_tick(void) { return 0; }

/* 起動の記録。切り詰めた行で子が起きたかどうかを見る唯一の窓。 */
#define LAUNCH_LOG_CAP 512
static char g_launch_last[LAUNCH_LOG_CAP];
static int  g_launch_count;

static void launch_log_reset(void)
{
    g_launch_count = 0;
    g_launch_last[0] = '\0';
}

static i32 __cdecl h_launch_req(const char *cmdline)
{
    int i;
    for (i = 0; cmdline[i] && i < LAUNCH_LOG_CAP - 1; i++)
        g_launch_last[i] = cmdline[i];
    g_launch_last[i] = '\0';
    g_launch_count++;
    return OS32_ERR_INVAL;      /* GUI 外 = 「起こさなかった」 */
}
static i32 __cdecl h_launch_poll(i32 token, i32 *status)
{
    (void)token;
    if (status) *status = LAUNCH_ST_DONE;
    return 0;
}
static i32 __cdecl h_sys_yield(void) { return 0; }

/* kbd_trygetkey は script_exec の ESC 判定が毎行引く。-1 = 何も来ていない。 */
static int __cdecl h_kbd_trygetkey(void) { return -1; }
static int __cdecl h_kbd_getchar(void)   { return 0x0D; }
static int __cdecl h_kbd_trygetchar(void) { return -1; }

static int  __cdecl h_console_get_cursor_x(void)   { return 0; }
static int  __cdecl h_console_get_cursor_y(void)   { return 0; }
static void __cdecl h_console_set_cursor(int x, int y) { (void)x; (void)y; }
static void __cdecl h_tvram_clear(void) {}

static KernelAPI g_fake;
KernelAPI *g_api;

static void build_api(void)
{
    u32 i;
    u8 *raw = (u8 *)&g_fake;
    for (i = 0; i < (u32)sizeof(g_fake); i++) raw[i] = 0;
    g_fake.kprintf = h_kprintf;
    g_fake.shell_putchar = h_shell_putchar;
    g_fake.shell_print_utf8 = h_shell_print_utf8;
    g_fake.mem_alloc = h_mem_alloc;
    g_fake.mem_free = h_mem_free;
    g_fake.sys_open = h_sys_open;
    g_fake.sys_read = h_sys_read;
    g_fake.sys_write = h_sys_write;
    g_fake.sys_ls = h_sys_ls;
    g_fake.sys_stat = h_sys_stat;
    g_fake.sys_close = h_sys_close;
    g_fake.sys_isatty = h_sys_isatty;
    g_fake.sys_getcwd = h_sys_getcwd;
    g_fake.sys_chdir = h_sys_chdir;
    g_fake.sys_mkdir = h_sys_mkdir;
    g_fake.sys_unlink = h_sys_unlink;
    g_fake.sys_redirect_fd = h_sys_redirect_fd;
    g_fake.sys_redirect_fd_buf = h_sys_redirect_fd_buf;
    g_fake.sys_redirect_get_buf_len = h_sys_redirect_get_buf_len;
    g_fake.sys_reset_redirect = h_sys_reset_redirect;
    g_fake.get_tick = h_get_tick;
    g_fake.launch_req = h_launch_req;
    g_fake.launch_poll = h_launch_poll;
    g_fake.sys_yield = h_sys_yield;
    g_fake.kbd_trygetkey = h_kbd_trygetkey;
    g_fake.kbd_getchar = h_kbd_getchar;
    g_fake.kbd_trygetchar = h_kbd_trygetchar;
    g_fake.console_get_cursor_x = h_console_get_cursor_x;
    g_fake.console_get_cursor_y = h_console_get_cursor_y;
    g_fake.console_set_cursor = h_console_set_cursor;
    g_fake.tvram_clear = h_tvram_clear;
    g_api = &g_fake;
}

/* ---- ui.c の代わり (sh_redraw.inc が引くもの) -------------------------- */

static int prev_draw_len = 0;

static void show_prompt(void) { out_str("sh> "); }

#include "../../userland/shell/sh_redraw.inc"

/* ui.c の公開分。行編集は段 4 の担当なのでここでは呼ばない。 */
void shell_run(void) {}
void hist_save(void) {}
void hist_load(void) {}

/* sdk/crt/help.c の代わり (man ページは読まない) */
int os32_help_show(const char *name)   { (void)name; return -1; }
int os32_help_exists(const char *name) { (void)name; return 0; }

/* sh_redraw.inc のうちこの試験が直接は呼ばないもの (行編集は段 4)。
 * -Wall の unused-function を黙らせるためだけに参照を 1 本持つ —
 * 実物をそのまま取り込んでいることの証でもある。 */
void (*const sh_redraw_keep[])() = {
    (void (*)())redraw_line,
    (void (*)())sh_backspace_tail
};

/* cmd_filer.c は filer_draw.c (GFX) を丸ごと引くので取り込まない。
 * SHELL_AS_APP では sh_is_cui_only が `filer` を先に断つので、
 * 切り詰めの経路としては段 4 で別に見る (§2-3 の表に載せてある)。 */
void shell_cmd_filer_init(void) {}

/* ---- 実物のシェル ------------------------------------------------------ */

#include "../../userland/shell/main.c"
#include "../../userland/shell/cmd_base.c"
#include "../../userland/shell/cmd_dir.c"
#include "../../userland/shell/cmd_env.c"
#include "../../userland/shell/cmd_fs_shared.c"
#include "../../userland/shell/cmd_file.c"
#include "../../userland/shell/cmd_mnt.c"
#include "../../userland/shell/cmd_script.c"
#include "../../userland/shell/cmd_sys.c"

/* main.c の main() と同じ順で登録表を作る (表そのものは実物) */
static void sh_boot(void)
{
    env_init();
    shell_cmd_base_init();
    shell_cmd_file_init();
    shell_cmd_dir_init();
    shell_cmd_mnt_init();
    shell_cmd_sys_init();
    shell_cmd_env_init();
    shell_cmd_script_init();
    shell_cmd_filer_init();
}

/* ---- 試験の道具 -------------------------------------------------------- */

/* 長さ n の `a` の並びを作り、最後の 1 文字だけ tail に差し替える。
 * 先頭 255 文字が同じで 256 文字目が違う 2 本を作るのに使う。 */
static void fill_run(char *dst, int n, char tail)
{
    int i;
    for (i = 0; i < n; i++) dst[i] = 'a';
    if (n > 0) dst[n - 1] = tail;
    dst[n] = '\0';
}

static char g_line[CMD_BUF_SIZE];

static void line_reset(void) { g_line[0] = '\0'; }

static void line_add(const char *s)
{
    int n = 0;
    while (g_line[n]) n++;
    while (*s && n < (int)sizeof(g_line) - 1) g_line[n++] = *s++;
    g_line[n] = '\0';
}

/* スクリプト本文の組み立て (贋 FS は本文のポインタを持つだけなので静的領域) */
#define SBUF_COUNT  3
#define SBUF_SIZE   1024

static char g_sbuf[SBUF_COUNT][SBUF_SIZE];
static int  g_sn[SBUF_COUNT];
static int  g_scur;

static void s_begin(int i) { g_scur = i; g_sn[i] = 0; g_sbuf[i][0] = '\0'; }

static void s_add(const char *t)
{
    int n = g_sn[g_scur];
    while (*t && n < SBUF_SIZE - 1) g_sbuf[g_scur][n++] = *t++;
    g_sbuf[g_scur][n] = '\0';
    g_sn[g_scur] = n;
}

/* `a` を count 個 */
static void s_run(int count)
{
    int n = g_sn[g_scur];
    int i;
    for (i = 0; i < count && n < SBUF_SIZE - 1; i++) g_sbuf[g_scur][n++] = 'a';
    g_sbuf[g_scur][n] = '\0';
    g_sn[g_scur] = n;
}

static const char *s_body(int i) { return g_sbuf[i]; }

/* 未知のコマンド名 <name> が実際に解決されにいったか。
 * run_cmd_internal は最後に "<name>: command not found" を出す。 */
static int ran(const char *name)
{
    char needle[64];
    int i = 0;
    const char *t = ": command not found";
    int j = 0;

    while (name[i] && i < 40) { needle[i] = name[i]; i++; }
    while (t[j]) needle[i++] = t[j++];
    needle[i] = '\0';
    return out_has(needle);
}

/* 断りの 1 行が出ているか (何が上限を超えたか + 上限) */
static int refused_msg(const char *what)
{
    return out_has(what) && out_has("too long (max ");
}

static void fresh(void)
{
    files_reset();
    out_reset();
    launch_log_reset();
    redir_log_reset();
    dir_reset();
    g_glob_alloc_budget = -1;
    g_glob_allocs = 0;
    g_frees = 0;
}

/* 同じ文字列を n 回続けて足す (クォート再付与ぶんの反例を組むのに使う) */
static void line_add_rep(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) line_add(s);
}

/* 1 文字を n 個足す */
static void line_add_run(char c, int n)
{
    char one[2];
    int i;
    one[0] = c;
    one[1] = '\0';
    for (i = 0; i < n; i++) line_add(one);
}

/* T13 用: CMD_BUF_SIZE を **超える** 行。g_line (= CMD_BUF_SIZE) には
 * 4095 バイトまでしか入らないので別に持つ。 */
static char g_big[CMD_BUF_SIZE + 64];

/* prefix + 'a' * pad (合計の長さを返す) */
static int big_line(const char *prefix, int pad)
{
    int n = 0;
    int i;

    while (prefix[n] && n < (int)sizeof(g_big) - 1) { g_big[n] = prefix[n]; n++; }
    for (i = 0; i < pad && n < (int)sizeof(g_big) - 1; i++) g_big[n++] = 'a';
    g_big[n] = '\0';
    return n;
}

/* ========================================================================
 *  1. U1 — `if` の両辺がクォート除去後 256 文字以上で先頭 255 文字が同じ
 *
 *  段 1 はここを「今は条件が真になって右辺が走る」と記録していた
 *  ([EXPECTED_TO_CHANGE])。段 2 で票 §4 U1 の形へ反転させた:
 *    - コマンドを実行しない (`==` も `!=` も)
 *    - 上限超過を報告する
 *    - スクリプト中なら後続行も実行しない (§2-1)
 * ======================================================================== */
static void case_if_compare_refuses(void)
{
    char a[600], b[600];

    report("1 U1: if の両辺が 256 文字以上・先頭 255 文字が同じ\n");

    /* 256 文字。先頭 255 文字は同じで 256 文字目だけ違う */
    fill_run(a, 256, 'x');
    fill_run(b, 256, 'y');
    check(strcmp(a, b) != 0, "1a 反例そのものは別の文字列 (256 文字目が違う)");
    check(strncmp(a, b, 255) == 0, "1b 先頭 255 文字は同じ");

    /* `==` — 切り詰めて比べれば真になる反例。断って実行しない。 */
    fresh();
    line_reset();
    line_add("if "); line_add(a); line_add(" == "); line_add(b);
    line_add(" markif");
    execute_command(g_line);
    check(!ran("markif") && g_launch_count == 0,
          "1c == : 右辺のコマンドを実行しない");
    check(refused_msg("if: left value"),
          "1d == : 何が上限を超えたか + 上限を 1 行で報告する");

    /* `!=` — 走らない点は前と同じだが、理由がメッセージで分かること */
    fresh();
    line_reset();
    line_add("if "); line_add(a); line_add(" != "); line_add(b);
    line_add(" markif");
    execute_command(g_line);
    check(!ran("markif") && g_launch_count == 0,
          "1e != : 右辺のコマンドを実行しない");
    check(refused_msg("if: left value"), "1f != : 同じく報告する");

    /* 右辺だけが溢れる場合は「右辺」と言う */
    fresh();
    line_reset();
    line_add("if short == "); line_add(b); line_add(" markif");
    execute_command(g_line);
    check(!ran("markif"), "1g 右辺だけ溢れても実行しない");
    check(refused_msg("if: right value"), "1h 溢れた側を名指しする");

    /* スクリプト中: 後続行も実行しない (§2-1 の反例そのもの) */
    fresh();
    s_begin(0);
    s_add("set A=");  s_run(240);  s_add("\n");
    s_add("if ${A}${A}x == ${A}${A}y markif\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!ran("markif"), "1i スクリプト: if の右辺を実行しない");
    check(!ran("marknext"), "1j スクリプト: 後続行も実行しない (§2-1)");
    check(out_has("script: aborted"), "1k スクリプト: 打ち切ったことを言う");
}

/* ========================================================================
 *  2. 足場が本物であることの確認 (段 1 から据え置き)
 * ======================================================================== */
static void case_registry_is_real(void)
{
    int count;
    const ShellCmd *cmds;
    int i, seen_if = 0, seen_set = 0, seen_source = 0, seen_time = 0;

    report("2 登録表と execute_command は実物 (スタブではない)\n");

    cmds = shell_get_cmds(&count);
    for (i = 0; i < count; i++) {
        if (str_eq(cmds[i].name, "if"))     seen_if = 1;
        if (str_eq(cmds[i].name, "set"))    seen_set = 1;
        if (str_eq(cmds[i].name, "source")) seen_source = 1;
        if (str_eq(cmds[i].name, "time"))   seen_time = 1;
    }
    check(count > 20,  "2a 登録表に実物の件数が入っている");
    check(seen_if,     "2b if が実物の表にある");
    check(seen_set,    "2c set が実物の表にある");
    check(seen_source, "2d source が実物の表にある");
    check(seen_time,   "2e time が実物の表にある");

    fresh();
    execute_command("if abc == abd markif");
    check(!ran("markif") && !out_has("too long"),
          "2f 255 文字以下なら `==` は正しく偽になる (断りも出さない)");

    fresh();
    execute_command("if abc == abc markif");
    check(ran("markif"), "2g 255 文字以下で一致すれば右辺が走る");
}

/* ========================================================================
 *  3. U2 — 境界。255 文字ちょうどは通る / 256 文字は断る。
 *     長さは**クォート除去後**で数える (sh_args.inc が既に落としている)。
 * ======================================================================== */
static void case_boundary(void)
{
    char v255[600], v256[600], w255[600];

    report("3 U2: 255 は通る / 256 は断る (クォート除去後で数える)\n");

    fill_run(v255, 255, 'z');
    fill_run(w255, 255, 'z');       /* v255 と同じ内容 */
    fill_run(v256, 256, 'z');

    fresh();
    line_reset();
    line_add("if "); line_add(v255); line_add(" == "); line_add(w255);
    line_add(" markif");
    execute_command(g_line);
    check(ran("markif"), "3a 255 文字ちょうど同士は比較でき、真なら走る");
    check(!out_has("too long"), "3b 255 文字では断らない");

    fresh();
    line_reset();
    line_add("if "); line_add(v255); line_add(" != "); line_add(w255);
    line_add(" markif");
    execute_command(g_line);
    check(!ran("markif") && !out_has("too long"),
          "3c 255 文字ちょうどの != は正しく偽 (断りではない)");

    fresh();
    line_reset();
    line_add("if "); line_add(v256); line_add(" == "); line_add(v256);
    line_add(" markif");
    execute_command(g_line);
    check(!ran("markif") && refused_msg("if: left value"),
          "3d 256 文字は**中身が同じでも**断る");

    /* クォート付き: 生では 257 バイトだが、除去後は 255 なので通る。
     * 長さを「除去前」で数えると、ここが誤って断られる。 */
    fresh();
    line_reset();
    line_add("if \""); line_add(v255); line_add("\" == \""); line_add(w255);
    line_add("\" markif");
    execute_command(g_line);
    check(ran("markif"), "3e クォート込み 257 バイトでも除去後 255 なら通る");
    check(!out_has("too long"), "3f クォートの分を数に入れない");

    /* 除去後 256: クォート込み 258 バイト */
    fresh();
    line_reset();
    line_add("if \""); line_add(v256); line_add("\" == \""); line_add(v256);
    line_add("\" markif");
    execute_command(g_line);
    check(!ran("markif") && out_has("too long"),
          "3g 除去後 256 なら (クォートが付いていても) 断る");
}

/* ========================================================================
 *  4. 経路 1 — `if` / `time` 経由の入れ子 execute_command
 *
 *  どちらも組み立てた行を execute_command へ渡す。内側で断ったことが
 *  外の script_exec まで届くこと (取りこぼしが無いこと) を見る。
 * ======================================================================== */
static void case_nested_execute_command(void)
{
    report("4 経路 1: if / time 経由の入れ子 execute_command\n");

    /* if の右辺がさらに if。内側の if が断る。
     * 片側だけ 300 文字にして、外側の join_args (4096) には収める。 */
    fresh();
    s_begin(0);
    s_add("set B=");  s_run(150);  s_add("\n");
    s_add("if a == a if ${B}${B} == b markinner\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!ran("markinner"), "4a if 入れ子: 内側の右辺を実行しない");
    check(refused_msg("if: left value"), "4b if 入れ子: 断りが出る");
    check(!ran("marknext"), "4c if 入れ子: 外のスクリプトも打ち切る");

    /* time が組み立てた行。cmd_buf は 510 なので 300 文字なら溢れない。 */
    fresh();
    s_begin(0);
    s_add("set B=");  s_run(150);  s_add("\n");
    s_add("time if ${B}${B} == b markinner\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!ran("markinner"), "4d time 経由: 内側の右辺を実行しない");
    check(refused_msg("if: left value"), "4e time 経由: 断りが出る");
    check(!ran("marknext"), "4f time 経由: 外のスクリプトも打ち切る");

    /* 誤発火の裏: 入れ子が**通った**ら印は立たない */
    fresh();
    s_begin(0);
    s_add("if a == a time mk1\n");
    s_add("mk2\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(ran("mk1"), "4g 通る入れ子は今までどおり走る");
    check(ran("mk2"), "4h 通る入れ子の後も打ち切らない (誤発火なし)");
    check(sh_refused_flag == 0, "4i 通った行は印を残さない");

    /* 入口の掃除は「いちばん外側だけ」を直に見る。
     *
     * 段 2 のときは「断った段の後ろの段が time なら取りこぼす」(6g) が
     * この規則の唯一の証人だったが、PM 決裁で断った後の段を走らせなく
     * なったので、その経路からは見えなくなった。規則自体は shell.h の
     * 契約なので、入れ子の深さを直に作って押さえる
     * (`if` / `time` が execute_command を呼ぶときと同じ状態)。 */
    fresh();
    g_exec_depth++;                 /* if / time の中にいるときと同じ */
    sh_refuse_mark();
    execute_command("mk1");
    g_exec_depth--;
    check(ran("mk1"), "4j 入れ子の execute_command は今までどおり走る");
    check(sh_refused_flag == 1,
          "4k 入れ子の入口では印を消さない (内側の断りを外へ届ける)");

    /* いちばん外側 (深さ 0) の入口では消す */
    execute_command("mk2");
    check(sh_refused_flag == 0,
          "4l いちばん外側の入口では印を消す");
}

/* ========================================================================
 *  5. 経路 2 — 入れ子の source (script_source_file の戻り値で親に伝える)
 * ======================================================================== */
static void case_nested_source(void)
{
    report("5 経路 2: 入れ子の source\n");

    fresh();
    s_begin(1);                      /* 子 */
    s_add("set A=");  s_run(240);  s_add("\n");
    s_add("if ${A}${A}x == ${A}${A}y markif\n");
    s_add("markchild\n");
    s_begin(0);                      /* 親 */
    s_add("source /child.sh\n");
    s_add("markparent\n");
    file_add("/parent.sh", s_body(0));
    file_add("/child.sh", s_body(1));
    execute_command("source /parent.sh");
    check(!ran("markif"),     "5a 子: if の右辺を実行しない");
    check(!ran("markchild"),  "5b 子: 後続行も実行しない");
    check(!ran("markparent"), "5c 親: 子が断ったら親も打ち切る (戻り値で伝播)");

    /* 直接 script_source_file を呼んだときの戻り値 */
    fresh();
    s_begin(1);
    s_add("set A=");  s_run(240);  s_add("\n");
    s_add("if ${A}${A}x == ${A}${A}y markif\n");
    file_add("/child.sh", s_body(1));
    check(script_source_file("/child.sh") == SCRIPT_ERR_REFUSED,
          "5d script_source_file は断りを SCRIPT_ERR_REFUSED で返す");

    /* 誤発火の裏: 通る子 source は親を止めない */
    fresh();
    s_begin(1);
    s_add("mk1\n");
    s_begin(0);
    s_add("source /child.sh\n");
    s_add("markparent\n");
    file_add("/parent.sh", s_body(0));
    file_add("/child.sh", s_body(1));
    execute_command("source /parent.sh");
    check(ran("mk1") && ran("markparent"),
          "5e 通る子の後も親は続く (誤発火なし)");
    check(script_source_file("/child.sh") == 0, "5f 通った source は 0 を返す");
}

/* ========================================================================
 *  6. 経路 3 — パイプの段
 *
 *  段は execute_single を直に呼ぶので、段の中で断った印は execute_command
 *  の入口の掃除に消されない。
 *
 *  段 2 の時点では「行を打ち切るだけで、段の続行は bash の `false | cat`
 *  どおり」にしていた。**PM 決裁 (独立レビューの指摘を受けて) で票
 *  §2の「行全体を実行しない」に合わせた** — 断った段があったら
 *  **後続の段も実行しない**。理由は `<断られる段> | tee 重要ファイル` —
 *  続けると断ったのに書き込みが起き、`> file` は O_TRUNC なので
 *  リダイレクト先が空で上書きされ得る。
 * ======================================================================== */
static void case_pipe_stage(void)
{
    char a[600], b[600];

    report("6 経路 3: パイプの段\n");

    /* 先頭 255 文字が同じ 256 文字の 2 本 (1c と同じ反例) */
    fill_run(a, 256, 'x');
    fill_run(b, 256, 'y');

    fresh();
    s_begin(0);
    s_add("set B=");  s_run(150);  s_add("\n");
    s_add("if ${B}${B} == b markpipe | echo tail\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!ran("markpipe"), "6a 段の中の if は右辺を実行しない");
    check(refused_msg("if: left value"), "6b 段の中の断りも 1 行出る");
    check(!ran("marknext"), "6c 段で断ったら後続行を実行しない (取りこぼしなし)");

    /* 取りこぼしの罠: 断った段の**後ろ**の段が execute_command を入れ子で
     * 呼ぶ (`time ...`)。入口の掃除を入れ子でも走らせると、ここで印が
     * 消えて後続行へ落ちる。 */
    fresh();
    s_begin(0);
    s_add("set B=");  s_run(150);  s_add("\n");
    s_add("if ${B}${B} == b markpipe | time echo tail\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!ran("markpipe"), "6f 後段が time でも右辺を実行しない");
    check(!ran("marknext"),
          "6g 後段の入れ子に印を消させない (取りこぼしなし)");

    /* 誤発火の裏: 通るパイプは今までどおり */
    fresh();
    s_begin(0);
    s_add("echo hello | echo tail\n");
    s_add("mk2\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(ran("mk2"), "6d 通るパイプの後も打ち切らない");
    check(sh_refused_flag == 0, "6e 通るパイプは印を残さない");

    /* ---- PM 決裁: 断った段があったら後続の段を実行しない ----------- */

    /* 6h: 後続の段が走ったかは、その段が書く痕跡 (echo の出力) で見る。
     * SHELL_AS_APP では外部段を含むパイプが丸ごと断られるので、
     * 段は全部内蔵コマンドにする。 */
    fresh();
    line_reset();
    line_add("if "); line_add(a); line_add(" == "); line_add(b);
    line_add(" markpipe | echo STAGE2RAN");
    execute_command(g_line);
    check(refused_msg("if: left value"), "6h 段 1 で断る");
    check(!ran("markpipe"),      "6i 断った段の右辺は実行しない");
    check(!out_has("STAGE2RAN"),
          "6j 断った段の**後続の段**を実行しない");

    /* 6k: 後続の段の `> file` は O_TRUNC。走らせると断ったのに
     * リダイレクト先が空で上書きされる (この決裁の根拠)。 */
    fresh();
    file_add("/keep.txt", "IMPORTANT");
    line_reset();
    line_add("if "); line_add(a); line_add(" == "); line_add(b);
    line_add(" markpipe | echo tail > /keep.txt");
    execute_command(g_line);
    check(!redir_opened("/keep.txt"),
          "6k 後続の段のリダイレクト先を開かない (空で上書きしない)");
    check(g_redir_count == 0, "6l 断った行はリダイレクトを 1 つも張らない");

    /* 6m: 3 段の真ん中で断ったときも 3 段目は走らない。
     * 1 段目 (STAGE1RAN) は断る前なので今までどおり走る。 */
    fresh();
    line_reset();
    line_add("echo STAGE1RAN | if ");
    line_add(a); line_add(" == "); line_add(b);
    line_add(" markpipe | echo STAGE3RAN");
    execute_command(g_line);
    check(out_has("STAGE1RAN"), "6m 断る前の段は走る");
    check(!out_has("STAGE3RAN"), "6n 断った段の後ろの段は走らない (3 段)");

    /* 誤発火の裏 1: 断っていないパイプは今までどおり全段走る */
    fresh();
    execute_command("echo ONE | echo TWO | echo THREE");
    check(out_has("ONE") && out_has("TWO") && out_has("THREE"),
          "6o 誤発火なし: 通る 3 段は全段走る");
    check(sh_refused_flag == 0, "6p 通る 3 段は印を残さない");

    /* 誤発火の裏 2: 通るパイプの最終段の `> file` は今までどおり張る */
    fresh();
    file_add("/keep.txt", "IMPORTANT");
    execute_command("echo head | echo tail > /keep.txt");
    check(redir_opened("/keep.txt"),
          "6q 誤発火なし: 通るパイプのリダイレクトは今までどおり張る");

    /* 誤発火の裏 3: `if` が**偽**で右辺を走らせないだけのときは
     * 断りではないので、後続の段は今までどおり走る。 */
    fresh();
    execute_command("if abc == abd markpipe | echo STAGE2RAN");
    check(!ran("markpipe"), "6r 偽の if は右辺を走らせない");
    check(out_has("STAGE2RAN"),
          "6s 誤発火なし: 偽の if は断りではないので後続の段は走る");
}

/* ========================================================================
 *  7. 経路 4 / 5 — rshell と対話 (スクリプトではないので打ち切らない)
 *
 *  rshell.c は serial を握るのでこの試験には取り込んでいない。rshell が
 *  やっているのは「1 行ずつ execute_command を呼ぶ」だけ (rshell.c:149)
 *  なので、ここでは同じ呼び方を並べて「次の行が動くこと」を押さえる。
 *  EOT (票 §2-2) は段 4 の担当。
 * ======================================================================== */
static void case_interactive_not_aborted(void)
{
    char a[600], b[600];

    report("7 経路 4/5: rshell / 対話では打ち切らない\n");

    fill_run(a, 256, 'x');
    fill_run(b, 256, 'y');

    fresh();
    execute_command("mk1");
    line_reset();
    line_add("if "); line_add(a); line_add(" == "); line_add(b);
    line_add(" markif");
    execute_command(g_line);               /* ← 断られる行 */
    execute_command("mk2");                /* ← 次の行 */
    check(ran("mk1"), "7a 断る前の行は走る");
    check(!ran("markif"), "7b 断った行は走らない");
    check(ran("mk2"), "7c 断った**次の**行は今までどおり走る");
    check(sh_refused_flag == 0, "7d 次の行の入口で印が消えている");

    /* 対話で断った印が、その後のスクリプトを巻き添えにしないこと */
    fresh();
    line_reset();
    line_add("if "); line_add(a); line_add(" == "); line_add(b);
    line_add(" markif");
    execute_command(g_line);               /* 印が立ったまま誰も読まない */
    s_begin(0);
    s_add("mk1\n");
    s_add("mk2\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(ran("mk1") && ran("mk2"),
          "7e 直前に対話で断っても、次のスクリプトは最後まで走る");
}

/* ========================================================================
 *  8. 経路 6 — 起動時の /etc/profile
 *
 *  断ったらメッセージを出して**既定値で続行する** (起動を止めない)。
 *  ui.c の shell_run() は端末を握るので取り込めない。shell_run が呼ぶ
 *  script_source_profile() を実物のまま通す。
 * ======================================================================== */
static void case_profile_continues(void)
{
    report("8 経路 6: /etc/profile は断っても起動を止めない\n");

    fresh();
    s_begin(0);
    s_add("set A=");  s_run(240);  s_add("\n");
    s_add("if ${A}${A}x == ${A}${A}y markif\n");
    s_add("markprofile\n");
    file_add("/etc/profile", s_body(0));

    script_source_profile("/etc/profile");
    check(!ran("markif"),      "8a profile: 断った行は実行しない");
    check(!ran("markprofile"), "8b profile: その後の行も実行しない");
    check(out_has("continuing with defaults"),
          "8c profile: 既定値で続けると言う");
    check(sh_refused_flag == 0,
          "8d profile: 印を残さない (次の行を巻き添えにしない)");

    /* 起動は続く — profile の後の行が動く */
    execute_command("mk1");
    check(ran("mk1"), "8e profile の後のコマンドは動く (起動を止めない)");

    /* 誤発火の裏: 通る profile は何も言わない */
    fresh();
    s_begin(0);
    s_add("mk2\n");
    file_add("/etc/profile", s_body(0));
    script_source_profile("/etc/profile");
    check(ran("mk2"), "8f 通る profile は今までどおり走る");
    check(!out_has("continuing with defaults"), "8g 通ったら黙っている");
}

/* ========================================================================
 *  9. 誤発火の総まとめ — 断る理由が無いスクリプトは最後まで走る
 * ======================================================================== */
static void case_no_false_abort(void)
{
    report("9 誤発火なし: 断る理由が無ければ最後まで走る\n");

    fresh();
    s_begin(0);
    s_add("mk1\n");
    s_add("if abc == abc mk2\n");
    s_add("if abc == abd mk3\n");        /* 偽 — 断りではない */
    s_add("mk4\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(ran("mk1"), "9a 1 行目が走る");
    check(ran("mk2"), "9b 真の if が走る");
    check(!ran("mk3"), "9c 偽の if は走らない (が打ち切りではない)");
    check(ran("mk4"), "9d 偽の if の後も走り続ける");
    check(!out_has("script: aborted"), "9e 打ち切りの報告を出さない");
}

/* ========================================================================
 *  10. U4 — try_exec の再構築が溢れたら **子を起こさない** (T3)
 *
 *  「子が起きたか」の窓は g_launch_count (= launch_req を呼んだ回数)。
 *  段 2b の報告どおり、この窓が無いと「断りの行が出た」だけで偽の緑になる。
 *  h_launch_req は OS32_ERR_INVAL を返す (= GUI 外 = 起こさなかった) が、
 *  **呼ばれたこと自体**が「切り詰めた行で起動しにいった」証拠になる。
 *
 *  数え方はクォートの再付与ぶんを含める (票 §1 T3):
 *    区切りの空白 1 + 本体 (" と \ は 2 倍) + クォートが要るなら +2
 * ======================================================================== */
static void case_try_exec_refuses(void)
{
    report("10 U4: try_exec の再構築が溢れたら子を起こさない (T3)\n");

    /* 名前に `/` を入れて 2a (直接実行) の枝へ落とす — PATH 走査に入ると
     * 候補ごとに接頭辞のぶん長さが変わり、境界の検査が読めなくなる。
     * 以下 bin は "./mk1.bin" = 9 バイト。
     *
     * 境界の **裏側** (ちょうど 510) は、sh.bin では try_exec を通り抜けても
     * 要求表の上限 (LAUNCH_CMDLINE_MAX - 1 = 255、T11) に当たって断られる。
     * したがって裏側の証人は「launch_req を呼んだか」ではなく
     * 「**この層** (sh: argument list) が断っていないこと」— 層が別なのは
     * 断りの文言で見分ける。子が起きないことは両方で launch_count が示す。 */

    /* --- (a) 素の長さで溢れる場合 ------------------------------------- */
    /* 9 + 1 + 501 = 511 > 510 */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_run('a', 501);
    execute_command(g_line);
    check(g_launch_count == 0, "10a 511 バイト: 子を起こさない");
    check(refused_msg("sh: argument list"), "10b 何が溢れたか + 上限を出す");

    /* 誤発火の裏: ちょうど 510 ではこの層は断らない (境界は「以上」で数える) */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_run('a', 500);
    execute_command(g_line);
    check(!out_has("sh: argument list"), "10c 510 ちょうどはこの層で断らない");
    check(out_has("sh: launch command line"),
          "10d 510 は次の層 (要求表 255) が断る — 層が違うことを文言で見る");

    /* --- (b) クォートの再付与で伸びる場合 ------------------------------ */
    /*  引数 1 つが `"` 250 個。素直に数えると 9 + 1 + 250 = 260 で余裕だが、
     *  try_exec は `"` を \" にして前後をクォートで包むので
     *  9 + 1 + 2 + 250*2 = 512 > 510。**エスケープを数えない実装はここを
     *  通してしまい、引数が途中で切れたまま子が起きる**。 */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_rep("\\\"", 250);
    execute_command(g_line);
    check(g_launch_count == 0, "10e \" の 2 倍を数える: 子を起こさない");
    check(refused_msg("sh: argument list"), "10f 同上: 断りが出る");

    /* 境界の裏: 249 個なら 9 + 1 + 2 + 498 = 510 でちょうど通る */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_rep("\\\"", 249);
    execute_command(g_line);
    check(!out_has("sh: argument list"), "10g \" 249 個 (= 510) はこの層を通る");

    /* --- (c) 空白入りの引数は前後の `"` で +2 -------------------------- */
    /*  84 個の `a b` = 84 * (1 + 3 + 2) = 504、+ 9 = 513 > 510。
     *  クォートを数えないと 9 + 84*4 = 345 で通ってしまう。 */
    fresh();
    line_reset();
    line_add("./mk1.bin");
    line_add_rep(" \"a b\"", 84);
    execute_command(g_line);
    check(g_launch_count == 0, "10i 空白入り引数の +2 を数える: 子を起こさない");
    check(refused_msg("sh: argument list"), "10j 同上: 断りが出る");

    fresh();
    line_reset();
    line_add("./mk1.bin");
    line_add_rep(" \"a b\"", 83);       /* 9 + 498 = 507 */
    execute_command(g_line);
    check(!out_has("sh: argument list"), "10k 83 個 (= 507) はこの層を通る");

    /* --- (d) スクリプト中なら打ち切る --------------------------------- */
    /*  スクリプトの 1 行は 255 バイトまで (T2、段 4 の担当) なので、
     *  511 バイトの行は変数展開で作る。${A} は 247 バイト。
     *  9 + 3 * (1 + 247) = 753 > 510 */
    fresh();
    s_begin(0);
    s_add("set A=");  s_run(247);  s_add("\n");
    s_add("./mk1.bin ${A} ${A} ${A}\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(g_launch_count == 0, "10l スクリプト: 子を起こさない");
    check(refused_msg("sh: argument list"), "10m スクリプト: 断りが出る");
    check(!ran("marknext"),    "10n スクリプト: 後続行も実行しない");
}

/* ========================================================================
 *  11. U4 — 内蔵 `exec` (255) と `time` (510) も起動前に断る (T3)
 * ======================================================================== */
static void case_exec_time_refuse(void)
{
    report("11 U4: 内蔵 exec (255) と time (510)\n");

    /* exec: argv[1..] を空白で繋いだ長さが 256 以上なら断る。
     * "mk1.bin" (7) + " " + 'a'*248 = 256 */
    fresh();
    line_reset();
    line_add("exec mk1.bin ");
    line_add_run('a', 248);
    execute_command(g_line);
    check(g_launch_count == 0, "11a exec 256 バイト: 子を起こさない");
    check(refused_msg("exec: command line"), "11b exec: 断りが出る");

    /* 誤発火の裏: 255 ちょうどは通る */
    fresh();
    line_reset();
    line_add("exec mk1.bin ");
    line_add_run('a', 247);
    execute_command(g_line);
    check(g_launch_count == 1, "11c exec 255 ちょうどは launch_req まで行く");
    check(!out_has("too long"), "11d exec 255 では断らない");

    /* time: 組み立てる行が 511 以上なら断る。
     * "./mk1.bin" (9) + " " + 'a'*501 = 511 */
    fresh();
    line_reset();
    line_add("time ./mk1.bin ");
    line_add_run('a', 501);
    execute_command(g_line);
    check(g_launch_count == 0, "11e time 511 バイト: 内側を実行しない");
    check(refused_msg("time: command line"), "11f time: 断りが出る");
    check(!out_has("real  "), "11g time: 計測結果も出さない");
    check(!out_has("sh: argument list"),
          "11h time: 内側の try_exec まで行かせない (断るのは time の層)");

    /* 誤発火の裏: 510 ちょうどは time を通り抜けて内側へ渡る。
     * (内側は要求表の 255 に当たるので子は起きない — 層が違う) */
    fresh();
    line_reset();
    line_add("time ./mk1.bin ");
    line_add_run('a', 500);
    execute_command(g_line);
    check(!out_has("time: command line"), "11i time 510 では断らない");
    check(out_has("sh: launch command line"),
          "11j time 510 は内側へ渡る (次の層が断る)");
}

/* ========================================================================
 *  12. U5 — 251 バイトで切った名前に .bin を付けて **別のファイル** を
 *      起動しない (T4)
 *
 *  窓は g_launch_last: 切り詰める実装はここに「先頭 251 文字 + .bin」が
 *  入る (= 実在する別のファイル)。直った実装は launch_req を呼ばない。
 * ======================================================================== */
static void case_cmd_name_refuses(void)
{
    report("12 U5: 251 で切った名前で別のファイルを起動しない (T4)\n");

    fresh();
    line_reset();
    line_add_run('P', 252);              /* PATH_MAX_LEN - 5 = 251 を 1 超える */
    execute_command(g_line);
    check(g_launch_count == 0, "12a 252 文字の名前: 子を起こさない");
    check(refused_msg("sh: command name"), "12b 断りが出る");
    check(!out_has("command not found"),
          "12c 断った行に \"command not found\" を足さない");

    /* 誤発火の裏: 251 ちょうどは今までどおり解決を試みる */
    fresh();
    line_reset();
    line_add_run('P', 251);
    execute_command(g_line);
    check(g_launch_count >= 1, "12d 251 ちょうどは起動しにいく");
    check(!out_has("sh: command name"), "12e 251 では名前の断りを出さない");
}

/* ========================================================================
 *  13. U6 — パイプの段を捨てない (T5)
 *
 *  窓は段が書く痕跡 (echo の出力)。以前は 9 段目と空の段を **黙って捨てて**
 *  いたので、`echo ok |` が 1 段として実行されていた。
 *
 *  クォートは今までどおり見ない。したがって `echo "a||b"` の中の `|` も
 *  区切りのままで、空の段として断られる (票 §6 で範囲外と決めた分割規則)。
 * ======================================================================== */
static void case_pipeline_stages(void)
{
    report("13 U6: 9 段 / 空の段 / a || b を捨てずに断る (T5)\n");

    /* 9 段 — 1 段目も実行しない (行全体を断る) */
    fresh();
    execute_command("echo S1|echo S2|echo S3|echo S4|echo S5|echo S6|echo S7|"
                    "echo S8|echo S9");
    check(refused_msg("sh: pipeline"), "13a 9 段: 上限つきで断る");
    check(!out_has("S1") && !out_has("S8") && !out_has("S9"),
          "13b 9 段: どの段も実行しない");

    /* 誤発火の裏: 8 段ちょうどは全段走る */
    fresh();
    execute_command("echo S1|echo S2|echo S3|echo S4|echo S5|echo S6|echo S7|"
                    "echo S8");
    check(out_has("S1") && out_has("S8"), "13c 8 段ちょうどは全段走る");
    check(!out_has("too long"), "13d 8 段では断らない");

    /* 末尾の空の段 — 以前は 1 段として **実行されていた** */
    fresh();
    execute_command("echo PIPEOK |");
    check(out_has("empty pipeline stage"), "13e `cmd |`: 空の段を断る");
    check(!out_has("PIPEOK"), "13f `cmd |`: 前の段も実行しない");
    check(sh_refused_flag == 1, "13g 断りの印が立っている");

    /* 先頭の空の段 */
    fresh();
    execute_command("| echo PIPEOK");
    check(out_has("empty pipeline stage"), "13h `| cmd`: 空の段を断る");
    check(!out_has("PIPEOK"), "13i `| cmd`: 後ろの段も実行しない");

    /* `a || b` — 真ん中が空 */
    fresh();
    execute_command("echo PA || echo PB");
    check(out_has("empty pipeline stage"), "13j `a || b`: 断る");
    check(!out_has("PA") && !out_has("PB"), "13k `a || b`: どちらも実行しない");

    /* クォートの中の `|` も今までどおり区切り = 同じ規則で断る (票 §6) */
    fresh();
    execute_command("echo \"a||b\"");
    check(out_has("empty pipeline stage"),
          "13l `echo \"a||b\"`: クォートは見ないので同じ規則で断る");

    /* スクリプト中なら後続行も実行しない (印が立っていること) */
    fresh();
    s_begin(0);
    s_add("echo PIPEOK |\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command("source /t.sh");
    check(!out_has("PIPEOK"),  "13m スクリプト: 断った行は実行しない");
    check(!ran("marknext"),    "13n スクリプト: 後続行も実行しない (印が立つ)");

    /* 誤発火の裏: 素直な 2 段は今までどおり */
    fresh();
    execute_command("echo ONE | echo TWO");
    check(out_has("ONE") && out_has("TWO"), "13o 通る 2 段は今までどおり");
    check(sh_refused_flag == 0, "13p 通る 2 段は印を残さない");
}

/* ========================================================================
 *  14. U7 — glob の mem_alloc が失敗したら行ごと断る (T6)
 *
 *  以前は黙って戻っていたので、一致の **一部だけ** が handler へ渡った
 *  (`rm /tmp/item*` が 1 件だけ消えて成功に見える)。
 * ======================================================================== */
static void case_glob_alloc_fail(void)
{
    report("14 U7: glob の確保失敗は行ごと断る (T6)\n");

    fresh();
    dir_set("/d/");
    dir_add("item1");
    dir_add("item2");
    dir_add("item3");
    g_glob_alloc_budget = 1;            /* 2 件目の確保で失敗する */
    execute_command("echo /d/item*");
    check(!out_has("item1") && !out_has("item2"),
          "14a 一致の一部だけを handler へ渡さない");
    check(out_has("glob: out of memory"), "14b 理由を 1 行出す");
    check(g_glob_allocs == 1 && g_frees >= 1,
          "14c 確保済みの文字列を解放する");

    /* 誤発火の裏: 確保が通れば今までどおり全件展開して handler を呼ぶ */
    fresh();
    dir_set("/d/");
    dir_add("item1");
    dir_add("item2");
    execute_command("echo /d/item*");
    check(out_has("item1") && out_has("item2"),
          "14d 確保が通れば今までどおり全件渡す");
    check(!out_has("out of memory"), "14e 誤発火なし");

    /* スクリプト中なら打ち切る */
    fresh();
    dir_set("/d/");
    dir_add("item1");
    dir_add("item2");
    s_begin(0);
    s_add("echo /d/item*\n");
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    g_glob_alloc_budget = 1;
    execute_command("source /t.sh");
    check(out_has("glob: out of memory"),
          "14f スクリプト: 断ったのは glob の確保失敗 (別の理由ではない)");
    check(!ran("marknext"), "14g スクリプト: 後続行も実行しない");
}

/* ========================================================================
 *  15. U8 — 上限を超える glob パターン / ディレクトリ部は **照合を試みない**
 *      (T7)
 *
 *  窓は g_ls_calls (sys_ls を呼んだ回数)。切ったパターンで照合すると
 *  別のファイルに当たるので、呼ぶ前に断ること。
 * ======================================================================== */
static void case_glob_pattern_refuses(void)
{
    report("15 U8: 長い glob パターン / ディレクトリ部は照合しない (T7)\n");

    /* パターン 256 文字 ('a'*255 + '*') */
    fresh();
    dir_set(".");
    dir_add("aaa");
    line_reset();
    line_add("echo ");
    line_add_run('a', 255);
    line_add("*");
    execute_command(g_line);
    check(g_ls_calls == 0, "15a 256 文字のパターン: 照合を試みない");
    check(refused_msg("sh: glob pattern"), "15b 断りが出る");

    /* 誤発火の裏: 255 文字ちょうど ('a'*254 + '*') は照合する */
    fresh();
    dir_set(".");
    dir_add("aaa");
    line_reset();
    line_add("echo ");
    line_add_run('a', 254);
    line_add("*");
    execute_command(g_line);
    check(g_ls_calls == 1, "15c 255 文字ちょうどは照合する");
    check(!out_has("too long"), "15d 255 文字では断らない");

    /* ディレクトリ部 256 文字 ('/' + 'a'*254 + '/') */
    fresh();
    dir_set(".");
    line_reset();
    line_add("echo /");
    line_add_run('a', 254);
    line_add("/x*");
    execute_command(g_line);
    check(g_ls_calls == 0, "15e 256 文字のディレクトリ部: 照合を試みない");
    check(refused_msg("sh: glob directory"), "15f 断りが出る");

    /* 誤発火の裏: 255 文字ちょうど ('/' + 'a'*253 + '/') は照合する */
    fresh();
    dir_set(".");
    line_reset();
    line_add("echo /");
    line_add_run('a', 253);
    line_add("/x*");
    execute_command(g_line);
    check(g_ls_calls == 1, "15g 255 文字ちょうどのディレクトリ部は照合する");
    check(!out_has("too long"), "15h 同上: 断らない");
}

/* ========================================================================
 *  16. U12 — 256 バイト以上の行で launch_req を呼ばない (T11)
 *
 *  try_exec の上限 (510) は通るが要求表の上限 (256) は超える長さで、
 *  「送る前に測って断る」が効いていることを見る。以前は launch_req が
 *  OS32_ERR_INVAL を返し、sh.bin がそれを **GUI 外** と読み違えていた。
 * ======================================================================== */
static void case_launch_cmdline_refuses(void)
{
    report("16 U12: 256 バイト以上は launch_req を呼ばない (T11)\n");

    /* "./mk1.bin" (9) + " " + 'a'*246 = 256。名前に `/` を入れて PATH 走査を
     * 避ける (候補ごとに接頭辞のぶん長さが変わるため)。 */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_run('a', 246);
    execute_command(g_line);
    check(g_launch_count == 0, "16a 256 バイト: launch_req を呼ばない");
    check(refused_msg("sh: launch command line"), "16b 断りが出る");
    check(!out_has("need the GUI terminal"),
          "16c 理由を「GUI 外」と取り違えない");

    /* 誤発火の裏: 255 ちょうどは今までどおり送る */
    fresh();
    line_reset();
    line_add("./mk1.bin ");
    line_add_run('a', 245);
    execute_command(g_line);
    check(g_launch_count == 1, "16d 255 ちょうどは launch_req を呼ぶ");
    check(!out_has("too long"), "16e 255 では断らない");

    /* PATH 走査の途中で断ったら **そこで止める**。候補ごとに接頭辞のぶん
     * 行が伸びるので、止めないと同じ赤字が候補の数だけ出る。
     *   "mk1.bin" (7)            + 1 + 240 = 248  … 通る
     *   "/bin/mk1.bin" (12)      + 1 + 240 = 253  … 通る
     *   "/usr/bin/mk1.bin" (16)  + 1 + 240 = 257  … 断る (ここで止める)
     *   "/usr/local/bin/…" (22)  + 1 + 240 = 263  … 止めていなければもう 1 行 */
    fresh();
    env_set("PATH", "/bin:/usr/bin:/usr/local/bin");
    line_reset();
    line_add("mk1.bin ");
    line_add_run('a', 240);
    execute_command(g_line);
    check(out_count("sh: launch command line") == 1,
          "16f 断りは 1 行だけ (PATH 走査を止める)");
    check(g_launch_count == 2,
          "16g 断った後の候補を試さない (カレント + /bin の 2 回だけ)");
    check(!out_has("command not found"),
          "16h 断った行に \"command not found\" を足さない");
    env_set("PATH", SYS_DEFAULT_PATH);
}

/* ========================================================================
 *  17. U13 — CMD_BUF_SIZE 以上の行は空行と区別して断る (T13)
 * ======================================================================== */
static void case_long_line_refuses(void)
{
    report("17 U13: 長すぎる行を空行と同じ扱いにしない (T13)\n");

    /* execute_command: "echo " (5) + 'a'*4091 = 4096 */
    fresh();
    (void)big_line("echo ", CMD_BUF_SIZE - 5);
    execute_command(g_big);
    check(refused_msg("sh: command line"), "17a execute_command: 断る");

    /* 誤発火の裏: 4095 ちょうどは今までどおり走る */
    fresh();
    (void)big_line("echo ", CMD_BUF_SIZE - 6);
    execute_command(g_big);
    check(out_has("aaaa"), "17b 4095 ちょうどは今までどおり走る");
    check(!out_has("too long"), "17c 4095 では断らない");

    /* 空行は今までどおり黙って戻る (断りではない) */
    fresh();
    execute_command("");
    check(!out_has("too long"), "17d 空行は断らない (黙って戻る)");
    check(sh_refused_flag == 0, "17e 空行は印を立てない");

    /* execute_single も同じ規則 (パイプの段はここを直に通る) */
    fresh();
    (void)big_line("echo ", CMD_BUF_SIZE - 5);
    execute_single(g_big);
    check(refused_msg("sh: command"), "17f execute_single: 断る");

    fresh();
    execute_single("");
    check(!out_has("too long"), "17g execute_single: 空行は断らない");

    /* スクリプト中なら後続行も実行しない */
    fresh();
    (void)big_line("echo ", CMD_BUF_SIZE - 5);
    s_begin(0);
    s_add("marknext\n");
    file_add("/t.sh", s_body(0));
    execute_command(g_big);
    execute_command("source /t.sh");
    check(ran("marknext"),
          "17h 対話では次の行を巻き添えにしない (印は入口で消える)");
}

/* ========================================================================
 *  18. U17 — 254 文字を超える PATH 項目で区切りを見失わない (T17)
 *
 *  以前は `di < PATH_MAX_LEN - 2` で止まって残りが **次の項目** になり、
 *  別のディレクトリの同名バイナリを試していた。窓は g_launch_count と
 *  g_launch_last (どのパスを起こしにいったか)。
 * ======================================================================== */
static void case_path_entry_refuses(void)
{
    static char path_buf[ENV_VALUE_MAX];
    int i;

    report("18 U17: 長い PATH 項目で区切りを見失わない (T17)\n");

    /* 項目 255 文字 ('/' + 'a'*254) — 254 バイトで切れて残り 1 文字が
     * 次の項目になる */
    path_buf[0] = '/';
    for (i = 1; i < 255; i++) path_buf[i] = 'a';
    path_buf[255] = '\0';

    fresh();
    env_set("PATH", path_buf);
    execute_command("mk1");
    check(g_launch_count == 1,
          "18a 255 文字の項目: カレントの 1 回だけ (別のディレクトリを試さない)");
    check(refused_msg("sh: PATH entry"), "18b 断りが出る");

    /* 項目は収まるが dir + '/' + name が入り切らない場合も断る
     * ('/' + 'a'*249 = 250、+ '/' + "mk1.bin" (7) = 258 > 255) */
    path_buf[0] = '/';
    for (i = 1; i < 250; i++) path_buf[i] = 'a';
    path_buf[250] = '\0';

    fresh();
    env_set("PATH", path_buf);
    execute_command("mk1");
    check(g_launch_count == 1,
          "18c 連結が入り切らない: カレントの 1 回だけ (切ったパスを試さない)");
    check(refused_msg("sh: command path"), "18d 断りが出る");

    /* 誤発火の裏: 普通の PATH は今までどおり全候補を試す */
    fresh();
    env_set("PATH", "/bin:/usr/bin");
    execute_command("mk1");
    check(g_launch_count >= 2, "18e 普通の PATH は候補ぶん試す");
    check(!out_has("too long"), "18f 普通の PATH では断らない");
    check(out_has("command not found"), "18g 見つからなければ今までどおり");

    env_set("PATH", SYS_DEFAULT_PATH);   /* 後の試験のために戻す */
}

/* ========================================================================
 *  19. I1 — 引数が多すぎて行を捨てるときも印を立てる (PM 決裁 2026-09-16)
 *
 *  T6 と同じ関数の中にある同じ型の欠陥。`sh: too many arguments` は赤字を
 *  出すが印を立てていなかったので、**スクリプトが次の行へ落ちていた**。
 *  文言は据え置きで印だけ足す。
 *
 *  MAX_ARGS は 256 で、argv[argc] へ NUL を置くぶん **格納は 255 個まで**。
 *  つまり語が 255 個の行は通り、256 個目で断る。
 *  断る場所は 3 か所あるので全部踏む:
 *    (a) glob の展開中に溢れる  (ctx.overflow)
 *    (b) 一致しない glob を足せない (!matched_any の側)
 *    (c) 素の語を足せない       (else の側)
 * ======================================================================== */
static void case_too_many_args_marks(void)
{
    report("19 I1: 引数が多すぎて捨てるときも印を立てる\n");

    /* --- (c) 素の語で溢れる: `echo` + 255 個 = 256 語 ------------------ */
    /*  `echo` は内蔵なので try_exec を通らない (T3 の断りと混ざらない) */
    fresh();
    line_reset();
    line_add("echo");
    line_add_rep(" a", 255);
    execute_command(g_line);
    check(out_has("too many arguments"), "19a 256 語: 行ごと捨てる");
    check(sh_refused_flag == 1, "19b 256 語: 印を立てる");

    /* 誤発火の裏: 255 語ちょうどは今までどおり通って echo が走る */
    fresh();
    line_reset();
    line_add("echo");
    line_add_rep(" a", 254);
    execute_command(g_line);
    check(!out_has("too many arguments"), "19c 255 語ちょうどは通る");
    check(sh_refused_flag == 0, "19d 255 語は印を残さない");

    /* --- (a) glob の展開中に溢れる (ctx.overflow) --------------------- */
    /*  echo (1) + 素の語 250 個 = argc 251。/d/ の 8 件のうち 4 件までは
     *  入り (argc 255)、5 件目で ctx.overflow が立つ。 */
    fresh();
    dir_set("/d/");
    dir_add("item1"); dir_add("item2"); dir_add("item3"); dir_add("item4");
    dir_add("item5"); dir_add("item6"); dir_add("item7"); dir_add("item8");
    line_reset();
    line_add("echo");
    line_add_rep(" a", 250);
    line_add(" /d/item*");
    execute_command(g_line);
    check(out_has("too many arguments"), "19e glob の展開で溢れたら捨てる");
    check(sh_refused_flag == 1, "19f 同上: 印を立てる");
    check(!out_has("item1"), "19g 同上: 一部だけ渡さない");

    /* --- (b) 一致しない glob を足せない (!matched_any の側) ----------- */
    /*  echo (1) + 素の語 254 個 = argc 255。一致しない glob はそのまま
     *  1 語として足したいが、もう入らない。 */
    fresh();
    dir_set("/d/");
    dir_add("item1");
    line_reset();
    line_add("echo");
    line_add_rep(" a", 254);
    line_add(" /d/zzz*");
    execute_command(g_line);
    check(out_has("too many arguments"), "19h 一致しない glob も足せなければ捨てる");
    check(sh_refused_flag == 1, "19i 同上: 印を立てる");

    /* --- スクリプト中なら後続の行を実行しない ------------------------- */
    /*  スクリプトの 1 行は 255 バイトまで (T2、段 4) なので、語は変数展開で
     *  増やす。${A} は 60 語ぶんの並び (値はクォートで 1 語として渡す)。
     *  展開後は 1 + 60*5 = 301 語で、上限 255 を超える。 */
    {
        int k;
        fresh();
        s_begin(0);
        s_add("set A=\"a");
        for (k = 1; k < 60; k++) s_add(" a");
        s_add("\"\n");
        s_add("echo ${A} ${A} ${A} ${A} ${A}\n");
        s_add("marknext\n");
        file_add("/t.sh", s_body(0));
        execute_command("source /t.sh");
        check(out_has("too many arguments"), "19j スクリプト: 301 語の行を捨てる");
        check(!ran("marknext"),
              "19k スクリプト: 後続の行を実行しない (印が立っている)");
        check(out_has("script: aborted"), "19l スクリプト: 打ち切ったと言う");
    }

    /* --- 対話では打ち切らない ----------------------------------------- */
    fresh();
    line_reset();
    line_add("echo");
    line_add_rep(" a", 255);
    execute_command(g_line);         /* ← 捨てられる行 */
    execute_command("mk1");          /* ← 次の行 */
    check(out_has("too many arguments"), "19m 対話: 捨てた行は報せる");
    check(ran("mk1"), "19n 対話: 次の行は今までどおり走る");
    check(sh_refused_flag == 0, "19o 対話: 次の行の入口で印が消えている");

    /* --- パイプの段で捨てたら後続の段も実行しない (段 2b の規則が効く) -- */
    fresh();
    line_reset();
    line_add("echo");
    line_add_rep(" a", 255);
    line_add(" | echo STAGE2RAN");
    execute_command(g_line);
    check(out_has("too many arguments"), "19p 段の中で捨てる");
    check(!out_has("STAGE2RAN"), "19q 捨てた段の後続の段を実行しない");
}

/* ---- entry ------------------------------------------------------------- */

void _start(void)
{
    build_api();
    sh_boot();
    case_registry_is_real();
    case_if_compare_refuses();
    case_boundary();
    case_nested_execute_command();
    case_nested_source();
    case_pipe_stage();
    case_interactive_not_aborted();
    case_profile_continues();
    case_no_false_abort();
    /* 段 3「ルーター」 — T3 / T4 / T5 / T6 / T7 / T11 / T13 / T17 */
    case_try_exec_refuses();
    case_exec_time_refuse();
    case_cmd_name_refuses();
    case_pipeline_stages();
    case_glob_alloc_fail();
    case_glob_pattern_refuses();
    case_launch_cmdline_refuses();
    case_long_line_refuses();
    case_path_entry_refuses();
    case_too_many_args_marks();
    report(failures ? "SOME FAIL\n" : "ALL PASS\n");
    die(failures ? 1 : 0);
}
