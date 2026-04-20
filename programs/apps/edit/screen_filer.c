/*
 * screen_filer.c - OS32 VZ Editor ファイラー領域の描画
 * C89 compatible
 */

#include "vz.h"

/*
 * ファイラー画面の描画
 */
void su_redraw_filer(void) {
    int i, x, y;
    int max_disp = SCREEN_H - 2; /* 1画面の表示項目数 */
    
    /* 画面全体をクリア */
    su_fill_rect(0, 0, SCREEN_W, FUNCBAR_Y, ' ', ATR_NORMAL);
    
    /* ステータスバー（カレントディレクトリ） */
    su_clear_line(0, ATR_STATUS);
    su_put_string(0, 0, " Filer: ", ATR_STATUS);
    su_put_string(8, 0, vz.filer.current_dir, ATR_STATUS);
    
    /* リスト描画 */
    for (i = 0; i < max_disp; i++) {
        int idx = vz.filer.scroll_top + i;
        y = i + 1;
        
        if (idx >= vz.filer.entry_count) break;
        
        {
            unsigned char attr = (idx == vz.filer.cursor_idx) ? ATR_STATUS : ATR_NORMAL;
            FilerEntry* e = &vz.filer.entries[idx];
            
            /* 行全体を塗りつぶし */
            if (idx == vz.filer.cursor_idx) {
                su_clear_line(y, attr);
            }
            
            /* "[DIR]" かサイズか */
            if (e->is_dir) {
                su_put_string(2, y, "<DIR>", attr);
            } else {
                /* 数値描画 (右寄せ8桁) */
                su_put_int(2, y, (int)e->size, 8, 1, ' ', attr);
            }
            
            /* ファイル名 */
            su_put_string(12, y, e->name, attr);
        }
    }
    
    /* ファンクションバーはそのまま維持 (su_redraw_windowから呼ばれないため独立で描画) */
    su_draw_funcbar();
}
