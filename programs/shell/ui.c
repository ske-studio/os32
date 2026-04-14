#include "shell.h"

/* ======================================================================== */
/*  シェルメインループ・UI（履歴・補完）                                     */
/* ======================================================================== */

#define HIST_SIZE    16

static char hist_buf[HIST_SIZE][CMD_BUF_SIZE];
static int  hist_count = 0;
static int  hist_idx   = 0;
static int  prev_draw_len = 0;

static void hist_add(const char *s) {
    int i;
    if (str_len(s) == 0) return;
    if (hist_count > 0 && str_eq(hist_buf[(hist_count - 1) % HIST_SIZE], s)) return;
    i = hist_count % HIST_SIZE;
    {
        int j;
        for (j = 0; s[j] && j < CMD_BUF_SIZE - 1; j++) hist_buf[i][j] = s[j];
        hist_buf[i][j] = 0;
    }
    hist_count++;
}

static void show_prompt(void) {
    g_api->kprintf(ATTR_GREEN, "OS32");
    g_api->kprintf(ATTR_GREEN, ":");
    g_api->kprintf(ATTR_CYAN, "%s", g_api->sys_getcwd());
    g_api->kprintf(ATTR_WHITE, "$ ");
}

static void redraw_line(const char *buf, int len, int cursor) {
    int i, cl;
    char tmp_buf[CMD_BUF_SIZE];
    g_api->console_set_cursor(0, g_api->console_get_cursor_y());
    show_prompt();
    /* バッファ内容をNUL終端コピーしてUTF-8対応印字 */
    for (i = 0; i < len && i < CMD_BUF_SIZE - 1; i++) tmp_buf[i] = buf[i];
    tmp_buf[i] = '\0';
    g_api->shell_print_utf8(tmp_buf, ATTR_WHITE);
    cl = prev_draw_len - len;
    for (i = 0; i < cl; i++) g_api->shell_putchar(' ', ATTR_WHITE);
    prev_draw_len = len;

    g_api->console_set_cursor(0, g_api->console_get_cursor_y());
    show_prompt();
    for (i = 0; i < cursor; i++) g_api->console_set_cursor(g_api->console_get_cursor_x() + 1, g_api->console_get_cursor_y());
}

/* ======================================================================== */
/*  タブ補完: コマンド名 (内部+PATH) + ファイル名                           */
/* ======================================================================== */

#define TAB_MAX_MATCHES 40

/* PATH内.binファイル列挙用コールバック構造体 */
struct PathCompCtx {
    const char *matches[TAB_MAX_MATCHES];
    char name_store[TAB_MAX_MATCHES][64]; /* .bin除去後の名前を格納 */
    int count;
    const char *prefix;
    int prefix_len;
    const char **cmd_names_ref; /* 内部コマンド名との重複チェック用 */
};

static void path_comp_cb(const DirEntry_Ext *entry, void *c)
{
    struct PathCompCtx *ctx = (struct PathCompCtx *)c;
    int nlen, i, ok, dup;
    const char *name = entry->name;

    if (ctx->count >= TAB_MAX_MATCHES) return;
    if (entry->type == OS32_FILE_TYPE_DIR) return;

    /* .bin拡張子チェック */
    nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen < 5) return; /* 少なくとも "x.bin" */
    if (name[nlen-4] != '.' || name[nlen-3] != 'b' ||
        name[nlen-2] != 'i' || name[nlen-1] != 'n') return;

    /* .bin を除去した名前を構築 */
    {
        int base_len = nlen - 4;
        if (base_len >= 63) base_len = 63;

        /* プレフィックスマッチ */
        ok = 1;
        for (i = 0; i < ctx->prefix_len && i < base_len; i++) {
            if (name[i] != ctx->prefix[i]) { ok = 0; break; }
        }
        if (!ok || ctx->prefix_len > base_len) return;

        /* 内部コマンドとの重複チェック */
        for (i = 0; i < base_len; i++)
            ctx->name_store[ctx->count][i] = name[i];
        ctx->name_store[ctx->count][base_len] = '\0';

        dup = 0;
        if (ctx->cmd_names_ref) {
            for (i = 0; ctx->cmd_names_ref[i]; i++) {
                if (str_eq(ctx->name_store[ctx->count], ctx->cmd_names_ref[i])) {
                    dup = 1; break;
                }
            }
        }
        /* 既に追加済みの名前との重複チェック */
        if (!dup) {
            for (i = 0; i < ctx->count; i++) {
                if (str_eq(ctx->name_store[ctx->count], ctx->name_store[i])) {
                    dup = 1; break;
                }
            }
        }
        if (dup) return;

        ctx->matches[ctx->count] = ctx->name_store[ctx->count];
        ctx->count++;
    }
}

/* ファイル名補完用コールバック構造体 */
struct FileCompCtx {
    const char *matches[TAB_MAX_MATCHES];
    char name_store[TAB_MAX_MATCHES][128];
    int count;
    const char *prefix;
    int prefix_len;
};

static void file_comp_cb(const DirEntry_Ext *entry, void *c)
{
    struct FileCompCtx *ctx = (struct FileCompCtx *)c;
    int nlen, i, ok;
    const char *name = entry->name;

    if (ctx->count >= TAB_MAX_MATCHES) return;
    /* . と .. をスキップ */
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return;

    nlen = 0;
    while (name[nlen]) nlen++;

    /* プレフィックスマッチ */
    ok = 1;
    for (i = 0; i < ctx->prefix_len && i < nlen; i++) {
        if (name[i] != ctx->prefix[i]) { ok = 0; break; }
    }
    if (!ok || ctx->prefix_len > nlen) return;

    /* 名前を格納 (ディレクトリなら末尾に '/' 付加) */
    for (i = 0; i < nlen && i < 126; i++)
        ctx->name_store[ctx->count][i] = name[i];
    if (entry->type == OS32_FILE_TYPE_DIR && i < 127) {
        ctx->name_store[ctx->count][i++] = '/';
    }
    ctx->name_store[ctx->count][i] = '\0';
    ctx->matches[ctx->count] = ctx->name_store[ctx->count];
    ctx->count++;
}

static int tab_complete(char *buf, int pos, int show_candidates) {
    const char *matches[TAB_MAX_MATCHES];
    int match_count = 0, i, j, common_len;
    int has_space = 0;

    if (pos == 0) return pos;

    /* バッファ内にスペースがあるかチェック (コマンド名 vs 引数) */
    for (i = 0; i < pos; i++) {
        if (buf[i] == ' ') { has_space = 1; break; }
    }

    if (!has_space) {
        /* ====== コマンド名補完 (最初のワード) ====== */

        /* 内部コマンド名の検索 */
        for (i = 0; cmd_names[i]; i++) {
            int ok = 1;
            for (j = 0; j < pos; j++) {
                if (cmd_names[i][j] == 0 || cmd_names[i][j] != buf[j]) { ok = 0; break; }
            }
            if (ok && match_count < TAB_MAX_MATCHES)
                matches[match_count++] = cmd_names[i];
        }

        /* PATH内の.binファイルを検索 */
        {
            struct PathCompCtx pctx;
            const char *path_str = shell_get_path();
            const char *pp = path_str;
            char prefix_tmp[PATH_MAX_LEN];

            /* バッファからプレフィックスを取得 */
            for (i = 0; i < pos && i < PATH_MAX_LEN - 1; i++) prefix_tmp[i] = buf[i];
            prefix_tmp[i] = '\0';

            pctx.count = 0;
            pctx.prefix = prefix_tmp;
            pctx.prefix_len = pos;
            pctx.cmd_names_ref = cmd_names;

            while (*pp) {
                char dir[PATH_MAX_LEN];
                int di = 0;
                while (*pp && *pp != ':' && di < PATH_MAX_LEN - 1)
                    dir[di++] = *pp++;
                dir[di] = '\0';
                if (*pp == ':') pp++;
                if (di > 0) {
                    g_api->sys_ls(dir, (void *)path_comp_cb, &pctx);
                }
            }

            /* 結果をmatches配列に追加 */
            for (i = 0; i < pctx.count && match_count < TAB_MAX_MATCHES; i++) {
                matches[match_count++] = pctx.matches[i];
            }
        }
    } else {
        /* ====== ファイル名補完 (2番目以降のワード) ====== */
        struct FileCompCtx fctx;
        char dir_path[PATH_MAX_LEN];
        const char *file_prefix;
        int word_start, last_slash;

        /* 現在のワードの開始位置を特定 */
        word_start = pos - 1;
        while (word_start > 0 && buf[word_start - 1] != ' ') word_start--;

        /* ワード内の最後の '/' を見つける */
        last_slash = -1;
        for (i = word_start; i < pos; i++) {
            if (buf[i] == '/') last_slash = i;
        }

        if (last_slash >= word_start) {
            /* ディレクトリ部分 + ファイルプレフィックス */
            int dlen = last_slash - word_start + 1;
            for (i = 0; i < dlen && i < PATH_MAX_LEN - 1; i++)
                dir_path[i] = buf[word_start + i];
            dir_path[i] = '\0';
            file_prefix = &buf[last_slash + 1];
            fctx.prefix_len = pos - last_slash - 1;
        } else {
            dir_path[0] = '.'; dir_path[1] = '\0';
            file_prefix = &buf[word_start];
            fctx.prefix_len = pos - word_start;
        }

        fctx.count = 0;
        fctx.prefix = file_prefix;

        g_api->sys_ls(dir_path, (void *)file_comp_cb, &fctx);

        for (i = 0; i < fctx.count && match_count < TAB_MAX_MATCHES; i++) {
            matches[match_count++] = fctx.matches[i];
        }

        /* ファイル名補完: ワードのプレフィックス部分を保持して補完 */
        if (match_count == 1) {
            /* 完全一致: ワード部分を書き換え */
            int new_pos = word_start;
            if (last_slash >= word_start) {
                /* ディレクトリ部分を保持 */
                new_pos = last_slash + 1;
            }
            i = 0;
            while (matches[0][i] && new_pos < CMD_BUF_SIZE - 2) {
                buf[new_pos++] = matches[0][i++];
            }
            /* ディレクトリでない場合はスペースを追加 */
            if (i > 0 && matches[0][i - 1] != '/') {
                buf[new_pos++] = ' ';
            }
            buf[new_pos] = '\0';
            return new_pos;
        }
        if (match_count > 1) {
            /* 共通プレフィックスを計算 */
            common_len = 0;
            for (;;) {
                char c = matches[0][common_len];
                int ok = 1;
                if (c == 0) break;
                for (i = 1; i < match_count; i++) {
                    if (matches[i][common_len] != c) { ok = 0; break; }
                }
                if (!ok) break;
                common_len++;
            }
            if (common_len > fctx.prefix_len) {
                int new_pos = (last_slash >= word_start) ? last_slash + 1 : word_start;
                for (i = 0; i < common_len && new_pos < CMD_BUF_SIZE - 1; i++) {
                    buf[new_pos++] = matches[0][i];
                }
                buf[new_pos] = '\0';
                return new_pos;
            }
            if (show_candidates) {
                g_api->kprintf(ATTR_WHITE, "%s", "\n");
                for (i = 0; i < match_count; i++)
                    g_api->kprintf(ATTR_CYAN, "  %s", matches[i]);
                g_api->kprintf(ATTR_WHITE, "%s", "\n");
                return -1;
            }
        }
        return pos;
    }

    /* コマンド名補完の共通処理 */
    if (match_count == 0) return pos;
    if (match_count == 1) {
        i = 0;
        while (matches[0][i] && i < CMD_BUF_SIZE - 2) { buf[i] = matches[0][i]; i++; }
        buf[i++] = ' '; buf[i] = 0;
        return i;
    }
    common_len = pos;
    for (;;) {
        char c = matches[0][common_len]; int ok = 1;
        if (c == 0) break;
        for (i = 1; i < match_count; i++) if (matches[i][common_len] != c) { ok = 0; break; }
        if (!ok) break;
        common_len++;
    }
    if (common_len > pos) {
        for (i = pos; i < common_len && i < CMD_BUF_SIZE - 1; i++) buf[i] = matches[0][i];
        buf[common_len] = 0;
        return common_len;
    }
    if (show_candidates) {
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
        for (i = 0; i < match_count; i++) g_api->kprintf(ATTR_CYAN, "  %s", matches[i]);
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
        return -1;
    }
    return pos;
}

void shell_run(void) {
    char cmd_buf[CMD_BUF_SIZE];
    int cmd_pos, cmd_len, key, last_tab = 0;

    g_api->kprintf(ATTR_CYAN, "%s", "================================\n");
    g_api->kprintf(ATTR_CYAN, "%s", " OS32 External Shell Started\n");
    g_api->kprintf(ATTR_CYAN, "%s", "================================\n");

    /* .profile の読み込み */
    {
        const char *home = env_get("HOME");
        if (home) {
            char profile_path[PATH_MAX_LEN];
            char profile_buf[2048];
            int pi = 0, sz;
            const char *h = home;
            while (*h && pi < PATH_MAX_LEN - 12) profile_path[pi++] = *h++;
            if (pi > 0 && profile_path[pi - 1] != '/') profile_path[pi++] = '/';
            { const char *pn = ".profile"; while (*pn) profile_path[pi++] = *pn++; }
            profile_path[pi] = '\0';

            sz = g_api->sys_read(profile_path, profile_buf, (int)sizeof(profile_buf) - 1);
            if (sz > 0) {
                int li = 0;
                char line[CMD_BUF_SIZE];
                int bi;
                profile_buf[sz] = '\0';
                for (bi = 0; bi <= sz; bi++) {
                    if (profile_buf[bi] == '\n' || profile_buf[bi] == '\0') {
                        line[li] = '\0';
                        /* コメント行・空行をスキップ */
                        if (li > 0 && line[0] != '#') {
                            execute_command(line);
                        }
                        li = 0;
                    } else if (li < CMD_BUF_SIZE - 1) {
                        line[li++] = profile_buf[bi];
                    }
                }
            }
        }
    }

    /* 初期化時に自動シリアル＆rshell開始 */
    g_api->serial_init(38400);
    execute_command("rshell");

    for (;;) {
        show_prompt();
        cmd_pos = cmd_len = cmd_buf[0] = prev_draw_len = 0;
        hist_idx = hist_count;

        for (;;) {
            key = g_api->ime_getkey();

            if ((key & 0xFF) == 0x0D) { g_api->shell_putchar('\n', ATTR_WHITE); break; }
            if ((key & 0xFF) == 0x08) {
                if (cmd_pos > 0) {
                    if (cmd_pos == cmd_len) {
                        cmd_pos--; cmd_len--; cmd_buf[cmd_len] = 0;
                        g_api->shell_putchar(0x08, ATTR_WHITE); prev_draw_len = cmd_len;
                    } else {
                        int i; for (i = cmd_pos - 1; i < cmd_len - 1; i++) cmd_buf[i] = cmd_buf[i+1];
                        cmd_len--; cmd_pos--; cmd_buf[cmd_len] = 0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                    }
                }
                continue;
            }
            if ((key >> 8) == 0x3A) { /* UP */
                if (hist_idx > 0 && hist_idx > hist_count - HIST_SIZE) {
                    int i = (--hist_idx) % HIST_SIZE; cmd_len = 0;
                    while (hist_buf[i][cmd_len]) { cmd_buf[cmd_len] = hist_buf[i][cmd_len]; cmd_len++; }
                    cmd_buf[cmd_len] = 0; cmd_pos = cmd_len; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x3D) { /* DOWN */
                if (hist_idx < hist_count - 1) {
                    int i = (++hist_idx) % HIST_SIZE; cmd_len = 0;
                    while (hist_buf[i][cmd_len]) { cmd_buf[cmd_len] = hist_buf[i][cmd_len]; cmd_len++; }
                    cmd_buf[cmd_len] = 0; cmd_pos = cmd_len; redraw_line(cmd_buf, cmd_len, cmd_pos);
                } else {
                    hist_idx = hist_count; cmd_len = cmd_pos = cmd_buf[0] = 0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x3B) { /* LEFT */
                if (cmd_pos > 0) { cmd_pos--; g_api->shell_putchar(0x08, ATTR_WHITE); } continue;
            }
            if ((key >> 8) == 0x3C) { /* RIGHT */
                if (cmd_pos < cmd_len) { g_api->shell_putchar(cmd_buf[cmd_pos], ATTR_WHITE); cmd_pos++; } continue;
            }
            if ((key >> 8) == 0x3E) { /* HOME */
                while (cmd_pos > 0) { cmd_pos--; g_api->shell_putchar(0x08, ATTR_WHITE); } continue;
            }
            if ((key >> 8) == 0x39) { /* DEL */
                if (cmd_pos < cmd_len) {
                    int i; for(i=cmd_pos; i<cmd_len-1; i++) cmd_buf[i]=cmd_buf[i+1];
                    cmd_len--; cmd_buf[cmd_len]=0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x0F || (key & 0xFF) == 0x09) { /* TAB */
                int npos = tab_complete(cmd_buf, cmd_len, last_tab);
                if (npos == -1) { show_prompt(); redraw_line(cmd_buf, cmd_len, cmd_pos); }
                else if (npos != cmd_len) { cmd_len = cmd_pos = npos; redraw_line(cmd_buf, cmd_len, cmd_pos); last_tab = 0; }
                else last_tab = 1;
                continue;
            }
            last_tab = 0;
            if ((key & 0xFF) == 0x1B) { cmd_len=cmd_pos=cmd_buf[0]=0; redraw_line(cmd_buf, cmd_len, cmd_pos); continue; }
            /* 印字可能文字: ASCII (0x20-0x7E) および IME確定UTF-8バイト (0x80+, scancode=0) */
            {
                u8 ascii_byte = (u8)(key & 0xFF);
                int scancode = (key >> 8) & 0x7F;
                int is_printable = (ascii_byte >= 0x20 && ascii_byte < 0x7F);
                int is_ime_byte  = (scancode == 0x00 && ascii_byte >= 0x80);
                if ((is_printable || is_ime_byte) && cmd_len < CMD_BUF_SIZE - 4) {
                    if (is_ime_byte) {
                        /* UTF-8マルチバイト: 蓄積して一括表示 */
                        char utf8_tmp[5];
                        int utf8_len = 0;
                        int expect;
                        utf8_tmp[utf8_len++] = (char)ascii_byte;
                        /* UTF-8先頭バイトから期待バイト数を判定 */
                        if ((ascii_byte & 0xE0) == 0xC0) expect = 2;
                        else if ((ascii_byte & 0xF0) == 0xE0) expect = 3;
                        else if ((ascii_byte & 0xF8) == 0xF0) expect = 4;
                        else expect = 1;
                        /* 後続バイトを読み取る */
                        while (utf8_len < expect) {
                            int nk = g_api->ime_getkey();
                            u8 nb = (u8)(nk & 0xFF);
                            if ((nb & 0xC0) != 0x80) break;
                            utf8_tmp[utf8_len++] = (char)nb;
                        }
                        utf8_tmp[utf8_len] = '\0';
                        /* コマンドバッファに追加 */
                        if (cmd_len + utf8_len < CMD_BUF_SIZE - 1) {
                            int bi;
                            if (cmd_pos == cmd_len) {
                                for (bi = 0; bi < utf8_len; bi++) {
                                    cmd_buf[cmd_pos++] = utf8_tmp[bi];
                                    cmd_len++;
                                }
                                cmd_buf[cmd_len] = 0;
                                g_api->shell_print_utf8(utf8_tmp, ATTR_WHITE);
                                prev_draw_len = cmd_len;
                            } else {
                                int i;
                                for (i = cmd_len - 1 + utf8_len; i >= cmd_pos + utf8_len; i--)
                                    cmd_buf[i] = cmd_buf[i - utf8_len];
                                for (bi = 0; bi < utf8_len; bi++)
                                    cmd_buf[cmd_pos++] = utf8_tmp[bi];
                                cmd_len += utf8_len;
                                cmd_buf[cmd_len] = 0;
                                redraw_line(cmd_buf, cmd_len, cmd_pos);
                            }
                        }
                    } else {
                        /* ASCII文字 */
                        char ch = (char)ascii_byte;
                        if (cmd_pos == cmd_len) {
                            cmd_buf[cmd_pos++] = ch; cmd_len++; cmd_buf[cmd_len] = 0;
                            g_api->shell_putchar(ch, ATTR_WHITE); prev_draw_len = cmd_len;
                        } else {
                            int i; for (i = cmd_len; i > cmd_pos; i--) cmd_buf[i] = cmd_buf[i-1];
                            cmd_buf[cmd_pos++] = ch; cmd_len++; cmd_buf[cmd_len] = 0;
                            redraw_line(cmd_buf, cmd_len, cmd_pos);
                        }
                    }
                }
            }
        }
        cmd_buf[cmd_len] = 0;
        if (cmd_len > 0) hist_add(cmd_buf);
        execute_command(cmd_buf);
    }
}
