//! desktop.rs — デスクトップ (背景と操作の手引き)。v1.2 でタスクバー。
//!
//! `wm::composite_rect` から「どのウィンドウにも覆われていない矩形」ごとに
//! 呼ばれる。塗るのは背景色 (G6 の `DESKTOP`) だけで、文字は**掛かるウィンドウが
//! 無いときにだけ**描く (kcg にはクリップが無く、窓の上へはみ出せないため)。

use crate::wm::{GuiState, Rect};
use os32api::gfx;
use os32api::gui::proto::{GUI_COLOR_DESKTOP, GUI_COLOR_TITLE_TEXT};

/// 手引きの位置 (左上) と占有矩形。ANK 8x16 なので 1 文字 8px。
const HINT_X: i32 = 8;
const HINT_Y: i32 = 8;
const HINT_W: i32 = 8 * 44;
const HINT_H: i32 = 16;

/// 単独起動時の手引き (UTF-8, NUL 終端)。全角は 16px 幅。
static HINT: &[u8] = b"OS32 GUI shell   ESC: CUI  /  F1: gui_demo\0";

/// 背景の 1 矩形を塗る (画面座標、バックバッファ)。
pub fn fill(_st: &GuiState, r: Rect) {
    if r.is_empty() {
        return;
    }
    unsafe {
        gfx::gfx_fill_rect(r.x, r.y, r.w, r.h, GUI_COLOR_DESKTOP);
    }
}

/// 手引きの占有矩形。
#[inline]
pub fn hint_rect() -> Rect {
    Rect::new(HINT_X, HINT_Y, HINT_W, HINT_H)
}

/// 手引きの文字を描く。掛かるウィンドウが 1 枚でもあれば描かない
/// (kcg_draw_utf8 にクリップが無いので、はみ出して窓を壊さないための保険)。
pub fn draw_hint(st: &GuiState) {
    let hr = hint_rect();
    let mut z = 0;
    while z < st.z_count {
        let idx = st.zorder[z];
        let w = &st.windows[idx];
        if w.used && w.visible && w.outer().intersects(&hr) {
            return;
        }
        z += 1;
    }
    unsafe {
        gfx::kcg_set_scale(1);
        gfx::kcg_draw_utf8(HINT_X, HINT_Y, HINT.as_ptr(), GUI_COLOR_TITLE_TEXT, GUI_COLOR_DESKTOP);
    }
}
