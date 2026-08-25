/*
 * text_block.c - OS32 VZ Editor テキストギャップバッファ (ブロック操作系)
 * C89 compatible
 */

#include "vz.h"

void te_calc_block_bounds(TEXT* w, int* p_bs, int* p_be, int* p_rect_mode, int* p_bs_line, int* p_be_line, int* p_bs_col, int* p_be_col) {
    if (!w || w->block_mode <= 0 || w->tblock_start == -1) {
        *p_bs = -1; *p_be = -1; *p_rect_mode = 0;
        return;
    }
    if (w->block_mode == 3) {
        *p_rect_mode = 1;
        *p_bs = w->tblock_start;
        *p_be = w->tblock_end;
        if (*p_bs > *p_be) { int tmp = *p_bs; *p_bs = *p_be; *p_be = tmp; }
        *p_bs_col = w->block_start_col;
        *p_be_col = w->block_end_col;
        if (*p_bs_col > *p_be_col) { int tmp = *p_bs_col; *p_bs_col = *p_be_col; *p_be_col = tmp; }
        
        *p_bs_line = 0; *p_be_line = 0;
        {
            char* q = (char*)w->ttop;
            int idx = 0, line = 0;
            while (q < (char*)w->tmax) {
                if (q == (char*)w->tcp) { q = (char*)w->tend; if (q >= (char*)w->tmax) break; }
                if (idx == *p_bs) *p_bs_line = line;
                if (idx == *p_be) *p_be_line = line;
                if (*q == '\n') line++;
                q++; idx++;
            }
            if (*p_be > idx) *p_be_line = line;
        }
    } else {
        *p_rect_mode = 0;
        *p_bs = w->tblock_start;
        *p_be = (w->block_mode == 2) ? w->tblock_end : te_get_logical_index(w, (char*)w->tend);
        if (*p_bs > *p_be) { int tmp = *p_bs; *p_bs = *p_be; *p_be = tmp; }
    }
}

void te_mark_block_start(TEXT* w) {
    w->tblock_start = te_get_logical_index(w, (char*)w->tend);
    w->block_mode = 1;
}

void te_mark_block_end(TEXT* w) {
    w->tblock_end = te_get_logical_index(w, (char*)w->tend);
    w->block_mode = 2;
}

void te_unmark_block(TEXT* w) {
    w->block_mode = 0;
    w->tblock_start = -1;
    w->tblock_end = -1;
}

void te_block_copy(TEXT* w) {
    int start, end, len, i;
    char* p;
    if (w->block_mode != 2) return;
    
    start = w->tblock_start;
    end = w->tblock_end;
    if (start > end) {
        int tmp = start; start = end; end = tmp;
    }
    
    len = end - start;
    if (len <= 0) return;
    
    if (vz.clipboard_buf) {
        os32_free(vz.clipboard_buf);
    }
    vz.clipboard_buf = os32_malloc(len + 1);
    if (!vz.clipboard_buf) return;
    
    vz.clipboard_max = len + 1;
    vz.clipboard_len = len;
    
    p = te_get_pointer_from_index(w, start);
    for (i = 0; i < len; i++) {
        if (p == (char*)w->tcp) p = (char*)w->tend;
        vz.clipboard_buf[i] = *p;
        p++;
    }
    vz.clipboard_buf[len] = '\0';
}

void te_block_delete(TEXT* w) {
    int start, end, len;
    if (w->block_mode != 2) return;
    
    start = w->tblock_start;
    end = w->tblock_end;
    if (start > end) {
        int tmp = start; start = end; end = tmp;
    }
    
    len = end - start;
    if (len <= 0) return;
    
    /* カーソルを start の位置に戻す */
    te_move_to_index(w, start);
    
    /* そこからlen文字消す */
    w->tend = (char*)w->tend + len;
    if (w->tend > w->tmax) w->tend = w->tmax;
    w->tchf = 1;

    te_unmark_block(w);
}

void te_block_paste(TEXT* w) {
    int i;
    if (!vz.clipboard_buf || vz.clipboard_len <= 0) return;
    
    if ((char*)w->tend - (char*)w->tcp < vz.clipboard_len + 256) {
        if (!te_resize_buffer(w, w->tbmax + vz.clipboard_len + TE_INITIAL_BUFFER_SIZE)) {
            return;
        }
    }

    for (i = 0; i < vz.clipboard_len; i++) {
        if ((char*)w->tend - (char*)w->tcp <= 1) break; /* バッファ満杯 */
        *((char*)w->tcp) = vz.clipboard_buf[i];
        w->tcp = (char*)w->tcp + 1;
    }
    w->tchf = 1;
}

/* ================================================================ */
/*  矩形ブロック操作用ヘルパー                                      */
/* ================================================================ */

/*
 * 指定行の某列にいるポインタ(gap外、tend以降)を返すヘルパー。
 */
static char* find_line_col(TEXT* w, int line_idx, int col, int* actual_col)
{
    char* p;
    int cur_line;
    int cur_col;

    /* ギャップバッファ全体を線形に走査 */
    p = (char*)w->ttop;
    cur_line = 0;
    cur_col = 0;

    while (1) {
        char c;
        if (p == (char*)w->tcp) p = (char*)w->tend;
        if (p >= (char*)w->tmax) break;

        c = *p;

        if (cur_line == line_idx) {
            if (cur_col >= col) {
                if (actual_col) *actual_col = cur_col;
                return p;
            }
            if (c == '\n') {
                if (actual_col) *actual_col = cur_col;
                return p;
            }
            if (c == '\t') {
                cur_col = (cur_col / 8 + 1) * 8;
            } else if (IS_SJIS_1ST((unsigned char)c)) {
                cur_col += 2;
                p++; 
                if (p == (char*)w->tcp) p = (char*)w->tend;
            } else {
                cur_col++;
            }
            p++;
            continue;
        }

        if (c == '\n') {
            cur_line++;
            cur_col = 0;
        }
        p++;
    }

    if (actual_col) *actual_col = cur_col;
    return (void*)0;
}

/*
 * 綺形領域内のテキストをバッファに転送するヘルパー
 */
static int extract_rect_text(TEXT* w, int start_line, int end_line,
                             int col_start, int col_end,
                             char* out_buf, int out_max)
{
    int line;
    int out_pos = 0;

    for (line = start_line; line <= end_line; line++) {
        int cur_col = 0;
        char* p;

        /* 行の先頭を探す */
        p = (char*)w->ttop;
        {
            int cl = 0;
            while (1) {
                if (p == (char*)w->tcp) p = (char*)w->tend;
                if (p >= (char*)w->tmax) break;
                if (cl == line) break;
                if (*p == '\n') cl++;
                p++;
            }
        }

        /* col_startまでスキップ */
        cur_col = 0;
        while (1) {
            char c;
            if (p == (char*)w->tcp) p = (char*)w->tend;
            if (p >= (char*)w->tmax) break;
            c = *p;
            if (c == '\n' || c == '\r') break;
            if (cur_col >= col_start) break;
            if (c == '\t') {
                cur_col = (cur_col / 8 + 1) * 8;
            } else if (IS_SJIS_1ST((unsigned char)c)) {
                cur_col += 2;
                p++;
                if (p == (char*)w->tcp) p = (char*)w->tend;
            } else {
                cur_col++;
            }
            p++;
        }

        /* col_endまでの文字を取得 */
        while (1) {
            char c;
            if (p == (char*)w->tcp) p = (char*)w->tend;
            if (p >= (char*)w->tmax) break;
            c = *p;
            if (c == '\n' || c == '\r') break;
            if (cur_col >= col_end) break;
            if (out_pos < out_max - 2) {
                out_buf[out_pos++] = c;
            }
            if (c == '\t') {
                cur_col = (cur_col / 8 + 1) * 8;
            } else if (IS_SJIS_1ST((unsigned char)c)) {
                cur_col += 2;
                p++;
                if (p == (char*)w->tcp) p = (char*)w->tend;
                if (out_pos < out_max - 2) {
                    out_buf[out_pos++] = *p;
                }
            } else {
                cur_col++;
            }
            p++;
        }

        /* 行末に改行を付加 */
        if (line < end_line && out_pos < out_max - 1) {
            out_buf[out_pos++] = '\n';
        }
    }

    if (out_pos < out_max) out_buf[out_pos] = '\0';
    return out_pos;
}

/*
 * 論理インデックスから行番号を算出
 */
static int get_line_from_index(TEXT* w, int idx)
{
    char* p;
    int line = 0;
    int i = 0;

    p = (char*)w->ttop;
    while (i < idx) {
        if (p == (char*)w->tcp) p = (char*)w->tend;
        if (p >= (char*)w->tmax) break;
        if (*p == '\n') line++;
        p++;
        i++;
    }
    return line;
}

/*
 * 論理インデックスから列位置(表示列)を算出
 */
static int get_col_from_index(TEXT* w, int idx)
{
    char* p;
    int col = 0;
    int i = 0;

    p = (char*)w->ttop;
    /* まずidxの行頭まで進む */
    {
        int last_nl = -1;
        int j;
        char* q = (char*)w->ttop;
        for (j = 0; j < idx; j++) {
            if (q == (char*)w->tcp) q = (char*)w->tend;
            if (q >= (char*)w->tmax) break;
            if (*q == '\n') last_nl = j;
            q++;
        }
        if (last_nl >= 0) {
            i = last_nl + 1;
        } else {
            i = 0;
        }
    }

    p = (char*)w->ttop + i;
    if (i > (int)((char*)w->tcp - (char*)w->ttop)) {
        p = (char*)w->tend + (i - (int)((char*)w->tcp - (char*)w->ttop));
    }
    col = 0;
    while (i < idx) {
        char c;
        if (p == (char*)w->tcp) p = (char*)w->tend;
        if (p >= (char*)w->tmax) break;
        c = *p;
        if (c == '\t') {
            col = (col / 8 + 1) * 8;
        } else if (IS_SJIS_1ST((unsigned char)c)) {
            col += 2;
            p++; i++;
            if (p == (char*)w->tcp) p = (char*)w->tend;
        } else {
            col++;
        }
        p++; i++;
    }
    return col;
}

/* ================================================================ */
/*  矩形ブロックコピー                                                */
/* ================================================================ */
void te_block_copy_rect(TEXT* w)
{
    int start_line, end_line, col_start, col_end;
    int tmp;

    if (w->block_mode != 3) return;

    start_line = get_line_from_index(w, w->tblock_start);
    end_line   = get_line_from_index(w, w->tblock_end);
    col_start  = w->block_start_col;
    col_end    = w->block_end_col;

    if (start_line > end_line) {
        tmp = start_line; start_line = end_line; end_line = tmp;
    }
    if (col_start > col_end) {
        tmp = col_start; col_start = col_end; col_end = tmp;
    }
    if (col_start == col_end) return;

    /* クリップボードに矩形テキストを転送 */
    if (vz.clipboard_buf) {
        os32_free(vz.clipboard_buf);
        vz.clipboard_buf = (void*)0;
    }
    {
        int max_size = (end_line - start_line + 1) * (col_end - col_start + 2) + 1;
        vz.clipboard_buf = os32_malloc(max_size);
        if (!vz.clipboard_buf) return;
        vz.clipboard_max = max_size;
        vz.clipboard_len = extract_rect_text(w, start_line, end_line,
                                             col_start, col_end,
                                             vz.clipboard_buf, max_size);
    }
}

/* ================================================================ */
/*  矩形ブロック削除                                                */
/* ================================================================ */
void te_block_delete_rect(TEXT* w)
{
    /* まずコピーしてから削除 */
    te_block_copy_rect(w);
    /* 矩形削除は複雑なため、現状は通知のみ */
    set_notification("矩形削除は未実装です。");
    te_unmark_block(w);
}

/* ================================================================ */
/*  矩形ブロックペースト                                              */
/* ================================================================ */
void te_block_paste_rect(TEXT* w)
{
    int i;
    if (!vz.clipboard_buf || vz.clipboard_len <= 0) return;

    /* 矩形ペースト: 各行のクリップボードデータを
       現在行から順に挿入する (簡易実装: ストリーム挿入) */
    if ((char*)w->tend - (char*)w->tcp < vz.clipboard_len + 256) {
        if (!te_resize_buffer(w, w->tbmax + vz.clipboard_len + TE_INITIAL_BUFFER_SIZE)) {
            return;
        }
    }

    for (i = 0; i < vz.clipboard_len; i++) {
        if ((char*)w->tend - (char*)w->tcp <= 1) break;
        *((char*)w->tcp) = vz.clipboard_buf[i];
        w->tcp = (char*)w->tcp + 1;
    }
    w->tchf = 1;
}
