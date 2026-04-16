/*
 * text_edit.c - OS32 VZ Editor テキストギャップバッファ (基本編集・消去系)
 * C89 compatible
 */

#include "vz.h"

/*
 * カーソル位置で文字挿入
 */
void te_insert_char(TEXT* w, int c)
{
    if (!w || !w->tcp || !w->tend) return;

    /* ギャップが残り少ない場合、バッファを拡張 */
    if ((char*)w->tend - (char*)w->tcp < 256) {
        if (!te_resize_buffer(w, w->tbmax + TE_INITIAL_BUFFER_SIZE)) {
            return; /* メモリ確保失敗 */
        }
    }

    /* 上書きモード (0) かつ、挿入文字が改行でない場合、現在位置の文字を1バイト消す */
    if (vz.insert_mode == 0 && c != '\n' && c != '\r') {
        if (w->tend < w->tmax) {
            char next_c = *((char*)w->tend);
            /* ただし、行末やEOFは上書きせずに挿入する */
            if (next_c != '\n' && next_c != '\r') {
                w->tend = (char*)w->tend + 1;
            }
        }
    }

    /* Undo に記録 */
    if (!vz.undo_in_progress) {
        int pos = te_get_logical_index(w, (char*)w->tcp);
        te_record_undo_insert((char)c, pos);
    }

    /* tcpに文字挿入 */
    *((char*)w->tcp) = (char)c;
    w->tcp = (char*)w->tcp + 1;

    w->last_tcp = NULL;
    w->last_thom = NULL;
    w->tchf = 1; /* 修正フラグ */
}

/*
 * カーソル位置で文字削除 (Backspace/Delete)
 */
void te_delete_char(TEXT* w, int backspace)
{
    if (!w || !w->tcp || !w->tend) return;

    if (backspace) {
        /* Backspace: ギャップ開始を1バイト前に */
        if (w->tcp > w->ttop) {
            w->tcp = (char*)w->tcp - 1;
            if (!vz.undo_in_progress) {
                int pos = te_get_logical_index(w, (char*)w->tcp);
                te_record_undo_delete(*((char*)w->tcp), pos);
            }
        }
    } else {
        /* Delete: ギャップ終了を1バイト後ろに */
        if (w->tend < w->tmax) {
            if (!vz.undo_in_progress) {
                int pos = te_get_logical_index(w, (char*)w->tend);
                te_record_undo_delete(*((char*)w->tend), pos);
            }
            w->tend = (char*)w->tend + 1;
        }
    }
    w->last_tcp = NULL;
    w->last_thom = NULL;
    w->tchf = 1; /* 修正フラグ */
}

/*
 * 行削除 (Ctrl-Y 用)
 */
void te_delete_line(TEXT* w) {
    te_move_to_bol(w);
    /* \n が見つかるまで tend を進める */
    while ((char*)w->tend < (char*)w->tmax) {
        if (*(char*)w->tend == '\n') {
            w->tend = (char*)w->tend + 1; /* \n自身も削除に含める */
            break;
        }
        w->tend = (char*)w->tend + 1;
    }
    w->last_tcp = NULL;
    w->last_thom = NULL;
    w->tchf = 1;
}
