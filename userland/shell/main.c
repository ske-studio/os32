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

/* ======================================================================== */
/*  コマンドの起動経路 (has_ext / has_slash / try_exec /                     */
/*  try_exec_from_path / sh_is_cui_only / run_cmd_internal) の実体。         */
/*  ホスト試験がそのまま #include できるように別ファイルにしてある            */
/*  (tools/tests/sh_truncation_host.c)。並びは切り出す前と同一。             */
/* ======================================================================== */
#include "sh_exec.inc"

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

    /* T13: 空行と長大行を同じ扱いにしない。空行は今までどおり黙って戻り、
     * 収まらない行は **断る** (黙って消すと入力が無かったことになる)。 */
    if (strlen(cmd) == 0) return;
    if (strlen(cmd) >= CMD_BUF_SIZE) {
        sh_refuse("sh: command", CMD_BUF_SIZE - 1);
        return;
    }

    src = cmd;
    p = tmp_buf;
    while (*src) { *p++ = *src++; }
    *p = '\0';

    /* I1: 引数が多すぎる行は一部だけ実行せず丸ごと捨てる */
    if (parse_args_and_glob(tmp_buf, argv, &argc, MAX_ARGS,
                            allocated_strings, &alloc_count) < 0) {
#ifdef SHELL_AS_APP
        sh_glob_failed = 0;
#endif
        for (j = 0; j < alloc_count; j++) {
            g_api->mem_free(allocated_strings[j]);
        }
        return;
    }

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

/* T5: 段数 (> 0) / -1 = 行ごと断った。
 *
 *  以前は 9 段目以降と空の段を **黙って捨てて** いたので、実行されない段の
 *  失敗が見えなかった。捨てずに断る:
 *    - 段が max_stages を超える      (`a|b|c|d|e|f|g|h|i`)
 *    - 空の段がある                  (`echo ok |` / `| echo` / `a || b`)
 *    - 1 段が seg_size に収まらない  (行全体が CMD_BUF_SIZE 未満なので実際に
 *                                     は届かないが、規則としては同じ)
 *
 *  クォートは **今までどおり見ない**。したがって `echo "a||b"` の中の `|` も
 *  区切りのままで、空の段として断られる。クォートを見る分割は票 §6 で
 *  範囲外と決めてあるので、ここでは分割の規則を変えない。 */
static int split_pipeline(const char *cmd, char *seg_buf, int seg_size, int max_stages)
{
    int count = 0;
    int pos = 0;
    const char *p = cmd;
    char *seg;

    for (;;) {
        if (count >= max_stages) {
            sh_refuse("sh: pipeline", max_stages);
            return -1;
        }
        seg = seg_buf + count * seg_size;
        while (*p == ' ') p++;
        pos = 0;
        while (*p && *p != '|') {
            if (pos >= seg_size - 1) {
                sh_refuse("sh: pipeline stage", seg_size - 1);
                return -1;
            }
            seg[pos++] = *p++;
        }
        while (pos > 0 && seg[pos - 1] == ' ') pos--;
        seg[pos] = '\0';
        if (pos == 0) {
            /* 上限ではなく「段が空」なので sh_refuse の書式には乗らない。
             * 赤字 1 行 + 印だけ立てる (env_expand の断りと同じ形)。 */
            g_api->kprintf(ATTR_RED, "%s", "sh: empty pipeline stage\n");
            sh_refuse_mark();
            return -1;
        }
        count++;
        if (*p != '|') break;
        p++;
    }
    return count;
}


/* ======================================================================== */
/*  「切り詰めたので行を断った」印 (票 TASK_SH_TRUNCATION §2-1)              */
/*                                                                          */
/*  規則と寿命は shell.h の宣言のところに書いてある。ここは実体だけ。        */
/* ======================================================================== */
int sh_refused_flag = 0;

/* execute_command の入れ子の深さ。if / time が組み立てた行やパイプの段から      */
/* 呼ばれた execute_command は印を消さない — 消すと内側の断りが外へ届かない。 */
static int g_exec_depth = 0;

void sh_refuse_mark(void)
{
    sh_refused_flag = 1;
}

void sh_refuse(const char *what, int limit)
{
    /* 「何が上限を超えたか」と「上限」を赤字 1 行で (票 §2 の 2)。
     * 上限は呼び手が定数から渡す ([C4])。 */
    g_api->kprintf(ATTR_RED, "%s too long (max %d)\n", what, limit);
    sh_refused_flag = 1;
}

int sh_refused_take(void)
{
    int r = sh_refused_flag;
    sh_refused_flag = 0;
    return r;
}

/* 読むだけ — **消さない**。パイプの段ループが「この段で断ったか」を見るのに
 * 使う (票 §2 の「行全体を実行しない」)。ここで take してしまうと、断りが
 * script_exec まで届かず後続の**行**が走る。 */
int sh_refused_peek(void)
{
    return sh_refused_flag;
}

/* ======================================================================== */
/*  公開API: execute_command                                                 */
/* ======================================================================== */
static void execute_command_line(const char *cmd)
{
    static char expanded_buf[CMD_BUF_SIZE];
    const char *src;
    int has_pipe = 0;

    /* T13: 空行と長大行を同じ扱いにしない (execute_single と同じ規則)。 */
    if (strlen(cmd) == 0) return;
    if (strlen(cmd) >= CMD_BUF_SIZE) {
        sh_refuse("sh: command line", CMD_BUF_SIZE - 1);
        return;
    }

    /* $VAR / ~ 展開 */
    /* I-2: 展開しきれない行は**切れたまま実行しない** */
    if (env_expand(cmd, expanded_buf, CMD_BUF_SIZE) < 0) {
        g_api->kprintf(ATTR_RED, "%s", "sh: line too long after expansion\n");
        /* §2-1: これも「断った行」— スクリプト中なら後続行へ落とさない */
        sh_refuse_mark();
        return;
    }
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
        /* T5: 分割の時点で断ったら段を 1 つも実行しない (印は split_pipeline
         * が立てている)。パイプバッファはまだ 1 つも取っていない。 */
        if (stage_count < 0) {
            g_api->mem_free(seg_buf);
            return;
        }
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

                /* 票 §2「切り詰めたら行全体を実行しない」— 段で断りの印が
                 * 立ったら、**後続の段を実行せずに行を終える**。
                 *
                 * bash の `false | cat` に寄せて段を続けると、
                 * `<断られる段> | tee 重要ファイル` のように**断ったのに
                 * 後段の書き込みが起きる**。後段の `> file` は
                 * apply_redirects が O_TRUNC で開くので、リダイレクト先が
                 * 空で上書きされる。
                 *
                 * 印は**消さない** (§2-1)。消すのはいちばん外側の
                 * execute_command の入口だけで、スクリプト中ならこの行の
                 * 後で script_exec が打ち切る。抜けた後の後始末
                 * (reset_all_redirects / sh_pipe_free / mem_free) は
                 * ループの外と上でそのまま通る。 */
                if (sh_refused_peek()) break;
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

/* 実体は execute_command_line。ここは「断った印」の寿命を 1 行に閉じるための
 * 薄い包み (票 TASK_SH_TRUNCATION §2-1)。
 *
 * 入口で消すのは **いちばん外側** の呼び出しだけ。`if` / `time` が組み立てた
 * 行はこの関数を入れ子で呼ぶので、そこで消すと内側で断ったことが
 * script_exec まで届かなくなる (取りこぼし)。パイプの段は execute_single を
 * 直に呼ぶので入れ子にはならないが、段の中の `time ...` が入れ子になる。 */
void execute_command(const char *cmd)
{
    if (g_exec_depth == 0) sh_refused_flag = 0;
    g_exec_depth++;
    execute_command_line(cmd);
    g_exec_depth--;
}
