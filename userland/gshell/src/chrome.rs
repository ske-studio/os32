//! chrome.rs — ウィンドウの装飾 (枠・タイトルバー・閉じるボタン) とドラッグ枠の
//! 描画 (契約 U1 / R2 / G6 / G8)。**クライアント面の内側は描かない** (アプリの領分)。
//!
//! 色は G6 の役割名 (`GUI_COLOR_*`) で参照する。gshell が起動時に
//! `GUI_SYSTEM_PALETTE` を `gfx_set_palette` で入れているので index = 役割。
//!
//! **2 色クローム (第 2 モード、票 W2 の B-3)**: 14 色リース中はシステム色が
//! アプリのものに置き換わっているので、WM は不可侵の 2 色 (`TEXT` = 0 と
//! `WINDOW` = 7) とパターンだけで描く (契約 G8):
//!
//! | 部位 | 通常 | 2 色 |
//! |---|---|---|
//! | 文字・枠 | `TEXT` | `TEXT` |
//! | 面 | `FACE` / `WINDOW` | `WINDOW` |
//! | 影・無効面 | `SHADOW` | `DITHER50` (50% 市松) |
//! | フォーカス | `HIGHLIGHT` | `DOTTED` (点線) |
//! | アクティブタイトル | `TITLE_ACTIVE` 地 + `TITLE_TEXT` 字 | 黒地 + 白字 |
//! | 非アクティブタイトル | `TITLE_INACTIVE` 地 | 白地 + 黒字 |
//! | デスクトップ | `DESKTOP` | 市松 ([`crate::desktop`]) |
//!
//! ウィンドウの外 (他のウィンドウ・デスクトップ) の色崩れは許容する
//! (ユーザ決定 2026-09-04)。

use crate::wm::Win;
use os32api::gfx;
use os32api::gui::proto::{
    GUI_COLOR_CLOSE, GUI_COLOR_HIGHLIGHT, GUI_COLOR_LIGHT, GUI_COLOR_SHADOW, GUI_COLOR_TEXT,
    GUI_COLOR_TITLE_ACTIVE, GUI_COLOR_TITLE_INACTIVE, GUI_COLOR_TITLE_TEXT, GUI_COLOR_WINDOW,
};

/* デスクトップの背景は desktop.rs (背景 + 手引き) が持つ。 */

/* ================================================================ */
/*  パターン (Style.flags の DITHER50 / DOTTED に対応)               */
/* ================================================================ */

/// 50% 市松で塗る (`GUI_STYLE_DITHER50`)。地色は塗らない (透過)。
/// 小さな面 (影・無効面) 向け。広い面は [`crate::desktop`] の粗い市松を使う。
pub fn dither50(x: i32, y: i32, w: i32, h: i32, fg: u8) {
    if w <= 0 || h <= 0 {
        return;
    }
    let mut yy = 0;
    while yy < h {
        let mut xx = (yy & 1) as i32;
        while xx < w {
            unsafe { gfx::gfx_pixel(x + xx, y + yy, fg) };
            xx += 2;
        }
        yy += 1;
    }
}

/// 点線の 1px 矩形 (`GUI_STYLE_DOTTED`)。フォーカスリング用。
pub fn dotted_rect(x: i32, y: i32, w: i32, h: i32, fg: u8) {
    if w <= 0 || h <= 0 {
        return;
    }
    let mut i = 0;
    while i < w {
        if (i & 1) == 0 {
            unsafe {
                gfx::gfx_pixel(x + i, y, fg);
                gfx::gfx_pixel(x + i, y + h - 1, fg);
            }
        }
        i += 1;
    }
    let mut j = 0;
    while j < h {
        if (j & 1) == 0 {
            unsafe {
                gfx::gfx_pixel(x, y + j, fg);
                gfx::gfx_pixel(x + w - 1, y + j, fg);
            }
        }
        j += 1;
    }
}

/* ================================================================ */
/*  ウィンドウの装飾                                                 */
/* ================================================================ */

/// ウィンドウの装飾を描く (バックバッファ)。
/// `active` = 最前面 (フォーカス)、`mono` = 14 色リース中の 2 色モード。
pub fn draw_window_chrome(win: &Win, active: bool, mono: bool) {
    if !win.visible {
        return;
    }
    unsafe {
        /* 立体枠 (2px)。外周 = 影 / ハイライトで簡易ベベル。 */
        if win.has_border() {
            /* 外枠 (黒) */
            gfx::gfx_rect(win.x, win.y, win.w, win.h, GUI_COLOR_TEXT);
            if mono {
                /* 影は市松、ハイライトは白。 */
                gfx::gfx_hline(win.x + 1, win.y + 1, win.w - 2, GUI_COLOR_WINDOW);
                gfx::gfx_vline(win.x + 1, win.y + 1, win.h - 2, GUI_COLOR_WINDOW);
                dither50(win.x + 1, win.y + win.h - 2, win.w - 2, 1, GUI_COLOR_TEXT);
                dither50(win.x + win.w - 2, win.y + 1, 1, win.h - 2, GUI_COLOR_TEXT);
            } else {
                /* 内側ベベル: 上/左 = LIGHT、下/右 = SHADOW */
                gfx::gfx_hline(win.x + 1, win.y + 1, win.w - 2, GUI_COLOR_LIGHT);
                gfx::gfx_vline(win.x + 1, win.y + 1, win.h - 2, GUI_COLOR_LIGHT);
                gfx::gfx_hline(win.x + 1, win.y + win.h - 2, win.w - 2, GUI_COLOR_SHADOW);
                gfx::gfx_vline(win.x + win.w - 2, win.y + 1, win.h - 2, GUI_COLOR_SHADOW);
            }
        }

        /* タイトルバー */
        let tb = win.titlebar_rect();
        let (tcol, ttxt) = if mono {
            /* アクティブ = 黒地に白文字、非アクティブ = 白地に黒文字。 */
            if active {
                (GUI_COLOR_TEXT, GUI_COLOR_WINDOW)
            } else {
                (GUI_COLOR_WINDOW, GUI_COLOR_TEXT)
            }
        } else if active {
            (GUI_COLOR_TITLE_ACTIVE, GUI_COLOR_TITLE_TEXT)
        } else {
            (GUI_COLOR_TITLE_INACTIVE, GUI_COLOR_TITLE_TEXT)
        };
        gfx::gfx_fill_rect(tb.x, tb.y, tb.w, tb.h, tcol);
        gfx::kcg_set_scale(1);
        /* title は create/set_title で必ず NUL 終端されている。 */
        gfx::kcg_draw_utf8(tb.x + 4, tb.y + 1, win.title.as_ptr(), ttxt, tcol);

        /* 閉じるボタン */
        if win.has_close() {
            let cr = win.close_rect();
            let face = if mono { GUI_COLOR_WINDOW } else { GUI_COLOR_CLOSE };
            let mark = if mono { GUI_COLOR_TEXT } else { GUI_COLOR_WINDOW };
            gfx::gfx_fill_rect(cr.x, cr.y, cr.w, cr.h, face);
            gfx::gfx_rect(cr.x, cr.y, cr.w, cr.h, GUI_COLOR_TEXT);
            gfx::gfx_line(cr.x + 3, cr.y + 3, cr.x + cr.w - 4, cr.y + cr.h - 4, mark);
            gfx::gfx_line(cr.x + cr.w - 4, cr.y + 3, cr.x + 3, cr.y + cr.h - 4, mark);
        }
    }

    /* フォーカスの点線 (2 色モードでは色でなく形で示す)。 */
    if mono && active && win.has_border() {
        dotted_rect(win.x + 2, win.y + 2, win.w - 4, win.h - 4, GUI_COLOR_TEXT);
    }
}

/// ドラッグ枠の 1px アウトラインを描く (バックバッファ)。
/// 消去は下地 (デスクトップ + クローム) の再合成で行う (wm.rs の compositor)。
/// 2 色モードでは色を使えないので**点線**で描く (契約 G8 の DOTTED)。
pub fn draw_drag_outline(x: i32, y: i32, w: i32, h: i32, mono: bool) {
    if w <= 0 || h <= 0 {
        return;
    }
    if mono {
        dotted_rect(x, y, w, h, GUI_COLOR_TEXT);
    } else {
        unsafe {
            gfx::gfx_rect(x, y, w, h, GUI_COLOR_HIGHLIGHT);
        }
    }
}
