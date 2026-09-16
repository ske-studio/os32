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

/* スクリプト行配列 — 動的確保 (使用時のみメモリ消費) */
static char (*script_lines)[SCRIPT_MAX_LINE] = NULL;
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
    int raw_buf_size = SCRIPT_MAX_LINES * SCRIPT_MAX_LINE;
    char *raw_buf;
    int fd, sz;
    int bi, li;
    int in_block_comment = 0;
    int cls;
    int more;
    int refused = 0;
    char line_tmp[SCRIPT_MAX_LINE];

    /* raw_buf を動的確保 */
    raw_buf = (char *)g_api->mem_alloc(raw_buf_size);
    if (!raw_buf) {
        g_api->kprintf(ATTR_RED, "%s", "source: out of memory (raw)\n");
        return -1;
    }

    /* script_lines を動的確保 */
    script_lines = (void *)g_api->mem_alloc(SCRIPT_MAX_LINES * SCRIPT_MAX_LINE);
    if (!script_lines) {
        g_api->mem_free(raw_buf);
        g_api->kprintf(ATTR_RED, "%s", "source: out of memory (lines)\n");
        return -1;
    }

    fd = g_api->sys_open(path, KAPI_O_RDONLY);
    if (fd < 0) {
        g_api->kprintf(ATTR_RED, "source: cannot open %s\n", path);
        g_api->mem_free(raw_buf);
        g_api->mem_free(script_lines);
        script_lines = NULL;
        return -1;
    }
    sz = g_api->sys_read(fd, raw_buf, raw_buf_size - 1);
    /* T2': 読み切れたかを**閉じる前に**確かめる。raw_buf を埋め切ったときは
     * 続きが残っているかもしれず、残っていればスクリプトの途中から先が
     * 無かったことになる (32KB を超えるスクリプトが黙って切れていた)。 */
    more = 0;
    if (sz == raw_buf_size - 1) {
        char probe;
        if (g_api->sys_read(fd, &probe, 1) > 0) more = 1;
    }
    g_api->sys_close(fd);
    if (sz <= 0) {
        g_api->kprintf(ATTR_RED, "source: cannot read %s\n", path);
        g_api->mem_free(raw_buf);
        g_api->mem_free(script_lines);
        script_lines = NULL;
        return -1;
    }
    if (more) {
        /* 上限の書式には乗らない (読み切れなかった) ので印だけ立てる。 */
        g_api->kprintf(ATTR_RED, "source: %s too large to read in full\n", path);
        sh_refuse_mark();
        g_api->mem_free(raw_buf);
        g_api->mem_free(script_lines);
        script_lines = NULL;
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

            /* T2: 行が収まらなかったら**切り詰めた行を実行しない**。
             * 以前はここで 255 バイトへ切って、切れた行がそのまま走った。 */
            if (li > SCRIPT_MAX_LINE - 1) {
                sh_refuse("source: script line", SCRIPT_MAX_LINE - 1);
                refused = 1;
                break;
            }
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
                    /* T2: 129 行目以降を捨てて先頭 128 行を実行すると、
                     * 捨てた行 (後始末など) が無かったことになる。
                     * 上限の書式には乗らないので印だけ立てる。 */
                    g_api->kprintf(ATTR_RED, "source: too many lines (max %d)\n",
                                   SCRIPT_MAX_LINES);
                    sh_refuse_mark();
                    refused = 1;
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

    g_api->mem_free(raw_buf);
    if (refused) {
        /* 1 行でも切り詰めたら**スクリプトを実行しない** (票 U3)。
         * script_source_file はここで止まり、source は失敗する。 */
        g_api->mem_free(script_lines);
        script_lines = NULL;
        script_line_count = 0;
        return -1;
    }
    return 0;
}

/* ======================================================================== */
/*  実行フェーズ: script_lines[] を順次実行                                  */
/*                                                                          */
/*  戻り値: 0 = 最後まで / ESC / return で終わった                           */
/*          1 = 行を断ったので打ち切った (票 TASK_SH_TRUNCATION §2-1)        */
/* ======================================================================== */
static int script_exec(void)
{
    int refused = 0;

    script_current_line = 0;
    script_abort_flag = 0;

    /* 入口では印を触らない。印を消すのは「いちばん外側の execute_command の
     * 入口」1 か所だけで、script_exec へ来る経路は必ずそこを通っている
     * (source / .sh / if / time のどれでも)。印を立てる側は立てたらすぐ
     * 戻るので、ここに古い印が残っていることはない。 */

    while (script_current_line < script_line_count && !script_abort_flag) {
        const char *line = script_lines[script_current_line];
        const char *p = skip_spaces(line);

#ifdef SHELL_AS_APP
        /* D2(d): source 中の `exit` はその場で打ち切る。**各行の前**に見るので
         * goto がここへ巻き戻しても回り続けず、ラベル行でも抜ける。ネストした
         * source は内側がこれで戻り、script_source_file が script_lines を
         * 解放してコンテキストを戻した先で外側もまた同じ判定で抜ける。
         * 常駐では sh_exit_flag が存在しない (`exit` を登録していない) ので
         * この判定ごと消える。 */
        if (sh_exit_flag) break;
#endif

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

        /* §2-1: 切り詰めで行を断ったら、そこでスクリプトを打ち切る。
         * 断った行を捨てて次へ進むと、本来 goto で飛び越されるはずだった
         * 後続行 (`rm -rf /data` など) へ落ちてしまう。goto のラベルが
         * 見つからないときと同じ扱いにする。
         * 印は 1 行ぶんの寿命なので、ここで読んで消す。 */
        if (sh_refused_take()) {
            g_api->kprintf(ATTR_RED, "%s", "script: aborted (line refused)\n");
            script_abort_flag = 1;
            refused = 1;
            break;
        }

        script_current_line++;
    }

    return refused;
}

/* ======================================================================== */
/*  公開API: script_source_file — ファイルを読み込んで実行                   */
/*                                                                          */
/*  戻り値: 0=成功, -1=エラー,                                              */
/*          SCRIPT_ERR_REFUSED=行を断って打ち切った (票 §2-1)               */
/* ======================================================================== */
int script_source_file(const char *path)
{
    char (*saved_lines)[SCRIPT_MAX_LINE];
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
    saved_lines = script_lines;
    saved_line_count = script_line_count;
    saved_current_line = script_current_line;
    saved_abort_flag = script_abort_flag;
    script_lines = NULL;

    g_script_depth++;

    /* ロード→実行 */
    result = script_load(path);
    if (result == 0) {
        /* 断って打ち切ったことは戻り値で親へ伝える。印そのものは
         * script_exec が消しているので、ここで勝手に立て直さない —
         * 立て直すかどうかは呼び手が決める (source は立て直し、
         * 起動時の profile は立て直さずに続行する)。 */
        if (script_exec()) result = SCRIPT_ERR_REFUSED;
    }

    /* 現在のスクリプト行を解放 */
    if (script_lines) {
        g_api->mem_free(script_lines);
    }

    /* コンテキスト復元 */
    script_lines = saved_lines;
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
    /* 入れ子の source: 内側が断って打ち切ったら、外側のスクリプトも
     * 打ち切る (§2-1)。印を立て直して execute_command 経由で親の
     * script_exec に見せる。 */
    if (script_source_file(argv[1]) == SCRIPT_ERR_REFUSED) sh_refuse_mark();
}

/* ======================================================================== */
/*  起動スクリプト (/etc/profile, $HOME/.profile) の入口                     */
/*                                                                          */
/*  断られても**起動は止めない** (票 §2-1 末尾 / 受入 R2)。メッセージを     */
/*  出して既定値のまま続ける。印は立て直さないので、この後の 1 行目が        */
/*  巻き添えで捨てられることもない。                                         */
/* ======================================================================== */
void script_source_profile(const char *path)
{
    int r = script_source_file(path);

    /* 起動は止めないので印は**先に**下ろす。script_load が断った (T2) 場合は
     * script_exec を通らないため印が立ったままで、そのままだと起動後の
     * 1 行目が巻き添えで捨てられる。 */
    if (r < 0) (void)sh_refused_take();

    if (r == SCRIPT_ERR_REFUSED) {
        g_api->kprintf(ATTR_RED,
                       "sh: %s aborted; continuing with defaults\n", path);
    }
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
    char prompt[ASK_PROMPT_MAX];
    char input[ASK_INPUT_MAX];
    int pi = 0;
    int i, j, len, key;
    int dropped = 0;

    if (argc < 3) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: ask \"prompt\" VAR_NAME\n");
        return;
    }

    /* argv[1]..argv[argc-2] をスペース区切りで結合 (引用符除去)。
     * T18: 収まらないプロンプトは切って出さない — 何を訊かれているか
     * 分からないまま答えを変数に入れることになる。 */
    for (i = 1; i < argc - 1; i++) {
        for (j = 0; argv[i][j]; j++) {
            if (argv[i][j] == '"') continue;
            if (pi >= ASK_PROMPT_MAX - 2) {
                sh_refuse("ask: prompt", ASK_PROMPT_MAX - 2);
                return;
            }
            prompt[pi++] = argv[i][j];
        }
        if (i < argc - 2) {
            if (pi >= ASK_PROMPT_MAX - 2) {
                sh_refuse("ask: prompt", ASK_PROMPT_MAX - 2);
                return;
            }
            prompt[pi++] = ' ';
        }
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
#ifdef SHELL_AS_APP
                /* B5: 端末の BS は 1 セル左へ動くだけでセルを消さないので、
                 * BS + 空白 + BS で上書きする (ui.c の行編集と同じ扱い)。
                 * 常駐は console が BS で消すので従来どおり。 */
                sh_erase_cells(input[len]);
#else
                g_api->shell_putchar(0x08, ATTR_WHITE);
#endif
            }
            continue;
        }
        if ((key & 0xFF) >= 0x20 && (key & 0xFF) < 0x7F) {
            /* T18: 255 文字目以降を黙って捨てて変数へ入れると、
             * 打ったものと違う値が登録される。捨てたら印を立てておき、
             * ENTER のところで断る (ui.c の行編集と同じ形)。 */
            if (len >= ASK_INPUT_MAX - 2) {
                dropped = 1;
                continue;
            }
            input[len++] = (char)(key & 0xFF);
            g_api->shell_putchar((char)(key & 0xFF), ATTR_WHITE);
        }
    }
    input[len] = '\0';
    g_api->shell_putchar('\n', ATTR_WHITE);

    if (dropped) {
        sh_refuse("ask: input", ASK_INPUT_MAX - 2);
        return;               /* 切れた値は登録しない */
    }

    /* 環境変数にセット (名前 / 値の長さは env_set が見る — 票 T8) */
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
/*        引用符 " は parse_args_and_glob() が既に落としている (sh_args.inc  */
/*        の「インプレースでクォート除去」)。argv に残るのはエスケープ等で    */
/*        生き残った " だけなので、strip_quotes はその取りこぼしを掃除する    */
/*        役目になっている。長さの上限は**クォート除去後**で数える。          */
/* ======================================================================== */

/* 内部ヘルパー: 引用符を除去して比較用文字列を取得
 *
 * 戻り値: 除去後の長さ / dst に収まらなければ -1 (票 T1)。
 * 以前はここで黙って max-1 文字に切っていたため、**先頭 255 文字が同じで
 * 256 文字目以降が違う 2 つの値が「等しい」と判定され**、`==` では本来
 * 実行されない枝が走り `!=` では逆に走らなかった。切り詰めた値では比べない。 */
static int strip_quotes(const char *src, char *dst, int max)
{
    int di = 0;
    while (*src) {
        if (*src != '"') {
            if (di >= max - 1) return -1;
            dst[di++] = *src;
        }
        src++;
    }
    dst[di] = '\0';
    return di;
}

/* 内部ヘルパー: argv[start]..argv[argc-1] をスペース区切りで結合
 *
 * 戻り値: 結合後の長さ / buf に収まらなければ -1 (票 T12)。
 * 以前は max - 1 で黙って切っていたので、**切れたコマンド行がそのまま
 * 実行された** (glob 展開で argv が伸びた行で届く)。 */
static int join_args(int argc, char **argv, int start, char *buf, int max)
{
    int bi = 0;
    int i, j;
    for (i = start; i < argc; i++) {
        if (i > start) {
            if (bi >= max - 1) return -1;
            buf[bi++] = ' ';
        }
        for (j = 0; argv[i][j]; j++) {
            if (bi >= max - 1) return -1;
            buf[bi++] = argv[i][j];
        }
    }
    buf[bi] = '\0';
    return bi;
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
        char v1[IF_VALUE_MAX], v2[IF_VALUE_MAX];
        int n1 = strip_quotes(argv[1], v1, IF_VALUE_MAX);
        int n2 = strip_quotes(argv[3], v2, IF_VALUE_MAX);

        /* 収まらない値は**比べない**。切り詰めて比べると条件が逆になり、
         * 本来実行されない枝が走る (票 T1 / U1)。断った行はここで終わり、
         * スクリプト中なら script_exec が後続行も実行しない (§2-1)。 */
        if (n1 < 0 || n2 < 0) {
            sh_refuse(n1 < 0 ? "if: left value" : "if: right value",
                      IF_VALUE_MAX - 1);
            return;
        }

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
        if (join_args(argc, argv, cmd_start, cmd_buf, CMD_BUF_SIZE) < 0) {
            sh_refuse("if: command line", CMD_BUF_SIZE - 1);
            return;
        }
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

