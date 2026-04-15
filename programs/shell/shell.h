/* ======================================================================== */
/*  SHELL.H — OS32 外部シェル 共通ヘッダ                                     */
/* ======================================================================== */
#ifndef SHELL_H
#define SHELL_H

#include "os32api.h"
#include <string.h>

#define str_eq(a, b) (strcmp((a), (b)) == 0)
#define str_startswith(a, b) (strncmp((a), (b), strlen(b)) == 0)
#define str_len(a) strlen(a)

/* ------------------------------------------------------------------------ */
/*  構造体・型定義                                                          */
/* ------------------------------------------------------------------------ */

#define CMD_BUF_SIZE 4096
#define MAX_ARGS OS32_MAX_ARGS
#define MAX_CMDS 128
#define PATH_MAX_LEN OS32_MAX_PATH

/* スクリプトエンジン定数 */
#define SCRIPT_MAX_LINES  256   /* スクリプト最大行数 */
#define SCRIPT_MAX_LINE   256   /* 1行の最大長 */
#define SCRIPT_MAX_DEPTH  4     /* source ネスト上限 */

/* コマンドハンドラ関数の型 */
typedef void (*CmdHandler)(int argc, char **argv);

/* コマンド登録用構造体 */
typedef struct {
    const char *name;
    CmdHandler handler;
    const char *usage;
    const char *description;
} ShellCmd;

/* ------------------------------------------------------------------------ */
/*  グローバル変数 (main.cで定義)                                           */
/* ------------------------------------------------------------------------ */
extern KernelAPI *g_api;

/* ------------------------------------------------------------------------ */
/*  関数プロトタイプ                                                        */
/* ------------------------------------------------------------------------ */

/* コマンド登録機構 (main.c) */
void shell_register_cmds(const ShellCmd *cmds);

/* コマンド実行エンジン (main.c) */
void execute_command(const char *cmd);
extern const char *cmd_names[];  /* タブ補完用 */
const ShellCmd *shell_get_cmds(int *count);
void shell_print_help(const char *cmd_name);

/* メインループ・UI制御 (ui.c) */
void shell_run(void);

/* 各モジュールの初期化関数 (コマンド登録用) */
void shell_cmd_base_init(void);
void shell_cmd_file_init(void);
void shell_cmd_dir_init(void);
void shell_cmd_mnt_init(void);
void shell_cmd_sys_init(void);
void shell_rshell_init(void);
void shell_cmd_env_init(void);
void shell_cmd_script_init(void);

/* 環境変数 (cmd_env.c) */
void env_init(void);
const char *env_get(const char *name);
void env_set(const char *name, const char *value);
void env_unset(const char *name);
int  env_expand(const char *src, char *dst, int max);

/* PATH検索 (main.c) */
const char *shell_get_path(void);

/* スクリプトエンジン (cmd_script.c) */
int script_source_file(const char *path);

#endif /* SHELL_H */
