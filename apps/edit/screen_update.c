/*
 * screen_update.c - OS32 VZ Editor 画面描画制御 (ウィンドウ管理)
 * C89 compatible
 */

#include "vz.h"

/*
 * ウインドウ全体再描画
 * 注意: VRAM転送(present)は行わない。呼び出し元(su_redraw_screen)が一括で行う。
 */
void su_redraw_window(TEXT* w)
{
    if (!w) return;

    /* 行番号を再計算 */
    te_count_position(w);

    /* スクロール位置を調整 */
    su_adjust_scroll(w);

    su_draw_statusbar(w);
    su_draw_text(w);
    su_draw_funcbar();
}

/*
 * su_redraw_screen
 * 画面全体の再描画 (マルチウインドウ対応)
 * 全ウインドウの論理VRAM書き込み後、一度だけ差分同期+present_dirtyを行う。
 */
void su_redraw_screen(void) {
    if (vz.filer.active) {
        su_redraw_filer();
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
    
    /* 一度だけ差分同期 → 変更された矩形のみVRAMに転送 */
    if (kapi) {
        su_sync_vram();
        kapi->gfx_present_dirty();
    }
}
