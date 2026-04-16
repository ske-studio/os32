/*
 * screen_text.c - OS32 VZ Editor テキスト領域の描画・スクロール制御
 * C89 compatible
 */

#include "vz.h"

/* PC-98 罫線・特殊文字 */
#define CH_NEWLINE   0x1F           /* PC-98 ANK: ↓ 改行マーク */
#define CH_EOF       '['            /* EOF表示用 (簡易) */

/*
 * テキスト編集エリア描画
 */
void su_draw_text(TEXT* w)
{
    int x, y;
    int cur_line;
    char* p;
    char* end_ptr;
    int cursor_x, cursor_y;
    int in_gap;
    int bs = -1, be = -1;
    int is_highlight;
    unsigned char c;
    
    int text_top_y;
    int text_btm_y;

    /* 矩形選択用の行・列範囲 */
    int rect_mode = 0;
    int bs_line = 0, be_line = 0, bs_col = 0, be_col = 0;
    int disp_line = 0; /* 表示中の行番号(thom基準) */

    /* ブロックハイライト用ポインタ (te_get_logical_index毎文字呼び出し回避) */
    char* bs_ptr = NULL;
    char* be_ptr = NULL;

    if (!w || !w->ttop || !w->tcp || !w->tmax) return;

    text_top_y = w->tw_py;
    text_btm_y = w->tw_py + w->tw_sy - 1;

    /* ブロックハイライト範囲の計算を分離 */
    te_calc_block_bounds(w, &bs, &be, &rect_mode, &bs_line, &be_line, &bs_col, &be_col);

    /* ブロック範囲をポインタに事前変換 (ループ内のte_get_logical_index排除) */
    if (!rect_mode && bs != -1) {
        bs_ptr = te_get_pointer_from_index(w, bs);
        be_ptr = te_get_pointer_from_index(w, be);
    }

    /* テキスト表示エリアをクリア */
    su_fill_rect(0, text_top_y, SCREEN_W, text_btm_y - text_top_y + 1, ' ', ATR_NORMAL);

    x = 0;
    y = text_top_y;
    cur_line = 1;
    cursor_x = 0;
    cursor_y = text_top_y;
    in_gap = 0;
    disp_line = 0;

    /* thomからの表示行番号(disp_line)をインクリメンタルに計算 */
    /* (ポインタ直接比較で走査 — te_get_logical_index呼び出しを排除) */
    if (w->last_thom == NULL || w->last_thom < w->ttop || w->last_thom > w->tmax) {
        /* 初回、またはキャッシュ無効時: ttop→thom間を全走査 */
        char* q;
        disp_line = 0;
        q = (char*)w->ttop;
        while (q < (char*)w->thom) {
            if (q == (char*)w->tcp) { q = (char*)w->tend; if (q >= (char*)w->tmax) break; }
            if (*q == '\n') disp_line++;
            q++;
        }
    } else {
        disp_line = w->last_thom_line;
        if ((char*)w->thom != (char*)w->last_thom) {
            char* from;
            char* to;
            int direction;
            /* 論理的な前後関係をポインタで判定 */
            int thom_idx = te_get_logical_index(w, (char*)w->thom);
            int last_idx = te_get_logical_index(w, (char*)w->last_thom);

            if (thom_idx > last_idx) {
                /* 進んだ場合: last_thom→thom走査、改行を加算 */
                from = (char*)w->last_thom;
                to = (char*)w->thom;
                direction = 1;
            } else {
                /* 戻った場合: thom→last_thom走査、改行を減算 */
                from = (char*)w->thom;
                to = (char*)w->last_thom;
                direction = -1;
            }
            /* fromからtoまでポインタ直接比較で走査 */
            {
                char* q = from;
                while (q < to) {
                    if (q == (char*)w->tcp) { q = (char*)w->tend; if (q >= (char*)w->tmax) break; }
                    if (q >= to) break;
                    if (*q == '\n') disp_line += direction;
                    q++;
                }
            }
        }
    }

    w->last_thom = w->thom;
    w->last_thom_line = disp_line;

    p = (char*)w->thom;
    end_ptr = (char*)w->tmax;

    while (p < end_ptr && y <= text_btm_y) {
        /* ギャップ部分をスキップ */
        if (!in_gap && p == (char*)w->tcp) {
            in_gap = 1;
            p = (char*)w->tend;
            /* ギャップ終了がバッファ終端と重なる場合もある */
            if (p >= end_ptr) break; 
        }

        /* 現在の文字がブロック範囲内か判定 (ポインタ比較) */
        if (rect_mode) {
            is_highlight = (disp_line >= bs_line && disp_line <= be_line && x >= bs_col && x < be_col);
        } else {
            is_highlight = (bs_ptr != NULL && p >= bs_ptr && p < be_ptr);
        }

        /* カーソル位置的判定 */
        if (p == (char*)w->tcp) {
            cursor_x = x;
            cursor_y = y;
        }

        c = (unsigned char)*p;

        if (c == '\r') {
            /* \r は無視 (CRLF対応) */
            p++;
            continue;
        }

        if (c == '\n') {
            /* 改行表示 (↓) */
            vz_putc(x, y, CH_NEWLINE, ATR_NEWLINE);
            
            p++;
            cur_line++;
            disp_line++;
            y++;
            x = 0;
            continue;
        }

        if (c == '\t') {
            /* タブ表示 (8文字の倍数までスペース) */
            int tab_spaces = 8 - (x % 8);
            int j;
            for (j = 0; j < tab_spaces && x < SCREEN_W; j++) {
                int th;
                if (rect_mode) {
                    th = (disp_line >= bs_line && disp_line <= be_line
                          && x >= bs_col && x < be_col);
                } else {
                    th = is_highlight;
                }
                vz_putc(x, y, ' ', th ? ATR_STATUS : ATR_NORMAL);
                x++;
            }
            p++;
            if (x >= SCREEN_W) {
                y++;
                x = 0;
            }
            continue;
        }

        /* ASCII 高速バイパス (関数呼び出しをスキップ) */
        if (c >= 0x20 && c <= 0x7E) {
            vz_putc(x, y, (char)c, is_highlight ? ATR_STATUS : ATR_NORMAL);
            x++;
            if (x >= SCREEN_W) {
                y++;
                x = 0;
            }
            p++;
            continue;
        }

        /* UTF-8 デコードによる画面描画 */
        {
            utf8_decode_t dec = utf8_decode((const unsigned char*)p);
            x += su_put_utf8_char(x, y, &dec, is_highlight ? ATR_STATUS : ATR_NORMAL);
            
            /* p を文字のバイト数に応じて進める */
            /* ただしループ末尾で p++ が呼ばれるため、bytes_used - 1 だけ進める */
            if (dec.bytes_used > 1) {
                p += (dec.bytes_used - 1);
            }
        }

        /* 行端折り返し */
        if (x >= SCREEN_W) {
            y++;
            x = 0;
        }

        p++;
    }

    /* ギャップがテキスト末尾にある場合のカーソル位置取得 */
    if (p == (char*)w->tcp) {
        cursor_x = x;
        cursor_y = y;
    }

    /* EOF マーク表示 */
    if (y <= text_btm_y && x < SCREEN_W) {
        vz_putc(x, y, CH_EOF, ATR_EOF);
    }

    /* カーソル位置を設定 */
    vz_set_cursor(cursor_x, cursor_y);
}

/*
 * スクロール調整
 * カーソル(tcp)がthomから表示可能な範囲内にあるか確認し、
 * 範囲外ならthomを更新する
 */
void su_adjust_scroll(TEXT* w)
{
    int visible_lines = w->tw_sy;
    int cursor_line_from_thom;
    char* p;
    char* tcp_pos;

    if (!w || !w->ttop || !w->tcp || !w->tmax) return;

    tcp_pos = (char*)w->tcp;

    /* thom から tcp までの行数をカウント */
    cursor_line_from_thom = 0;
    p = (char*)w->thom;
    while (p < tcp_pos && p < (char*)w->tmax) {
        if (*p == '\n') {
            cursor_line_from_thom++;
        }
        p++;
    }

    /* カーソルが画面外（下方向） */
    if (cursor_line_from_thom >= visible_lines) {
        /* thomを進めてカーソルが画面内に収まるようにする */
        int lines_to_skip = cursor_line_from_thom - visible_lines + 1;
        p = (char*)w->thom;
        while (lines_to_skip > 0 && p < tcp_pos) {
            if (*p == '\n') {
                lines_to_skip--;
            }
            p++;
        }
        w->thom = (void*)p;
    }

    /* カーソルが画面外（上方向） */
    if (tcp_pos < (char*)w->thom) {
        /* thomをtcpの行頭まで戻す */
        p = tcp_pos;
        while (p > (char*)w->ttop && *(p - 1) != '\n') {
            p--;
        }
        w->thom = (void*)p;
    }
}
