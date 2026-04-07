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

#endif /* SHELL_H */
