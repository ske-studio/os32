/* ======================================================================== */
/*  CMD_SCRIPT.C — OS32 シェル スクリプトエンジン (バッチ処理)                */
/*                                                                          */
/*  source, if, goto, ask, return コマンドを実装する。                       */
/*  Phase 1: source (逐次実行版) + コメント処理                             */
/* ======================================================================== */
#include "shell.h"

/* ======================================================================== */
/*  スクリプト実行コンテキスト                                               */
/* ======================================================================== */

/* 静的バッファ — mem_alloc は使わない (安全性優先) */
static char script_lines[SCRIPT_MAX_LINES][SCRIPT_MAX_LINE];
static int  script_line_count;
static int  script_current_line;
static int  script_abort_flag;

/* ネスト深度カウンタ */
static int g_script_depth = 0;

/* ======================================================================== */
/*  内部ヘルパー: 行の先頭空白をスキップ                                     */
/* ======================================================================== */
static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ======================================================================== */
/*  内部ヘルパー: コメント・空行判定                                         */
/*                                                                          */
/*  戻り値:                                                                 */
/*    0 = 通常行 (実行対象)                                                 */
/*    1 = コメントまたは空行 (スキップ対象)                                  */
/*    2 = ''' ブロックコメントのトグル行                                     */
/* ======================================================================== */
static int classify_line(const char *line)
{
    const char *p = skip_spaces(line);

    /* 空行 */
    if (*p == '\0') return 1;

    /* # コメント */
    if (*p == '#') return 1;

    /* // コメント */
    if (p[0] == '/' && p[1] == '/') return 1;

    /* ''' ブロックコメントトグル */
    if (p[0] == '\'' && p[1] == '\'' && p[2] == '\'') return 2;

    return 0;
}

/* ======================================================================== */
/*  ロードフェーズ: ファイルを読み込み、script_lines[] に格納                */
/*                                                                          */
/*  コメント行・空行・'''ブロックはこの段階で除外する。                       */
/*  ラベル行 (:LABEL) はそのまま保持する。                                   */
/* ======================================================================== */
static int script_load(const char *path)
{
    static char raw_buf[SCRIPT_MAX_LINES * SCRIPT_MAX_LINE];
    int fd, sz;
    int bi, li;
    int in_block_comment = 0;
    int cls;
    char line_tmp[SCRIPT_MAX_LINE];

    fd = g_api->sys_open(path, KAPI_O_RDONLY);
    if (fd < 0) {
        g_api->kprintf(ATTR_RED, "source: cannot open %s\n", path);
        return -1;
    }
    sz = g_api->sys_read(fd, raw_buf, (int)sizeof(raw_buf) - 1);
    g_api->sys_close(fd);
    if (sz <= 0) {
        g_api->kprintf(ATTR_RED, "source: cannot read %s\n", path);
        return -1;
    }
    raw_buf[sz] = '\0';

    script_line_count = 0;
    li = 0;

    for (bi = 0; bi <= sz; bi++) {
        if (raw_buf[bi] == '\n' || raw_buf[bi] == '\r' || raw_buf[bi] == '\0') {
            int i;

            /* \r\n 対応: \r の直後の \n はスキップ */
            if (raw_buf[bi] == '\r' && bi + 1 <= sz && raw_buf[bi + 1] == '\n') {
                bi++;
            }

            /* 行を終端 */
            if (li >= SCRIPT_MAX_LINE) li = SCRIPT_MAX_LINE - 1;
            line_tmp[li] = '\0';

            /* 行を分類 */
            cls = classify_line(line_tmp);

            if (cls == 2) {
                /* ''' トグル */
                in_block_comment = !in_block_comment;
            } else if (in_block_comment) {
                /* ブロックコメント内 — スキップ */
            } else if (cls == 0) {
                /* 通常行 — 配列に格納 */
                if (script_line_count >= SCRIPT_MAX_LINES) {
                    g_api->kprintf(ATTR_RED, "source: too many lines (max %d)\n",
                                   SCRIPT_MAX_LINES);
                    break;
                }
                for (i = 0; i < li && i < SCRIPT_MAX_LINE - 1; i++) {
                    script_lines[script_line_count][i] = line_tmp[i];
                }
                script_lines[script_line_count][i] = '\0';
                script_line_count++;
            }
            /* cls == 1 (コメント/空行) → スキップ */

            li = 0;
        } else {
            /* 文字を一時行バッファに蓄積 */
            if (li < SCRIPT_MAX_LINE - 1) {
                line_tmp[li] = raw_buf[bi];
            }
            li++;
        }
    }

    return 0;
}

/* ======================================================================== */
/*  実行フェーズ: script_lines[] を順次実行                                  */
/* ======================================================================== */
static void script_exec(void)
{
    script_current_line = 0;
    script_abort_flag = 0;

    while (script_current_line < script_line_count && !script_abort_flag) {
        const char *line = script_lines[script_current_line];
        const char *p = skip_spaces(line);

        /* ラベル行 (:LABEL) はスキップ */
        if (*p == ':') {
            script_current_line++;
            continue;
        }

        /* ESCキーブレーク: ノンブロッキングでキーバッファを確認 */
        {
            int k = g_api->kbd_trygetkey();
            if (k >= 0 && (k & 0xFF) == 0x1B) {
                g_api->kprintf(ATTR_RED, "%s", "^C Script aborted.\n");
                script_abort_flag = 1;
                break;
            }
        }

        /* コマンド実行 */
        execute_command(line);

        script_current_line++;
    }
}

/* ======================================================================== */
/*  公開API: script_source_file — ファイルを読み込んで実行                   */
/*                                                                          */
/*  戻り値: 0=成功, -1=エラー                                               */
/* ======================================================================== */
int script_source_file(const char *path)
{
    int saved_line_count;
    int saved_current_line;
    int saved_abort_flag;
    int result;

    /* ネスト深度チェック */
    if (g_script_depth >= SCRIPT_MAX_DEPTH) {
        g_api->kprintf(ATTR_RED, "source: nesting too deep (max %d)\n",
                       SCRIPT_MAX_DEPTH);
        return -1;
    }

    /* 現在のコンテキストを退避 (ネスト対応) */
    saved_line_count = script_line_count;
    saved_current_line = script_current_line;
    saved_abort_flag = script_abort_flag;

    g_script_depth++;

    /* ロード→実行 */
    result = script_load(path);
    if (result == 0) {
        script_exec();
    }

    /* コンテキスト復元 */
    script_line_count = saved_line_count;
    script_current_line = saved_current_line;
    script_abort_flag = saved_abort_flag;
    g_script_depth--;

    return result;
}

/* ======================================================================== */
/*  source コマンドハンドラ                                                  */
/* ======================================================================== */
static void cmd_source(int argc, char **argv)
{
    if (argc < 2) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: source <file>\n");
        return;
    }
    script_source_file(argv[1]);
}

/* ======================================================================== */
/*  ask コマンド — ユーザー入力を環境変数に格納                              */
/*                                                                          */
/*  書式: ask "プロンプト文字列" VAR_NAME                                    */
/*  最後の引数が変数名、それ以前の全引数を結合してプロンプトとする。         */
/*  引数中の二重引用符 " は除去する。                                        */
/* ======================================================================== */
static void cmd_ask(int argc, char **argv)
{
    char prompt[256];
    char input[256];
    int pi = 0;
    int i, j, len, key;

    if (argc < 3) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: ask \"prompt\" VAR_NAME\n");
        return;
    }

    /* argv[1]..argv[argc-2] をスペース区切りで結合 (引用符除去) */
    for (i = 1; i < argc - 1; i++) {
        for (j = 0; argv[i][j] && pi < 254; j++) {
            if (argv[i][j] != '"') {
                prompt[pi++] = argv[i][j];
            }
        }
        if (i < argc - 2 && pi < 254) prompt[pi++] = ' ';
    }
    prompt[pi] = '\0';

    /* プロンプト表示 */
    g_api->kprintf(ATTR_WHITE, "%s", prompt);

    /* キー入力ループ (Enter まで) */
    len = 0;
    for (;;) {
        key = g_api->kbd_getchar();
        if ((key & 0xFF) == 0x0D || (key & 0xFF) == '\n') {
            break;
        }
        if ((key & 0xFF) == 0x08) {
            /* バックスペース */
            if (len > 0) {
                len--;
                g_api->shell_putchar(0x08, ATTR_WHITE);
            }
            continue;
        }
        if ((key & 0xFF) >= 0x20 && (key & 0xFF) < 0x7F && len < 254) {
            input[len++] = (char)(key & 0xFF);
            g_api->shell_putchar((char)(key & 0xFF), ATTR_WHITE);
        }
    }
    input[len] = '\0';
    g_api->shell_putchar('\n', ATTR_WHITE);

    /* 環境変数にセット */
    env_set(argv[argc - 1], input);
}

/* ======================================================================== */
/*  if コマンド — 1行条件分岐                                                */
/*                                                                          */
/*  書式:                                                                    */
/*    if VAL1 == VAL2 COMMAND...     文字列一致                              */
/*    if VAL1 != VAL2 COMMAND...     文字列不一致                            */
/*    if exist PATH COMMAND...       ファイル存在                            */
/*    if not exist PATH COMMAND...   ファイル非存在                          */
/*                                                                          */
/*  注意: $VAR 展開は execute_command() 到達前に env_expand() で処理済み。   */
/*        引用符 " は parse_args_and_glob() で除去されないため残る。          */
/* ======================================================================== */

/* 内部ヘルパー: 引用符を除去して比較用文字列を取得 */
static void strip_quotes(const char *src, char *dst, int max)
{
    int di = 0;
    while (*src && di < max - 1) {
        if (*src != '"') dst[di++] = *src;
        src++;
    }
    dst[di] = '\0';
}

/* 内部ヘルパー: argv[start]..argv[argc-1] をスペース区切りで結合 */
static void join_args(int argc, char **argv, int start, char *buf, int max)
{
    int bi = 0;
    int i, j;
    for (i = start; i < argc; i++) {
        if (i > start && bi < max - 1) buf[bi++] = ' ';
        for (j = 0; argv[i][j] && bi < max - 1; j++) {
            buf[bi++] = argv[i][j];
        }
    }
    buf[bi] = '\0';
}

static void cmd_if(int argc, char **argv)
{
    int condition = 0;
    int cmd_start = 0;   /* COMMAND... の開始インデックス */

    if (argc < 4) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: if VAL1 == VAL2 COMMAND...\n");
        return;
    }

    /* "if not exist PATH COMMAND..." */
    if (str_eq(argv[1], "not") && argc >= 5 && str_eq(argv[2], "exist")) {
        int fd = g_api->sys_open(argv[3], KAPI_O_RDONLY);
        if (fd >= 0) {
            g_api->sys_close(fd);
            condition = 0; /* 存在する → not exist は偽 */
        } else {
            condition = 1; /* 存在しない → not exist は真 */
        }
        cmd_start = 4;
    }
    /* "if exist PATH COMMAND..." */
    else if (str_eq(argv[1], "exist") && argc >= 4) {
        int fd = g_api->sys_open(argv[2], KAPI_O_RDONLY);
        if (fd >= 0) {
            g_api->sys_close(fd);
            condition = 1;
        } else {
            condition = 0;
        }
        cmd_start = 3;
    }
    /* "if VAL1 == VAL2 COMMAND..." / "if VAL1 != VAL2 COMMAND..." */
    else if (argc >= 5) {
        char v1[256], v2[256];
        strip_quotes(argv[1], v1, 256);
        strip_quotes(argv[3], v2, 256);

        if (str_eq(argv[2], "==")) {
            condition = str_eq(v1, v2);
        } else if (str_eq(argv[2], "!=")) {
            condition = !str_eq(v1, v2);
        } else {
            g_api->kprintf(ATTR_RED, "if: unknown operator '%s'\n", argv[2]);
            return;
        }
        cmd_start = 4;
    } else {
        g_api->kprintf(ATTR_RED, "%s", "if: syntax error\n");
        return;
    }

    /* 条件が真のときのみコマンドを実行 */
    if (condition && cmd_start < argc) {
        static char cmd_buf[CMD_BUF_SIZE];
        join_args(argc, argv, cmd_start, cmd_buf, CMD_BUF_SIZE);
        execute_command(cmd_buf);
    }
}

/* ======================================================================== */
/*  goto コマンド — スクリプト内のラベルにジャンプ                            */
/*                                                                          */
/*  スクリプト実行コンテキスト外で呼ばれた場合は無害に無視する。             */
/* ======================================================================== */
static void cmd_goto(int argc, char **argv)
{
    int i;
    char label[SCRIPT_MAX_LINE];
    int li = 0;

    if (argc < 2) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: goto LABEL\n");
        return;
    }

    /* スクリプト実行中でなければ無視 */
    if (g_script_depth == 0) {
        g_api->kprintf(ATTR_RED, "%s", "goto: not in a script\n");
        return;
    }

    /* ":LABEL" 形式でラベルを構築 */
    label[li++] = ':';
    {
        const char *s = argv[1];
        while (*s && li < SCRIPT_MAX_LINE - 1) label[li++] = *s++;
    }
    label[li] = '\0';

    /* script_lines[] からラベルを検索 */
    for (i = 0; i < script_line_count; i++) {
        const char *p = skip_spaces(script_lines[i]);
        if (str_eq(p, label)) {
            /* ラベルの次の行から実行を再開 */
            /* script_exec() が current_line++ するので、ラベル行そのものにセット */
            script_current_line = i;
            return;
        }
    }

    /* ラベルが見つからない */
    g_api->kprintf(ATTR_RED, "goto: label '%s' not found\n", argv[1]);
    script_abort_flag = 1;
}

/* ======================================================================== */
/*  return コマンド — スクリプト実行を終了                                    */
/* ======================================================================== */
static void cmd_return(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (g_script_depth == 0) {
        /* 対話シェルから呼ばれた場合は無害に無視 */
        return;
    }

    /* current_line を末尾にセットして実行ループを終了させる */
    script_current_line = script_line_count;
}

/* ======================================================================== */
/*  コマンド登録テーブル                                                     */
/* ======================================================================== */
static const ShellCmd script_cmds[] = {
    { "source", cmd_source, "FILE",                  "Execute script file" },
    { ".",      cmd_source, "FILE",                  "Alias for source" },
    { "ask",    cmd_ask,    "\"prompt\" VAR",         "Read user input into variable" },
    { "if",     cmd_if,     "VAL1 == VAL2 CMD...",   "Conditional execution" },
    { "goto",   cmd_goto,   "LABEL",                 "Jump to label in script" },
    { "return", cmd_return, "",                      "Exit current script" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_script_init(void)
{
    shell_register_cmds(script_cmds);
}

