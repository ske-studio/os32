/*
 * text_search.c - OS32 VZ Editor テキストギャップバッファ (検索・置換系)
 * C89 compatible
 */

#include "vz.h"

/*
 * 文字列が指定ポインタにあるかどうかの判定
 */
static int match_forward(TEXT* w, char* p, const char* query, int len) {
    int i;
    for (i = 0; i < len; i++) {
        /* ギャップを跨ぐ場合のポインタ調整 */
        if (p == (char*)w->tcp) {
            p = (char*)w->tend;
        }
        if (p >= (char*)w->tmax || *p != query[i]) return 0;
        p++;
    }
    return 1;
}

/*
 * 大文字小文字を無視するマッチング
 */
static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int match_forward_ci(TEXT* w, char* p, const char* query, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (p == (char*)w->tcp) {
            p = (char*)w->tend;
        }
        if (p >= (char*)w->tmax || to_lower(*p) != to_lower(query[i])) return 0;
        p++;
    }
    return 1;
}

/*
 * 前方検索 (カーソル現在位置から)
 * 戻り値: 1=HIT, 0=NOT FOUND
 */
int te_search_forward(TEXT* w, const char* query) {
    int len = 0;
    char* p;
    if (!w || !query || query[0] == '\0') return 0;
    while (query[len] != '\0') len++;

    p = (char*)w->tend;
    while (p < (char*)w->tmax) {
        if (match_forward(w, p, query, len)) {
            /* マッチ位置(p)へカーソルを移動 */
            while ((char*)w->tend < p) {
                *((char*)w->tcp) = *((char*)w->tend);
                w->tcp = (char*)w->tcp + 1;
                w->tend = (char*)w->tend + 1;
            }
            return 1;
        }
        p++;
    }
    return 0;
}

/*
 * 後方検索 (カーソル現在位置の手前から)
 */
int te_search_backward(TEXT* w, const char* query) {
    int len = 0;
    char* p;
    if (!w || !query || query[0] == '\0') return 0;
    while (query[len] != '\0') len++;

    p = (char*)w->tcp;
    while (p > (char*)w->ttop) {
        p--;
        /* ギャップを跨ぐ場合のポインタ調整 */
        if (p == (char*)w->tend - 1) {
            p = (char*)w->tcp - 1;
            if (p < (char*)w->ttop) break;
        }

        if (match_forward(w, p, query, len)) {
            /* マッチ位置(p)へカーソルを移動 */
            while ((char*)w->tcp > p) {
                w->tend = (char*)w->tend - 1;
                w->tcp = (char*)w->tcp - 1;
                *((char*)w->tend) = *((char*)w->tcp);
            }
            return 1;
        }
    }
    return 0;
}

/*
 * 前方検索 (大文字小文字無視版)
 */
int te_search_forward_ci(TEXT* w, const char* query) {
    int len = 0;
    char* p;
    if (!w || !query || query[0] == '\0') return 0;
    while (query[len] != '\0') len++;

    p = (char*)w->tend;
    while (p < (char*)w->tmax) {
        if (match_forward_ci(w, p, query, len)) {
            while ((char*)w->tend < p) {
                *((char*)w->tcp) = *((char*)w->tend);
                w->tcp = (char*)w->tcp + 1;
                w->tend = (char*)w->tend + 1;
            }
            return 1;
        }
        p++;
    }
    return 0;
}

/*
 * 後方検索 (大文字小文字無視版)
 */
int te_search_backward_ci(TEXT* w, const char* query) {
    int len = 0;
    char* p;
    if (!w || !query || query[0] == '\0') return 0;
    while (query[len] != '\0') len++;

    p = (char*)w->tcp;
    while (p > (char*)w->ttop) {
        p--;
        if (p == (char*)w->tend - 1) {
            p = (char*)w->tcp - 1;
            if (p < (char*)w->ttop) break;
        }
        if (match_forward_ci(w, p, query, len)) {
            while ((char*)w->tcp > p) {
                w->tend = (char*)w->tend - 1;
                w->tcp = (char*)w->tcp - 1;
                *((char*)w->tend) = *((char*)w->tcp);
            }
            return 1;
        }
    }
    return 0;
}

/*
 * 置換機能
 * (カーソル位置にあるマッチ文字列を削除し、replacementを挿入する)
 */
int te_replace(TEXT* w, const char* query, const char* replacement, int all) {
    int query_len = 0;
    int rep_len = 0;
    int i;
    int count = 0;

    if (!w || !query || query[0] == '\0') return 0;
    while (query[query_len] != '\0') query_len++;
    if (replacement) {
        while (replacement[rep_len] != '\0') rep_len++;
    }

    do {
        /* まず現在位置から前方検索 */
        if (!te_search_forward(w, query)) {
            break;
        }

        /* 検索ヒット！ (カーソルは今ヒット文字列の先頭にある) */
        /* query_len 分だけ Delete (Deleteは tend を右にずらす) */
        for (i = 0; i < query_len; i++) {
            if (w->tend < w->tmax) {
                w->tend = (char*)w->tend + 1;
            }
        }

        /* replacement を挿入 */
        for (i = 0; i < rep_len; i++) {
            /* ギャップが満杯なら中止 */
            if ((char*)w->tend - (char*)w->tcp <= 1) break;
            *((char*)w->tcp) = replacement[i];
            w->tcp = (char*)w->tcp + 1;
        }
        w->tchf = 1;
        count++;

    } while (all); /* all=1 ならループ、0なら1回で終了 */

    return count;
}
