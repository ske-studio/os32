/* ======================================================================== */
/*  MAIN.C — OS32 外部シェル エントリ・コマンドルーターロジック         */
/* ======================================================================== */
#include "shell.h"

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
    int i;
    for (i = 0; i < g_cmd_count; i++) {
        if (str_eq(cmd_name, g_cmds[i].name)) {
            g_api->kprintf(ATTR_YELLOW, "Usage: %s %s\n", g_cmds[i].name, g_cmds[i].usage ? g_cmds[i].usage : "");
            if (g_cmds[i].description) {
                g_api->kprintf(ATTR_WHITE, "  %s\n", g_cmds[i].description);
            }
            return;
        }
    }
    g_api->kprintf(ATTR_RED, "%s: no help available\n", cmd_name);
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
        char *start;
        int has_star = 0;

        while (*p == ' ') p++;
        if (!*p) break;
        start = p;
        
        while (*p && *p != ' ') {
            if (*p == '*') has_star = 1;
            p++;
        }
        if (*p) { *p = '\0'; p++; }

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
                
                int patlen = 0;
                char *pat_ptr = last_slash + 1;
                while (*pat_ptr && patlen < 255) pattern[patlen++] = *pat_ptr++;
                pattern[patlen] = '\0';
            } else {
                dir_path[0] = '.'; dir_path[1] = '\0';
                int patlen = 0;
                char *pat_ptr = start;
                while (*pat_ptr && patlen < 255) pattern[patlen++] = *pat_ptr++;
                pattern[patlen] = '\0';
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
/*  コマンド実行エンジン                                                    */
/* ======================================================================== */
void execute_command(const char *cmd)
{
    char tmp_buf[CMD_BUF_SIZE];
    char expanded_buf[CMD_BUF_SIZE];
    char *argv[MAX_ARGS];
    char *allocated_strings[MAX_ARGS];
    int alloc_count = 0;
    int argc = 0, j;
    char *p;
    const char *src;

    if (strlen(cmd) == 0 || strlen(cmd) >= CMD_BUF_SIZE) return;

    /* $VAR / ~ 展開 */
    env_expand(cmd, expanded_buf, CMD_BUF_SIZE);
    src = expanded_buf;

    p = tmp_buf;
    while (*src) { *p++ = *src++; }
    *p = '\0';

    parse_args_and_glob(tmp_buf, argv, &argc, MAX_ARGS, allocated_strings, &alloc_count);
    
    if (argc > 0) {
        run_cmd_internal(argc, argv);
    }
    
    for (j = 0; j < alloc_count; j++) {
        g_api->mem_free(allocated_strings[j]);
    }
}
