/*
 * screen_update.c - OS32 VZ Editor 画面描画制御 (ウィンドウ管理)
 * C89 compatible
 */

#include "vz.h"

/*
 * ウインドウ全体再描画
 */
void su_redraw_window(TEXT* w)
{
    static TEXT* prev_w = NULL;
    static char* prev_thom = NULL;
    static int first_draw = 1;
    int scrolled = 0;
    int scroll_lines = 0;
    int is_scroll_down = 0;

    if (!w) return;

    /* 行番号を再計算 */
    te_count_position(w);

    /* スクロール位置を調整 */
    su_adjust_scroll(w);

    /* 仮想VRAMエンジンが完成したため、OS全体のハードウェアスクロールや全再描画によるチラツキの抑止を廃止 */
    /* 差分描画(Cell-based diffing)に完全に任せることで、最速のネイティブスクロールを実現する */

    prev_w = w;
    prev_thom = (char*)w->thom;
    first_draw = 0;

    su_draw_statusbar(w);
    su_draw_text(w);
    su_draw_funcbar();

    if (kapi) {
        su_sync_vram();
        kapi->gfx_present();
    }
}

/*
 * su_redraw_screen
 * 画面全体の再描画 (マルチウインドウ対応)
 */
void su_redraw_screen(void) {
    if (vz.filer.active) {
        su_redraw_filer();
        if (kapi) kapi->gfx_present();
    } else if (vz.split_mode == 0) {
        /* シングルウインドウ */
        if (vz.text_list[vz.window_text_idx[0]]) {
            TEXT* t = vz.text_list[vz.window_text_idx[0]];
            t->tw_py = 1;
            t->tw_sy = SCREEN_H - 2;
            su_redraw_window(t);
        }
    } else {
        /* マルチウインドウ (上下2分割) */
        TEXT* t0 = vz.text_list[vz.window_text_idx[0]];
        TEXT* t1 = vz.text_list[vz.window_text_idx[1]];
        if (t0) {
            t0->tw_py = 1;
            t0->tw_sy = (SCREEN_H - 3) / 2;
            su_redraw_window(t0);
        }
        if (t1) {
            t1->tw_py = 1 + ((SCREEN_H - 3) / 2) + 1;
            t1->tw_sy = SCREEN_H - 2 - t1->tw_py;
            su_redraw_window(t1);
        }
    }

    if (vz.help_active) {
        su_draw_help();
    }
    
    if (kapi) {
        su_sync_vram();
        kapi->gfx_present();
    }
}
