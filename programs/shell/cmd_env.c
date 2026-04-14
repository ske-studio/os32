/* ======================================================================== */
/*  CMD_ENV.C — OS32 シェル 環境変数管理モジュール                           */
/* ======================================================================== */
#include "shell.h"

/* ======================================================================== */
/*  環境変数テーブル                                                         */
/* ======================================================================== */

#define MAX_ENV_VARS 32
#define ENV_NAME_MAX 32
#define ENV_VALUE_MAX 256

typedef struct {
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
    int  used;
} EnvVar;

static EnvVar env_vars[MAX_ENV_VARS];

/* ======================================================================== */
/*  内部ヘルパー                                                             */
/* ======================================================================== */

static int env_find(const char *name)
{
    int i, j;
    for (i = 0; i < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used) continue;
        j = 0;
        while (name[j] && env_vars[i].name[j] && name[j] == env_vars[i].name[j]) j++;
        if (name[j] == '\0' && env_vars[i].name[j] == '\0') return i;
    }
    return -1;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

void env_init(void)
{
    int i;
    for (i = 0; i < MAX_ENV_VARS; i++) env_vars[i].used = 0;

    env_set("PATH",  "/bin:/sbin:/usr/bin");
    env_set("HOME",  "/home/user");
    env_set("SHELL", "/shell");
    env_set("USER",  "user");
}

const char *env_get(const char *name)
{
    int idx = env_find(name);
    if (idx < 0) return (const char *)0;
    return env_vars[idx].value;
}

void env_set(const char *name, const char *value)
{
    int idx = env_find(name);
    if (idx >= 0) {
        str_copy(env_vars[idx].value, value, ENV_VALUE_MAX);
        return;
    }
    /* 新規追加 */
    {
        int i;
        for (i = 0; i < MAX_ENV_VARS; i++) {
            if (!env_vars[i].used) {
                env_vars[i].used = 1;
                str_copy(env_vars[i].name, name, ENV_NAME_MAX);
                str_copy(env_vars[i].value, value, ENV_VALUE_MAX);
                return;
            }
        }
    }
    g_api->kprintf(ATTR_RED, "%s", "env: table full\n");
}

void env_unset(const char *name)
{
    int idx = env_find(name);
    if (idx >= 0) env_vars[idx].used = 0;
}

/* $VAR / ${VAR} / ~ 展開 */
int env_expand(const char *src, char *dst, int max)
{
    int si = 0, di = 0;
    int expanded = 0;

    while (src[si] && di < max - 1) {
        if (src[si] == '~' && (si == 0 || src[si - 1] == ' ') &&
            (src[si + 1] == '\0' || src[si + 1] == '/' || src[si + 1] == ' ')) {
            /* チルダ展開 → $HOME */
            const char *home = env_get("HOME");
            if (home) {
                while (*home && di < max - 1) dst[di++] = *home++;
                expanded = 1;
            } else {
                dst[di++] = '~';
            }
            si++;
        } else if (src[si] == '$') {
            /* $VAR or ${VAR} 展開 */
            char var_name[ENV_NAME_MAX];
            int vi = 0;
            const char *val;
            int braced = 0;

            si++; /* '$' をスキップ */
            if (src[si] == '{') { braced = 1; si++; }

            while (src[si] && vi < ENV_NAME_MAX - 1) {
                if (braced) {
                    if (src[si] == '}') { si++; break; }
                } else {
                    if (src[si] == ' ' || src[si] == '/' ||
                        src[si] == '.' || src[si] == ':' ||
                        src[si] == '$') break;
                }
                var_name[vi++] = src[si++];
            }
            var_name[vi] = '\0';

            val = env_get(var_name);
            if (val) {
                while (*val && di < max - 1) dst[di++] = *val++;
                expanded = 1;
            }
            /* 変数が見つからない場合は空文字に展開 (UNIXの慣習) */
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return expanded;
}

/* ======================================================================== */
/*  シェルコマンド                                                           */
/* ======================================================================== */

static void cmd_env(int argc, char **argv)
{
    int i;
    (void)argc; (void)argv;
    for (i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used) {
            g_api->kprintf(ATTR_WHITE, "%s=%s\n",
                           env_vars[i].name, env_vars[i].value);
        }
    }
}

static void cmd_set(int argc, char **argv)
{
    char name[ENV_NAME_MAX];
    const char *val_start;

    if (argc < 2) {
        /* 引数なし → env と同じ */
        cmd_env(argc, argv);
        return;
    }

    /* "VAR=VALUE" 形式をパース */
    {
        const char *arg = argv[1];
        int ni = 0;
        while (*arg && *arg != '=' && ni < ENV_NAME_MAX - 1)
            name[ni++] = *arg++;
        name[ni] = '\0';

        if (*arg == '=') {
            arg++; /* '=' をスキップ */
            val_start = arg;
        } else if (argc >= 3 && str_eq(argv[2], "=") && argc >= 4) {
            /* set VAR = VALUE 形式 (スペース区切り) */
            val_start = argv[3];
        } else if (argc >= 3) {
            /* set VAR VALUE 形式 (古い形式, fallback) */
            val_start = argv[2];
        } else {
            /* 値の表示 */
            const char *v = env_get(name);
            if (v) {
                g_api->kprintf(ATTR_WHITE, "%s=%s\n", name, v);
            } else {
                g_api->kprintf(ATTR_RED, "%s: not set\n", name);
            }
            return;
        }

        /* 残りの引数を値として結合 */
        {
            char value[ENV_VALUE_MAX];
            int vi = 0;
            const char *s = val_start;
            while (*s && vi < ENV_VALUE_MAX - 1) value[vi++] = *s++;

            /* argc>2 かつ '=' の後に追加引数がある場合 (set PATH=/bin:/sbin の場合は不要) */
            /* この場合は val_start に全て含まれている */
            value[vi] = '\0';
            env_set(name, value);
        }
    }
}

static void cmd_unset(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    for (i = 1; i < argc; i++) {
        env_unset(argv[i]);
    }
}

/* 登録用テーブル */
static const ShellCmd env_cmds[] = {
    { "env",    cmd_env,   "",            "Show all environment variables" },
    { "set",    cmd_set,   "VAR=VALUE",   "Set environment variable" },
    { "export", cmd_set,   "VAR=VALUE",   "Set environment variable" },
    { "unset",  cmd_unset, "VAR...",      "Unset environment variables" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_env_init(void)
{
    shell_register_cmds(env_cmds);
}
