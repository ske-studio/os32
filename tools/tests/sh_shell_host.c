/* ========================================================================
 *  sh_shell_host.c — sh.bin の行再描画とスクリプトの exit を **実物のソース
 *                    で** 確かめる
 *
 *  対象票: docs/tasks/gui/v13/TASK_T9_sh.md §1 D2(d) と実装レビュー
 *          (往復 1/3) の blocker 1 / 2
 *  実行:   python3 -B tools/tests/test_sh_shell.py
 *  記録:   tools/tests/t9_tdd.md
 *
 *  1 行も写さずそのまま #include する実物は 2 つ:
 *    - userland/shell/sh_redraw.inc  (SHELL_AS_APP の redraw_line)
 *    - userland/shell/cmd_script.c   (script_load / script_exec / goto / source)
 *  カーネルの代わりに置くのは KernelAPI 表と、シェルの他モジュールが出す
 *  数本 (execute_command / env_* / shell_register_cmds) だけ。
 *
 *  tools/tests/launch_host.c と同じ様式 — ホスト ILP32 GNU89、libc 無し
 *  (-nostdlib、Linux の int 0x80 で write/exit)。<string.h> は python 側が
 *  一時ディレクトリに置く薄いシム。
 * ======================================================================== */

#include "shell.h"

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

#define OUT_CAP  1024

static char g_out[OUT_CAP];
static int  g_out_len;

static void out_reset(void) { g_out_len = 0; g_out[0] = '\0'; }

static void out_byte(char c)
{
    if (g_out_len + 1 < OUT_CAP) g_out[g_out_len++] = c;
    g_out[g_out_len] = '\0';
}

static void out_str(const char *s) { while (*s) out_byte(*s++); }

/* 目に見えない制御文字を読める形に直してから比べる ('\n' -> "\\n" 等) */
static void out_escaped(char *dst, int cap)
{
    int i, n = 0;
    for (i = 0; i < g_out_len && n + 3 < cap; i++) {
        char c = g_out[i];
        if (c == '\n')        { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (c == '\b')   { dst[n++] = '\\'; dst[n++] = 'b'; }
        else                   dst[n++] = c;
    }
    dst[n] = '\0';
}

static int out_is(const char *want)
{
    char shown[OUT_CAP];
    out_escaped(shown, OUT_CAP);
    if (strcmp(shown, want) == 0) return 1;
    report("       got \"");
    report(shown);
    report("\" want \"");
    report(want);
    report("\"\n");
    return 0;
}

/* ---- ごく小さなヒープと疑似ファイル ------------------------------------ */

#define POOL_SIZE  (128 * 1024)

static char g_pool[POOL_SIZE];
static unsigned long g_pool_used;

#define FILE_MAX  4

static struct { const char *path; const char *body; } g_files[FILE_MAX];
static int g_file_count;
static int g_open_fd;          /* いま開いている疑似ファイルの添字 + 1 */
static int g_open_leak;        /* close されずに次の open が来たら 1 */

static void files_reset(void)
{
    g_file_count = 0;
    g_open_fd = 0;
    g_open_leak = 0;
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

/* B2: sys_ls のコールバック内で KAPI が呼ばれたら数える。生の int 0x80 の
 * 再入は CPL=3 で落ちるので、ホストでは「呼んだかどうか」だけを見る。 */
static int g_kapi_calls;

static void __cdecl h_kprintf(u8 attr, const char *fmt, ...)
{
    __builtin_va_list ap;
    const char *p = fmt;
    (void)attr;
    g_kapi_calls++;
    __builtin_va_start(ap, fmt);
    while (*p) {
        if (p[0] == '%' && p[1] == 's') {
            out_str(__builtin_va_arg(ap, const char *));
            p += 2;
        } else if (p[0] == '%' && p[1] == 'd') {
            (void)__builtin_va_arg(ap, int);
            out_byte('#');
            p += 2;
        } else {
            out_byte(*p++);
        }
    }
    __builtin_va_end(ap);
}

static void __cdecl h_shell_putchar(char c, u8 attr)
{
    g_kapi_calls++;
    (void)attr;
    out_byte(c);
}
static void __cdecl h_shell_print_utf8(const char *s, u8 attr)
{
    g_kapi_calls++;
    (void)attr;
    out_str(s);
}

static void *__cdecl h_mem_alloc(u32 size)
{
    char *p;
    unsigned long n = (unsigned long)size;

    g_kapi_calls++;
    n = (n + 7UL) & ~7UL;
    if (g_pool_used + n > (unsigned long)POOL_SIZE) return (void *)0;
    p = g_pool + g_pool_used;
    g_pool_used += n;
    return (void *)p;
}

static void __cdecl h_mem_free(void *p) { g_kapi_calls++; (void)p; }

static int __cdecl h_sys_open(const char *path, int flags)
{
    int i;
    (void)flags;
    if (g_open_fd) g_open_leak = 1;
    for (i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            g_open_fd = i + 1;
            return i + 1;
        }
    }
    return -1;
}

static int __cdecl h_sys_read(int fd, void *buf, u32 size)
{
    const char *src;
    char *dst = (char *)buf;
    int n = 0;
    if (fd <= 0 || fd > g_file_count) return -1;
    src = g_files[fd - 1].body;
    while (src[n] && (u32)n < size) { dst[n] = src[n]; n++; }
    return n;
}

static void __cdecl h_sys_close(int fd) { (void)fd; g_open_fd = 0; }
static int __cdecl h_sys_isatty(int fd) { g_kapi_calls++; (void)fd; return 1; }

/* sh_launch がパイプ判定を抜けた先で NULL を踏まないための最小の受け皿。
 * GUI 外を模して INVAL を返す (この試験では起動そのものは見ない —
 * 起動の 4 経路は tools/tests/sh_launch_host.c の担当)。 */
static i32 __cdecl h_launch_req(const char *cmdline)
{
    (void)cmdline;
    g_kapi_calls++;
    return OS32_ERR_INVAL;
}
static i32 __cdecl h_launch_poll(i32 token, i32 *status)
{
    (void)token;
    if (status) *status = LAUNCH_ST_DONE;
    return 0;
}
static i32 __cdecl h_sys_yield(void) { return 0; }
static int __cdecl h_kbd_trygetkey(void) { return -1; }
static int __cdecl h_kbd_getchar(void)   { return 0x0D; }

/* blocker 1 の肝: GUI 中は座標が動かない (K6C-2) ので、行の再描画が
 * console_* を 1 度でも引いたら落とす。 */
static int g_cursor_calls;

static int  __cdecl h_console_get_cursor_x(void)   { g_cursor_calls++; return 0; }
static int  __cdecl h_console_get_cursor_y(void)   { g_cursor_calls++; return 0; }
static void __cdecl h_console_set_cursor(int x, int y)
{
    (void)x; (void)y;
    g_cursor_calls++;
}

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
    g_fake.sys_close = h_sys_close;
    g_fake.sys_isatty = h_sys_isatty;
    g_fake.launch_req = h_launch_req;
    g_fake.launch_poll = h_launch_poll;
    g_fake.sys_yield = h_sys_yield;
    g_fake.kbd_trygetkey = h_kbd_trygetkey;
    g_fake.kbd_getchar = h_kbd_getchar;
    g_fake.console_get_cursor_x = h_console_get_cursor_x;
    g_fake.console_get_cursor_y = h_console_get_cursor_y;
    g_fake.console_set_cursor = h_console_set_cursor;
    g_api = &g_fake;
}

/* ---- ui.c の周辺 (sh_redraw.inc が引くもの) ---------------------------- */

int sh_exit_flag = 0;
static int prev_draw_len = 0;

static void show_prompt(void) { out_str("sh> "); }

#include "../../userland/shell/sh_redraw.inc"
#include "../../userland/shell/sh_pipe.inc"
#include "../../userland/shell/sh_ls.inc"
#include "../../userland/shell/sh_launch.inc"

/* ---- 実物のスクリプトエンジン ------------------------------------------ */

#include "../../userland/shell/cmd_script.c"

/* ---- シェルの他モジュールの代わり -------------------------------------- */

void shell_register_cmds(const ShellCmd *cmds) { (void)cmds; }
void env_set(const char *name, const char *value) { (void)name; (void)value; }

int env_expand(const char *src, char *dst, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* 実行した行の記録。source / goto / exit だけ意味を持たせ、他は記録のみ。 */
#define TRACE_MAX   16
#define TRACE_LINE  64

static char g_trace[TRACE_MAX][TRACE_LINE];
static int  g_trace_n;
static int  g_trace_over;          /* 走りすぎ (goto の無限ループ) の印 */

static void trace_reset(void) { g_trace_n = 0; g_trace_over = 0; }

static int word_is(const char *line, const char *word)
{
    int i = 0;
    while (word[i] && line[i] == word[i]) i++;
    if (word[i]) return 0;
    return line[i] == '\0' || line[i] == ' ';
}

void execute_command(const char *cmd)
{
    int i;

    if (g_trace_n >= TRACE_MAX) { g_trace_over = 1; return; }
    for (i = 0; cmd[i] && i < TRACE_LINE - 1; i++) g_trace[g_trace_n][i] = cmd[i];
    g_trace[g_trace_n][i] = '\0';
    g_trace_n++;

    if (word_is(cmd, "exit")) {
        sh_exit_flag = 1;
        return;
    }
    if (word_is(cmd, "source")) {
        script_source_file(cmd + 7);
        return;
    }
    if (word_is(cmd, "goto")) {
        char label[TRACE_LINE];
        char *argv[3];
        int n = 0;
        /* "goto " は 5 バイト ("source " が 7)。7 のままだと `goto loop` を
         * `op` と読み、ラベルが見つからず script_abort_flag で止まってしまう
         * ので、巻き戻しからの exit 脱出を検査できていなかった。 */
        while (cmd[5 + n] && n < TRACE_LINE - 1) { label[n] = cmd[5 + n]; n++; }
        label[n] = '\0';
        argv[0] = (char *)"goto";
        argv[1] = label;
        argv[2] = (char *)0;
        cmd_goto(2, argv);
        return;
    }
}

static int trace_is(const char *joined)
{
    char got[TRACE_MAX * TRACE_LINE];
    int n = 0, i, j;
    for (i = 0; i < g_trace_n; i++) {
        if (i > 0) got[n++] = '|';
        for (j = 0; g_trace[i][j]; j++) got[n++] = g_trace[i][j];
    }
    got[n] = '\0';
    if (!g_trace_over && strcmp(got, joined) == 0) return 1;
    report("       got \"");
    report(got);
    report(g_trace_over ? "\" (走りすぎ) want \"" : "\" want \"");
    report(joined);
    report("\"\n");
    return 0;
}

/* ========================================================================
 *  1. redraw_line — 延長は差分だけ、それ以外は行の作り直し (blocker 1)
 * ======================================================================== */
static void case_redraw_extend(void)
{
    report("1 redraw_line: 純粋な延長は差分バイトだけ (TAB 補完)\n");

    /* いま画面に "sh> l" が出ている状態から "ls" へ補完された */
    sh_set_drawn("l", 1, 1);
    out_reset();
    g_cursor_calls = 0;
    redraw_line("ls", 2, 2);
    check(out_is("s"), "1a 増えた 1 バイトだけを印字する");
    check(sh_drawn_len == 2, "1b 写しが伸びる");

    /* 何も増えなければ何も出さない */
    out_reset();
    redraw_line("ls", 2, 2);
    check(out_is(""), "1c 変化なしなら無音");

    check(g_cursor_calls == 0, "1d console_set/get_cursor を 1 度も引かない");
}

static void case_redraw_rebuild(void)
{
    report("2 redraw_line: 延長でなければ行を作り直す\n");

    /* 履歴で丸ごと別の行に差し替わった */
    sh_set_drawn("ls -la", 6, 6);
    out_reset();
    g_cursor_calls = 0;
    redraw_line("cat", 3, 3);
    check(out_is("\\nsh> cat"), "2a 改行 + プロンプト + 行全体");
    check(g_cursor_calls == 0, "2a' 作り直しも座標を引かない");

    /* 短くなる向き (BS) も作り直し */
    sh_set_drawn("cat", 3, 3);
    out_reset();
    redraw_line("ca", 2, 2);
    check(out_is("\\nsh> ca"), "2b 縮む側も作り直す");

    /* 候補一覧を挟んだ後は延長でも作り直す */
    sh_set_drawn("ls", 2, 2);
    /* ui.c の sh_drop_drawn() の中身 (行が流れた印) */
    sh_set_drawn((const char *)0, -1, -1);
    out_reset();
    redraw_line("ls", 2, 2);
    check(out_is("\\nsh> ls"), "2c 行が流れた印のあとは必ず作り直す");

    /* カーソルが行末より前なら BS で戻す (端末の BS は消さずに左へ) */
    sh_set_drawn("abc", 3, 3);
    out_reset();
    redraw_line("abd", 3, 1);
    check(out_is("\\nsh> abd\\b\\b"), "2d 行末より前は BS で戻す");
}

/* ========================================================================
 *  3. 画面カーソルが行末に無いときは延長しない (再レビュー blocker 2)
 *
 *  CUI 直起動の `hel` -> LEFT -> TAB。LEFT は内容を変えないので写しは `hel`
 *  のままだが、画面カーソルは 1 つ左にある。内容の前方一致だけで「延長」と
 *  判定して差分 `p ` を出すと、画面は `hep ` (CUI の BS は消す) でバッファは
 *  `help ` という食い違いになる。
 * ======================================================================== */
static void case_redraw_cursor_not_at_end(void)
{
    report("3 画面カーソルが行末に無ければ延長しない (hel -> LEFT -> TAB)\n");

    /* 補完後に redraw_line が写しへ残したカーソル位置をそのまま使う経路 */
    sh_set_drawn("hel", 3, 2);              /* LEFT で 1 つ左に居る */
    out_reset();
    g_cursor_calls = 0;
    redraw_line("help ", 5, 5);             /* TAB 補完の結果 */
    check(out_is("\\nsh> help "), "3a 差分ではなく行を作り直す");
    check(g_cursor_calls == 0,   "3b 作り直しも座標を引かない");
    check(sh_drawn_pos == 5,     "3c 写しのカーソルは新しい行末へ");

    /* 行末より前で redraw した直後の TAB も同じ (BS で戻した状態が残る) */
    sh_set_drawn("abc", 3, 3);
    out_reset();
    redraw_line("abc", 3, 1);               /* 行末より前へ戻す */
    check(sh_drawn_pos == 1,     "3d redraw はカーソル位置も写しへ残す");
    out_reset();
    redraw_line("abcd", 4, 4);              /* ここで延長したら壊れる */
    check(out_is("\\nsh> abcd"), "3e その次も延長せず作り直す");

    /* ui.c の LEFT / RIGHT / HOME が呼ぶ sh_drop_drawn() の効き目 */
    sh_set_drawn("hel", 3, 3);
    sh_set_drawn((const char *)0, -1, -1);
    out_reset();
    redraw_line("help ", 5, 5);
    check(out_is("\\nsh> help "), "3f 写しを捨てた後も作り直す");
}

/* ========================================================================
 *  4. 行末 BS は画面からも消す (往復 3 の blocker)
 *
 *  端末の BS はカーソルを 1 セル左へ動かすだけでセルを消さない
 *  (libos32term の model.rs)。BS だけを出して短縮後の写しを確定すると、
 *  `echo abc` -> BS BS -> `x` で画面 `echo axc` / バッファ `echo ax` と
 *  食い違う。BS + 空白 + BS で消してから写しを確定する。
 * ======================================================================== */
static void case_backspace_erases(void)
{
    char line[8];

    report("4 行末 BS は BS + 空白 + BS で画面からも消す\n");

    /* abc -> BS -> BS -> x */
    line[0] = 'a'; line[1] = 'b'; line[2] = 'c'; line[3] = '\0';
    sh_set_drawn(line, 3, 3);
    out_reset();
    g_cursor_calls = 0;

    sh_backspace_tail(line, 2);             /* 'c' を消す */
    check(sh_drawn_len == 2 && sh_drawn_pos == 2, "4a 写しは縮んで末尾に居る");
    sh_backspace_tail(line, 1);             /* 'b' を消す */

    /* ここから先は ui.c の直接印字 (行末への 1 文字追加) と同じ */
    line[1] = 'x'; line[2] = '\0';
    g_api->shell_putchar('x', ATTR_WHITE);
    sh_set_drawn(line, 2, 2);

    check(out_is("\\b \\b\\b \\bx"), "4b 出力は BS 空白 BS の 2 回 + 'x'");
    check(sh_drawn_len == 2 && sh_drawn[0] == 'a' && sh_drawn[1] == 'x',
                                            "4c 写しは ax");
    check(g_cursor_calls == 0,              "4d 座標 KAPI を引かない");

    /* 3 バイト列の先頭 (全角) は 2 セルぶん */
    line[0] = (char)0xE3; line[1] = '\0';
    sh_set_drawn(line, 1, 1);
    out_reset();
    sh_backspace_tail(line, 0);
    check(out_is("\\b\\b  \\b\\b"),   "4e 全角の先頭は 2 セルぶん消す");
}

/* ========================================================================
 *  5. パイプバッファは sh 自身の .bss から配る (往復 5 の blocker)
 *
 *  `sys_pipe_get_buf()` が返すのはカーネル帯 (fs/pipe_buffer.c の kmalloc)
 *  で、それを `sys_redirect_fd_buf()` へ渡すと `ring3_ptr_ok` に落ちて
 *  CPL=3 の sh が畳まれる (`sh> echo a | cat`)。返るポインタが sh の静的
 *  配列の中にあること、解放後に再確保できること、上限を超えたら失敗する
 *  ことを見る。
 * ======================================================================== */
static void case_pipe_buffers(void)
{
    int a, b, c;
    u8 *pa, *pb;
    u8 *pool_lo = &sh_pipe_pool[0][0];
    u8 *pool_hi = &sh_pipe_pool[SH_PIPE_SLOTS - 1][PIPE_BUF_SIZE - 1];

    report("5 パイプバッファは sh の .bss から (カーネル帯を渡さない)\n");

    a = sh_pipe_alloc();
    b = sh_pipe_alloc();
    check(a >= 0 && b >= 0 && a != b, "5a 2 本を別々に確保できる");

    pa = sh_pipe_get_buf(a);
    pb = sh_pipe_get_buf(b);
    check(pa >= pool_lo && pa <= pool_hi,
                                      "5b 返るのは sh の静的配列の中");
    check(pb >= pool_lo && pb <= pool_hi && pb != pa,
                                      "5c 2 本目も配列内で別の番地");

    /* 上限超過 */
    c = sh_pipe_alloc();
    check(c < 0,                      "5d 上限を超えたら負を返す");
    check(sh_pipe_get_buf(c) == (u8 *)0,
                                      "5e 無効なスロットは NULL");

    /* 解放後に再確保できる */
    sh_pipe_free(a);
    check(sh_pipe_get_buf(a) == (u8 *)0, "5f 解放した枠は NULL になる");
    c = sh_pipe_alloc();
    check(c == a && sh_pipe_get_buf(c) == pa,
                                      "5g 解放後に同じ枠を再確保できる");

    sh_pipe_free(b);
    sh_pipe_free(c);
    check(sh_pipe_alloc() >= 0,       "5h 全部返せばまた確保できる");
    sh_pipe_free(0);
    sh_pipe_free(1);
}

/* ========================================================================
 *  6. sys_ls のコールバックは KAPI を呼ばない (往復 6 の B2)
 *
 *  CPL=3 で sys_ls のコールバックから int 0x80 を再入すると落ちる
 *  (実機で `find /etc -name filetypes` が [Process crashed])。sh の内蔵 ls と
 *  glob 展開はコールバックを写し取りだけにして、戻ってから処理する。
 * ======================================================================== */
static void case_ls_callback_no_kapi(void)
{
    DirEntry_Ext e;
    DirEntry_Ext got;
    int i;

    report("6 sys_ls のコールバックは KAPI を呼ばない\n");
    sh_ls_reset();
    g_kapi_calls = 0;

    /* SH_LS_MAX を 2 つ超える件数を流す */
    for (i = 0; i < SH_LS_MAX + 2; i++) {
        int n = 0;
        e.name[n++] = 'f';
        e.name[n++] = (char)('0' + (i % 10));
        e.name[n] = '\0';
        e.size = (u32)i;
        e.type = (i == 0) ? OS32_FILE_TYPE_DIR : OS32_FILE_TYPE_FILE;
        sh_ls_collect_cb(&e, (void *)0);
    }

    check(g_kapi_calls == 0,          "6a コールバック内の KAPI 呼び出しは 0 回");
    check(sh_ls_count_get() == SH_LS_MAX, "6b 上限まで写す");
    check(sh_ls_dropped() == 2,       "6c 溢れた分は数だけ数える");

    /* 写しから元の形に戻せる (呼び手は既存のコールバックへ流せる) */
    got.name[0] = '\0';
    got.size = 0xFFFFFFFFu;
    got.type = 0;
    sh_ls_fill(1, &got);
    check(got.name[0] == 'f' && got.name[1] == '1' && got.name[2] == '\0' &&
          got.size == 1 && got.type == OS32_FILE_TYPE_FILE,
                                      "6d 名前 / サイズ / 種別を写している");

    sh_ls_reset();
    check(sh_ls_count_get() == 0 && sh_ls_dropped() == 0,
                                      "6e reset で空になる");
}

/* ========================================================================
 *  7. パイプの中から外部プログラムは起動しない (往復 6 の B4)
 *
 *  先頭語だけの事前判定は `exec /bin/sh.bin | echo tail` を通してしまうので、
 *  最終起動口 (sh_launch) で確かめる。
 * ======================================================================== */
static void case_pipe_blocks_launch(void)
{
    int rc;

    report("7 パイプ実行中は sh_launch が断る\n");
    out_reset();
    sh_pipeline_enter();
    rc = sh_launch("/bin/kbd_echo.bin");
    check(rc < 0,                     "7a 負を返す");
    check(out_is("sh: pipe to external command is not supported\\n"),
                                      "7b 理由を出す");
    sh_pipeline_leave();

    /* 入れ子 (source 経由のパイプ) も数えているので、1 段抜けても残る */
    out_reset();
    sh_pipeline_enter();
    sh_pipeline_enter();
    sh_pipeline_leave();
    rc = sh_launch("/bin/kbd_echo.bin");
    check(rc < 0,                     "7c 入れ子は数えるので外側でもまだ断る");
    sh_pipeline_leave();
}

/* ========================================================================
 *  8. ask の行末 BS も画面から消す (往復 6 の B5)
 * ======================================================================== */
static void case_ask_backspace(void)
{
    report("8 ask の BS も BS + 空白 + BS\n");
    out_reset();
    sh_erase_cells('c');
    check(out_is("\\b \\b"),        "8a 半角は 1 セル");
    out_reset();
    sh_erase_cells((char)0xE3);
    check(out_is("\\b\\b  \\b\\b"), "8b 3 バイト列の先頭は 2 セル");
}

/* ========================================================================
 *  9. source 中の exit (往復 1 の blocker 2 / D2(d))
 * ======================================================================== */
static void case_exit_stops_rest(void)
{
    report("9 source: exit の次の行は走らない\n");
    sh_exit_flag = 0;
    files_reset();
    trace_reset();
    out_reset();
    file_add("/a.sh", "echo 1\nexit\necho 2\n");

    check(script_source_file("/a.sh") == 0, "9a source は 0 で戻る");
    check(trace_is("echo 1|exit"),          "9b exit の後は実行しない");
    check(sh_exit_flag == 1,                "9c 印は立ったまま (shell_run の入口が見る)");
    check(g_open_leak == 0,                 "9d FD を開いたままにしない");
}

static void case_exit_breaks_goto_loop(void)
{
    report("10 source: goto の無限ループでも exit で抜ける\n");
    sh_exit_flag = 0;
    files_reset();
    trace_reset();
    out_reset();
    /* exit が無ければ :loop <- goto loop で永久に回る */
    file_add("/b.sh", "echo a\nexit\n:loop\ngoto loop\n");

    check(script_source_file("/b.sh") == 0, "10a source は戻ってくる");
    check(trace_is("echo a|exit"),          "10b ラベルも goto も走らない");
}

static void case_exit_unwinds_nested(void)
{
    report("11 source: ネストした source の外側も抜ける\n");
    sh_exit_flag = 0;
    files_reset();
    trace_reset();
    out_reset();
    file_add("/outer.sh", "source /inner.sh\necho outer2\n");
    file_add("/inner.sh", "exit\necho inner2\n");

    check(script_source_file("/outer.sh") == 0, "11a 外側の source も 0 で戻る");
    check(trace_is("source /inner.sh|exit"),    "11b 内側も外側も後続を止める");
    check(g_open_leak == 0,                     "11c どの段でも FD を残さない");
}

/* ---- entry ------------------------------------------------------------- */

void _start(void)
{
    build_api();
    case_redraw_extend();
    case_redraw_rebuild();
    case_redraw_cursor_not_at_end();
    case_backspace_erases();
    case_pipe_buffers();
    case_ls_callback_no_kapi();
    case_pipe_blocks_launch();
    case_ask_backspace();
    case_exit_stops_rest();
    case_exit_breaks_goto_loop();
    case_exit_unwinds_nested();
    report(failures ? "SOME FAIL\n" : "ALL PASS\n");
    die(failures ? 1 : 0);
}
