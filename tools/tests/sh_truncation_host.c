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

static char g_line[1024];

static void line_reset(void) { g_line[0] = '\0'; }

static void line_add(const char *s)
{
    int n = 0;
    while (g_line[n]) n++;
    while (*s && n < (int)sizeof(g_line) - 1) g_line[n++] = *s++;
    g_line[n] = '\0';
}

/* ========================================================================
 *  1. 現状の記録 — `if` の両辺が 256 文字以上で先頭 255 文字が同じとき、
 *     **今は**条件が真になって右辺のコマンドが走る (T1 / U1)
 *
 *  EXPECTED_TO_CHANGE: 段 2 でここを反転させる。この段は「今どうなって
 *  いるか」を実物のソースで固定するだけなので、RED → GREEN ではない。
 *  段 2 で `if` が断るようになったら、この case は
 *   - launch を起こさない
 *   - 上限超過のメッセージを出す
 *  を見る形へ書き換わる (票 §4 U1)。
 * ======================================================================== */
static void case_if_compare_truncates(void)
{
    char a[600], b[600];

    report("1 [EXPECTED_TO_CHANGE] if: 256 文字以上の両辺は 255 で切って比べる\n");

    /* 256 文字。先頭 255 文字は同じで 256 文字目だけ違う */
    fill_run(a, 256, 'x');
    fill_run(b, 256, 'y');
    check(strcmp(a, b) != 0, "1a 反例そのものは別の文字列 (256 文字目が違う)");
    check(strncmp(a, b, 255) == 0, "1b 先頭 255 文字は同じ");

    files_reset();
    out_reset();
    launch_log_reset();

    /* `if <A> == <B> nosuchprog` — 実物の execute_command を通す。
     * 条件が偽なら run_cmd_internal に届かず、launch_req は呼ばれない。 */
    line_reset();
    line_add("if ");
    line_add(a);
    line_add(" == ");
    line_add(b);
    line_add(" truncmark");
    execute_command(g_line);

    /* 今の挙動: strip_quotes が両辺を 255 文字へ切るので条件が真になり、
     * `truncmark` が外部コマンドとして解決されにいく (= 起動を試みる)。 */
    check(g_launch_count > 0,
          "1c [EXPECTED_TO_CHANGE] 今は条件が真になり右辺が実行される");
    check(out_has("command not found"),
          "1d [EXPECTED_TO_CHANGE] 実行された証拠 (未知のコマンドとして解決)");

    /* `!=` は逆に倒れる — 違う文字列なのに偽 */
    files_reset();
    out_reset();
    launch_log_reset();
    line_reset();
    line_add("if ");
    line_add(a);
    line_add(" != ");
    line_add(b);
    line_add(" truncmark");
    execute_command(g_line);
    check(g_launch_count == 0 && !out_has("command not found"),
          "1e [EXPECTED_TO_CHANGE] 今は != が偽になり右辺が実行されない");
}

/* ========================================================================
 *  2. 足場が本物であることの確認
 *
 *  登録表も execute_command も実物だという前提が崩れたら 1 の記録に意味が
 *  無くなるので、ここで押さえておく。
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

    /* 短い比較は今も正しく効く (反例の作り方が悪いのではないことの確認) */
    files_reset();
    out_reset();
    launch_log_reset();
    execute_command("if abc == abd truncmark");
    check(g_launch_count == 0 && !out_has("command not found"),
          "2f 255 文字以下なら `==` は正しく偽になる");

    files_reset();
    out_reset();
    launch_log_reset();
    execute_command("if abc == abc truncmark");
    check(out_has("command not found"),
          "2g 255 文字以下で一致すれば右辺が走る");
}

/* ---- entry ------------------------------------------------------------- */

void _start(void)
{
    build_api();
    sh_boot();
    case_registry_is_real();
    case_if_compare_truncates();
    report(failures ? "SOME FAIL\n" : "ALL PASS\n");
    die(failures ? 1 : 0);
}
