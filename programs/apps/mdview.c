/* ======================================================================== */
/*  MDVIEW.C — GFXグラフィカルMarkdownビューア                              */
/*                                                                          */
/*  640x400 GFXモードでMarkdownファイルをカラー表示するビューア。            */
/*  KCGスケーリングによる可変フォントサイズ (H1:48px, H2:32px, 本文:16px)。 */
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
/* #define OS32_DBG_SERIAL */
#include "libos32/dbgserial.h"
#include "libfiler/libfiler.h"

static KernelAPI *api;

/* ======================================================================== */
/*  定数                                                                    */
/* ======================================================================== */

#define SCREEN_W        640
#define SCREEN_H        400

#define MARGIN_LEFT     16     /* 本文・リスト・コード等の左マージン */
#define MARGIN_RIGHT    16
#define CONTENT_W       (SCREEN_W - MARGIN_LEFT - MARGIN_RIGHT)  /* 608px */

#define STATUS_H        20     /* ステータスバー高さ */
#define PAGE_H          (SCREEN_H - STATUS_H)  /* 380px */

/* 見出しインデント (見出しだけがインデントされる) */
#define INDENT_H1       32
#define INDENT_H2       24
#define INDENT_H3       16

/* フォントサイズ (KCGスケール別) */
#define FONT_H_S1       16    /* scale=1 の行高さ */
#define FONT_W_ANK_S1   8     /* scale=1 のANK幅 */
#define FONT_W_KJ_S1    16    /* scale=1 の漢字幅 */

#define FONT_H_S2       32    /* scale=2 */
#define FONT_W_ANK_S2   16
#define FONT_W_KJ_S2    32

#define FONT_H_S3       48    /* scale=3 */
#define FONT_W_ANK_S3   24
#define FONT_W_KJ_S3    48

/* 行間・余白 */
#define TEXT_LINE_GAP   4
#define TEXT_LINE_H     (FONT_H_S1 + TEXT_LINE_GAP)  /* 20px */

#define H1_MARGIN_TOP   12
#define H1_MARGIN_BTM   8
#define H1_BORDER_H     2

#define H2_MARGIN_TOP   10
#define H2_MARGIN_BTM   6
#define H2_BORDER_H     1

#define H3_MARGIN_TOP   6
#define H3_MARGIN_BTM   4

#define CODE_PAD_V      4     /* コードブロック上下パディング */
#define CODE_PAD_H      8     /* コードブロック左右パディング */
#define CODE_LINE_H     18

#define BLANK_H         10
#define HRULE_H         12

#define LIST_BULLET_W   12    /* bullet記号の幅 */

#define TABLE_ROW_H     20    /* テーブル1行の高さ */
#define TABLE_PAD       4     /* テーブルセル内パディング */

#define QUOTE_BAR_W     3     /* 引用左バー幅 */
#define QUOTE_PAD       8     /* 引用バー後の余白 */
#define QUOTE_MARGIN    4     /* 引用上下余白 */

/* ======================================================================== */
/*  パレット                                                                */
/* ======================================================================== */

enum {
    COL_BG         = 0,
    COL_H1         = 1,
    COL_H2         = 2,
    COL_H3         = 3,
    COL_TEXT       = 4,
    COL_CODE_FG    = 5,
    COL_CODE_BG    = 6,
    COL_RULER      = 7,
    COL_BOLD       = 8,
    COL_LINK       = 9,
    COL_BULLET     = 10,
    COL_H1_BAR     = 11,
    COL_STATUS_BG  = 12,
    COL_STATUS_FG  = 13,
    COL_SEARCH_BG  = 14,  /* 検索ハイライト背景 */
    COL_QUOTE_BAR  = 15   /* 引用バー / テーブル交互背景 */
};

static const unsigned char md_palette[16][3] = {
    /*  R,  G,  B  (各0-15) */
    {  1,  1,  3 },  /* 0: 背景 ダークネイビー */
    { 15, 13,  4 },  /* 1: H1 ゴールド */
    {  5, 12, 15 },  /* 2: H2 スカイブルー */
    {  4, 14, 10 },  /* 3: H3 エメラルド */
    { 15, 15, 15 },  /* 4: 本文 白 */
    { 12,  9, 15 },  /* 5: コード ラベンダー */
    {  3,  3,  4 },  /* 6: コードブロック背景 */
    {  7,  7,  8 },  /* 7: 装飾線 グレー */
    { 15,  7,  6 },  /* 8: 太字 サーモン */
    {  5, 13, 15 },  /* 9: リンク シアン */
    { 10, 10, 10 },  /* 10: bullet */
    { 11,  9,  3 },  /* 11: H1バー 暗ゴールド */
    {  2,  2,  4 },  /* 12: ステータスBG */
    { 13, 13, 14 },  /* 13: ステータスFG */
    { 15, 14,  4 },  /* 14: 検索ハイライト (明るい黄色) */
    {  6, 10, 13 }   /* 15: 引用バー / テーブル交互 (青灰) */
};

/* 退避用パレット */
static u8 saved_palette[16][3];

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
/*  ワードラップ                                                            */
/* ======================================================================== */

#define WRAP_MAX_LINES 32

typedef struct {
    const char *start[WRAP_MAX_LINES];
    int         len[WRAP_MAX_LINES];
    int         count;
} WrapResult;

/* UTF-8の1文字のバイト数を返す */
static int utf8_char_bytes(const unsigned char *p)
{
    if (*p < 0x80) return 1;
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    if ((*p & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 1文字のピクセル幅を返す (scale=1ベース) */
static int char_pixel_width(const unsigned char *p)
{
    /* ASCII: 半角 = 8px, マルチバイト: 全角 = 16px */
    if (*p < 0x80) return FONT_W_ANK_S1;
    return FONT_W_KJ_S1;
}

static void word_wrap(const char *text, int text_len,
                      int max_width_px, WrapResult *out)
{
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + text_len;
    const unsigned char *line_start = p;
    const unsigned char *last_space = NULL;
    int cur_width = 0;

    out->count = 0;

    while (p < end && out->count < WRAP_MAX_LINES) {
        int cw;
        int cb;

        if (*p == '\0') break;

        cb = utf8_char_bytes(p);
        cw = char_pixel_width(p);

        /* スペースを折り返し候補として記録 */
        if (*p == ' ') {
            last_space = p;
        }

        /* 幅超過 → 折り返し */
        if (cur_width + cw > max_width_px) {
            if (last_space && last_space > line_start) {
                /* スペースで折り返し */
                out->start[out->count] = (const char *)line_start;
                out->len[out->count] = (int)(last_space - line_start);
                out->count++;
                p = last_space + 1;
            } else {
                /* 強制折り返し */
                out->start[out->count] = (const char *)line_start;
                out->len[out->count] = (int)(p - line_start);
                out->count++;
            }
            line_start = p;
            last_space = NULL;
            cur_width = 0;
            continue;
        }

        cur_width += cw;
        p += cb;
    }

    /* 残りの部分 */
    if (p > line_start && out->count < WRAP_MAX_LINES) {
        out->start[out->count] = (const char *)line_start;
        out->len[out->count] = (int)(p - line_start);
        out->count++;
    }

    /* 空テキストの場合 */
    if (out->count == 0) {
        out->start[0] = text;
        out->len[0] = 0;
        out->count = 1;
    }
}

/* ======================================================================== */
/*  レイアウト計算                                                          */
/* ======================================================================== */

/* 各ノードの仮想Y座標とコードブロック連続範囲 */
static int node_y[MD_MAX_NODES];
static int node_height[MD_MAX_NODES];

/* コードブロックの連続範囲を検出するフラグ */
static int node_code_first[MD_MAX_NODES]; /* コードブロック連続の最初 */
static int node_code_last[MD_MAX_NODES];  /* コードブロック連続の最後 */

static int layout_pass(MdDocument *doc)
{
    int y = 8; /* 上マージン */
    int i;
    WrapResult wrap;

    /* コードブロック連続範囲の検出 */
    for (i = 0; i < doc->node_count; i++) {
        node_code_first[i] = 0;
        node_code_last[i] = 0;
    }
    for (i = 0; i < doc->node_count; i++) {
        if (doc->nodes[i].type == MD_CODE_BLOCK) {
            if (i == 0 || doc->nodes[i - 1].type != MD_CODE_BLOCK) {
                node_code_first[i] = 1;
            }
            if (i == doc->node_count - 1 || doc->nodes[i + 1].type != MD_CODE_BLOCK) {
                node_code_last[i] = 1;
            }
        }
    }

    for (i = 0; i < doc->node_count; i++) {
        MdNode *n = &doc->nodes[i];
        int h = 0;

        node_y[i] = y;

        switch (n->type) {
        case MD_H1:
            h = H1_MARGIN_TOP + FONT_H_S3 + H1_BORDER_H + H1_MARGIN_BTM;
            break;
        case MD_H2:
            h = H2_MARGIN_TOP + FONT_H_S2 + H2_BORDER_H + H2_MARGIN_BTM;
            break;
        case MD_H3:
            h = H3_MARGIN_TOP + FONT_H_S1 + H3_MARGIN_BTM;
            break;
        case MD_PARAGRAPH:
            word_wrap(n->text, n->text_len, CONTENT_W, &wrap);
            h = wrap.count * TEXT_LINE_H;
            break;
        case MD_LIST_ITEM:
            word_wrap(n->text, n->text_len,
                      CONTENT_W - LIST_BULLET_W, &wrap);
            h = wrap.count * TEXT_LINE_H;
            break;
        case MD_CODE_BLOCK:
            h = CODE_LINE_H;
            if (node_code_first[i]) h += CODE_PAD_V;
            if (node_code_last[i]) h += CODE_PAD_V;
            break;
        case MD_TABLE_ROW:
            h = TABLE_ROW_H;
            break;
        case MD_BLOCKQUOTE:
            word_wrap(n->text, n->text_len,
                      CONTENT_W - QUOTE_BAR_W - QUOTE_PAD, &wrap);
            h = wrap.count * TEXT_LINE_H + QUOTE_MARGIN;
            break;
        case MD_HRULE:
            h = HRULE_H;
            break;
        case MD_BLANK:
            h = BLANK_H;
            break;
        }

        node_height[i] = h;
        y += h;
    }

    return y + 8; /* 下マージン */
}

/* ======================================================================== */
/*  テキスト描画 (スパン対応)                                               */
/* ======================================================================== */

/* 前方宣言 (検索関連 — 定義はファイル後方) */
extern int kcg_scale;
static int ci_match(char a, char b);
static int node_contains(const MdNode *n, const char *term, int term_len);

/* 検索状態 (前方宣言 — 定義はファイル後方の検索セクション) */
#define SEARCH_MAX 32
static char search_term[SEARCH_MAX];
static int  search_len;
static int  search_active;
static int  search_current_idx;

/* KCGスケールを切り替えて文字列を描画 */
static int draw_text_simple(int x, int y, const char *text, int len, u8 color)
{
    char buf[256];
    int n;

    n = (len < 255) ? len : 255;
    memcpy(buf, text, n);
    buf[n] = '\0';
    return kcg_draw_utf8(x, y, buf, color, 0xFF);
}

/* テキスト内の指定バイト位置のピクセルX座標を計算 */
static int byte_to_px(const char *text, int byte_pos)
{
    const unsigned char *p = (const unsigned char *)text;
    int px = 0;
    int pos = 0;
    int scale = kcg_scale;

    while (pos < byte_pos && p[pos]) {
        if (p[pos] >= 0xE0) {
            px += 16 * scale;  /* UTF-8 3バイト = 全角 */
            pos += 3;
        } else if (p[pos] >= 0xC0) {
            px += 8 * scale;   /* UTF-8 2バイト */
            pos += 2;
        } else {
            px += 8 * scale;   /* ASCII */
            pos += 1;
        }
    }
    return px;
}

/* 検索ハイライト付きテキスト描画 */
static int draw_text_highlighted(int x, int y, const char *text, int len,
                                 u8 color, u8 hl_bg, int is_current)
{
    int i, j;
    int scale = kcg_scale;
    int font_h = 16 * scale;

    /* 現在のマッチノードには左サイドバー */
    if (is_current) {
        gfx_fill_rect(x - 6, y, 3, font_h, COL_H1);
    }

    /* マッチ箇所の背景をハイライト */
    for (i = 0; i <= len - search_len; i++) {
        int match = 1;
        for (j = 0; j < search_len; j++) {
            if (!ci_match(text[i + j], search_term[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            int hl_x = x + byte_to_px(text, i);
            int hl_w = byte_to_px(text + i, search_len);
            gfx_fill_rect(hl_x, y, hl_w, font_h, hl_bg);
        }
    }

    /* テキスト本体を描画 */
    return draw_text_simple(x, y, text, len, color);
}

/* スパン付きテキスト描画 (1物理行分) */
static void draw_spans(int x, int y, const char *full_text,
                       const MdSpan *spans, int span_count, u8 base_color)
{
    int i;
    int cx = x;

    for (i = 0; i < span_count; i++) {
        u8 color;
        const char *seg_text;
        int seg_len;

        switch (spans[i].type) {
        case MD_SPAN_CODE: color = COL_CODE_FG; break;
        case MD_SPAN_BOLD: color = COL_BOLD; break;
        default:           color = base_color; break;
        }

        seg_text = full_text + spans[i].start;
        seg_len = spans[i].len;
        cx += draw_text_simple(cx, y, seg_text, seg_len, color);
    }
}

/* 折り返し済みテキストを描画 (段落/リスト用) */
static int draw_wrapped_text(int x, int y,
                             const char *text, int text_len,
                             int max_width_px, u8 color,
                             const MdSpan *spans, int span_count)
{
    WrapResult wrap;
    int line;
    int cy = y;

    word_wrap(text, text_len, max_width_px, &wrap);

    for (line = 0; line < wrap.count; line++) {
        /* 簡易: 折り返し後の各行はスパン情報なしで単色描画 */
        /* (スパンが行をまたぐケースは将来拡張) */
        if (wrap.count == 1 && span_count > 0) {
            /* 1行で収まる場合のみスパン描画 */
            draw_spans(x, cy, text, spans, span_count, color);
        } else {
            draw_text_simple(x, cy, wrap.start[line], wrap.len[line], color);
        }
        cy += TEXT_LINE_H;
    }

    return cy - y;
}

/* ======================================================================== */
/*  ノード描画                                                              */
/* ======================================================================== */

static void render_node(MdDocument *doc, MdNode *n, int y_offset, int idx)
{
    int draw_y = node_y[idx] - y_offset;
    int draw_x;

    /* 画面外チェック */
    if (draw_y + node_height[idx] < 0) return;
    if (draw_y >= PAGE_H) return;

    switch (n->type) {
    case MD_H1:
        draw_y += H1_MARGIN_TOP;
        draw_x = MARGIN_LEFT + INDENT_H1;
        /* 左装飾バー */
        gfx_fill_rect(MARGIN_LEFT, draw_y, 4, FONT_H_S3, COL_H1_BAR);
        /* テキスト (scale=3) */
        kcg_set_scale(3);
        draw_text_simple(draw_x, draw_y, n->text, n->text_len, COL_H1);
        kcg_set_scale(1);
        /* 下線 */
        draw_y += FONT_H_S3 + 2;
        gfx_hline(MARGIN_LEFT, draw_y, CONTENT_W, COL_RULER);
        if (H1_BORDER_H > 1) {
            gfx_hline(MARGIN_LEFT, draw_y + 1, CONTENT_W, COL_RULER);
        }
        break;

    case MD_H2:
        draw_y += H2_MARGIN_TOP;
        draw_x = MARGIN_LEFT + INDENT_H2;
        /* テキスト (scale=2) */
        kcg_set_scale(2);
        draw_text_simple(draw_x, draw_y, n->text, n->text_len, COL_H2);
        kcg_set_scale(1);
        /* 下線 */
        draw_y += FONT_H_S2 + 2;
        gfx_hline(MARGIN_LEFT, draw_y, CONTENT_W, COL_RULER);
        break;

    case MD_H3:
        draw_y += H3_MARGIN_TOP;
        draw_x = MARGIN_LEFT + INDENT_H3;
        /* 左装飾マーカー */
        gfx_fill_rect(MARGIN_LEFT + INDENT_H3 - 6, draw_y + 2, 3, 12, COL_H3);
        /* テキスト (scale=1, スパン対応) */
        draw_spans(draw_x, draw_y, n->text,
                   n->spans, n->span_count, COL_H3);
        break;

    case MD_PARAGRAPH:
        draw_x = MARGIN_LEFT;
        if (search_active && node_contains(n, search_term, search_len)) {
            int is_cur = (idx == search_current_idx);
            draw_text_highlighted(draw_x, draw_y, n->text, n->text_len,
                                  COL_TEXT, COL_SEARCH_BG, is_cur);
        } else {
            draw_wrapped_text(draw_x, draw_y, n->text, n->text_len,
                              CONTENT_W, COL_TEXT,
                              n->spans, n->span_count);
        }
        break;

    case MD_LIST_ITEM:
        draw_x = MARGIN_LEFT;
        /* bullet */
        draw_text_simple(draw_x, draw_y, ">", 1, COL_BULLET);
        /* テキスト */
        if (search_active && node_contains(n, search_term, search_len)) {
            int is_cur = (idx == search_current_idx);
            draw_text_highlighted(draw_x + LIST_BULLET_W, draw_y,
                                  n->text, n->text_len,
                                  COL_TEXT, COL_SEARCH_BG, is_cur);
        } else {
            draw_wrapped_text(draw_x + LIST_BULLET_W, draw_y,
                              n->text, n->text_len,
                              CONTENT_W - LIST_BULLET_W, COL_TEXT,
                              n->spans, n->span_count);
        }
        break;

    case MD_CODE_BLOCK: {
        int bg_y = draw_y;
        int bg_h = CODE_LINE_H;
        int text_y = draw_y;

        if (node_code_first[idx]) {
            bg_y = draw_y;
            bg_h += CODE_PAD_V;
            text_y += CODE_PAD_V;
        }
        if (node_code_last[idx]) {
            bg_h += CODE_PAD_V;
        }

        /* 背景色 */
        gfx_fill_rect(MARGIN_LEFT, bg_y,
                       CONTENT_W, bg_h, COL_CODE_BG);
        /* テキスト */
        draw_text_simple(MARGIN_LEFT + CODE_PAD_H, text_y,
                         n->text, n->text_len, COL_CODE_FG);
        break;
    }

    case MD_HRULE: {
        int hy = draw_y + HRULE_H / 2;
        int x;
        /* 破線 */
        for (x = MARGIN_LEFT; x < MARGIN_LEFT + CONTENT_W; x += 6) {
            gfx_hline(x, hy, 3, COL_RULER);
        }
        break;
    }

    case MD_TABLE_ROW: {
        /* テーブル行描画 */
        int col;
        int col_w;
        int cx;
        int is_header;

        /* ヘッダー行判定 (先頭のテーブル行かどうか) */
        is_header = (idx == 0 ||
                     doc->nodes[idx - 1].type != MD_TABLE_ROW);

        /* 偶数行は背景色 */
        if (!is_header) {
            /* テーブル先頭からの行数を数える */
            int row_idx = 0;
            int k;
            for (k = idx - 1; k >= 0 && doc->nodes[k].type == MD_TABLE_ROW; k--) {
                row_idx++;
            }
            if (row_idx % 2 == 0) {
                gfx_fill_rect(MARGIN_LEFT, draw_y,
                              CONTENT_W, TABLE_ROW_H, COL_QUOTE_BAR);
            }
        } else {
            /* ヘッダー行の背景 */
            gfx_fill_rect(MARGIN_LEFT, draw_y,
                          CONTENT_W, TABLE_ROW_H, COL_CODE_BG);
        }

        /* 列幅を均等分割 */
        if (n->col_count > 0) {
            col_w = CONTENT_W / n->col_count;
        } else {
            col_w = CONTENT_W;
        }

        /* 各セルを描画 */
        cx = MARGIN_LEFT;
        for (col = 0; col < n->col_count; col++) {
            u8 fg = is_header ? COL_H2 : COL_TEXT;
            draw_text_simple(cx + TABLE_PAD, draw_y + 2,
                             n->cols[col], n->col_lens[col], fg);
            cx += col_w;
        }

        /* 列区切線 */
        cx = MARGIN_LEFT;
        for (col = 1; col < n->col_count; col++) {
            cx += col_w;
            gfx_vline(cx, draw_y, TABLE_ROW_H, COL_RULER);
        }

        /* 下罫線 */
        gfx_hline(MARGIN_LEFT, draw_y + TABLE_ROW_H - 1,
                  CONTENT_W, COL_RULER);
        break;
    }

    case MD_BLOCKQUOTE: {
        /* 引用ブロック */
        int bq_x = MARGIN_LEFT + QUOTE_BAR_W + QUOTE_PAD;
        int bq_w = CONTENT_W - QUOTE_BAR_W - QUOTE_PAD;

        /* 左バー */
        gfx_fill_rect(MARGIN_LEFT, draw_y,
                      QUOTE_BAR_W, node_height[idx], COL_QUOTE_BAR);

        /* テキスト */
        draw_wrapped_text(bq_x, draw_y + 2,
                          n->text, n->text_len,
                          bq_w, COL_TEXT,
                          n->spans, n->span_count);
        break;
    }

    case MD_BLANK:
        /* 何も描画しない */
        break;
    }
}

/* ======================================================================== */
/*  ページ描画 (スクロール差分最適化)                                        */
/* ======================================================================== */

static int prev_scroll_y = -9999;  /* 前回のスクロール位置 */

/* バックバッファの指定範囲を背景色でクリア */
static void clear_strip(int y, int h)
{
    if (y < 0) { h += y; y = 0; }
    if (y + h > PAGE_H) h = PAGE_H - y;
    if (h > 0) gfx_fill_rect(0, y, SCREEN_W, h, COL_BG);
}

/* バックバッファ4プレーンを縦方向にシフト (memmove) */
static void shift_backbuffer(int delta)
{
    int p;
    int pitch = gfx_fb.pitch;   /* 80 bytes/line */
    int abs_delta = (delta > 0) ? delta : -delta;
    int move_lines = PAGE_H - abs_delta;

    if (move_lines <= 0) return;

    for (p = 0; p < 4; p++) {
        u8 *plane = gfx_fb.planes[p];
        if (delta > 0) {
            /* 下スクロール → コンテンツを上にシフト */
            memmove(plane,
                    plane + delta * pitch,
                    move_lines * pitch);
        } else {
            /* 上スクロール → コンテンツを下にシフト */
            memmove(plane + abs_delta * pitch,
                    plane,
                    move_lines * pitch);
        }
    }
}

/* 指定strip範囲(screen座標)に掛かるノードのみ描画 */
static void render_strip_nodes(MdDocument *doc, int scroll_y,
                               int strip_top, int strip_bottom)
{
    int i;
    for (i = 0; i < doc->node_count; i++) {
        int ny = node_y[i] - scroll_y;
        int nh = node_height[i];

        if (ny >= strip_bottom) break;
        if (ny + nh < strip_top) continue;

        render_node(doc, &doc->nodes[i], scroll_y, i);
    }
}

static void render_page(MdDocument *doc, int scroll_y)
{
    int delta = scroll_y - prev_scroll_y;
    int abs_delta = (delta > 0) ? delta : -delta;

    if (abs_delta >= PAGE_H || prev_scroll_y < 0) {
        /* フル再描画 (大ジャンプ or 初回) */
        gfx_fill_rect(0, 0, SCREEN_W, PAGE_H, COL_BG);

        {
            int i;
            for (i = 0; i < doc->node_count; i++) {
                int ny = node_y[i] - scroll_y;
                int nh = node_height[i];

                if (ny >= PAGE_H) break;
                if (ny + nh < 0) continue;

                render_node(doc, &doc->nodes[i], scroll_y, i);
            }
        }
    } else if (delta > 0) {
        /* 下スクロール: バッファを上にシフト、下端の帯のみ描画 */
        shift_backbuffer(delta);
        clear_strip(PAGE_H - delta, delta);
        render_strip_nodes(doc, scroll_y, PAGE_H - delta, PAGE_H);
    } else if (delta < 0) {
        /* 上スクロール: バッファを下にシフト、上端の帯のみ描画 */
        shift_backbuffer(delta);
        clear_strip(0, abs_delta);
        render_strip_nodes(doc, scroll_y, 0, abs_delta);
    }
    /* delta == 0: 何もしない */

    prev_scroll_y = scroll_y;
}


/* ======================================================================== */
/*  ステータスバー                                                          */
/* ======================================================================== */

static void draw_statusbar(const char *filename, int scroll_y, int total_h)
{
    int pct;
    char buf[80];
    int len;

    /* 背景 */
    gfx_fill_rect(0, PAGE_H, SCREEN_W, STATUS_H, COL_STATUS_BG);

    /* ファイル名 */
    len = 0;
    buf[len++] = ' ';
    {
        const char *p = filename;
        while (*p && len < 40) buf[len++] = *p++;
    }
    buf[len] = '\0';
    kcg_draw_utf8(4, PAGE_H + 2, buf, COL_STATUS_FG, 0xFF);

    /* 進捗率 */
    if (total_h > PAGE_H) {
        pct = (scroll_y * 100) / (total_h - PAGE_H);
        if (pct > 100) pct = 100;
    } else {
        pct = 100;
    }
    sprintf(buf, "%d%%", pct);
    kcg_draw_utf8(SCREEN_W - 60, PAGE_H + 2, buf, COL_STATUS_FG, 0xFF);

    /* 操作ヒント */
    kcg_draw_utf8(SCREEN_W / 2 - 120, PAGE_H + 2,
                  "Spc:Pg b:Back /:Find t:TOC F1:Open q:Quit",
                  COL_STATUS_FG, 0xFF);
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

/* search_term, search_len, search_active, search_current_idx は
   テキスト描画セクションで定義済み */

/* 大文字小文字を無視して比較 */
static int ci_match(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* ノードテキスト内に検索語が含まれるか (大文字小文字無視) */
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
        if (node_contains(&doc->nodes[i], search_term, search_len)) {
            return i;
        }
    }
    /* 先頭からラップ検索 */
    for (i = 0; i < start_idx && i < doc->node_count; i++) {
        if (node_contains(&doc->nodes[i], search_term, search_len)) {
            return i;
        }
    }
    return -1;
}

/* 検索語入力 UI (ステータスバーに入力欄を表示) */
static int do_search_input(void)
{
    int ch;
    search_len = 0;
    search_term[0] = '\0';

    for (;;) {
        char prompt[80];
        /* ステータスバーに検索プロンプトを描画 */
        gfx_fill_rect(0, PAGE_H, SCREEN_W, STATUS_H, COL_STATUS_BG);
        sprintf(prompt, " /%s_", search_term);
        kcg_draw_utf8(4, PAGE_H + 2, prompt, COL_H1, 0xFF);
        api->gfx_add_dirty_rect(0, PAGE_H, SCREEN_W, STATUS_H);
        api->gfx_present_dirty();

        ch = api->kbd_getchar();

        if (ch == 0x0D || ch == 0x0A) {
            /* Enter: 検索確定 */
            if (search_len > 0) {
                search_active = 1;
                return 1;
            }
            return 0;
        } else if (ch == 0x1B) {
            /* ESC: キャンセル */
            search_len = 0;
            search_term[0] = '\0';
            return 0;
        } else if (ch == 0x08 || ch == 0x7F) {
            /* BS/DEL */
            if (search_len > 0) {
                search_len--;
                search_term[search_len] = '\0';
            }
        } else if (ch >= 0x20 && ch < 0x7F && search_len < SEARCH_MAX - 1) {
            /* 通常文字 */
            search_term[search_len++] = (char)ch;
            search_term[search_len] = '\0';
        }
    }
}

/* ======================================================================== */
/*  目次ジャンプ (TOC)                                                      */
/* ======================================================================== */

#define TOC_MAX 64

static int toc_indices[TOC_MAX];  /* 見出しノードのindex */
static int toc_count = 0;

/* 目次を構築 (H1/H2を収集) */
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

/* 目次オーバーレイを描画 */
static void draw_toc_overlay(MdDocument *doc, int cursor)
{
    int i;
    int ox = 40;   /* オーバーレイ左 */
    int oy = 20;   /* オーバーレイ上 */
    int ow = 560;  /* オーバーレイ幅 */
    int line_h = 20;
    int max_visible;
    int scroll_top = 0;
    int oh;

    /* 表示可能行数 */
    max_visible = (PAGE_H - 80) / line_h;
    if (max_visible > toc_count) max_visible = toc_count;
    oh = max_visible * line_h + 40;

    /* スクロール */
    if (cursor >= scroll_top + max_visible) {
        scroll_top = cursor - max_visible + 1;
    }
    if (cursor < scroll_top) {
        scroll_top = cursor;
    }

    /* 背景 */
    gfx_fill_rect(ox, oy, ow, oh, COL_CODE_BG);
    gfx_rect(ox, oy, ow, oh, COL_RULER);

    /* タイトル */
    kcg_draw_utf8(ox + 8, oy + 4, "-- TABLE OF CONTENTS --", COL_H2, 0xFF);

    /* 項目描画 */
    for (i = 0; i < max_visible && (scroll_top + i) < toc_count; i++) {
        int idx = toc_indices[scroll_top + i];
        MdNode *n = &doc->nodes[idx];
        int ly = oy + 24 + i * line_h;
        u8 fg;
        char buf[72];
        int blen;
        const char *prefix;

        /* カーソル行はハイライト */
        if (scroll_top + i == cursor) {
            gfx_fill_rect(ox + 2, ly, ow - 4, line_h, COL_STATUS_BG);
        }

        fg = (n->type == MD_H1) ? COL_H1 : COL_H2;
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

    /* 下部ヒント */
    kcg_draw_utf8(ox + 8, oy + oh - 18,
                  "Up/Down:Select  Enter:Jump  ESC:Cancel",
                  COL_STATUS_FG, 0xFF);
}

/* 目次ジャンプUI: 選択された見出しのノードindexを返す (-1=キャンセル) */
static int do_toc_jump(MdDocument *doc)
{
    int cursor = 0;

    if (toc_count == 0) return -1;

    for (;;) {
        int key, kd, kc;

        /* オーバーレイ描画 */
        draw_toc_overlay(doc, cursor);
        gfx_present();
        api->gfx_present_dirty();

        key = api->kbd_getkey();
        kd = KEYDATA(key);
        kc = KEYCODE(key);

        if (kd == 0x1B) {
            /* ESC: キャンセル */
            return -1;
        } else if (kd == 0x0D || kd == 0x0A) {
            /* Enter: 選択確定 */
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
    static char cur_filename[256]; /* 現在表示中のファイル名 */
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

    /* ファイラー初期化 */
    filer_init(api);

    /* レイアウト計算 */
    total_height = layout_pass(&doc);
    scroll_y = 0;
    last_search_idx = 0;

    /* 目次構築 */
    build_toc(&doc);

    /* 初回描画 */
    prev_scroll_y = -9999;
    render_page(&doc, scroll_y);
    draw_statusbar(cur_filename, scroll_y, total_height);
    api->gfx_add_dirty_rect(0, 0, SCREEN_W, SCREEN_H);
    api->gfx_present_dirty();

    /* イベントループ */
    quit = 0;
    while (!quit) {
        int key = api->kbd_getkey();
        int kd = KEYDATA(key);
        int kc = KEYCODE(key);
        int need_redraw = 0;
        int max_scroll;

        max_scroll = total_height - PAGE_H;
        if (max_scroll < 0) max_scroll = 0;

        if (kd == 'q' || kd == 'Q' || kd == 0x1B) {
            quit = 1;

        } else if (kd == ' ' || kc == VK_ROLLDOWN) {
            scroll_y += PAGE_H;
            need_redraw = 1;

        } else if (kd == 'b' || kd == 'B' || kc == VK_ROLLUP) {
            scroll_y -= PAGE_H;
            need_redraw = 1;

        } else if (kc == VK_DOWN) {
            scroll_y += TEXT_LINE_H;
            need_redraw = 1;

        } else if (kc == VK_UP) {
            scroll_y -= TEXT_LINE_H;
            need_redraw = 1;

        } else if (kc == VK_HOME) {
            scroll_y = 0;
            need_redraw = 1;

        } else if (kd == '/') {
            /* 検索入力 */
            if (do_search_input()) {
                int found = find_next(&doc, 0);
                if (found >= 0) {
                    scroll_y = node_y[found];
                    last_search_idx = found;
                    search_current_idx = found;
                }
            } else {
                /* ESCキャンセル時は検索解除 */
                search_current_idx = -1;
            }
            prev_scroll_y = -9999;  /* フル再描画を強制 */
            need_redraw = 1;

        } else if (kd == 'n' || kd == 'N') {
            /* 次の検索結果 */
            if (search_active) {
                int found = find_next(&doc, last_search_idx + 1);
                if (found >= 0) {
                    scroll_y = node_y[found];
                    last_search_idx = found;
                    search_current_idx = found;
                    prev_scroll_y = -9999;  /* フル再描画 */
                    need_redraw = 1;
                }
            }

        } else if (kd == 't' || kd == 'T') {
            /* 目次ジャンプ */
            int toc_result = do_toc_jump(&doc);
            if (toc_result >= 0) {
                scroll_y = node_y[toc_result];
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

                        /* ファイル名更新 */
                        strncpy(cur_filename, path,
                                sizeof(cur_filename) - 1);
                        cur_filename[sizeof(cur_filename) - 1] = '\0';

                        /* レイアウト再計算 */
                        total_height = layout_pass(&doc);
                        scroll_y = 0;
                        last_search_idx = 0;
                        search_active = 0;

                        /* 目次再構築 */
                        build_toc(&doc);

                        /* フル再描画を強制 */
                        prev_scroll_y = -9999;
                    }
                }
            }
            /* ファイラー後は必ず再描画 (パレットはmdview用のまま) */
            need_redraw = 1;
        }

        /* scroll_yクランプ */
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;

        if (need_redraw) {
            render_page(&doc, scroll_y);
            draw_statusbar(cur_filename, scroll_y, total_height);
            api->gfx_add_dirty_rect(0, 0, SCREEN_W, SCREEN_H);
            api->gfx_present_dirty();
        }
    }

    /* クリーンアップ */
    mdview_gfx_shutdown();
    return 0;
}

