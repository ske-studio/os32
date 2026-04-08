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

    /* 各モジュールのコマンド登録 */
    shell_cmd_base_init();
    shell_cmd_file_init();
    shell_cmd_dir_init();
    shell_cmd_mnt_init();
    shell_cmd_sys_init();
    shell_rshell_init();

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

static void run_cmd_internal(int argc, char **argv) {
    int j;
    
    if (argc > 1 && (str_eq(argv[1], "-h") || str_eq(argv[1], "--help") || str_eq(argv[1], "/?"))) {
        shell_print_help(argv[0]);
        return;
    }

    for (j = 0; j < g_cmd_count; j++) {
        if (str_eq(argv[0], g_cmds[j].name)) {
            g_cmds[j].handler(argc, argv);
            return;
        }
    }

    {
        char tb[PATH_MAX_LEN]; char cmd_buf[512];
        int ti=0; char *ap = argv[0]; int rc;
        char *p; int i;
        while (*ap && ti < PATH_MAX_LEN - 8) tb[ti++] = *ap++;
        tb[ti] = '\0';
        if (ti < 4 || tb[ti-4]!='.' || tb[ti-3]!='b') {
            tb[ti++]='.'; tb[ti++]='b'; tb[ti++]='i'; tb[ti++]='n'; tb[ti]='\0';
        }
        
        p = cmd_buf;
        for (i = 0; tb[i] && p < cmd_buf + 510; i++) *p++ = tb[i];
        for (i = 1; i < argc; i++) {
            *p++ = ' ';
            char *arg = argv[i];
            while (*arg && p < cmd_buf + 510) *p++ = *arg++;
        }
        *p = '\0';

        rc = g_api->exec_run(cmd_buf);
        /* GFXモードのプログラム終了後、テキスト画面に復帰 */
        g_api->gfx_shutdown();
        if (rc == EXEC_SUCCESS) {
            g_api->kprintf(ATTR_GREEN, "%s", "\n");
            return;
        } else if (rc == EXEC_ERR_FAULT) {
            g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
            return;
        } else if (rc != EXEC_ERR_GENERAL) {
            return;
        }
    }

    if (argv[0][0] == '.' && argv[0][1] == '/') {
        char cmd_buf[512]; char *p = cmd_buf; int i; int rc;
        for (i = 0; argv[0][i] && p < cmd_buf + 510; i++) *p++ = argv[0][i];
        for (i = 1; i < argc; i++) {
            *p++ = ' ';
            char *arg = argv[i];
            while (*arg && p < cmd_buf + 510) *p++ = *arg++;
        }
        *p = '\0';

        rc = g_api->exec_run(cmd_buf);
        g_api->gfx_shutdown();
        if (rc == EXEC_SUCCESS) g_api->kprintf(ATTR_GREEN, "%s", "\n");
        else if (rc == EXEC_ERR_FAULT) g_api->kprintf(ATTR_RED, "%s", "\n[Process crashed]\n");
        else g_api->kprintf(ATTR_RED, "%s", "Not found.\n");
        return;
    }

    g_api->kprintf(ATTR_RED, "%s: command not found\n", argv[0]);
}

/* ======================================================================== */
/*  コマンド実行エンジン                                                    */
/* ======================================================================== */
void execute_command(const char *cmd)
{
    char tmp_buf[CMD_BUF_SIZE];
    char *argv[MAX_ARGS];
    char *allocated_strings[MAX_ARGS];
    int alloc_count = 0;
    int argc = 0, j;
    char *p = tmp_buf;

    if (strlen(cmd) == 0 || strlen(cmd) >= CMD_BUF_SIZE) return;

    while (*cmd) { *p++ = *cmd++; }
    *p = '\0';

    parse_args_and_glob(tmp_buf, argv, &argc, MAX_ARGS, allocated_strings, &alloc_count);
    
    if (argc > 0) {
        run_cmd_internal(argc, argv);
    }
    
    for (j = 0; j < alloc_count; j++) {
        g_api->mem_free(allocated_strings[j]);
    }
}
