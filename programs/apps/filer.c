/* ======================================================================== */
/*  FILER.C — OS32 TVRAMベースCUIファイラ                                    */
/*                                                                          */
/*  PC-98 テキストVRAM (80x25) を使用したマルチカラムファイルマネージャ。    */
/*  ディレクトリ閲覧、.bin プログラム実行に対応。                            */
/*                                                                          */
/*  操作: ←→↑↓=移動, Enter=開く/実行, BS=親ディレクトリ, Q/ESC=終了     */
/* ======================================================================== */

#include <string.h>
#include "os32api.h"

/* ======================================================================== */
/*  定数                                                                    */
/* ======================================================================== */

/* 画面レイアウト (PC-98 TVRAM: 80桁×25行) */
#define SCREEN_COLS      80
#define SCREEN_ROWS      25

#define HEADER_Y         0     /* ヘッダ行 (パス表示) */
#define SEPARATOR_Y      1     /* 区切り線行 */
#define LIST_TOP_Y       2     /* ファイルリスト開始行 */
#define LIST_ROWS        21    /* ファイルリスト行数 (行2〜22) */
#define FOOTER_Y         23    /* フッタ行 (ファイル情報) */
#define HELP_Y           24    /* ヘルプ行 (キー操作) */

/* マルチカラム */
#define NUM_COLUMNS      3     /* 表示カラム数 */
#define COL_WIDTH         26   /* 1カラムの幅 */
#define PAGE_ITEMS        (NUM_COLUMNS * LIST_ROWS)

/* 容量制限 */
#define MAX_ENTRIES      512
#define MAX_NAME_LEN     64
#define MAX_PATH_LEN     256

/* TVRAM属性値 (pc98.h TATTR_*) */
#define ATTR_HEADER      0xA1  /* 水色 */
#define ATTR_SEPARATOR   0x21  /* 暗い青 */
#define ATTR_DIR         0xA1  /* ディレクトリ: 水色 */
#define ATTR_EXE         0x81  /* 実行可能: 緑 */
#define ATTR_FILE        0xE1  /* 通常ファイル: 白 */
#define ATTR_SELECTED    0xE5  /* 選択: 白反転 */
#define ATTR_DIR_SEL     0xA5  /* 選択ディレクトリ: 水色反転 */
#define ATTR_EXE_SEL     0x85  /* 選択EXE: 緑反転 */
#define ATTR_FOOTER      0xE1  /* フッタ: 白 */
#define ATTR_HELP        0xC1  /* ヘルプ: 黄色 */
#define ATTR_STATUS      0x81  /* ステータス: 緑 */

/* キーコード (kbd_getchar が返すASCIIコード) */
#define FKEY_ESC          0x1B
#define FKEY_ENTER        0x0D
#define FKEY_BS           0x08
#define FKEY_UP           0x1E
#define FKEY_DOWN         0x1F
#define FKEY_LEFT         0x1D
#define FKEY_RIGHT        0x1C

/* キーリピートウェイト (tick単位、1tick ≈ 10ms) */
#define KEY_WAIT_TICKS    6    /* 約60ms — 押しっぱなし時のカーソル移動速度 */

/* ======================================================================== */
/*  データ構造                                                              */
/* ======================================================================== */

typedef struct {
    char name[MAX_NAME_LEN];
    u32  size;
    u8   is_dir;
    u8   is_exe;
} FilerEntry;

typedef struct {
    FilerEntry entries[MAX_ENTRIES];
    int count;
    int cursor;
    int page_top;
    char cwd[MAX_PATH_LEN];
} FilerState;

/* ======================================================================== */
/*  グローバル変数                                                          */
/* ======================================================================== */

static KernelAPI *api;
static FilerState state;
static int running;  /* メインループ制御フラグ */

/* ======================================================================== */
/*  パスユーティリティ                                                      */
/* ======================================================================== */

static void path_join(char *out, int out_sz,
                      const char *dir, const char *name)
{
    int i = 0, j = 0;
    while (dir[j] && i < out_sz - 2) out[i++] = dir[j++];
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    j = 0;
    while (name[j] && i < out_sz - 1) out[i++] = name[j++];
    out[i] = '\0';
}

static void path_parent(char *path)
{
    int len = 0;
    int last_slash = 0;
    int k;

    while (path[len]) len++;

    /* 末尾の / を除去して探索 */
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

/* 拡張子が .bin かチェック (大文字小文字対応) */
static int is_bin_file(const char *name)
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

/* sys_ls コールバック (DirEntry_Ext バイナリレイアウト) */
static void ls_callback(const void *entry_raw, void *ctx)
{
    const char *name = (const char *)entry_raw;
    const u8 *base = (const u8 *)entry_raw;
    u32 size;
    u8 type;
    FilerEntry *e;
    int i;

    (void)ctx;

    if (state.count >= MAX_ENTRIES) return;

    size = *(const u32 *)(base + 256);
    type = base[260];

    /* "." と ".." をスキップ (手動追加済み) */
    if (name[0] == '.') {
        if (name[1] == '\0') return;
        if (name[1] == '.' && name[2] == '\0') return;
    }

    e = &state.entries[state.count];
    for (i = 0; name[i] && i < MAX_NAME_LEN - 1; i++)
        e->name[i] = name[i];
    e->name[i] = '\0';
    e->size = size;
    e->is_dir = (type == OS32_FILE_TYPE_DIR) ? 1 : 0;
    e->is_exe = 0;
    if (type == OS32_FILE_TYPE_FILE && is_bin_file(name))
        e->is_exe = 1;
    state.count++;
}

/* 挿入ソート: ディレクトリ優先 → アルファベット順 */
static void sort_entries(void)
{
    int start;
    int i, j;
    FilerEntry tmp;

    /* ".." がインデックス0にある場合はスキップ */
    start = 0;
    if (state.count > 0 &&
        state.entries[0].name[0] == '.' &&
        state.entries[0].name[1] == '.' &&
        state.entries[0].name[2] == '\0') {
        start = 1;
    }

    for (i = start + 1; i < state.count; i++) {
        memcpy(&tmp, &state.entries[i], sizeof(FilerEntry));
        j = i - 1;
        while (j >= start) {
            int swap = 0;
            FilerEntry *a = &state.entries[j];

            /* ディレクトリ優先 */
            if (tmp.is_dir && !a->is_dir) {
                swap = 1;
            } else if (tmp.is_dir == a->is_dir) {
                /* 同種の場合はアルファベット順 */
                if (strcmp(tmp.name, a->name) < 0) swap = 1;
            }

            if (!swap) break;
            memcpy(&state.entries[j + 1], a, sizeof(FilerEntry));
            j--;
        }
        memcpy(&state.entries[j + 1], &tmp, sizeof(FilerEntry));
    }
}

/* カレントディレクトリを走査してentriesを構築 */
static void scan_dir(void)
{
    state.count = 0;
    state.cursor = 0;
    state.page_top = 0;

    /* ".." エントリを手動追加 (ルートでなければ) */
    if (!(state.cwd[0] == '/' && state.cwd[1] == '\0')) {
        FilerEntry *e = &state.entries[0];
        e->name[0] = '.'; e->name[1] = '.'; e->name[2] = '\0';
        e->size = 0;
        e->is_dir = 1;
        e->is_exe = 0;
        state.count = 1;
    }

    api->sys_ls(state.cwd, (void *)ls_callback, NULL);
    sort_entries();
}

/* ======================================================================== */
/*  描画関数                                                                */
/* ======================================================================== */

/* 指定行を空白で埋める */
static void clear_line(int y, u8 attr)
{
    int x;
    for (x = 0; x < SCREEN_COLS; x++)
        api->tvram_putchar_at(x, y, ' ', attr);
}

/* 指定位置に文字列を描画 (最大max_len文字) */
static void draw_str(int x, int y, const char *s, int max_len, u8 attr)
{
    int max_x = x + max_len;
    if (max_x > SCREEN_COLS) max_x = SCREEN_COLS;
    while (*s && x < max_x) {
        api->tvram_putchar_at(x, y, *s, attr);
        s++;
        x++;
    }
}

/* 数値を文字列化して描画 */
static void draw_number(int x, int y, u32 val, u8 attr)
{
    char buf[12];
    int len = 0;
    int i;

    if (val == 0) {
        api->tvram_putchar_at(x, y, '0', attr);
        return;
    }
    while (val > 0 && len < 11) {
        buf[len++] = '0' + (val % 10);
        val /= 10;
    }
    for (i = len - 1; i >= 0; i--) {
        api->tvram_putchar_at(x, y, buf[i], attr);
        x++;
    }
}

/* サイズを人間が読みやすい形式に変換 */
static void draw_size_str(int x, int y, u32 size, u8 attr)
{
    char buf[16];
    int len = 0;
    u32 val;

    if (size >= 1048576) {
        val = size / 1048576;
        while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
        { int i; for (i = len - 1; i >= 0; i--) api->tvram_putchar_at(x++, y, buf[i], attr); }
        draw_str(x, y, " MB", 3, attr);
    } else if (size >= 1024) {
        val = size / 1024;
        while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
        { int i; for (i = len - 1; i >= 0; i--) api->tvram_putchar_at(x++, y, buf[i], attr); }
        draw_str(x, y, " KB", 3, attr);
    } else {
        val = size;
        if (val == 0) {
            api->tvram_putchar_at(x++, y, '0', attr);
        } else {
            while (val > 0 && len < 10) { buf[len++] = '0' + (val % 10); val /= 10; }
            { int i; for (i = len - 1; i >= 0; i--) api->tvram_putchar_at(x++, y, buf[i], attr); }
        }
        draw_str(x, y, " B", 2, attr);
    }
}

/* ヘッダ行 (Y=0) */
static void draw_header(void)
{
    char count_buf[16];
    int n, ci, x;

    clear_line(HEADER_Y, ATTR_HEADER);
    draw_str(0, HEADER_Y, "[Filer] ", 8, ATTR_HEADER);
    draw_str(8, HEADER_Y, state.cwd, SCREEN_COLS - 20, ATTR_HEADER);

    /* エントリ数を右端に表示 */
    n = state.count;
    ci = 0;
    if (n >= 100) count_buf[ci++] = '0' + (n / 100) % 10;
    if (n >= 10)  count_buf[ci++] = '0' + (n / 10) % 10;
    count_buf[ci++] = '0' + (n % 10);
    count_buf[ci++] = ' ';
    count_buf[ci++] = 'i'; count_buf[ci++] = 't'; count_buf[ci++] = 'e';
    count_buf[ci++] = 'm'; count_buf[ci++] = 's';
    count_buf[ci] = '\0';
    x = SCREEN_COLS - ci - 1;
    draw_str(x, HEADER_Y, count_buf, ci, ATTR_STATUS);
}

/* 区切り線 (Y=1) */
static void draw_separator(void)
{
    int x;
    for (x = 0; x < SCREEN_COLS; x++)
        api->tvram_putchar_at(x, SEPARATOR_Y, '-', ATTR_SEPARATOR);
}

/* 1エントリの描画 (カラム幅全体を上書きするのでクリア不要) */
static void draw_entry(int col_x, int y, const FilerEntry *e, int selected)
{
    u8 attr;
    u8 bg_attr;
    int x;
    const char *name;
    int max_name;

    /* 属性の決定 */
    if (selected) {
        if (e->is_dir) attr = ATTR_DIR_SEL;
        else if (e->is_exe) attr = ATTR_EXE_SEL;
        else attr = ATTR_SELECTED;
        bg_attr = attr;
    } else {
        if (e->is_dir) attr = ATTR_DIR;
        else if (e->is_exe) attr = ATTR_EXE;
        else attr = ATTR_FILE;
        bg_attr = ATTR_FILE;
    }

    /* カラム幅全体を背景色でクリア（チラツキ防止） */
    for (x = col_x; x < col_x + COL_WIDTH && x < SCREEN_COLS; x++)
        api->tvram_putchar_at(x, y, ' ', bg_attr);

    /* ファイル名を描画 */
    x = col_x + 1;
    max_name = COL_WIDTH - 2;

    if (e->is_dir) {
        api->tvram_putchar_at(x, y, '/', attr);
        x++;
        max_name--;
    }

    name = e->name;
    while (*name && max_name > 0 && x < SCREEN_COLS) {
        api->tvram_putchar_at(x, y, *name, attr);
        name++;
        x++;
        max_name--;
    }
}

/* インデックスから画面座標を計算してエントリを描画 (横方向優先) */
static void draw_entry_at(int idx, int selected)
{
    int page_idx, col, row, col_x, y;

    if (idx < state.page_top || idx >= state.page_top + PAGE_ITEMS) return;
    if (idx >= state.count) return;

    page_idx = idx - state.page_top;
    row = page_idx / NUM_COLUMNS;
    col = page_idx % NUM_COLUMNS;
    col_x = col * COL_WIDTH;
    y = LIST_TOP_Y + row;

    draw_entry(col_x, y, &state.entries[idx], selected);
}

/* ファイルリスト全体 (Y=2〜22, 横方向優先: 左から右に埋めてから次の行へ) */
static void draw_list(void)
{
    int i, idx;
    int col, row;
    int col_x, y;

    /* リスト領域をクリア */
    for (i = 0; i < LIST_ROWS; i++)
        clear_line(LIST_TOP_Y + i, ATTR_FILE);

    /* エントリを横方向優先で描画 */
    for (i = 0; i < PAGE_ITEMS; i++) {
        idx = state.page_top + i;
        if (idx >= state.count) break;

        row = i / NUM_COLUMNS;
        col = i % NUM_COLUMNS;
        col_x = col * COL_WIDTH;
        y = LIST_TOP_Y + row;

        draw_entry(col_x, y, &state.entries[idx], (idx == state.cursor));
    }
}

/* フッタ行 (Y=23) */
static void draw_footer(void)
{
    FilerEntry *e;

    clear_line(FOOTER_Y, ATTR_FOOTER);

    if (state.count == 0) {
        draw_str(2, FOOTER_Y, "(empty directory)", 20, ATTR_FOOTER);
        return;
    }

    e = &state.entries[state.cursor];

    if (e->is_dir) {
        draw_str(2, FOOTER_Y, "Dir: ", 5, ATTR_FOOTER);
        draw_str(7, FOOTER_Y, e->name, 30, ATTR_DIR);
    } else {
        draw_str(2, FOOTER_Y, "File: ", 6, ATTR_FOOTER);
        draw_str(8, FOOTER_Y, e->name, 30, ATTR_FOOTER);
        draw_str(42, FOOTER_Y, "Size: ", 6, ATTR_FOOTER);
        draw_size_str(48, FOOTER_Y, e->size, ATTR_STATUS);
    }

    /* カーソル位置 / 総数 を右端に表示 */
    {
        int pos_x = 68;
        draw_number(pos_x, FOOTER_Y, (u32)(state.cursor + 1), ATTR_STATUS);
        {
            int digits = 1;
            u32 tmp = (u32)(state.cursor + 1);
            while (tmp >= 10) { digits++; tmp /= 10; }
            pos_x += digits;
        }
        api->tvram_putchar_at(pos_x, FOOTER_Y, '/', ATTR_FOOTER);
        pos_x++;
        draw_number(pos_x, FOOTER_Y, (u32)state.count, ATTR_STATUS);
    }
}

/* ヘルプ行 (Y=24) */
static void draw_help(void)
{
    int total_pages, current_page;
    char page_buf[16];
    int pi, x;

    clear_line(HELP_Y, ATTR_HELP);
    draw_str(0, HELP_Y, "[Enter]Open  [BS]Parent  [Q]Quit", 40, ATTR_HELP);

    /* ページ情報 */
    if (state.count > PAGE_ITEMS) {
        total_pages = (state.count + PAGE_ITEMS - 1) / PAGE_ITEMS;
        current_page = (state.page_top / PAGE_ITEMS) + 1;
        pi = 0;
        page_buf[pi++] = 'P';
        page_buf[pi++] = 'a';
        page_buf[pi++] = 'g';
        page_buf[pi++] = 'e';
        page_buf[pi++] = ' ';
        if (current_page >= 10) page_buf[pi++] = '0' + (current_page / 10);
        page_buf[pi++] = '0' + (current_page % 10);
        page_buf[pi++] = '/';
        if (total_pages >= 10) page_buf[pi++] = '0' + (total_pages / 10);
        page_buf[pi++] = '0' + (total_pages % 10);
        page_buf[pi] = '\0';
        x = SCREEN_COLS - pi - 1;
        draw_str(x, HELP_Y, page_buf, pi, ATTR_STATUS);
    }
}

/* 画面全体の再描画 (ディレクトリ変更時のみ使用) */
static void draw_all(void)
{
    draw_header();
    draw_separator();
    draw_list();
    draw_footer();
    draw_help();
}

/* カーソル差分描画 (前のカーソル位置を消して新しい位置をハイライト) */
static void draw_cursor_update(int old_cursor)
{
    /* 前のカーソル位置を通常表示に戻す */
    draw_entry_at(old_cursor, 0);
    /* 新しいカーソル位置をハイライト表示 */
    draw_entry_at(state.cursor, 1);
    /* フッタ更新 (選択ファイル情報) */
    draw_footer();
}

/* ======================================================================== */
/*  カーソル操作とアクション                                                */
/* ======================================================================== */

/* カーソル移動。ページが変わったかどうかを返す (1=ページ変更、0=同一ページ) */
static int move_cursor(int delta)
{
    int old_page_top = state.page_top;
    int new_cursor = state.cursor + delta;

    if (new_cursor < 0) new_cursor = 0;
    if (new_cursor >= state.count) new_cursor = state.count - 1;
    if (new_cursor < 0) new_cursor = 0;

    state.cursor = new_cursor;

    /* ページスクロール */
    if (state.cursor < state.page_top) {
        state.page_top = (state.cursor / PAGE_ITEMS) * PAGE_ITEMS;
    }
    if (state.cursor >= state.page_top + PAGE_ITEMS) {
        state.page_top = (state.cursor / PAGE_ITEMS) * PAGE_ITEMS;
    }

    return (state.page_top != old_page_top) ? 1 : 0;
}

static void action_parent(void)
{
    path_parent(state.cwd);
    scan_dir();
}

static void action_enter(void)
{
    FilerEntry *e;

    if (state.count == 0) return;
    e = &state.entries[state.cursor];

    if (e->is_dir) {
        if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
            action_parent();
        } else {
            char new_cwd[MAX_PATH_LEN];
            path_join(new_cwd, MAX_PATH_LEN, state.cwd, e->name);
            strncpy(state.cwd, new_cwd, MAX_PATH_LEN - 1);
            state.cwd[MAX_PATH_LEN - 1] = '\0';
            scan_dir();
        }
    } else if (e->is_exe) {
        char fullpath[MAX_PATH_LEN];
        path_join(fullpath, MAX_PATH_LEN, state.cwd, e->name);
        api->tvram_clear();
        api->exec_run(fullpath);
        /* 実行後、ファイラ画面を再構築 */
        scan_dir();
    }
}

/* ======================================================================== */
/*  メインエントリ                                                          */
/* ======================================================================== */

static void filer_init(const char *start_dir)
{
    memset(&state, 0, sizeof(state));
    strncpy(state.cwd, start_dir, MAX_PATH_LEN - 1);
    state.cwd[MAX_PATH_LEN - 1] = '\0';
    scan_dir();
}

static void filer_loop(void)
{
    int need_full_redraw = 1;
    running = 1;
    api->tvram_clear();

    while (running) {
        int ch;
        int old_cursor;
        int page_changed;

        /* 描画 */
        if (need_full_redraw) {
            draw_all();
            need_full_redraw = 0;
        }

        /* キー入力 (ブロッキング) */
        ch = api->kbd_getchar();
        old_cursor = state.cursor;

        switch (ch) {
        case FKEY_UP:
            page_changed = move_cursor(-NUM_COLUMNS);
            if (page_changed) need_full_redraw = 1;
            else draw_cursor_update(old_cursor);
            break;
        case FKEY_DOWN:
            page_changed = move_cursor(NUM_COLUMNS);
            if (page_changed) need_full_redraw = 1;
            else draw_cursor_update(old_cursor);
            break;
        case FKEY_LEFT:
            page_changed = move_cursor(-1);
            if (page_changed) need_full_redraw = 1;
            else draw_cursor_update(old_cursor);
            break;
        case FKEY_RIGHT:
            page_changed = move_cursor(1);
            if (page_changed) need_full_redraw = 1;
            else draw_cursor_update(old_cursor);
            break;
        case FKEY_ENTER:
            action_enter();
            need_full_redraw = 1;
            break;
        case FKEY_BS:
            action_parent();
            need_full_redraw = 1;
            break;
        case FKEY_ESC:
        case 'q':
        case 'Q':
            running = 0;
            break;
        default:
            break;
        }

        /* カーソル移動後のウェイト: 一定時間待ってからバッファを破棄 */
        /* これにより適度なキーリピート速度になる */
        {
            u32 wait_start = api->get_tick();
            while ((api->get_tick() - wait_start) < KEY_WAIT_TICKS) {
                api->sys_halt();
            }
            while (api->kbd_trygetkey() >= 0)
                ;
        }
    }
}

void main(int argc, char **argv, KernelAPI *sys_api)
{
    const char *start_dir;

    api = sys_api;

    /* 初期ディレクトリの決定 */
    if (argc > 1) {
        start_dir = argv[1];
    } else {
        start_dir = api->sys_getcwd();
    }

    filer_init(start_dir);
    filer_loop();
    api->tvram_clear();
}

