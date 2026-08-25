/*
 * text_buffer.c - OS32 VZ Editor テキストギャップバッファ (メモリ管理部分)
 * C89 compatible
 */

#include "vz.h"

/* TE_INITIAL_BUFFER_SIZE は vz.h で定義 */

/*
 * ギャップバッファ初期化
 */
void te_init_buffer(TEXT* w)
{
    if (!w) return;

    /* フラットテキストバッファを確保 */
    w->ttop = os32_malloc(TE_INITIAL_BUFFER_SIZE);
    if (!w->ttop) {
        os32_printf("te_init_buffer: out of memory\n");
        return;
    }

    w->tmax = (void*)((char*)w->ttop + TE_INITIAL_BUFFER_SIZE);
    w->tbmax = TE_INITIAL_BUFFER_SIZE;

    /* 初期状態: ギャップが全バッファ */
    w->tcp = w->ttop;           /* ギャップ開始 */
    w->tend = w->tmax;          /* ギャップ終了 */

    /* 画面表示状態 */
    w->thom = w->ttop;          /* 画面先頭 = バッファ先頭 */
    w->lx = 0;
    w->ly = 0;
    w->lnumb = 1;               /* 行番号は1から */

    /* ウインドウサイズデフォルト */
    if (w->tw_sx == 0) w->tw_sx = 80;
    if (w->tw_sy == 0) w->tw_sy = 23;

    /* ブロック操作状態 */
    w->tblock_start = -1;
    w->tblock_end = -1;
    w->block_mode = 0;
    w->block_start_col = 0;
    w->block_end_col = 0;

    /* 行番号キャッシュの初期化 */
    w->last_tcp = NULL;
    w->last_tcp_line = 1;
    w->last_thom = NULL;
    w->last_thom_line = 0;
}

/*
 * バッファサイズを拡張（または縮小）する
 */
int te_resize_buffer(TEXT* w, unsigned int new_size) {
    char* new_buf;
    unsigned int size1, size2;
    int cur_idx_thom, cur_idx_tbtm, cur_idx_tnow, cur_idx_tnxt;

    if (!w || new_size <= w->tbmax) return 0;

    new_buf = (char*)os32_malloc(new_size);
    if (!new_buf) return 0; /* メモリ確保失敗 */

    /* ポインタを論理インデックスに変換して一時保存 */
    cur_idx_thom = w->thom ? te_get_logical_index(w, (char*)w->thom) : -1;
    cur_idx_tbtm = w->tbtm ? te_get_logical_index(w, (char*)w->tbtm) : -1;
    cur_idx_tnow = w->tnow ? te_get_logical_index(w, (char*)w->tnow) : -1;
    cur_idx_tnxt = w->tnxt ? te_get_logical_index(w, (char*)w->tnxt) : -1;

    /* 前半部、後半部のサイズを計算 */
    size1 = (unsigned int)((char*)w->tcp - (char*)w->ttop);
    size2 = (unsigned int)((char*)w->tmax - (char*)w->tend);

    if (size1 > 0) {
        int i;
        char* src = (char*)w->ttop;
        char* dst = new_buf;
        for(i = 0; i < size1; i++) dst[i] = src[i];
    }
    
    if (size2 > 0) {
        int i;
        char* src = (char*)w->tend;
        char* dst = new_buf + new_size - size2;
        for(i = 0; i < size2; i++) dst[i] = src[i];
    }

    /* 旧バッファを解放 */
    os32_free(w->ttop);

    /* 新しいポインタ構造の構築 */
    w->ttop = new_buf;
    w->tmax = new_buf + new_size;
    w->tbmax = new_size;
    
    w->tcp = new_buf + size1;
    w->tend = new_buf + new_size - size2;

    /* 保存した論理インデックスからポインタを復元 */
    w->thom = (cur_idx_thom != -1) ? te_get_pointer_from_index(w, cur_idx_thom) : w->ttop;
    w->tbtm = (cur_idx_tbtm != -1) ? te_get_pointer_from_index(w, cur_idx_tbtm) : NULL;
    w->tnow = (cur_idx_tnow != -1) ? te_get_pointer_from_index(w, cur_idx_tnow) : NULL;
    w->tnxt = (cur_idx_tnxt != -1) ? te_get_pointer_from_index(w, cur_idx_tnxt) : NULL;
    
    /* tcp, tend に関連する行バッファのポインタ郡 (btop, bmax等) は
       文字入力や表示ループで都度初期化・再設定されるため、ここではクリアすればOK */
    w->btop = NULL;
    w->bend = NULL;
    w->bmax = NULL;

    /* 行番号キャッシュの無効化 (ポインタが変わったため) */
    w->last_tcp = NULL;
    w->last_thom = NULL;

    return 1;
}

/*
 * バッファ縮小
 */
int te_shrink_buffer(TEXT* w)
{
    unsigned int text_size;
    unsigned int target;
    if (!w || !w->ttop) return 0;

    text_size = (unsigned int)((char*)w->tcp - (char*)w->ttop)
              + (unsigned int)((char*)w->tmax - (char*)w->tend);

    /* 縮小先サイズ: テキスト量の倍 + 64KB (最低64KB) */
    target = text_size * 2 + TE_INITIAL_BUFFER_SIZE;
    if (target < TE_INITIAL_BUFFER_SIZE) target = TE_INITIAL_BUFFER_SIZE;
    if (target >= w->tbmax) return 0; /* 縮小不要 */

    return te_resize_buffer(w, target);
}

/*
 * 論理インデックス(0〜テキスト長)からポインターを取得/逆変換
 */
int te_get_logical_index(TEXT* w, char* p) {
    if (p < (char*)w->tcp) return (int)(p - (char*)w->ttop);
    return (int)((p - (char*)w->tend) + ((char*)w->tcp - (char*)w->ttop));
}

char* te_get_pointer_from_index(TEXT* w, int index) {
    int left_size = (int)((char*)w->tcp - (char*)w->ttop);
    if (index < left_size) return (char*)w->ttop + index;
    return (char*)w->tend + (index - left_size);
}

/*
 * ファイル保存
 * ギャップバッファの内容(ttop〜tcp、tend〜tmax)をファイルに書き出す
 * 戻り値: 成功1、失敗0
 */
int te_save_file(TEXT* w)
{
    int fd;
    unsigned int size1, size2;
    
    if (!w || !w->namep || w->namep[0] == '\0') return 0;

    /* O_WRONLY(0x01) | O_CREAT(0x0100) | O_TRUNC(0x0200) */
    fd = vz_open(w->namep, 0x01 | 0x0100 | 0x0200);
    if (fd < 0) {
        return 0;
    }

    size1 = (unsigned int)((char*)w->tcp - (char*)w->ttop);
    size2 = (unsigned int)((char*)w->tmax - (char*)w->tend);

    if (size1 > 0) {
        vz_write(fd, w->ttop, size1);
    }
    if (size2 > 0) {
        vz_write(fd, w->tend, size2);
    }

    vz_close(fd);

    /* 変更フラグをクリア */
    w->tchf = 0;

    return 1;
}
