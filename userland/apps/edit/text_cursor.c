/*
 * text_cursor.c - OS32 VZ Editor テキストギャップバッファ (カーソル移動系)
 * C89 compatible
 */

#include "vz.h"

/*
 * カーソルを任意の論理インデックスへ移動させるヘルパー
 */
void te_move_to_index(TEXT* w, int target_idx) {
    if (target_idx < 0) return;
    while (te_get_logical_index(w, (char*)w->tend) > target_idx) {
        w->tend = (char*)w->tend - 1;
        w->tcp = (char*)w->tcp - 1;
        *((char*)w->tend) = *((char*)w->tcp);
    }
    while (te_get_logical_index(w, (char*)w->tend) < target_idx) {
        *((char*)w->tcp) = *((char*)w->tend);
        w->tcp = (char*)w->tcp + 1;
        w->tend = (char*)w->tend + 1;
    }
}

/*
 * カーソル位置の行番号と列番号を再計算
 * ギャップバッファの先頭からtcpまでを走査して行番号を数える
 */
void te_count_position(TEXT* w)
{
    char* p;
    int line, col;

    if (!w || !w->ttop || !w->tcp) return;

    /* == 行番号(lnumb) の計算 == */
    if (w->last_tcp == NULL || w->last_tcp < w->ttop || w->last_tcp > w->tmax || w->last_tcp_line == 0) {
        /* 初回、またはキャッシュ無効時: 全走査 */
        line = 1;
        p = (char*)w->ttop;
        while (p < (char*)w->tcp) {
            if (*p == '\n') line++;
            p++;
        }
    } else {
        line = w->last_tcp_line;
        if ((char*)w->tcp > (char*)w->last_tcp) {
            /* 進んだ分だけ足す */
            p = (char*)w->last_tcp;
            while (p < (char*)w->tcp) {
                if (*p == '\n') line++;
                p++;
            }
        } else if ((char*)w->tcp < (char*)w->last_tcp) {
            /* 戻った分だけ引く */
            p = (char*)w->tcp;
            while (p < (char*)w->last_tcp) {
                if (*p == '\n') line--;
                p++;
            }
        }
    }

    /* キャッシュ更新 */
    w->last_tcp = w->tcp;
    w->last_tcp_line = line;
    w->lnumb = (WORD)line;

    /* == 列番号(lx) の計算 == */
    /* 現在行の先頭から tcp までをスキャンして列を数える (高々1行分なので一瞬) */
    col = 0;
    p = (char*)w->tcp;
    /* tcpから後方に遡って行頭、またはファイル先頭を探す */
    while (p > (char*)w->ttop) {
        if (*(p - 1) == '\n') break;
        p--;
    }
    /* 行頭からtcpまで前方に進んでカウント (\rはカウントしない) */
    while (p < (char*)w->tcp) {
        if (*p != '\r') col++;
        p++;
    }

    w->lx = (BYTE)col;
    w->ly = 0; /* lyは画面上のカーソルY位置 (thomからの相対行) として後で計算 */
}

/*
 * カーソル移動 (水平方向)
 * dx: 左(-1) / 右(+1)
 */
void te_move_cursor(TEXT* w, int dx, int dy)
{
    if (!w || !w->tcp || !w->tend) return;

    if (dy != 0) {
        te_move_line(w, dy);
        return;
    }

    /* 左移動 */
    while (dx < 0 && w->tcp > w->ttop) {
        w->tend = (char*)w->tend - 1;
        w->tcp = (char*)w->tcp - 1;
        *((char*)w->tend) = *((char*)w->tcp);
        dx++;
    }
    /* 右移動 */
    while (dx > 0 && w->tend < w->tmax) {
        *((char*)w->tcp) = *((char*)w->tend);
        w->tcp = (char*)w->tcp + 1;
        w->tend = (char*)w->tend + 1;
        dx--;
    }
}

/*
 * 行単位のカーソル移動
 * dy: 上(-1) / 下(+1)
 */
void te_move_line(TEXT* w, int dy)
{
    int target_col;
    char* p;

    if (!w || !w->tcp || !w->tend) return;

    /* 現在の列位置を記憶 */
    te_count_position(w);
    target_col = w->lx;

    if (dy < 0) {
        /* 上方向移動 */
        /* まず現在行の先頭まで戻る */
        while (w->tcp > w->ttop) {
            char c;
            p = (char*)w->tcp - 1;
            c = *p;
            /* ギャップを左に移動 */
            w->tend = (char*)w->tend - 1;
            w->tcp = (char*)w->tcp - 1;
            *((char*)w->tend) = *((char*)w->tcp);
            if (c == '\n') {
                break;
            }
        }
        /* さらに1行前の先頭まで戻る */
        if (w->tcp > w->ttop) {
            while (w->tcp > w->ttop) {
                char c;
                p = (char*)w->tcp - 1;
                c = *p;
                if (c == '\n') {
                    break;
                }
                w->tend = (char*)w->tend - 1;
                w->tcp = (char*)w->tcp - 1;
                *((char*)w->tend) = *((char*)w->tcp);
            }
        }
        /* 目標の列位置まで右に進む */
        {
            int col = 0;
            while (col < target_col && w->tend < w->tmax) {
                char c = *((char*)w->tend);
                if (c == '\n' || c == '\r') break;
                *((char*)w->tcp) = c;
                w->tcp = (char*)w->tcp + 1;
                w->tend = (char*)w->tend + 1;
                col++;
            }
        }
    } else if (dy > 0) {
        /* 下方向移動 */
        /* 次の改行まで進む */
        while (w->tend < w->tmax) {
            char c = *((char*)w->tend);
            *((char*)w->tcp) = c;
            w->tcp = (char*)w->tcp + 1;
            w->tend = (char*)w->tend + 1;
            if (c == '\n') {
                break;
            }
        }
        /* 目標の列位置まで右に進む */
        {
            int col = 0;
            while (col < target_col && w->tend < w->tmax) {
                char c = *((char*)w->tend);
                if (c == '\n' || c == '\r') break;
                *((char*)w->tcp) = c;
                w->tcp = (char*)w->tcp + 1;
                w->tend = (char*)w->tend + 1;
                col++;
            }
        }
    }
}

/*
 * 行頭への移動 (Home)
 */
void te_move_to_bol(TEXT* w)
{
    char* p;
    if (!w || !w->tcp || !w->tend) return;

    while (w->tcp > w->ttop) {
        char c;
        p = (char*)w->tcp - 1;
        c = *p;
        if (c == '\n') {
            break;
        }
        /* ギャップを左に移動 */
        w->tend = (char*)w->tend - 1;
        w->tcp = (char*)w->tcp - 1;
        *((char*)w->tend) = *((char*)w->tcp);
    }
}

/*
 * 行末への移動 (End)
 */
void te_move_to_eol(TEXT* w)
{
    if (!w || !w->tcp || !w->tend) return;

    while (w->tend < w->tmax) {
        char c = *((char*)w->tend);
        if (c == '\n' || c == '\r') {
            break;
        }
        /* ギャップを右に移動 */
        *((char*)w->tcp) = c;
        w->tcp = (char*)w->tcp + 1;
        w->tend = (char*)w->tend + 1;
    }
}

/*
 * ファイル先頭への移動
 */
void te_move_to_top(TEXT* w)
{
    if (!w || !w->tcp || !w->tend) return;

    while (w->tcp > w->ttop) {
        w->tend = (char*)w->tend - 1;
        w->tcp = (char*)w->tcp - 1;
        *((char*)w->tend) = *((char*)w->tcp);
    }
    w->thom = w->ttop; /* 画面先頭もファイル先頭に合わせる */
}

/*
 * ファイル末尾への移動
 */
void te_move_to_bottom(TEXT* w)
{
    if (!w || !w->tcp || !w->tend) return;

    while (w->tend < w->tmax) {
        *((char*)w->tcp) = *((char*)w->tend);
        w->tcp = (char*)w->tcp + 1;
        w->tend = (char*)w->tend + 1;
    }
    /* スクロール調整は su_adjust_scroll で自動化される */
}

/*
 * 行番号ジャンプ
 */
void te_goto_line(TEXT* w, int line_num)
{
    int cur_line;
    if (!w || !w->ttop || line_num < 1) return;

    /* まずファイル先頭へ */
    te_move_to_top(w);

    /* 1行目から数えて目標行まで進む */
    cur_line = 1;
    while (cur_line < line_num && w->tend < w->tmax) {
        char c = *((char*)w->tend);
        *((char*)w->tcp) = c;
        w->tcp = (char*)w->tcp + 1;
        w->tend = (char*)w->tend + 1;
        if (c == '\n') {
            cur_line++;
        }
    }
}
