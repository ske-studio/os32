/* ======================================================================== */
/*  MAIN.C — OS32 外部シェル エントリ・コマンドルーターロジック         */
/* ======================================================================== */
#include "shell.h"
#include "libos32/help.h"

KernelAPI *g_api;

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

    /* 環境変数の初期化 (コマンド登録より先に) */
    env_init();

    /* 各モジュールのコマンド登録 */
    shell_cmd_base_init();
    shell_cmd_file_init();
    shell_cmd_dir_init();
    shell_cmd_mnt_init();
    shell_cmd_sys_init();
    shell_rshell_init();
    shell_cmd_env_init();
    shell_cmd_script_init();

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
    /* manページを参照 */
    if (os32_help_show(cmd_name) != 0) {
        g_api->kprintf(ATTR_RED, "No manual entry for %s\n", cmd_name);
    }
}

/* ======================================================================== */
/* ======================================================================== */
int wildcard_match(const char *pattern, const char *str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++; 
            if (!*pattern) return 1; 
            while (*str) {
                if (wildcard_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') { 
            pattern++; str++;
        } else if (*pattern == *str) {
            pattern++; str++;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *str == '\0');
}

struct GlobCtx {
    char **argv;
    int *argc;
    int max_args;
    const char *pattern;
    const char *dir_prefix;
    int matched_any;
    char **allocated_strings;
    int *alloc_count;
};

static void glob_cb(const DirEntry_Ext *entry, void *c) {
    struct GlobCtx *ctx = (struct GlobCtx *)c;
    int dirlen, namelen, i;
    char *full;

    if (entry->name[0] == '.' && (entry->name[1]=='\0' || (entry->name[1]=='.' && entry->name[2]=='\0'))) return;

    if (*(ctx->argc) < ctx->max_args && wildcard_match(ctx->pattern, entry->name)) {
        dirlen = strlen(ctx->dir_prefix);
        namelen = strlen(entry->name);
        full = (char *)g_api->mem_alloc(dirlen + namelen + 1);
        if (!full) return;

        for (i = 0; i < dirlen; i++) full[i] = ctx->dir_prefix[i];
        for (i = 0; i < namelen; i++) full[dirlen + i] = entry->name[i];
        full[dirlen + namelen] = '\0';
        
        ctx->argv[*(ctx->argc)] = full;
        *(ctx->argc) += 1;
        ctx->matched_any = 1;
        
        if (*(ctx->alloc_count) < ctx->max_args) {
            ctx->allocated_strings[*(ctx->alloc_count)] = full;
            *(ctx->alloc_count) += 1;
        }
    }
}

void parse_args_and_glob(char *cmd_line, char **argv, int *argc_out, int max_args, char **allocated_strings, int *alloc_count) {
    int argc = 0;
    char *p = cmd_line;

    *alloc_count = 0;

    while (*p && argc < max_args) {
        char *start, *out;
        int has_star = 0;
        char quote = 0;

        while (*p == ' ') p++;
        if (!*p) break;
        start = p;
        out = p;   /* インプレースでクォート除去 (out <= p 常に成立) */

        while (*p) {
            if (quote) {
                /* クォート内 */
                if (*p == quote) {
                    /* クォート終了 — クォート文字自体は出力しない */
                    quote = 0;
                    p++;
                } else if (*p == '\\' && quote == '"' && *(p + 1)) {
                    /* ダブルクォート内のバックスラッシュエスケープ */
                    p++;
                    *out++ = *p++;
                } else {
                    /* シングルクォート内は全てリテラル */
                    *out++ = *p++;
                }
            } else {
                /* クォート外 */
                if (*p == ' ') break;
                if (*p == '"' || *p == '\'') {
                    /* クォート開始 — クォート文字自体は出力しない */
                    quote = *p++;
                } else if (*p == '\\' && *(p + 1)) {
                    /* バックスラッシュエスケープ */
                    p++;
                    *out++ = *p++;
                } else {
                    /* クォート外の * のみ glob 対象 */
                    if (*p == '*') has_star = 1;
                    *out++ = *p++;
                }
            }
        }
        /* p は空白か文字列末尾を指している */
        if (*p == ' ') p++;
        *out = '\0';

        if (has_star) {
            char *last_slash = (char *)0;
            char *q = start;
            char dir_path[PATH_MAX_LEN];
            char pattern[256];
            struct GlobCtx ctx;

            while (*q) {
                if (*q == '/' || *q == '\\') last_slash = q;
                q++;
            }

            if (last_slash) {
                int dirlen = last_slash - start + 1;
                int i;
                for (i = 0; i < dirlen && i < PATH_MAX_LEN - 1; i++) dir_path[i] = start[i];
                dir_path[i] = '\0';
                
                {
                    int patlen = 0;
                    char *pat_ptr = last_slash + 1;
                    while (*pat_ptr && patlen < 255) pattern[patlen++] = *pat_ptr++;
                    pattern[patlen] = '\0';
                }
            } else {
                dir_path[0] = '.'; dir_path[1] = '\0';
                {
                    int patlen = 0;
                    char *pat_ptr = start;
                    while (*pat_ptr && patlen < 255) pattern[patlen++] = *pat_ptr++;
                    pattern[patlen] = '\0';
                }
            }

            ctx.argv = argv;
            ctx.argc = &argc;
            ctx.max_args = max_args;
            ctx.pattern = pattern;
            ctx.dir_prefix = last_slash ? dir_path : "";
            ctx.matched_any = 0;
            ctx.allocated_strings = allocated_strings;
            ctx.alloc_count = alloc_count;

            g_api->sys_ls(dir_path, glob_cb, &ctx);

            if (!ctx.matched_any) {
                argv[argc++] = start;
            }
        } else {
            argv[argc++] = start;
        }
    }
    *argc_out = argc;
}

/* ======================================================================== */
/*  PATH管理                                                                */
/* ======================================================================== */
static char g_path[512] = "/bin:/sbin:/usr/bin";

const char *shell_get_path(void)
{
    const char *p = env_get("PATH");
    if (p) return p;
    return g_path; /* フォールバック */
}

/* .bin拡張子が付いているかチェック */
static int has_bin_ext(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    if (len < 4) return 0;
    return (s[len-4] == '.' && s[len-3] == 'b' &&
            s[len-2] == 'i' && s[len-1] == 'n');
}

/* パスにスラッシュが含まれるかチェック */
static int has_slash(const char *s)
{
    while (*s) { if (*s == '/') return 1; s++; }
    return 0;
}

/* cmdline (コマンド名+引数) を構築して exec_run を試行 */
static int try_exec(const char *bin_path, int argc, char **argv)
{
    char cmd_buf[512];
    char *p = cmd_buf;
    int i;
    const char *s;

    /* バイナリパスをコピー */
    s = bin_path;
    while (*s && p < cmd_buf + 500) *p++ = *s++;

    /* 引数を追加 */
    for (i = 1; i < argc; i++) {
        *p++ = ' ';
        s = argv[i];
        while (*s && p < cmd_buf + 510) *p++ = *s++;
    }
    *p = '\0';

    return g_api->exec_run(cmd_buf);
}

static void run_cmd_internal(int argc, char **argv) {
    int j, rc;
    char name_buf[PATH_MAX_LEN];

    if (argc > 1 && (str_eq(argv[1], "-h") || str_eq(argv[1], "--help") || str_eq(argv[1], "/?"))) {
        shell_print_help(argv[0]);
        return;
    }

    /* 0. .bat/.sh 拡張子 → 暗黙的に source として実行 */
    {
        int len = 0;
        const char *s = argv[0];
        while (s[len]) len++;
        if ((len >= 4 && s[len-4]=='.' && s[len-3]=='b' &&
             s[len-2]=='a' && s[len-1]=='t') ||
            (len >= 3 && s[len-3]=='.' && s[len-2]=='s' &&
             s[len-1]=='h')) {
            script_source_file(argv[0]);
            return;
        }
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
        /* コマンド名に.bin拡張子を付加 */
        int ni = 0;
        const char *ap = argv[0];
        while (*ap && ni < PATH_MAX_LEN - 8) name_buf[ni++] = *ap++;
        name_buf[ni] = '\0';
        if (!has_bin_ext(name_buf)) {
            name_buf[ni++] = '.';
            name_buf[ni++] = 'b';
            name_buf[ni++] = 'i';
            name_buf[ni++] = 'n';
            name_buf[ni] = '\0';
        }
    }

    /* 2a. パスにスラッシュが含まれる場合 → 直接実行 */
    if (has_slash(argv[0])) {
        rc = try_exec(name_buf, argc, argv);
        g_api->gfx_shutdown();
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
        g_api->gfx_shutdown();
        if (rc == EXEC_SUCCESS) {
            g_api->kprintf(ATTR_GREEN, "%s", "\n");
        } else if (rc == EXEC_ERR_FAULT) {
            g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
        }
        return;
    }

    /* 2c. PATH内の各ディレクトリで試行 */
    {
        const char *path_str = shell_get_path();
        const char *p = path_str;

        while (*p) {
            char dir_buf[PATH_MAX_LEN];
            char full_path[PATH_MAX_LEN];
            int di = 0, fi = 0, ni2 = 0;

            /* ':' で区切られたディレクトリを取得 */
            while (*p && *p != ':' && di < PATH_MAX_LEN - 2)
                dir_buf[di++] = *p++;
            dir_buf[di] = '\0';
            if (*p == ':') p++;
            if (di == 0) continue;

            /* フルパス構築: dir + '/' + name_buf */
            fi = 0;
            ni2 = 0;
            while (fi < PATH_MAX_LEN - 2 && dir_buf[fi])
                full_path[fi] = dir_buf[fi], fi++;
            if (fi > 0 && full_path[fi - 1] != '/')
                full_path[fi++] = '/';
            while (fi < PATH_MAX_LEN - 1 && name_buf[ni2])
                full_path[fi++] = name_buf[ni2++];
            full_path[fi] = '\0';

            rc = try_exec(full_path, argc, argv);
            if (rc != EXEC_ERR_NOT_FOUND && rc != EXEC_ERR_GENERAL) {
                g_api->gfx_shutdown();
                if (rc == EXEC_SUCCESS) {
                    g_api->kprintf(ATTR_GREEN, "%s", "\n");
                } else if (rc == EXEC_ERR_FAULT) {
                    g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
                }
                return;
            }
        }
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
}

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
    
    if (argc > 0) {
        /* リダイレクト演算子の解析・適用 */
        argc = apply_redirects(argc, argv);
        if (argc > 0) {
            run_cmd_internal(argc, argv);
        }
    }
    
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

static int split_pipeline(const char *cmd, char segments[][CMD_BUF_SIZE], int max_stages)
{
    int count = 0;
    int pos = 0;
    const char *p = cmd;

    while (*p && count < max_stages) {
        /* 先頭の空白をスキップ */
        while (*p == ' ') p++;
        pos = 0;
        while (*p && *p != '|') {
            if (pos < CMD_BUF_SIZE - 1) {
                segments[count][pos++] = *p;
            }
            p++;
        }
        /* 末尾の空白をトリム */
        while (pos > 0 && segments[count][pos - 1] == ' ') pos--;
        segments[count][pos] = '\0';
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
        static char segments[MAX_PIPE_STAGES][CMD_BUF_SIZE];
        int stage_count;
        int i;
        int cur_buf, prev_buf;

        stage_count = split_pipeline(src, segments, MAX_PIPE_STAGES);
        if (stage_count <= 1) {
            /* パイプ演算子があるが結果的に1段だけ */
            execute_single(segments[0]);
            reset_all_redirects();
            return;
        }

        /* バッファID: 交互使用 (0, 1, 0, 1, ...) */
        prev_buf = -1;
        {
            u32 saved_len = 0;
            for (i = 0; i < stage_count; i++) {
                int is_first = (i == 0);
                int is_last = (i == stage_count - 1);

                /* stdin のリダイレクト (最初以外) */
                if (!is_first && prev_buf >= 0) {
                    u8 *buf = g_api->sys_pipe_get_buf(prev_buf);
                    g_api->sys_redirect_fd_buf(0, buf, PIPE_BUF_SIZE, saved_len);
                }

                /* stdout のリダイレクト (最後以外) */
                if (!is_last) {
                    cur_buf = (i % 2 == 0) ? 0 : 1;
                    {
                        u8 *buf = g_api->sys_pipe_get_buf(cur_buf);
                        g_api->sys_redirect_fd_buf(1, buf, PIPE_BUF_SIZE, 0);
                    }
                }

                /* コマンド実行 */
                execute_single(segments[i]);

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
        }
    }
}
