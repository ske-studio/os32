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

static void *__cdecl h_mem_alloc(u32 size)
{
    char *p;
    unsigned long n = (unsigned long)size;

    n = (n + 7UL) & ~7UL;
    if (g_pool_used + n > (unsigned long)POOL_SIZE) return (void *)0;
    p = g_pool + g_pool_used;
    g_pool_used += n;
    return (void *)p;
}

static void __cdecl h_mem_free(void *p) { (void)p; }

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

static int __cdecl h_sys_write(int fd, const void *buf, u32 size)
{
    (void)fd; (void)buf;
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

/* sys_ls: 疑似 FS のうち dir で始まるものを 1 階層ぶん返す (glob 用) */
static int __cdecl h_sys_ls(const char *path, void *cb, void *ctx)
{
    (void)path; (void)cb; (void)ctx;
    return 0;
}

static int __cdecl h_sys_isatty(int fd) { (void)fd; return 1; }
static const char *__cdecl h_sys_getcwd(void) { return "/cwd"; }
static int __cdecl h_sys_chdir(const char *p) { (void)p; return 0; }
static int __cdecl h_sys_mkdir(const char *p) { (void)p; return 0; }
static int __cdecl h_sys_unlink(const char *p) { (void)p; return 0; }
static int __cdecl h_sys_redirect_fd(int fd, const char *p, int mode)
{
    (void)fd; (void)p; (void)mode;
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
 *  の入口の掃除に消されない。打ち切るのは**行**なので、後続の行が走らない
 *  ことを見る (段そのものの続行はシェルの通常の意味論どおり)。
 * ======================================================================== */
static void case_pipe_stage(void)
{
    report("6 経路 3: パイプの段\n");

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
    report(failures ? "SOME FAIL\n" : "ALL PASS\n");
    die(failures ? 1 : 0);
}
