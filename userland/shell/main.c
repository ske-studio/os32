/* ======================================================================== */
/*  MAIN.C — OS32 外部シェル エントリ・コマンドルーターロジック         */
/* ======================================================================== */
#include "shell.h"
#include "config.h"
#include "os32/help.h"
#include <stdio.h>

KernelAPI *g_api;

#ifdef SHELL_AS_APP
/* D2(d): 内蔵 `exit` が立て、shell_run() の外側ループが見て抜ける */
int sh_exit_flag = 0;
/* B2: sys_ls の写し取り。glob (このファイル) と ls (cmd_dir.c) が使う。 */
#include "sh_ls.inc"
#endif

static ShellCmd g_cmds[MAX_CMDS];
static int g_cmd_count = 0;

/* タブ補完用 (ui.c から参照される) */
const char *cmd_names[MAX_CMDS + 1];

/* ======================================================================== */
/*  エントリポイント (ファイルの最初にあること)                              */
/* ======================================================================== */
int main(int argc, char **argv, KernelAPI *api)
{
    g_api = api;

    /* stdout は常に行バッファ。newlib は最初の出力時に isatty で
     * バッファ方式を一度だけ決めるので、最初の printf がリダイレクト中に
     * 走るとその後ずっと全バッファになり出力がコンソールへ遅れて漏れる */
    setvbuf(stdout, (char *)0, _IOLBF, BUFSIZ);

    /* 環境変数の初期化 (コマンド登録より先に) */
    env_init();

    /* 各モジュールのコマンド登録 */
    shell_cmd_base_init();
    shell_cmd_file_init();
    shell_cmd_dir_init();
    shell_cmd_mnt_init();
    shell_cmd_sys_init();
#ifndef SHELL_AS_APP
    /* D2(a): sh.bin はシリアル / rshell を持たない。登録もしないので
     * `serial` `terminal` `rshell` `send` … は最初から表に載らない。 */
    shell_rshell_init();
#endif
    shell_cmd_env_init();
    shell_cmd_script_init();
    shell_cmd_filer_init();

    /* メインループ開始 (ui.c) */
    shell_run();
    return 0;
}

/* ======================================================================== */
/*  ヘルパー関数は shell.h でマクロとして定義済み                             */
/* ======================================================================== */
/*  コマンド登録機構                                                        */
/* ======================================================================== */
void shell_register_cmds(const ShellCmd *cmds)
{
    while (cmds->name != 0) {
        if (g_cmd_count < MAX_CMDS) {
            g_cmds[g_cmd_count] = *cmds;
            cmd_names[g_cmd_count] = cmds->name;
            g_cmd_count++;
            cmd_names[g_cmd_count] = (const char *)0; /* 常に終端を付与 */
        }
        cmds++;
    }
}

const ShellCmd *shell_get_cmds(int *count)
{
    *count = g_cmd_count;
    return g_cmds;
}

void shell_print_help(const char *cmd_name)
{
    int count, i;
    const ShellCmd *cmds;

    /* manページを参照 */
    if (os32_help_show(cmd_name) == 0) return;

    /* man が無い内部コマンドは登録テーブルの usage を出す
     * (以前は `unset` / `cp` を引数なしで打つと "No manual entry" だけだった) */
    cmds = shell_get_cmds(&count);
    for (i = 0; i < count; i++) {
        if (str_eq(cmds[i].name, cmd_name)) {
            g_api->kprintf(ATTR_WHITE, "Usage: %s %s\n", cmds[i].name, cmds[i].usage);
            g_api->kprintf(ATTR_WHITE, "  %s\n", cmds[i].description);
            return;
        }
    }
    g_api->kprintf(ATTR_RED, "No manual entry for %s\n", cmd_name);
}

/* ======================================================================== */
/* ======================================================================== */
#include "sh_args.inc"

/* ======================================================================== */
/*  PATH管理                                                                */
/* ======================================================================== */
static char g_path[512] = SYS_DEFAULT_PATH;

const char *shell_get_path(void)
{
    const char *p = env_get("PATH");
    if (p) return p;
    return g_path; /* フォールバック */
}

/* 文字列の末尾が指定の拡張子と一致するかチェック */
static int has_ext(const char *s, const char *ext)
{
    int slen = strlen(s);
    int elen = strlen(ext);
    if (slen < elen) return 0;
    return strcmp(s + slen - elen, ext) == 0;
}

/* パスにスラッシュが含まれるかチェック */
static int has_slash(const char *s)
{
    while (*s) { if (*s == '/') return 1; s++; }
    return 0;
}

/* cmdline (コマンド名+引数) を構築して exec_run を試行 */
#define TRY_EXEC_BUF_SIZE  512
#define TRY_EXEC_MARGIN    12  /* パス末尾 + スペース + NUL の余裕 */

static int try_exec(const char *bin_path, int argc, char **argv)
{
    char cmd_buf[TRY_EXEC_BUF_SIZE];
    char *p = cmd_buf;
    char *limit_path = cmd_buf + TRY_EXEC_BUF_SIZE - TRY_EXEC_MARGIN;
    char *limit_args = cmd_buf + TRY_EXEC_BUF_SIZE - 2;
    int i;
    const char *s;

    /* バイナリパスをコピー */
    s = bin_path;
    while (*s && p < limit_path) *p++ = *s++;

    /* 引数を追加。
     * parse_args_and_glob が剥がしたクォートをここで復元する。
     * 復元せずに空白区切りで再結合すると、exec_run 側の再トークナイズで
     * 空白入り引数 (例: sndctl play "T120 O4 ...") がばらばらに割れる。 */
    for (i = 1; i < argc; i++) {
        int need_quote = 0;
        const char *q;

        if (p >= limit_args) break;
        *p++ = ' ';

        s = argv[i];
        for (q = s; *q; q++) {
            if (*q == ' ' || *q == '"' || *q == '\'' || *q == '\\') {
                need_quote = 1;
                break;
            }
        }
        if (*s == '\0') need_quote = 1;   /* 空引数もクォートで保存 */

        if (need_quote) {
            if (p < limit_args) *p++ = '"';
            /* 閉じクォート分の余裕を残してコピー。" と \ はエスケープ */
            while (*s && p + 2 < limit_args) {
                if (*s == '"' || *s == '\\') *p++ = '\\';
                *p++ = *s++;
            }
            if (p < limit_args) *p++ = '"';
        } else {
            while (*s && p < limit_args) *p++ = *s++;
        }
    }
    *p = '\0';

    return sh_launch(cmd_buf);
}

#ifdef SHELL_AS_APP
/* sh_launch / パイプバッファの実体。ホスト TDD が同じソースを #include
 * できるように別ファイルにしてある (tools/tests/sh_launch_host.c,
 * tools/tests/sh_shell_host.c)。 */
#include "sh_launch.inc"
#include "sh_pipe.inc"
#endif /* SHELL_AS_APP */

/* ======================================================================== */
/*  try_exec_from_path — PATH環境変数を走査してコマンドを検索・実行           */
/*                                                                          */
/*  shell_get_path() から取得したコロン区切りPATHの各ディレクトリについて     */
/*  dir + "/" + name_buf のフルパスを構築し try_exec を試行する。             */
/* ======================================================================== */
static int try_exec_from_path(const char *name_buf, int argc, char **argv)
{
    const char *path_str = shell_get_path();
    const char *p = path_str;

    while (*p) {
        char dir_buf[PATH_MAX_LEN];
        char full_path[PATH_MAX_LEN];
        int di = 0;
        int rc;

        /* ':' で区切られたディレクトリを取得 */
        while (*p && *p != ':' && di < PATH_MAX_LEN - 2)
            dir_buf[di++] = *p++;
        dir_buf[di] = '\0';
        if (*p == ':') p++;
        if (di == 0) continue;

        /* フルパス構築: dir + '/' + name_buf */
        strncpy(full_path, dir_buf, PATH_MAX_LEN - 1);
        full_path[PATH_MAX_LEN - 1] = '\0';
        if (di > 0 && dir_buf[di - 1] != '/') {
            strncat(full_path, "/", PATH_MAX_LEN - strlen(full_path) - 1);
        }
        strncat(full_path, name_buf, PATH_MAX_LEN - strlen(full_path) - 1);

        rc = try_exec(full_path, argc, argv);
        if (rc != EXEC_ERR_NOT_FOUND && rc != EXEC_ERR_GENERAL) {
            return rc;
        }
    }
    return EXEC_ERR_NOT_FOUND;
}

#ifdef SHELL_AS_APP
/* D2(b): カーソル位置に依存する TUI / シリアル前提のコマンドは sh.bin では
 * 動かせない。内蔵コマンドの表を引く手前で弾く。`filer` の起動経路
 * (fl_exec_program) もここで到達しなくなる。 */
static int sh_is_cui_only(int argc, char **argv)
{
    const char *name = argv[0];

    if (str_eq(name, "os32gui") || str_eq(name, "rshell") ||
        str_eq(name, "filer")) return 1;

    /* R5: ループデバイスの枠 (drivers/loop_dev.c の loop_slots) は sh が
     * 退場しても残り、backing FD だけが owner 回収で閉じる。その後 FD 番号が
     * 再利用されると枠が別ファイルを向く。カーネル側の本修正は別票なので、
     * sh.bin では枠を作る / 使う経路をまとめて断る。 */
    if (str_eq(name, "losetup")) return 1;
    if (str_eq(name, "dd") && argc > 1) {
        const char *d = argv[1];
        if (d[0] == 'l' && d[1] == 'o' && d[2] >= '0' && d[2] <= '9') return 1;
    }
    return 0;
}
#endif

static void run_cmd_internal(int argc, char **argv) {
    int j, rc;
    char name_buf[PATH_MAX_LEN];

#ifdef SHELL_AS_APP
    if (sh_is_cui_only(argc, argv)) {
        g_api->kprintf(ATTR_RED, "%s", "sh: cui only\n");
        return;
    }
#endif

    if (argc > 1 && (str_eq(argv[1], "-h") || str_eq(argv[1], "--help") || str_eq(argv[1], "/?"))) {
        shell_print_help(argv[0]);
        return;
    }

    /* 0. .bat/.sh 拡張子 → 暗黙的に source として実行 */
    if (has_ext(argv[0], ".bat") || has_ext(argv[0], ".sh")) {
        script_source_file(argv[0]);
        return;
    }

    /* 1. 内部コマンドの検索 */
    for (j = 0; j < g_cmd_count; j++) {
        if (str_eq(argv[0], g_cmds[j].name)) {
            g_cmds[j].handler(argc, argv);
            return;
        }
    }

    /* 2. 外部コマンドの検索・実行 */
    {
        /* コマンド名に.bin拡張子を付加 (".bin\0" = 5文字分を予約) */
        strncpy(name_buf, argv[0], PATH_MAX_LEN - 5);
        name_buf[PATH_MAX_LEN - 5] = '\0';
        if (!has_ext(name_buf, ".bin")) {
            strcat(name_buf, ".bin");
        }
    }

    /* 2a. パスにスラッシュが含まれる場合 → 直接実行 */
    if (has_slash(argv[0])) {
        rc = try_exec(name_buf, argc, argv);
        sh_gfx_restore();
        if (rc == EXEC_SUCCESS) {
            g_api->kprintf(ATTR_GREEN, "%s", "\n");
        } else if (rc == EXEC_ERR_FAULT) {
            g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
        } else if (rc == EXEC_ERR_NOT_FOUND) {
            g_api->kprintf(ATTR_RED, "%s: not found\n", argv[0]);
        }
        return;
    }

    /* 2b. カレントディレクトリで試行 */
    rc = try_exec(name_buf, argc, argv);
    if (rc != EXEC_ERR_NOT_FOUND && rc != EXEC_ERR_GENERAL) {
        sh_gfx_restore();
        if (rc == EXEC_SUCCESS) {
            g_api->kprintf(ATTR_GREEN, "%s", "\n");
        } else if (rc == EXEC_ERR_FAULT) {
            g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
        }
        return;
    }

    /* 2c. PATH内の各ディレクトリで試行 */
    rc = try_exec_from_path(name_buf, argc, argv);
    if (rc != EXEC_ERR_NOT_FOUND && rc != EXEC_ERR_GENERAL) {
        sh_gfx_restore();
        if (rc == EXEC_SUCCESS) {
            g_api->kprintf(ATTR_GREEN, "%s", "\n");
        } else if (rc == EXEC_ERR_FAULT) {
            g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
        }
        return;
    }

    g_api->kprintf(ATTR_RED, "%s: command not found\n", argv[0]);
}

/* ======================================================================== */
/*  リダイレクト演算子の解析・適用                                           */
/*                                                                          */
/*  argv配列からリダイレクト演算子を検出し、カーネルFDリダイレクトを設定。    */
/*  検出した演算子とそのオペランドをargvから除去して残りのargcを返す。        */
/*                                                                          */
/*  対応構文:                                                               */
/*    cmd > file     stdout を file に上書き                                */
/*    cmd >> file    stdout を file に追記                                  */
/*    cmd < file     stdin を file から読み込み                             */
/*    cmd 2> file    stderr を file に書き込み                              */
/*    cmd > file 2>&1   stdout+stderr を file に                           */
/* ======================================================================== */
static int apply_redirects(int argc, char **argv)
{
    int i, out_idx;
    int new_argc = 0;
    char *new_argv[MAX_ARGS];

    for (i = 0; i < argc; i++) {
        /* ">>" 追記リダイレクト */
        if (argv[i][0] == '>' && argv[i][1] == '>') {
            const char *target;
            if (argv[i][2] != '\0') {
                /* ">>file" (スペースなし) */
                target = &argv[i][2];
            } else if (i + 1 < argc) {
                /* ">> file" */
                target = argv[++i];
            } else {
                g_api->kprintf(ATTR_RED, "%s", "syntax error: missing redirect target\n");
                return -1;
            }
            if (g_api->sys_redirect_fd(1, target, FD_REDIR_APPEND) < 0) {
                g_api->kprintf(ATTR_RED, "redirect: cannot open %s\n", target);
                return -1;
            }
            sh_redirect_mark();
            continue;
        }

        /* ">" 出力リダイレクト (上書き) */
        if (argv[i][0] == '>' && argv[i][1] != '>') {
            const char *target;
            if (argv[i][1] != '\0') {
                target = &argv[i][1];
            } else if (i + 1 < argc) {
                target = argv[++i];
            } else {
                g_api->kprintf(ATTR_RED, "%s", "syntax error: missing redirect target\n");
                return -1;
            }
            if (g_api->sys_redirect_fd(1, target, FD_REDIR_WRITE) < 0) {
                g_api->kprintf(ATTR_RED, "redirect: cannot open %s\n", target);
                return -1;
            }
            sh_redirect_mark();
            continue;
        }

        /* "<" 入力リダイレクト */
        if (argv[i][0] == '<') {
            const char *target;
            if (argv[i][1] != '\0') {
                target = &argv[i][1];
            } else if (i + 1 < argc) {
                target = argv[++i];
            } else {
                g_api->kprintf(ATTR_RED, "%s", "syntax error: missing redirect target\n");
                return -1;
            }
            if (g_api->sys_redirect_fd(0, target, FD_REDIR_READ) < 0) {
                g_api->kprintf(ATTR_RED, "redirect: cannot open %s\n", target);
                return -1;
            }
            sh_redirect_mark();
            continue;
        }

        /* "2>" stderr リダイレクト */
        if (argv[i][0] == '2' && argv[i][1] == '>') {
            const char *target;
            /* "2>&1" — stderr を stdout と同じ先に */
            if (argv[i][2] == '&' && argv[i][3] == '1') {
                /* stdout がリダイレクト済みなら stderr も同じファイルに */
                /* 簡易実装: 2>&1 は無視 (stdout と stderr は同じコンソール) */
                continue;
            }
            if (argv[i][2] != '\0') {
                target = &argv[i][2];
            } else if (i + 1 < argc) {
                target = argv[++i];
            } else {
                g_api->kprintf(ATTR_RED, "%s", "syntax error: missing redirect target\n");
                return -1;
            }
            if (g_api->sys_redirect_fd(2, target, FD_REDIR_WRITE) < 0) {
                g_api->kprintf(ATTR_RED, "redirect: cannot open %s\n", target);
                return -1;
            }
            sh_redirect_mark();
            continue;
        }

        /* 通常の引数 — 保持 */
        new_argv[new_argc++] = argv[i];
    }

    /* リダイレクト演算子を除去した argv を再構築 */
    for (out_idx = 0; out_idx < new_argc; out_idx++) {
        argv[out_idx] = new_argv[out_idx];
    }
    argv[new_argc] = (char *)0;

    return new_argc;
}

/* リダイレクト状態のリセット */
static void reset_all_redirects(void)
{
    g_api->sys_reset_redirect(0);
    g_api->sys_reset_redirect(1);
    g_api->sys_reset_redirect(2);
    sh_redirect_clear();
}

#ifdef SHELL_AS_APP
/* そのコマンド名が内蔵コマンド (または .bat / .sh スクリプト) か。
 * 外部コマンドは要求表経由で WM が起こす**別アプリ**になるので、sh 自身の
 * FD に掛けたリダイレクト / パイプは届かない。 */
static int sh_name_is_builtin(const char *name)
{
    int j;
    if (name[0] == '\0') return 1;   /* 空段は execute_single が黙って捨てる */
    if (has_ext(name, ".bat") || has_ext(name, ".sh")) return 1;
    for (j = 0; j < g_cmd_count; j++) {
        if (str_eq(name, g_cmds[j].name)) return 1;
    }
    return 0;
}

/* パイプの 1 段 (split_pipeline が前後の空白を落とした文字列) の先頭語を見る */
static int sh_stage_is_builtin(const char *seg)
{
    char name[PATH_MAX_LEN];
    int n = 0;

    while (*seg == ' ') seg++;
    while (*seg && *seg != ' ' && *seg != '<' && *seg != '>' &&
           n < PATH_MAX_LEN - 1) {
        name[n++] = *seg++;
    }
    name[n] = '\0';
    return sh_name_is_builtin(name);
}

/* argv にリダイレクト演算子が混じっているか (apply_redirects が見る形と同じ)。
 * リダイレクトを**張る前**に呼ぶこと — 張ってしまうと、外部段を断った後も
 * 親のリダイレクト表を子が閉じる余地が残る (表は全アプリ共有)。 */
static int sh_has_redirect(int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '>' || a[0] == '<') return 1;
        if (a[0] == '2' && a[1] == '>') {
            /* "2>&1" は apply_redirects が黙って捨てるだけ (FD を開かない) */
            if (a[2] == '&' && a[3] == '1') continue;
            return 1;
        }
    }
    return 0;
}
#endif

/* ======================================================================== */
/*  コマンド実行エンジン (単一コマンド)                                       */
/* ======================================================================== */
static void execute_single(const char *cmd)
{
    static char tmp_buf[CMD_BUF_SIZE];
    static char *argv[MAX_ARGS];
    static char *allocated_strings[MAX_ARGS];
    int alloc_count = 0;
    int argc = 0, j;
    char *p;
    const char *src;

    if (strlen(cmd) == 0 || strlen(cmd) >= CMD_BUF_SIZE) return;

    src = cmd;
    p = tmp_buf;
    while (*src) { *p++ = *src++; }
    *p = '\0';

    parse_args_and_glob(tmp_buf, argv, &argc, MAX_ARGS, allocated_strings, &alloc_count);

#ifdef SHELL_AS_APP
    /* R4: 一致が多すぎて glob を諦めた行は、一部だけ展開して実行しない */
    if (sh_glob_failed) {
        sh_glob_failed = 0;
        for (j = 0; j < alloc_count; j++) {
            g_api->mem_free(allocated_strings[j]);
        }
        return;
    }
#endif

    if (argc > 0) {
#ifdef SHELL_AS_APP
        /* B3: 標準 FD のリダイレクト表は全アプリ共有で read/write/reset が
         * owner を見ないので、外部コマンド (別アプリ) に掛けると出力が親の
         * ファイルへ入り、子の reset が親の FD を閉じる。リダイレクトを
         * **張る前**に断る。内蔵コマンドは sh 自身の文脈で完結するので従来どおり。 */
        if (sh_has_redirect(argc, argv) && !sh_name_is_builtin(argv[0])) {
            g_api->kprintf(ATTR_RED, "%s",
                           "sh: redirect to external command is not supported\n");
            for (j = 0; j < alloc_count; j++) {
                g_api->mem_free(allocated_strings[j]);
            }
            return;
        }
#endif
        /* リダイレクト演算子の解析・適用 */
        argc = apply_redirects(argc, argv);
        if (argc > 0) {
            run_cmd_internal(argc, argv);
        }
    }

    /* newlib の stdout バッファをここで吐き出す。リダイレクト解除後に
     * 遅れて flush されると、ファイルに入るはずの出力が次のコマンドの
     * コンソールに混ざる (`ls > file` の後半が欠ける原因の一つ) */
    fflush(stdout);

    for (j = 0; j < alloc_count; j++) {
        g_api->mem_free(allocated_strings[j]);
    }
}

/* ======================================================================== */
/*  パイプライン実行エンジン                                                 */
/*                                                                          */
/*  "cmd1 | cmd2 | cmd3" をシーケンシャルに実行:                              */
/*    1. cmd1 の stdout → パイプバッファA に蓄積                             */
/*    2. cmd2 の stdin ← バッファA, stdout → パイプバッファB に蓄積          */
/*    3. cmd3 の stdin ← バッファB, stdout → コンソール                      */
/* ======================================================================== */
#define MAX_PIPE_STAGES 8

static int split_pipeline(const char *cmd, char *seg_buf, int seg_size, int max_stages)
{
    int count = 0;
    int pos = 0;
    const char *p = cmd;
    char *seg;

    while (*p && count < max_stages) {
        seg = seg_buf + count * seg_size;
        while (*p == ' ') p++;
        pos = 0;
        while (*p && *p != '|') {
            if (pos < seg_size - 1) {
                seg[pos++] = *p;
            }
            p++;
        }
        while (pos > 0 && seg[pos - 1] == ' ') pos--;
        seg[pos] = '\0';
        if (pos > 0) count++;
        if (*p == '|') p++;
    }
    return count;
}


/* ======================================================================== */
/*  公開API: execute_command                                                 */
/* ======================================================================== */
void execute_command(const char *cmd)
{
    static char expanded_buf[CMD_BUF_SIZE];
    const char *src;
    int has_pipe = 0;

    if (strlen(cmd) == 0 || strlen(cmd) >= CMD_BUF_SIZE) return;

    /* $VAR / ~ 展開 */
    env_expand(cmd, expanded_buf, CMD_BUF_SIZE);
    src = expanded_buf;

    /* パイプの有無を判定 */
    {
        const char *c = src;
        while (*c) { if (*c == '|') { has_pipe = 1; break; } c++; }
    }

    if (!has_pipe) {
        /* パイプなし: 単一コマンド実行 */
        execute_single(src);
        reset_all_redirects();
        return;
    }

    /* パイプあり: パイプライン実行 */
    {
        char *seg_buf;
        int stage_count;
        int i;
        int cur_buf, prev_buf;

        /* セグメントバッファを動的確保 */
        seg_buf = (char *)g_api->mem_alloc(MAX_PIPE_STAGES * CMD_BUF_SIZE);
        if (!seg_buf) {
            g_api->kprintf(ATTR_RED, "%s", "pipe: out of memory\n");
            return;
        }

        stage_count = split_pipeline(src, seg_buf, CMD_BUF_SIZE, MAX_PIPE_STAGES);
        if (stage_count <= 1) {
            execute_single(seg_buf);
            reset_all_redirects();
            g_api->mem_free(seg_buf);
            return;
        }

#ifdef SHELL_AS_APP
        /* 外部段が 1 つでもあれば、その段の出力は sh の FD を通らない */
        for (i = 0; i < stage_count; i++) {
            if (!sh_stage_is_builtin(seg_buf + i * CMD_BUF_SIZE)) {
                g_api->kprintf(ATTR_RED, "%s",
                               "sh: pipe to external command is not supported\n");
                g_api->mem_free(seg_buf);
                return;
            }
        }
#endif

        /* バッファID: 交互使用 (0, 1, 0, 1, ...) */
        prev_buf = -1;
        {
            u32 saved_len = 0;
            /* パイプバッファを事前確保 (最大2つ: 交互使用) */
            int alloc_buf[2];
            int num_alloc = (stage_count > 2) ? 2 : 1;
            int ai;
            for (ai = 0; ai < num_alloc; ai++) {
                alloc_buf[ai] = sh_pipe_alloc();
                if (alloc_buf[ai] < 0) {
                    g_api->kprintf(ATTR_RED, "%s", "pipe: buffer alloc failed\n");
                    /* 確保済みを解放 */
                    {
                        int aj;
                        for (aj = 0; aj < ai; aj++) {
                            sh_pipe_free(alloc_buf[aj]);
                        }
                    }
                    g_api->mem_free(seg_buf);
                    return;
                }
            }

            sh_pipeline_enter();
            for (i = 0; i < stage_count; i++) {
                int is_first = (i == 0);
                int is_last = (i == stage_count - 1);

#ifdef SHELL_AS_APP
                /* B6: 段の途中で `exit` が立ったらそこで打ち切る
                 * (`exit | ask "wait: " V` が入力待ちに入らないように) */
                if (sh_exit_flag) break;
#endif

                /* stdin のリダイレクト (最初以外) */
                if (!is_first && prev_buf >= 0) {
                    u8 *buf = sh_pipe_get_buf(prev_buf);
                    /* バッファが消えていたら黙って続けない。以前はここが NULL
                     * (前段の exec_exit がパイプを回収していた) でも続行し、
                     * 次段が stdin をキーボードから読んでハングした */
                    if (!buf || g_api->sys_redirect_fd_buf(0, buf, PIPE_BUF_SIZE, saved_len) < 0) {
                        g_api->kprintf(ATTR_RED, "%s", "pipe: stdin buffer lost\n");
                        break;
                    }
                }

                /* stdout のリダイレクト (最後以外) */
                if (!is_last) {
                    cur_buf = alloc_buf[i % num_alloc];
                    {
                        u8 *buf = sh_pipe_get_buf(cur_buf);
                        if (!buf || g_api->sys_redirect_fd_buf(1, buf, PIPE_BUF_SIZE, 0) < 0) {
                            g_api->kprintf(ATTR_RED, "%s", "pipe: stdout buffer lost\n");
                            reset_all_redirects();
                            break;
                        }
                    }
                }

                /* コマンド実行 */
                execute_single(seg_buf + i * CMD_BUF_SIZE);

                /* stdout バッファに書き込まれたデータ長を保存 (リセット前に取得) */
                if (!is_last) {
                    saved_len = g_api->sys_redirect_get_buf_len(1);
                }

                /* リダイレクト解除 */
                reset_all_redirects();

                /* 前段のバッファIDを記録 */
                if (!is_last) {
                    prev_buf = cur_buf;
                }
            }

            sh_pipeline_leave();

            /* パイプバッファを解放 */
            for (ai = 0; ai < num_alloc; ai++) {
                sh_pipe_free(alloc_buf[ai]);
            }
        }
        g_api->mem_free(seg_buf);
    }
}
