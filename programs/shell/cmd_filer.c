/* ======================================================================== */
/*  CMD_FILER.C — シェル内蔵TVRAMファイラ                                    */
/*                                                                          */
/*  シェル内蔵コマンドとして実装することで、exec_run で起動したプログラムが    */
/*  終了した後もファイラ画面に復帰できる。                                   */
/*  (シェルは Level 0 (0x300000) に常駐しており、Level 1 のプログラムが       */
/*  0x400000 を上書きしてもシェルのコードは無傷で残る)                       */
/*                                                                          */
/*  操作: ←→↑↓=移動, Enter=開く/実行, BS=親ディレクトリ, Q/ESC=終了     */
/* ======================================================================== */

#include "shell.h"

/* ======================================================================== */
/*  定数                                                                    */
/* ======================================================================== */

/* 画面レイアウト (PC-98 TVRAM: 80桁×25行) */
#define FL_SCREEN_COLS      80
#define FL_SCREEN_ROWS      25

#define FL_HEADER_Y         0     /* ヘッダ行 (パス表示) */
#define FL_SEPARATOR_Y      1     /* 区切り線行 */
#define FL_LIST_TOP_Y       2     /* ファイルリスト開始行 */
#define FL_LIST_ROWS        21    /* ファイルリスト行数 (行2〜22) */
#define FL_FOOTER_Y         23    /* フッタ行 (ファイル情報) */
#define FL_HELP_Y           24    /* ヘルプ行 (キー操作) */

/* マルチカラム */
#define FL_NUM_COLUMNS      3     /* 表示カラム数 */
#define FL_COL_WIDTH        26    /* 1カラムの幅 */
#define FL_PAGE_ITEMS       (FL_NUM_COLUMNS * FL_LIST_ROWS)

/* 容量制限 */
#define FL_MAX_ENTRIES      256
#define FL_MAX_NAME_LEN     64
#define FL_MAX_PATH_LEN     256

/* ファイルタイプ関連付け */
#define FL_FILETYPES_PATH   "/etc/filetypes"
#define FL_FILETYPES_MAXSZ  8192   /* 設定ファイル最大サイズ */
#define FL_MAX_ASSOC        128    /* 最大関連付け数 */
#define FL_POPUP_TICKS      200    /* ポップアップ表示時間 (約2秒) */

/* TVRAM属性値 */
#define FL_ATTR_HEADER      0xA1  /* 水色 */
#define FL_ATTR_SEPARATOR   0x21  /* 暗い青 */
#define FL_ATTR_DIR         0xA1  /* ディレクトリ: 水色 */
#define FL_ATTR_EXE         0x81  /* 実行可能: 緑 */
#define FL_ATTR_FILE        0xE1  /* 通常ファイル: 白 */
#define FL_ATTR_SELECTED    0xE5  /* 選択: 白反転 */
#define FL_ATTR_DIR_SEL     0xA5  /* 選択ディレクトリ: 水色反転 */
#define FL_ATTR_EXE_SEL     0x85  /* 選択EXE: 緑反転 */
#define FL_ATTR_FOOTER      0xE1  /* フッタ: 白 */
#define FL_ATTR_HELP        0xC1  /* ヘルプ: 黄色 */
#define FL_ATTR_STATUS      0x81  /* ステータス: 緑 */

/* キーコード */
#define FL_KEY_ESC          0x1B
#define FL_KEY_ENTER        0x0D
#define FL_KEY_BS           0x08
#define FL_KEY_UP           0x1E
#define FL_KEY_DOWN         0x1F
#define FL_KEY_LEFT         0x1D
#define FL_KEY_RIGHT        0x1C

/* キーリピートウェイト (tick単位、1tick ≈ 10ms) */
#define FL_KEY_WAIT_TICKS   6

/* ======================================================================== */
/*  データ構造                                                              */
/* ======================================================================== */

typedef struct {
    char name[FL_MAX_NAME_LEN];
    u32  size;
    u8   is_dir;
    u8   is_exe;
} FL_Entry;

typedef struct {
    FL_Entry entries[FL_MAX_ENTRIES];
    int count;
    int cursor;
    int page_top;
    char cwd[FL_MAX_PATH_LEN];
} FL_State;

/* ファイルタイプ関連付けエントリ (ポインタはft_bufの中を指す) */
typedef struct {
    const char *ext;    /* ".txt" 等 (ドット付き) */
    const char *cmd;    /* "/usr/bin/edit.bin" 等 */
} FL_Assoc;

/* ======================================================================== */
/*  静的変数                                                                */
/* ======================================================================== */

static FL_State fl_state;
static int fl_running;

/* ファイルタイプ関連付け (動的確保) */
static char *ft_buf = NULL;       /* 設定ファイル読み込みバッファ */
static FL_Assoc *ft_table = NULL; /* 関連付けテーブル */
static int ft_count = 0;          /* 有効エントリ数 */

/* ======================================================================== */
/*  ファイルタイプ関連付け読み込み                                           */
/* ======================================================================== */

/* /etc/filetypes を読み込んでインプレース解析する */
static void ft_load(void)
{
    int fd, sz, i, line_start, got_eq;
    char *p;

    ft_count = 0;

    fd = g_api->sys_open(FL_FILETYPES_PATH, O_RDONLY);
    if (fd < 0) return;

    ft_buf = (char *)g_api->mem_alloc(FL_FILETYPES_MAXSZ);
    if (!ft_buf) { g_api->sys_close(fd); return; }

    sz = g_api->sys_read(fd, ft_buf, FL_FILETYPES_MAXSZ - 1);
    g_api->sys_close(fd);
    if (sz <= 0) { g_api->mem_free(ft_buf); ft_buf = NULL; return; }
    ft_buf[sz] = '\0';

    ft_table = (FL_Assoc *)g_api->mem_alloc(sizeof(FL_Assoc) * FL_MAX_ASSOC);
    if (!ft_table) { g_api->mem_free(ft_buf); ft_buf = NULL; return; }

    /* インプレース解析: '\n' と '=' を '\0' に置換してポインタを設定 */
    line_start = 0;
    for (i = 0; i <= sz; i++) {
        if (ft_buf[i] == '\n' || ft_buf[i] == '\0') {
            ft_buf[i] = '\0';
            p = &ft_buf[line_start];

            /* 空行・コメント行をスキップ */
            if (*p != '\0' && *p != '#' && ft_count < FL_MAX_ASSOC) {
                /* '=' を探す */
                got_eq = 0;
                {
                    char *q = p;
                    while (*q) {
                        if (*q == '=') {
                            *q = '\0';
                            ft_table[ft_count].ext = p;
                            ft_table[ft_count].cmd = q + 1;
                            /* cmd が空でなければ登録 */
                            if (*(q + 1) != '\0') {
                                ft_count++;
                            }
                            got_eq = 1;
                            break;
                        }
                        q++;
                    }
                }
                (void)got_eq;
            }
            line_start = i + 1;
        }
    }
}

/* ファイルタイプテーブルを解放する */
static void ft_free(void)
{
    if (ft_table) { g_api->mem_free(ft_table); ft_table = NULL; }
    if (ft_buf) { g_api->mem_free(ft_buf); ft_buf = NULL; }
    ft_count = 0;
}

/* ファイル名の拡張子から対応プログラムを検索。見つからなければNULL */
static const char *ft_find(const char *filename)
{
    int i, len, elen;
    const char *dot = NULL;
    const char *p;

    /* 最後の '.' を探す */
    p = filename;
    while (*p) { if (*p == '.') dot = p; p++; }
    if (!dot) return NULL;

    len = (int)(p - dot); /* 拡張子の長さ (ドット含む) */

    for (i = 0; i < ft_count; i++) {
        elen = strlen(ft_table[i].ext);
        if (elen == len && strncmp(dot, ft_table[i].ext, len) == 0) {
            return ft_table[i].cmd;
        }
    }
    return NULL;
}

/* ======================================================================== */
/*  パスユーティリティ                                                      */
/* ======================================================================== */

static void fl_path_join(char *out, int out_sz,
                         const char *dir, const char *name)
{
    int i = 0, j = 0;
    while (dir[j] && i < out_sz - 2) out[i++] = dir[j++];
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    j = 0;
    while (name[j] && i < out_sz - 1) out[i++] = name[j++];
    out[i] = '\0';
}

static void fl_path_parent(char *path)
{
    int len = 0;
    int last_slash = 0;
    int k;

    while (path[len]) len++;
    if (len > 1 && path[len - 1] == '/') len--;

    for (k = 0; k < len; k++) {
        if (path[k] == '/') last_slash = k;
    }

    if (last_slash == 0) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        path[last_slash] = '\0';
    }
}

/* ======================================================================== */
/*  ファイルスキャン                                                        */
/* ======================================================================== */

static int fl_is_bin_file(const char *name)
{
    int len = 0;
    const char *p = name;
    while (*p) { len++; p++; }
    if (len < 4) return 0;
    p = name + len - 4;
    if (p[0] != '.') return 0;
    if (p[1] == 'b' && p[2] == 'i' && p[3] == 'n') return 1;
    if (p[1] == 'B' && p[2] == 'I' && p[3] == 'N') return 1;
    return 0;
}

static void fl_ls_callback(const void *entry_raw, void *ctx)
{
    const char *name = (const char *)entry_raw;
    const u8 *base = (const u8 *)entry_raw;
    u32 size;
    u8 type;
    FL_Entry *e;
    int i;

    (void)ctx;

    if (fl_state.count >= FL_MAX_ENTRIES) return;

    size = *(const u32 *)(base + 256);
    type = base[260];

    if (name[0] == '.') {
        if (name[1] == '\0') return;
        if (name[1] == '.' && name[2] == '\0') return;
    }

    e = &fl_state.entries[fl_state.count];
    for (i = 0; name[i] && i < FL_MAX_NAME_LEN - 1; i++)
        e->name[i] = name[i];
    e->name[i] = '\0';
    e->size = size;
    e->is_dir = (type == OS32_FILE_TYPE_DIR) ? 1 : 0;
    e->is_exe = 0;
    if (type == OS32_FILE_TYPE_FILE && fl_is_bin_file(name))
        e->is_exe = 1;
    fl_state.count++;
}

static void fl_sort_entries(void)
{
    int start;
    int i, j;
    FL_Entry tmp;

    start = 0;
    if (fl_state.count > 0 &&
        fl_state.entries[0].name[0] == '.' &&
        fl_state.entries[0].name[1] == '.' &&
        fl_state.entries[0].name[2] == '\0') {
        start = 1;
    }

    for (i = start + 1; i < fl_state.count; i++) {
        memcpy(&tmp, &fl_state.entries[i], sizeof(FL_Entry));
        j = i - 1;
        while (j >= start) {
            int swap = 0;
            FL_Entry *a = &fl_state.entries[j];

            if (tmp.is_dir && !a->is_dir) {
                swap = 1;
            } else if (tmp.is_dir == a->is_dir) {
                if (strcmp(tmp.name, a->name) < 0) swap = 1;
            }

            if (!swap) break;
            memcpy(&fl_state.entries[j + 1], a, sizeof(FL_Entry));
            j--;
        }
        memcpy(&fl_state.entries[j + 1], &tmp, sizeof(FL_Entry));
    }
}

static void fl_scan_dir(void)
{
    fl_state.count = 0;
    fl_state.cursor = 0;
    fl_state.page_top = 0;

    if (!(fl_state.cwd[0] == '/' && fl_state.cwd[1] == '\0')) {
        FL_Entry *e = &fl_state.entries[0];
        e->name[0] = '.'; e->name[1] = '.'; e->name[2] = '\0';
        e->size = 0;
        e->is_dir = 1;
        e->is_exe = 0;
        fl_state.count = 1;
    }

    g_api->sys_ls(fl_state.cwd, (void *)fl_ls_callback, NULL);
    fl_sort_entries();
}

/* ======================================================================== */
/*  描画関数                                                                */
/* ======================================================================== */

static void fl_clear_line(int y, u8 attr)
{
    int x;
    for (x = 0; x < FL_SCREEN_COLS; x++)
        g_api->tvram_putchar_at(x, y, ' ', attr);
}

static void fl_draw_str(int x, int y, const char *s, int max_len, u8 attr)
{
    int max_x = x + max_len;
    if (max_x > FL_SCREEN_COLS) max_x = FL_SCREEN_COLS;
    while (*s && x < max_x) {
        g_api->tvram_putchar_at(x, y, *s, attr);
        s++;
        x++;
    }
}

static void fl_draw_number(int x, int y, u32 val, u8 attr)
{
    char buf[12];
    int len = 0;
    int i;

    if (val == 0) {
        g_api->tvram_putchar_at(x, y, '0', attr);
        return;
    }
    while (val > 0 && len < 11) {
        buf[len++] = '0' + (val % 10);
        val /= 10;
    }
    for (i = len - 1; i >= 0; i--) {
        g_api->tvram_putchar_at(x, y, buf[i], attr);
        x++;
    }
}

static void fl_draw_size_str(int x, int y, u32 size, u8 attr)
{
    char buf[16];
    int len = 0;
    u32 val;

    if (size >= 1048576) {
        val = size / 1048576;
        while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
        { int i; for (i = len - 1; i >= 0; i--) g_api->tvram_putchar_at(x++, y, buf[i], attr); }
        fl_draw_str(x, y, " MB", 3, attr);
    } else if (size >= 1024) {
        val = size / 1024;
        while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
        { int i; for (i = len - 1; i >= 0; i--) g_api->tvram_putchar_at(x++, y, buf[i], attr); }
        fl_draw_str(x, y, " KB", 3, attr);
    } else {
        val = size;
        if (val == 0) {
            g_api->tvram_putchar_at(x++, y, '0', attr);
        } else {
            while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
            { int i; for (i = len - 1; i >= 0; i--) g_api->tvram_putchar_at(x++, y, buf[i], attr); }
        }
        fl_draw_str(x, y, " B", 2, attr);
    }
}

static void fl_draw_header(void)
{
    char count_buf[16];
    int n, ci, x;

    fl_clear_line(FL_HEADER_Y, FL_ATTR_HEADER);
    fl_draw_str(0, FL_HEADER_Y, "[Filer] ", 8, FL_ATTR_HEADER);
    fl_draw_str(8, FL_HEADER_Y, fl_state.cwd, FL_SCREEN_COLS - 20, FL_ATTR_HEADER);

    n = fl_state.count;
    ci = 0;
    if (n >= 100) count_buf[ci++] = '0' + (n / 100) % 10;
    if (n >= 10)  count_buf[ci++] = '0' + (n / 10) % 10;
    count_buf[ci++] = '0' + (n % 10);
    count_buf[ci++] = ' ';
    count_buf[ci++] = 'i'; count_buf[ci++] = 't'; count_buf[ci++] = 'e';
    count_buf[ci++] = 'm'; count_buf[ci++] = 's';
    count_buf[ci] = '\0';
    x = FL_SCREEN_COLS - ci - 1;
    fl_draw_str(x, FL_HEADER_Y, count_buf, ci, FL_ATTR_STATUS);
}

static void fl_draw_separator(void)
{
    int x;
    for (x = 0; x < FL_SCREEN_COLS; x++)
        g_api->tvram_putchar_at(x, FL_SEPARATOR_Y, '-', FL_ATTR_SEPARATOR);
}

static void fl_draw_entry(int col_x, int y, const FL_Entry *e, int selected)
{
    u8 attr;
    u8 bg_attr;
    int x;
    const char *name;
    int max_name;

    if (selected) {
        if (e->is_dir) attr = FL_ATTR_DIR_SEL;
        else if (e->is_exe) attr = FL_ATTR_EXE_SEL;
        else attr = FL_ATTR_SELECTED;
        bg_attr = attr;
    } else {
        if (e->is_dir) attr = FL_ATTR_DIR;
        else if (e->is_exe) attr = FL_ATTR_EXE;
        else attr = FL_ATTR_FILE;
        bg_attr = FL_ATTR_FILE;
    }

    for (x = col_x; x < col_x + FL_COL_WIDTH && x < FL_SCREEN_COLS; x++)
        g_api->tvram_putchar_at(x, y, ' ', bg_attr);

    x = col_x + 1;
    max_name = FL_COL_WIDTH - 2;

    if (e->is_dir) {
        g_api->tvram_putchar_at(x, y, '/', attr);
        x++;
        max_name--;
    }

    name = e->name;
    while (*name && max_name > 0 && x < FL_SCREEN_COLS) {
        g_api->tvram_putchar_at(x, y, *name, attr);
        name++;
        x++;
        max_name--;
    }
}

static void fl_draw_entry_at(int idx, int selected)
{
    int page_idx, col, row, col_x, y;

    if (idx < fl_state.page_top || idx >= fl_state.page_top + FL_PAGE_ITEMS) return;
    if (idx >= fl_state.count) return;

    page_idx = idx - fl_state.page_top;
    row = page_idx / FL_NUM_COLUMNS;
    col = page_idx % FL_NUM_COLUMNS;
    col_x = col * FL_COL_WIDTH;
    y = FL_LIST_TOP_Y + row;

    fl_draw_entry(col_x, y, &fl_state.entries[idx], selected);
}

static void fl_draw_list(void)
{
    int i, idx;
    int col, row;
    int col_x, y;

    for (i = 0; i < FL_LIST_ROWS; i++)
        fl_clear_line(FL_LIST_TOP_Y + i, FL_ATTR_FILE);

    for (i = 0; i < FL_PAGE_ITEMS; i++) {
        idx = fl_state.page_top + i;
        if (idx >= fl_state.count) break;

        row = i / FL_NUM_COLUMNS;
        col = i % FL_NUM_COLUMNS;
        col_x = col * FL_COL_WIDTH;
        y = FL_LIST_TOP_Y + row;

        fl_draw_entry(col_x, y, &fl_state.entries[idx], (idx == fl_state.cursor));
    }
}

static void fl_draw_footer(void)
{
    FL_Entry *e;

    fl_clear_line(FL_FOOTER_Y, FL_ATTR_FOOTER);

    if (fl_state.count == 0) {
        fl_draw_str(2, FL_FOOTER_Y, "(empty directory)", 20, FL_ATTR_FOOTER);
        return;
    }

    e = &fl_state.entries[fl_state.cursor];

    if (e->is_dir) {
        fl_draw_str(2, FL_FOOTER_Y, "Dir: ", 5, FL_ATTR_FOOTER);
        fl_draw_str(7, FL_FOOTER_Y, e->name, 30, FL_ATTR_DIR);
    } else {
        fl_draw_str(2, FL_FOOTER_Y, "File: ", 6, FL_ATTR_FOOTER);
        fl_draw_str(8, FL_FOOTER_Y, e->name, 30, FL_ATTR_FOOTER);
        fl_draw_str(42, FL_FOOTER_Y, "Size: ", 6, FL_ATTR_FOOTER);
        fl_draw_size_str(48, FL_FOOTER_Y, e->size, FL_ATTR_STATUS);
    }

    {
        int pos_x = 68;
        fl_draw_number(pos_x, FL_FOOTER_Y, (u32)(fl_state.cursor + 1), FL_ATTR_STATUS);
        {
            int digits = 1;
            u32 tmp = (u32)(fl_state.cursor + 1);
            while (tmp >= 10) { digits++; tmp /= 10; }
            pos_x += digits;
        }
        g_api->tvram_putchar_at(pos_x, FL_FOOTER_Y, '/', FL_ATTR_FOOTER);
        pos_x++;
        fl_draw_number(pos_x, FL_FOOTER_Y, (u32)fl_state.count, FL_ATTR_STATUS);
    }
}

static void fl_draw_help(void)
{
    int total_pages, current_page;
    char page_buf[16];
    int pi, x;

    fl_clear_line(FL_HELP_Y, FL_ATTR_HELP);
    fl_draw_str(0, FL_HELP_Y, "[Enter]Open  [BS]Parent  [Q]Quit", 40, FL_ATTR_HELP);

    if (fl_state.count > FL_PAGE_ITEMS) {
        total_pages = (fl_state.count + FL_PAGE_ITEMS - 1) / FL_PAGE_ITEMS;
        current_page = (fl_state.page_top / FL_PAGE_ITEMS) + 1;
        pi = 0;
        page_buf[pi++] = 'P'; page_buf[pi++] = 'a'; page_buf[pi++] = 'g';
        page_buf[pi++] = 'e'; page_buf[pi++] = ' ';
        if (current_page >= 10) page_buf[pi++] = '0' + (current_page / 10);
        page_buf[pi++] = '0' + (current_page % 10);
        page_buf[pi++] = '/';
        if (total_pages >= 10) page_buf[pi++] = '0' + (total_pages / 10);
        page_buf[pi++] = '0' + (total_pages % 10);
        page_buf[pi] = '\0';
        x = FL_SCREEN_COLS - pi - 1;
        fl_draw_str(x, FL_HELP_Y, page_buf, pi, FL_ATTR_STATUS);
    }
}

static void fl_draw_all(void)
{
    fl_draw_header();
    fl_draw_separator();
    fl_draw_list();
    fl_draw_footer();
    fl_draw_help();
}

static void fl_draw_cursor_update(int old_cursor)
{
    fl_draw_entry_at(old_cursor, 0);
    fl_draw_entry_at(fl_state.cursor, 1);
    fl_draw_footer();
}

/* ======================================================================== */
/*  カーソル操作とアクション                                                */
/* ======================================================================== */

static int fl_move_cursor(int delta)
{
    int old_page_top = fl_state.page_top;
    int new_cursor = fl_state.cursor + delta;

    if (new_cursor < 0) new_cursor = 0;
    if (new_cursor >= fl_state.count) new_cursor = fl_state.count - 1;
    if (new_cursor < 0) new_cursor = 0;

    fl_state.cursor = new_cursor;

    if (fl_state.cursor < fl_state.page_top) {
        fl_state.page_top = (fl_state.cursor / FL_PAGE_ITEMS) * FL_PAGE_ITEMS;
    }
    if (fl_state.cursor >= fl_state.page_top + FL_PAGE_ITEMS) {
        fl_state.page_top = (fl_state.cursor / FL_PAGE_ITEMS) * FL_PAGE_ITEMS;
    }

    return (fl_state.page_top != old_page_top) ? 1 : 0;
}

static void fl_action_parent(void)
{
    fl_path_parent(fl_state.cwd);
    fl_scan_dir();
}

/* プログラムを実行して復帰する共通処理 */
static void fl_exec_program(const char *cmdline)
{
    g_api->tvram_clear();
    g_api->exec_run(cmdline);
    g_api->gfx_shutdown();
    fl_scan_dir();
}

/* フッタ行にポップアップメッセージを一時表示 */
static void fl_popup_message(const char *msg, u8 attr)
{
    u32 start;
    fl_clear_line(FL_FOOTER_Y, attr);
    fl_draw_str(2, FL_FOOTER_Y, msg, FL_SCREEN_COLS - 4, attr);
    start = g_api->get_tick();
    while ((g_api->get_tick() - start) < FL_POPUP_TICKS) {
        if (g_api->kbd_trygetkey() >= 0) break;
        g_api->sys_halt();
    }
    /* キーバッファをフラッシュ */
    while (g_api->kbd_trygetkey() >= 0)
        ;
}

static void fl_action_enter(void)
{
    FL_Entry *e;

    if (fl_state.count == 0) return;
    e = &fl_state.entries[fl_state.cursor];

    if (e->is_dir) {
        if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
            fl_action_parent();
        } else {
            char new_cwd[FL_MAX_PATH_LEN];
            fl_path_join(new_cwd, FL_MAX_PATH_LEN, fl_state.cwd, e->name);
            strncpy(fl_state.cwd, new_cwd, FL_MAX_PATH_LEN - 1);
            fl_state.cwd[FL_MAX_PATH_LEN - 1] = '\0';
            fl_scan_dir();
        }
    } else if (e->is_exe) {
        /* .bin ファイル → 直接実行 */
        char fullpath[FL_MAX_PATH_LEN];
        fl_path_join(fullpath, FL_MAX_PATH_LEN, fl_state.cwd, e->name);
        fl_exec_program(fullpath);
    } else {
        /* ファイルタイプ関連付けで開く */
        const char *prog = ft_find(e->name);
        if (prog) {
            /* "prog filepath" のコマンドラインを構築 */
            char cmdline[FL_MAX_PATH_LEN * 2];
            char fullpath[FL_MAX_PATH_LEN];
            int ci = 0;
            const char *s;

            fl_path_join(fullpath, FL_MAX_PATH_LEN, fl_state.cwd, e->name);

            s = prog;
            while (*s && ci < (int)sizeof(cmdline) - 2) cmdline[ci++] = *s++;
            cmdline[ci++] = ' ';
            s = fullpath;
            while (*s && ci < (int)sizeof(cmdline) - 1) cmdline[ci++] = *s++;
            cmdline[ci] = '\0';

            fl_exec_program(cmdline);
        } else {
            fl_popup_message("No program associated with this file type", 0x41);
        }
    }
}

/* ======================================================================== */
/*  ファイラメインループ                                                    */
/* ======================================================================== */

static void fl_init(const char *start_dir)
{
    memset(&fl_state, 0, sizeof(fl_state));
    strncpy(fl_state.cwd, start_dir, FL_MAX_PATH_LEN - 1);
    fl_state.cwd[FL_MAX_PATH_LEN - 1] = '\0';
    ft_load();
    fl_scan_dir();
}

static void fl_loop(void)
{
    int need_full_redraw = 1;
    fl_running = 1;
    g_api->tvram_clear();

    while (fl_running) {
        int ch;
        int old_cursor;
        int page_changed;

        if (need_full_redraw) {
            fl_draw_all();
            need_full_redraw = 0;
        }

        ch = g_api->kbd_getchar();
        old_cursor = fl_state.cursor;

        switch (ch) {
        case FL_KEY_UP:
            page_changed = fl_move_cursor(-FL_NUM_COLUMNS);
            if (page_changed) need_full_redraw = 1;
            else fl_draw_cursor_update(old_cursor);
            break;
        case FL_KEY_DOWN:
            page_changed = fl_move_cursor(FL_NUM_COLUMNS);
            if (page_changed) need_full_redraw = 1;
            else fl_draw_cursor_update(old_cursor);
            break;
        case FL_KEY_LEFT:
            page_changed = fl_move_cursor(-1);
            if (page_changed) need_full_redraw = 1;
            else fl_draw_cursor_update(old_cursor);
            break;
        case FL_KEY_RIGHT:
            page_changed = fl_move_cursor(1);
            if (page_changed) need_full_redraw = 1;
            else fl_draw_cursor_update(old_cursor);
            break;
        case FL_KEY_ENTER:
            fl_action_enter();
            need_full_redraw = 1;
            break;
        case FL_KEY_BS:
            fl_action_parent();
            need_full_redraw = 1;
            break;
        case FL_KEY_ESC:
        case 'q':
        case 'Q':
            fl_running = 0;
            break;
        default:
            break;
        }

        /* キーリピート制御 */
        {
            u32 wait_start = g_api->get_tick();
            while ((g_api->get_tick() - wait_start) < FL_KEY_WAIT_TICKS) {
                g_api->sys_halt();
            }
            while (g_api->kbd_trygetkey() >= 0)
                ;
        }
    }
}

/* ======================================================================== */
/*  シェルコマンドハンドラ                                                  */
/* ======================================================================== */

static void cmd_filer(int argc, char **argv)
{
    const char *start_dir;

    if (argc > 1) {
        start_dir = argv[1];
    } else {
        start_dir = g_api->sys_getcwd();
    }

    fl_init(start_dir);
    fl_loop();
    ft_free();
    g_api->tvram_clear();
}

/* 登録用テーブル */
static const ShellCmd filer_cmds[] = {
    { "filer", cmd_filer, "[dir]", "File manager" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_filer_init(void)
{
    shell_register_cmds(filer_cmds);
}
