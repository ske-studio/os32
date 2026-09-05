//! chrome.rs — ウィンドウの装飾 (枠・タイトルバー・閉じるボタン) とドラッグ枠の
//! 描画 (契約 U1 / R2 / G6)。**クライアント面の内側は描かない** (アプリの領分)。
//!
//! 色は G6 の役割名 (GUI_COLOR_*) で参照する。gshell が起動時に
//! GUI_SYSTEM_PALETTE を gfx_set_palette で入れているので index = 役割。
//! 2 色クローム (14 色リース中) は W2 の領分。ここは通常のシステム色で描く。

use crate::wm::Win;
use os32api::gfx;
use os32api::gui::proto::{
    GUI_COLOR_CLOSE, GUI_COLOR_HIGHLIGHT, GUI_COLOR_LIGHT, GUI_COLOR_SHADOW, GUI_COLOR_TEXT,
    GUI_COLOR_TITLE_ACTIVE, GUI_COLOR_TITLE_INACTIVE, GUI_COLOR_TITLE_TEXT, GUI_COLOR_WINDOW,
};

/* デスクトップの背景は desktop.rs (背景 + 手引き) が持つ。 */

/// ウィンドウの装飾を描く (バックバッファ)。active = 最前面 (フォーカス)。
pub fn draw_window_chrome(win: &Win, active: bool) {
    if !win.visible {
        return;
    }
    unsafe {
        /* 立体枠 (2px)。外周 = 影 / ハイライトで簡易ベベル。 */
        if win.has_border() {
            /* 外枠 (黒) */
            gfx::gfx_rect(win.x, win.y, win.w, win.h, GUI_COLOR_TEXT);
            /* 内側ベベル: 上/左 = LIGHT、下/右 = SHADOW */
            gfx::gfx_hline(win.x + 1, win.y + 1, win.w - 2, GUI_COLOR_LIGHT);
            gfx::gfx_vline(win.x + 1, win.y + 1, win.h - 2, GUI_COLOR_LIGHT);
            gfx::gfx_hline(win.x + 1, win.y + win.h - 2, win.w - 2, GUI_COLOR_SHADOW);
            gfx::gfx_vline(win.x + win.w - 2, win.y + 1, win.h - 2, GUI_COLOR_SHADOW);
        }

        /* タイトルバー */
        let tb = win.titlebar_rect();
        let tcol = if active { GUI_COLOR_TITLE_ACTIVE } else { GUI_COLOR_TITLE_INACTIVE };
        gfx::gfx_fill_rect(tb.x, tb.y, tb.w, tb.h, tcol);
        gfx::kcg_set_scale(1);
        /* title は create/set_title で必ず NUL 終端されている。 */
        gfx::kcg_draw_utf8(tb.x + 4, tb.y + 1, win.title.as_ptr(), GUI_COLOR_TITLE_TEXT, tcol);

        /* 閉じるボタン */
        if win.has_close() {
            let cr = win.close_rect();
            gfx::gfx_fill_rect(cr.x, cr.y, cr.w, cr.h, GUI_COLOR_CLOSE);
            gfx::gfx_rect(cr.x, cr.y, cr.w, cr.h, GUI_COLOR_TEXT);
            gfx::gfx_line(cr.x + 3, cr.y + 3, cr.x + cr.w - 4, cr.y + cr.h - 4, GUI_COLOR_WINDOW);
            gfx::gfx_line(cr.x + cr.w - 4, cr.y + 3, cr.x + 3, cr.y + cr.h - 4, GUI_COLOR_WINDOW);
        }
    }
}

/// ドラッグ枠の 1px アウトラインを HIGHLIGHT 色で描く (バックバッファ)。
/// 消去は下地 (デスクトップ + クローム) の再合成で行う (wm.rs の compositor)。
/// XOR の 2 色版・パターン版は W2。
pub fn draw_drag_outline(x: i32, y: i32, w: i32, h: i32) {
    if w <= 0 || h <= 0 {
        return;
    }
    unsafe {
        gfx::gfx_rect(x, y, w, h, GUI_COLOR_HIGHLIGHT);
    }
}
