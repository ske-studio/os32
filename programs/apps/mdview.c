/* ======================================================================== */
/*  MDVIEW.C — GFXグラフィカルMarkdownビューア                              */
/*                                                                          */
/*  640x400 GFXモードでMarkdownファイルをカラー表示するビューア。            */
/*  KCGスケーリングによる可変フォントサイズ (H1:48px, H2:32px, 本文:16px)。 */
/*                                                                          */
/*  描画ロジックは libmd/md_render.c に分離済み。                           */
/*  本ファイルはUI/ページング/検索/目次/ファイラー連携を担当。              */
/*                                                                          */
/*  Usage: exec mdview FILE.md                                               */
/*                                                                          */
/*  キー操作:                                                               */
/*    Space/ROLLDOWN: 次ページ   b/ROLLUP: 前ページ                         */
/*    ↑/↓: 行スクロール         HOME: 先頭   q/ESC: 終了                   */
/*    /: 検索  n: 次の検索結果  t: 目次ジャンプ                          */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "os32api.h"
#include "libos32gfx.h"
#include "libmd/libmd.h"
#include "libmd/md_render.h"
/* #define OS32_DBG_SERIAL */
#include "libos32/dbgserial.h"
#include "libfiler/libfiler.h"

static KernelAPI *api;

/* 退避用パレット */
static u8 saved_palette[16][3];

/* スクロール行高さ (TEXT_LINE_Hに相当) */
#define SCROLL_LINE_H  20

/* ======================================================================== */
/*  GFX初期化/終了 (パレット退避・復元)                                     */
/* ======================================================================== */

static void mdview_gfx_init(void)
{
    int i;

    /* GFX初期化 */
    libos32gfx_init(api);

    /* パレット退避 */
    for (i = 0; i < 16; i++) {
        api->gfx_get_palette(i,
            &saved_palette[i][0],
            &saved_palette[i][1],
            &saved_palette[i][2]);
    }

    /* mdview独自パレット設定 */
    for (i = 0; i < 16; i++) {
        api->gfx_set_palette(i,
            md_palette[i][0],
            md_palette[i][1],
            md_palette[i][2]);
    }
}

static void mdview_gfx_shutdown(void)
{
    int i;

    /* VRAMクリア */
    gfx_clear(0);
    gfx_present();
    api->gfx_present_dirty();

    /* パレット復元 */
    for (i = 0; i < 16; i++) {
        api->gfx_set_palette(i,
            saved_palette[i][0],
            saved_palette[i][1],
            saved_palette[i][2]);
    }

    /* GFXシャットダウン (テキストモード復帰) */
    libos32gfx_shutdown();

    /* テキストVRAMクリア */
    api->tvram_clear();
}

/* ======================================================================== */
/*  キーコード定数                                                          */
/* ======================================================================== */

#define VK_ROLLUP   0x36
#define VK_ROLLDOWN 0x37
#define VK_UP       0x3A
#define VK_DOWN     0x3D
#define VK_HOME     0x3E
#define VK_HELP     0x3F
#define VK_F1       0x62

#define KEYCODE(k)  (((k) >> 8) & 0x7F)
#define KEYDATA(k)  ((k) & 0xFF)

/* ======================================================================== */
/*  検索機能                                                                */
/* ======================================================================== */

static MdSearchState search_state;

/* 大文字小文字を無視して比較 */
static int ci_match(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* ノードテキスト内に検索語が含まれるか */
static int node_contains(const MdNode *n, const char *term, int term_len)
{
    int i, j;
    int tlen;

    if (term_len == 0 || !n->text) return 0;
    tlen = n->text_len;
    if (tlen < term_len) return 0;

    for (i = 0; i <= tlen - term_len; i++) {
        int match = 1;
        for (j = 0; j < term_len; j++) {
            if (!ci_match(n->text[i + j], term[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* start_idxから検索してマッチするノードのindexを返す (-1=見つからない) */
static int find_next(MdDocument *doc, int start_idx)
{
    int i;
    for (i = start_idx; i < doc->node_count; i++) {
        if (node_contains(&doc->nodes[i], search_state.term, search_state.len)) {
            return i;
        }
    }
    /* 先頭からラップ検索 */
    for (i = 0; i < start_idx && i < doc->node_count; i++) {
        if (node_contains(&doc->nodes[i], search_state.term, search_state.len)) {
            return i;
        }
    }
    return -1;
}

/* 検索語入力 UI (ステータスバーに入力欄を表示) */
static int do_search_input(void)
{
    int ch;
    search_state.len = 0;
    search_state.term[0] = '\0';

    for (;;) {
        char prompt[80];
        /* ステータスバーに検索プロンプトを描画 */
        gfx_fill_rect(0, MDR_PAGE_H, MDR_SCREEN_W, MDR_STATUS_H, MDR_COL_STATUS_BG);
        sprintf(prompt, " /%s_", search_state.term);
        kcg_draw_utf8(4, MDR_PAGE_H + 2, prompt, MDR_COL_H1, 0xFF);
        api->gfx_add_dirty_rect(0, MDR_PAGE_H, MDR_SCREEN_W, MDR_STATUS_H);
        api->gfx_present_dirty();

        ch = api->kbd_getchar();

        if (ch == 0x0D || ch == 0x0A) {
            if (search_state.len > 0) {
                search_state.active = 1;
                md_render_set_search(&search_state);
                return 1;
            }
            return 0;
        } else if (ch == 0x1B) {
            search_state.len = 0;
            search_state.term[0] = '\0';
            return 0;
        } else if (ch == 0x08 || ch == 0x7F) {
            if (search_state.len > 0) {
                search_state.len--;
                search_state.term[search_state.len] = '\0';
            }
        } else if (ch >= 0x20 && ch < 0x7F && search_state.len < MDR_SEARCH_MAX - 1) {
            search_state.term[search_state.len++] = (char)ch;
            search_state.term[search_state.len] = '\0';
        }
    }
}

/* ======================================================================== */
/*  目次ジャンプ (TOC)                                                      */
/* ======================================================================== */

#define TOC_MAX 64

static int toc_indices[TOC_MAX];
static int toc_count = 0;

static void build_toc(MdDocument *doc)
{
    int i;
    toc_count = 0;
    for (i = 0; i < doc->node_count && toc_count < TOC_MAX; i++) {
        if (doc->nodes[i].type == MD_H1 || doc->nodes[i].type == MD_H2) {
            toc_indices[toc_count++] = i;
        }
    }
}

static void draw_toc_overlay(MdDocument *doc, int cursor)
{
    int i;
    int ox = 40;
    int oy = 20;
    int ow = 560;
    int line_h = 20;
    int max_visible;
    int scroll_top = 0;
    int oh;

    max_visible = (MDR_PAGE_H - 80) / line_h;
    if (max_visible > toc_count) max_visible = toc_count;
    oh = max_visible * line_h + 40;

    if (cursor >= scroll_top + max_visible) {
        scroll_top = cursor - max_visible + 1;
    }
    if (cursor < scroll_top) {
        scroll_top = cursor;
    }

    gfx_fill_rect(ox, oy, ow, oh, MDR_COL_CODE_BG);
    gfx_rect(ox, oy, ow, oh, MDR_COL_RULER);

    kcg_draw_utf8(ox + 8, oy + 4, "-- TABLE OF CONTENTS --", MDR_COL_H2, 0xFF);

    for (i = 0; i < max_visible && (scroll_top + i) < toc_count; i++) {
        int idx = toc_indices[scroll_top + i];
        MdNode *n = &doc->nodes[idx];
        int ly = oy + 24 + i * line_h;
        u8 fg;
        char buf[72];
        int blen;
        const char *prefix;

        if (scroll_top + i == cursor) {
            gfx_fill_rect(ox + 2, ly, ow - 4, line_h, MDR_COL_STATUS_BG);
        }

        fg = (n->type == MD_H1) ? MDR_COL_H1 : MDR_COL_H2;
        prefix = (n->type == MD_H1) ? "# " : "  ## ";

        blen = 0;
        {
            const char *s = prefix;
            while (*s && blen < 70) buf[blen++] = *s++;
        }
        {
            const char *s = n->text;
            int j;
            for (j = 0; j < n->text_len && blen < 70; j++) {
                buf[blen++] = s[j];
            }
        }
        buf[blen] = '\0';

        kcg_draw_utf8(ox + 12, ly + 2, buf, fg, 0xFF);
    }

    kcg_draw_utf8(ox + 8, oy + oh - 18,
                  "Up/Down:Select  Enter:Jump  ESC:Cancel",
                  MDR_COL_STATUS_FG, 0xFF);
}

static int do_toc_jump(MdDocument *doc)
{
    int cursor = 0;

    if (toc_count == 0) return -1;

    for (;;) {
        int key, kd, kc;

        draw_toc_overlay(doc, cursor);
        gfx_present();
        api->gfx_present_dirty();

        key = api->kbd_getkey();
        kd = KEYDATA(key);
        kc = KEYCODE(key);

        if (kd == 0x1B) {
            return -1;
        } else if (kd == 0x0D || kd == 0x0A) {
            return toc_indices[cursor];
        } else if (kc == VK_UP) {
            if (cursor > 0) cursor--;
        } else if (kc == VK_DOWN) {
            if (cursor < toc_count - 1) cursor++;
        } else if (kc == VK_HOME) {
            cursor = 0;
        }
    }
}

/* ======================================================================== */
/*  メイン                                                                  */
/* ======================================================================== */

int main(int argc, char **argv, KernelAPI *kapi)
{
    static char file_buf[65536];
    static MdDocument doc;
    static char cur_filename[256];
    int fd, sz;
    int total_height;
    int scroll_y;
    int quit;
    int last_search_idx;

    api = kapi;

    /* 引数チェック */
    if (argc < 2) {
        printf("mdview - GFX Markdown Viewer for OS32\n");
        printf("Usage: mdview FILE.md\n");
        printf("Keys: Space/b=page, /=search, n=next, t=TOC, F1=open, q=quit\n");
        return 1;
    }

    /* ファイル名をコピー */
    strncpy(cur_filename, argv[1], sizeof(cur_filename) - 1);
    cur_filename[sizeof(cur_filename) - 1] = '\0';

    /* ファイル読み込み */
    fd = api->sys_open(cur_filename, O_RDONLY);
    if (fd < 0) {
        printf("mdview: %s: No such file\n", cur_filename);
        return 1;
    }
    sz = api->sys_read(fd, file_buf, sizeof(file_buf) - 1);
    api->sys_close(fd);
    if (sz <= 0) {
        printf("mdview: %s: Empty file\n", cur_filename);
        return 1;
    }
    file_buf[sz] = '\0';

    /* パース */
    md_parse(&doc, file_buf, sz);
    if (doc.node_count == 0) {
        printf("mdview: No content\n");
        return 0;
    }

    /* GFX初期化 (パレット退避+設定) */
    mdview_gfx_init();
    kcg_set_scale(1);

    /* レンダラー初期化 */
    md_render_init(api);

    /* ファイラー初期化 */
    filer_init(api);

    /* 検索状態初期化 */
    memset(&search_state, 0, sizeof(search_state));

    /* レイアウト計算 */
    total_height = md_layout(&doc);
    scroll_y = 0;
    last_search_idx = 0;

    /* 目次構築 */
    build_toc(&doc);

    /* 初回描画 */
    md_render_page(&doc, scroll_y);
    md_render_statusbar(cur_filename, scroll_y, total_height);
    api->gfx_add_dirty_rect(0, 0, MDR_SCREEN_W, MDR_SCREEN_H);
    api->gfx_present_dirty();

    /* イベントループ */
    quit = 0;
    while (!quit) {
        int key = api->kbd_getkey();
        int kd = KEYDATA(key);
        int kc = KEYCODE(key);
        int need_redraw = 0;
        int max_scroll;

        max_scroll = total_height - MDR_PAGE_H;
        if (max_scroll < 0) max_scroll = 0;

        if (kd == 'q' || kd == 'Q' || kd == 0x1B) {
            quit = 1;

        } else if (kd == ' ' || kc == VK_ROLLDOWN) {
            scroll_y += MDR_PAGE_H;
            need_redraw = 1;

        } else if (kd == 'b' || kd == 'B' || kc == VK_ROLLUP) {
            scroll_y -= MDR_PAGE_H;
            need_redraw = 1;

        } else if (kc == VK_DOWN) {
            scroll_y += SCROLL_LINE_H;
            need_redraw = 1;

        } else if (kc == VK_UP) {
            scroll_y -= SCROLL_LINE_H;
            need_redraw = 1;

        } else if (kc == VK_HOME) {
            scroll_y = 0;
            need_redraw = 1;

        } else if (kd == '/') {
            /* 検索入力 */
            if (do_search_input()) {
                int found = find_next(&doc, 0);
                if (found >= 0) {
                    scroll_y = md_render_get_node_y(found);
                    last_search_idx = found;
                    search_state.current_idx = found;
                    md_render_set_search(&search_state);
                }
            } else {
                search_state.current_idx = -1;
                md_render_set_search(&search_state);
            }
            md_render_reset_scroll();
            need_redraw = 1;

        } else if (kd == 'n' || kd == 'N') {
            /* 次の検索結果 */
            if (search_state.active) {
                int found = find_next(&doc, last_search_idx + 1);
                if (found >= 0) {
                    scroll_y = md_render_get_node_y(found);
                    last_search_idx = found;
                    search_state.current_idx = found;
                    md_render_set_search(&search_state);
                    md_render_reset_scroll();
                    need_redraw = 1;
                }
            }

        } else if (kd == 't' || kd == 'T') {
            /* 目次ジャンプ */
            int toc_result = do_toc_jump(&doc);
            if (toc_result >= 0) {
                scroll_y = md_render_get_node_y(toc_result);
            }
            need_redraw = 1;

        } else if (kc == VK_F1) {
            /* F1: ファイラーでファイルを開く */
            if (filer_open("/", NULL)) {
                const char *path = filer_get_selected_path();
                int new_fd, new_sz;

                new_fd = api->sys_open(path, O_RDONLY);
                if (new_fd >= 0) {
                    new_sz = api->sys_read(new_fd, file_buf,
                                           sizeof(file_buf) - 1);
                    api->sys_close(new_fd);

                    if (new_sz > 0) {
                        file_buf[new_sz] = '\0';
                        md_parse(&doc, file_buf, new_sz);

                        strncpy(cur_filename, path,
                                sizeof(cur_filename) - 1);
                        cur_filename[sizeof(cur_filename) - 1] = '\0';

                        total_height = md_layout(&doc);
                        scroll_y = 0;
                        last_search_idx = 0;
                        search_state.active = 0;
                        md_render_set_search(&search_state);

                        build_toc(&doc);

                        md_render_reset_scroll();
                    }
                }
            }
            need_redraw = 1;
        }

        /* scroll_yクランプ */
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;

        if (need_redraw) {
            md_render_page(&doc, scroll_y);
            md_render_statusbar(cur_filename, scroll_y, total_height);
            api->gfx_add_dirty_rect(0, 0, MDR_SCREEN_W, MDR_SCREEN_H);
            api->gfx_present_dirty();
        }
    }

    /* クリーンアップ */
    mdview_gfx_shutdown();
    return 0;
}
