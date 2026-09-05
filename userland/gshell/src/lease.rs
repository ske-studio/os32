//! lease.rs — パレットのリース (契約 G8、票 W2 の B)。
//!
//! フォーカスを持つウィンドウの所有者だけが、バックエンドが許す範囲
//! (`gfx_screen_info()` の `lease_mask` / `lease_first` / `lease_count`) の
//! パレット項目を自分の色に置き換えられる。リースは**ウィンドウに紐づき、
//! フォーカスに従って**入れ替わる:
//!
//! ```text
//!   LEASE_PALETTE(first, count, rgb)  … 窓にリースを記録 (X1: 状態更新だけ)
//!   フォーカスが移る / リースが変わる … reconcile() が
//!        旧: gfx_set_palette でシステム色へ戻す + Palette{active:0}
//!        新: gfx_lease_palette でアプリ色を入れる + Palette{active:1}
//!   リース中は WM のクロームが 2 色 + パターンになる (chrome.rs / desktop.rs)
//! ```
//!
//! 実際に `gfx_lease_palette` を呼ぶのは **X2 (COMMIT) / X3 (WAIT)** で、
//! `gui_call` のハンドラ (X1) は記録だけを行う (契約 T8 の「X1 で KAPI を
//! 呼ばない」)。遅れは高々 1 周で、アプリから見た色は最初の commit から正しい。

use crate::wm::{self, GuiState, Rect};
use crate::{ring, visible};
use os32api::gui::proto::{GuiRgb, GUI_EV_PALETTE, OS32_ERR_INVAL};

/// 16 色機で貸せる最大項目数 (契約 G8 の 14 色 + 余裕)。
pub const MAX_LEASE: usize = 16;

/* ================================================================ */
/*  要求の受理 (X1)                                                  */
/* ================================================================ */

/// `LEASE_PALETTE` の中身を検証してフォーカス窓へ記録する。
///
/// - フォーカス (最前面) の窓が `owner` のものでなければ `OS32_ERR_INVAL`。
/// - `count == 0` は返却 (次の [`reconcile`] でシステム色へ戻る)。
/// - 範囲は `gfx_screen_info()` の値で弾く (H1 側でも二重に弾かれる)。
pub fn request(st: &mut GuiState, owner: i32, first: u16, count: u16, rgb: &[GuiRgb; 16]) -> i32 {
    let index = match st.front_index() {
        Some(i) if st.windows[i].owner == owner => i,
        _ => return OS32_ERR_INVAL,
    };
    if count == 0 {
        st.windows[index].lease_used = false;
        st.windows[index].lease_count = 0;
        st.lease_dirty = true;
        return 0;
    }
    if count as usize > MAX_LEASE {
        return OS32_ERR_INVAL;
    }
    if !range_ok(st, first, count) {
        return OS32_ERR_INVAL;
    }
    st.windows[index].lease_used = true;
    st.windows[index].lease_first = first;
    st.windows[index].lease_count = count;
    let mut i = 0;
    while i < MAX_LEASE {
        st.windows[index].lease_rgb[i] = rgb[i];
        i += 1;
    }
    st.lease_dirty = true;
    0
}

/// 貸せる範囲か (契約 G8。16 色機は `lease_mask`、256 色機は連続範囲)。
fn range_ok(st: &GuiState, first: u16, count: u16) -> bool {
    if st.lease_count > 0 {
        /* 256 色機: [lease_first, lease_first + lease_count) */
        first >= st.lease_first && (first as u32) + (count as u32)
            <= (st.lease_first as u32) + (st.lease_count as u32)
    } else {
        /* 16 色機: lease_mask のビットが立つ index だけ */
        if (first as u32) + (count as u32) > 16 {
            return false;
        }
        let mut i = first;
        while i < first + count {
            if (st.lease_mask & (1u16 << i)) == 0 {
                return false;
            }
            i += 1;
        }
        true
    }
}

/* ================================================================ */
/*  適用 / 返却 (X2 / X3)                                            */
/* ================================================================ */

/// いま適用されているべきリース元ウィンドウの完全な id (0 = リース無し)。
pub fn desired(st: &GuiState) -> u32 {
    match st.front_index() {
        Some(i) if st.windows[i].used && st.windows[i].lease_used => st.windows[i].id(i),
        _ => 0,
    }
}

/// リース中か (= WM のクロームを 2 色で描くか)。
#[inline]
pub fn mono(st: &GuiState) -> bool {
    st.lease_applied != 0
}

/// 望まれる状態と実際の状態を合わせる。差が無ければ即戻る (毎周呼んでよい)。
pub fn reconcile(st: &mut GuiState) {
    let want = desired(st);
    let cur = st.lease_applied;
    if want == cur && !st.lease_dirty {
        return;
    }
    st.lease_dirty = false;

    if cur != 0 && cur != want {
        wm::install_system_palette();
        notify(st, cur, false);
    }
    if want != 0 {
        apply(st, want);
        if cur != want {
            notify(st, want, true);
        }
    } else if cur != 0 {
        /* 上で戻し済み。 */
    }

    let mono_before = cur != 0;
    let mono_after = want != 0;
    st.lease_applied = want;
    if mono_before != mono_after {
        /* クロームとデスクトップの描き方が変わるので全面を積み直す。 */
        let whole = Rect::new(0, 0, st.screen_w, st.screen_h);
        st.dirty_screen(whole);
        visible::recompute_and_expose(st);
    }
}

/// フルスクリーン GFX からの復帰などで、いまのリースを入れ直す。
pub fn reapply(st: &mut GuiState) {
    if st.lease_applied != 0 {
        apply(st, st.lease_applied);
    }
}

fn apply(st: &GuiState, win_id: u32) {
    let index = match st.win_by_id(win_id) {
        Some(i) => i,
        None => return,
    };
    let w = &st.windows[index];
    if !w.lease_used || w.lease_count == 0 {
        return;
    }
    /* GuiRgb は u8 × 3 (align 1) なので配列は 3B/項目でそのまま渡せる。 */
    let p = w.lease_rgb.as_ptr() as *const u8;
    unsafe {
        (os32api::api().gfx_lease_palette)(w.lease_first as i32, w.lease_count as i32, p);
    }
}

/// `Palette{active}` をリース元のアプリへ届ける (契約 G8 / U2)。
fn notify(st: &mut GuiState, win_id: u32, active: bool) {
    let index = match st.win_by_id(win_id) {
        Some(i) => i,
        None => return,
    };
    let owner = st.windows[index].owner;
    if let Some(slot) = st.slot_of_owner(owner) {
        let ev = ring::ev_simple(GUI_EV_PALETTE, active as u8, win_id);
        ring::append(st, slot, &ev);
    }
}

/// ウィンドウが消える / 破棄されるときにリースを落とす。
pub fn release_window(st: &mut GuiState, index: usize) {
    if !st.windows[index].lease_used {
        return;
    }
    st.windows[index].lease_used = false;
    st.windows[index].lease_count = 0;
    if st.lease_applied == st.windows[index].id(index) {
        st.lease_dirty = true;
    }
}
