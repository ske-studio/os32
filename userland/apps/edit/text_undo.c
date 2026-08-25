/*
 * text_undo.c - OS32 VZ Editor テキストギャップバッファ (Undo/Redo系)
 * C89 compatible
 */

#include "vz.h"

/*
 * Undo 記録エポック操作
 */
static void advance_undo_top(void) {
    if (vz.undo_ptr != vz.undo_top) {
        /* Branching: we discard future states */
        int bottom = (vz.undo_top - vz.undo_count + UNDO_MAX) % UNDO_MAX;
        vz.undo_top = vz.undo_ptr;
        vz.undo_count = (vz.undo_top - bottom + UNDO_MAX) % UNDO_MAX;
    }
    
    if (vz.undo_count < UNDO_MAX) {
        vz.undo_count++;
    }
    vz.undo_top = (vz.undo_top + 1) % UNDO_MAX;
    vz.undo_ptr = vz.undo_top; /* 現在位置を最新にする */
}

/*
 * Undo に文字列挿入を記録
 */
void te_record_undo_insert(char c, int pos) {
    UndoEntry* ent;
    
    /* 1つ前のエントリが同じく挿入で、かつ連続しているかチェック */
    int prev_idx = (vz.undo_top - 1 + UNDO_MAX) % UNDO_MAX;
    if (vz.undo_count > 0 && vz.undo_ptr == vz.undo_top) {
        ent = &vz.undo_stack[prev_idx];
        if (ent->type == UNDO_INSERT && ent->position + ent->data_len == pos && ent->data_len < UNDO_DATA_MAX) {
            /* 文字を末尾にマージ */
            ent->data[ent->data_len++] = c;
            return;
        }
    }
    
    /* 新しいエントリを作る */
    ent = &vz.undo_stack[vz.undo_top];
    ent->type = UNDO_INSERT;
    ent->position = pos;
    ent->data[0] = c;
    ent->data_len = 1;
    advance_undo_top();
}

/*
 * Undo に文字列削除を記録
 */
void te_record_undo_delete(char c, int pos) {
    UndoEntry* ent;
    int prev_idx = (vz.undo_top - 1 + UNDO_MAX) % UNDO_MAX;
    
    if (vz.undo_count > 0 && vz.undo_ptr == vz.undo_top) {
        ent = &vz.undo_stack[prev_idx];
        
        /* Deleteキーでの連続削除 (同じ位置が削られ続ける) */
        if (ent->type == UNDO_DELETE && ent->position == pos && ent->data_len < UNDO_DATA_MAX) {
            ent->data[ent->data_len++] = c;
            return;
        }
        
        /* Backspaceキーでの連続削除 (位置が1つずつ戻っていく) */
        if (ent->type == UNDO_DELETE && ent->position - 1 == pos && ent->data_len < UNDO_DATA_MAX) {
            /* 文字列をずらして先頭に入れる */
            int i;
            for (i = ent->data_len; i > 0; i--) {
                ent->data[i] = ent->data[i - 1];
            }
            ent->data[0] = c;
            ent->position = pos;
            ent->data_len++;
            return;
        }
    }
    
    /* 新しいエントリを作る */
    ent = &vz.undo_stack[vz.undo_top];
    ent->type = UNDO_DELETE;
    ent->position = pos;
    ent->data[0] = c;
    ent->data_len = 1;
    advance_undo_top();
}

/*
 * Undo (元に戻す)
 */
void te_undo(TEXT* w) {
    UndoEntry* ent;
    int i;
    
    int used_count = (vz.undo_top - (vz.undo_top - vz.undo_count + UNDO_MAX) % UNDO_MAX + UNDO_MAX) % UNDO_MAX;
    if (vz.undo_count == 0 || used_count == 0) return;
    if (vz.undo_ptr == (vz.undo_top - vz.undo_count + UNDO_MAX) % UNDO_MAX) return; /* これ以上戻れない */
    
    /* 1つ戻る */
    vz.undo_ptr = (vz.undo_ptr - 1 + UNDO_MAX) % UNDO_MAX;
    ent = &vz.undo_stack[vz.undo_ptr];
    
    vz.undo_in_progress = 1; /* 自己記録防止 */
    
    te_move_to_index(w, ent->position);
    
    if (ent->type == UNDO_INSERT) {
        /* 文字が挿入された -> 削除する */
        w->tend = (char*)w->tend + ent->data_len;
        if (w->tend > w->tmax) w->tend = w->tmax;
    } else if (ent->type == UNDO_DELETE) {
        /* 文字が削除された -> 再挿入する */
        for (i = 0; i < ent->data_len; i++) {
            if ((char*)w->tend - (char*)w->tcp <= 1) break;
            *((char*)w->tcp) = ent->data[i];
            w->tcp = (char*)w->tcp + 1;
        }
    }
    
    w->tchf = 1;
    vz.undo_in_progress = 0;
}

/*
 * Redo (やり直し)
 */
void te_redo(TEXT* w) {
    UndoEntry* ent;
    int i;
    
    if (vz.undo_ptr == vz.undo_top) return; /* 最新なのでRedo不可 */
    
    ent = &vz.undo_stack[vz.undo_ptr];
    
    vz.undo_in_progress = 1;
    
    te_move_to_index(w, ent->position);
    
    if (ent->type == UNDO_INSERT) {
        /* 元々挿入だった操作をやり直す (= 再挿入する) */
        for (i = 0; i < ent->data_len; i++) {
            if ((char*)w->tend - (char*)w->tcp <= 1) break;
            *((char*)w->tcp) = ent->data[i];
            w->tcp = (char*)w->tcp + 1;
        }
    } else if (ent->type == UNDO_DELETE) {
        /* 元々削除だった操作をやり直す (= 再削除する) */
        w->tend = (char*)w->tend + ent->data_len;
        if (w->tend > w->tmax) w->tend = w->tmax;
    }
    
    vz.undo_ptr = (vz.undo_ptr + 1) % UNDO_MAX;
    
    w->tchf = 1;
    vz.undo_in_progress = 0;
}
