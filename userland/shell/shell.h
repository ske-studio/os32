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
#define HIST_LINE_MAX 512
#define MAX_ARGS OS32_MAX_ARGS
#define MAX_CMDS 128
#define PATH_MAX_LEN OS32_MAX_PATH

/* スクリプトエンジン定数 */
#define SCRIPT_MAX_LINES  128   /* スクリプト最大行数 */
#define SCRIPT_MAX_LINE   256   /* 1行の最大長 */
#define SCRIPT_MAX_DEPTH  4     /* source ネスト上限 */

/* `if VAL1 == VAL2` の比較値の幅 (cmd_script.c)。実効は IF_VALUE_MAX - 1。
 * [C4] 断りのメッセージにもこの定数から上限を出す。 */
#define IF_VALUE_MAX      256

/* script_source_file の戻り値。0 = 成功 / -1 = 読めない・深すぎる /
 * SCRIPT_ERR_REFUSED = 行を断って打ち切った (票 TASK_SH_TRUNCATION §2-1) */
#define SCRIPT_ERR_REFUSED (-2)

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

/* ------------------------------------------------------------------------ */
/*  「切り詰めたので行を断った」印 (票 TASK_SH_TRUNCATION §2-1、main.c)      */
/*                                                                          */
/*  切り詰めを見つけた側は sh_refuse() で赤字 1 行を出し、同時に印を立てる。 */
/*  組み込み handler は void のままなので (int 化は TASK_EXIT_STATUS の範囲)、*/
/*  断ったことはこのグローバル 1 本だけで伝える。                            */
/*                                                                          */
/*  印の寿命は **1 行ぶん**:                                                 */
/*    - いちばん外側の execute_command が入口で消す                          */
/*      (入れ子 = if / time が組み立てた行、パイプの段 では消さない。         */
/*       消すと内側の断りが外へ届かない)                                     */
/*    - script_exec が 1 行ごとに sh_refused_take() で読んで消し、            */
/*      立っていたらスクリプトを打ち切る (goto のラベル無しと同じ扱い)        */
/*    - 対話 / rshell は誰も読まないので、断った行の次の行は今までどおり動く  */
/*    - パイプの段ループは sh_refused_peek() で **読むだけ**。印が立って  */
/*      いたら後続の段を実行せずに行を終える (票 §2「行全体を実行しない」)  */
/* ------------------------------------------------------------------------ */
extern int sh_refused_flag;
void sh_refuse(const char *what, int limit);  /* 赤字 1 行 + 印 */
void sh_refuse_mark(void);                    /* 印だけ (伝播用) */
int  sh_refused_take(void);                   /* 読んで消す */
int  sh_refused_peek(void);                   /* 読むだけ (消さない) */
extern const char *cmd_names[];  /* タブ補完用 */
const ShellCmd *shell_get_cmds(int *count);
void shell_print_help(const char *cmd_name);

/* メインループ・UI制御 (ui.c) */
void shell_run(void);
void hist_save(void);
void hist_load(void);

/* 各モジュールの初期化関数 (コマンド登録用) */
void shell_cmd_base_init(void);
void shell_cmd_file_init(void);
void shell_cmd_dir_init(void);
void shell_cmd_mnt_init(void);
void shell_cmd_sys_init(void);
void shell_rshell_init(void);
void shell_cmd_env_init(void);
void shell_cmd_script_init(void);
void shell_cmd_filer_init(void);

/* 環境変数 (cmd_env.c) */
void env_init(void);
const char *env_get(const char *name);
void env_set(const char *name, const char *value);
void env_unset(const char *name);
int  env_expand(const char *src, char *dst, int max);

/* PATH検索 (main.c) */
const char *shell_get_path(void);

/* ------------------------------------------------------------------------ */
/*  sh_launch — 外部プログラムの唯一の起動口 (票 T9 D3a)                     */
/*                                                                          */
/*  常駐 shell.bin (SHELL_AS_APP 未定義) は従来どおり入れ子 exec_run。        */
/*  マクロなので展開後のトークンは以前の g_api->exec_run(...) と同一で、      */
/*  常駐のコード生成は 1 バイトも変わらない (受入 S7 の SHA-256 一致)。       */
/*                                                                          */
/*  sh.bin (SHELL_AS_APP) は入れ子 exec_run を使えない — その子は park でき   */
/*  ず協調型 GUI 全体が止まる (K5b D9-8)。代わりにカーネルの要求表に載せ、     */
/*  WM に起動してもらって sys_yield で譲りながら launch_poll で待つ。         */
/*  実体は sh_launch.inc (main.c が #include)。exec_run への参照はこのヘッダ  */
/*  の #else 側 1 か所だけ。                                                  */
/* ------------------------------------------------------------------------ */
#ifdef SHELL_AS_APP
int sh_launch(const char *cmdline);
/* B4: パイプの段を回している間だけ立てる入れ子カウンタ。sh_launch の入口で
 * 見て、外部プログラムの起動を断る (先頭語だけの事前判定では
 * `exec /bin/sh.bin | echo tail` や `source x.sh | echo tail` を通してしまう。
 * 最終起動口で確かめれば経路を問わず捕まえられる)。 */
void sh_pipeline_enter(void);
void sh_pipeline_leave(void);
/* R2: sh がリダイレクトを張っている間だけ立てる印。apply_redirects で立て、
 * reset_all_redirects で下ろす。sh_launch の入口で見て外部起動を断る —
 * 内蔵の `exec` / `if` / `time` / `source` はリダイレクトを張った**後**に
 * 外部へ行けてしまい、事前判定 (execute_single) をすり抜けるため。 */
void sh_redirect_mark(void);
void sh_redirect_clear(void);
/* exit コマンド (D2(d)) が立てる。shell_run() の外側ループが見て抜ける。 */
extern int sh_exit_flag;
#else
#define sh_launch(cmdline)  (g_api->exec_run(cmdline))
#define sh_pipeline_enter()  ((void)0)
#define sh_pipeline_leave()  ((void)0)
#define sh_redirect_mark()   ((void)0)
#define sh_redirect_clear()  ((void)0)
#endif

/* ------------------------------------------------------------------------ */
/*  B2: sys_ls のコールバックからは KAPI を呼ばない (SHELL_AS_APP)           */
/*                                                                          */
/*  CPL=3 で `sys_ls` のコールバックから KAPI (int 0x80) を呼ぶと落ちる      */
/*  (カーネル側の欠陥、別票)。名前と種別を写すだけのコールバックを使い、     */
/*  `sys_ls` が戻ってから表示 / mem_alloc を行う。実体は sh_ls.inc。         */
/* ------------------------------------------------------------------------ */
#ifdef SHELL_AS_APP
void sh_ls_reset(void);
void sh_ls_set_filter(int (*f)(const char *name));
/* R4: 一致が多すぎて glob を諦めた行の印 (execute_single が見て捨てる) */
extern int sh_glob_failed;
void sh_ls_collect_cb(const DirEntry_Ext *entry, void *ctx);
int  sh_ls_count_get(void);
int  sh_ls_dropped(void);
void sh_ls_fill(int i, DirEntry_Ext *out);
#endif

/* B5: 行末の 1 文字を画面からも消す (端末の BS は 1 セル左へ動くだけ)。
 * ui.c の行編集と cmd_script.c の `ask` が共有する。実体は sh_redraw.inc。
 * 常駐 (CUI) では console が BS で消すので何もしない。 */
#ifdef SHELL_AS_APP
void sh_erase_cells(char removed);
#else
#define sh_erase_cells(removed) ((void)0)
#endif

/* ------------------------------------------------------------------------ */
/*  パイプバッファの出どころ (Codex 往復 5 の blocker)                       */
/*                                                                          */
/*  常駐 (CPL=0) はカーネルの `sys_pipe_*` をそのまま使う — マクロなので      */
/*  展開後のトークンは以前と同一で、常駐のコード生成は変わらない。           */
/*  sh.bin (CPL=3) はカーネル帯のポインタを `sys_redirect_fd_buf` へ渡せない */
/*  (`ring3_ptr_ok` に落ちて fault kill) ので、sh 自身の .bss から配る。      */
/*  実体は sh_pipe.inc (main.c が #include)。                                */
/* ------------------------------------------------------------------------ */
#ifdef SHELL_AS_APP
int  sh_pipe_alloc(void);
u8  *sh_pipe_get_buf(int slot);
void sh_pipe_free(int slot);
#else
#define sh_pipe_alloc()        (g_api->sys_pipe_alloc())
#define sh_pipe_get_buf(slot)  (g_api->sys_pipe_get_buf(slot))
#define sh_pipe_free(slot)     (g_api->sys_pipe_free(slot))
#endif

/* ------------------------------------------------------------------------ */
/*  sh_gfx_restore — 子がグラフィクスを使った後の後始末                      */
/*                                                                          */
/*  CUI では子が VRAM を握ったまま戻ることがあるので表示をテキストへ戻す。   */
/*  GUI 中に画面を持っているのは WM で、CPL=3 の sh.bin が gfx_shutdown を    */
/*  呼ぶと (所有者検査が無いので) GUI ごと表示が止まる。だから sh.bin では    */
/*  何もしない。常駐側の展開は以前の g_api->gfx_shutdown() と同一トークン。  */
/* ------------------------------------------------------------------------ */
#ifdef SHELL_AS_APP
#define sh_gfx_restore() ((void)0)
#else
#define sh_gfx_restore() (g_api->gfx_shutdown())
#endif

/* スクリプトエンジン (cmd_script.c) */
int script_source_file(const char *path);
/* 起動スクリプト (/etc/profile, $HOME/.profile) 用。断られても起動は止めず、
 * メッセージを出して既定値で続ける (票 TASK_SH_TRUNCATION §2-1 末尾 / R2)。 */
void script_source_profile(const char *path);

#endif /* SHELL_H */
