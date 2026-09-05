//! damage.rs — 損傷矩形 (契約 G4 の dirty / issued、32px 境界、隣接結合)。
//!
//! 座標はすべてクライアントローカル。`add_dirty` は 32px グリッドへ丸めてから
//! 既存 dirty と重なる/隣接するものを結合する (既存 `gfx_add_dirty_rect` と同じ規則、
//! 上限 8/ウィンドウ)。上限を超える場合は外接矩形へ潰す (過剰申告は安全側)。

use crate::wm::{Rect, RectSet, Win, DAMAGE_SNAP, MAX_DMG};

/// 32px グリッドへ拡張する。
fn snap32(r: Rect) -> Rect {
    if r.is_empty() {
        return r;
    }
    let x0 = (r.x / DAMAGE_SNAP) * DAMAGE_SNAP;
    let y0 = (r.y / DAMAGE_SNAP) * DAMAGE_SNAP;
    let x1 = ((r.right() + DAMAGE_SNAP - 1) / DAMAGE_SNAP) * DAMAGE_SNAP;
    let y1 = ((r.bottom() + DAMAGE_SNAP - 1) / DAMAGE_SNAP) * DAMAGE_SNAP;
    Rect::new(x0, y0, x1 - x0, y1 - y0)
}

/// 2 矩形が重なる、または 32px 以内で隣接しているか (結合判定)。
fn near(a: &Rect, b: &Rect) -> bool {
    let ax0 = a.x - DAMAGE_SNAP;
    let ay0 = a.y - DAMAGE_SNAP;
    let aw = a.w + DAMAGE_SNAP * 2;
    let ah = a.h + DAMAGE_SNAP * 2;
    Rect::new(ax0, ay0, aw, ah).intersects(b)
}

/// dirty へ 1 矩形を足す (32px 丸め + 隣接結合、上限 8)。
pub fn add_dirty(win: &mut Win, rect_local: Rect) {
    /* クライアント矩形にクランプ。 */
    let (cw, ch) = win.client_size();
    let clamped = rect_local.intersect(&Rect::new(0, 0, cw, ch));
    if clamped.is_empty() {
        return;
    }
    let mut r = snap32(clamped);
    r = r.intersect(&Rect::new(0, 0, cw, ch));
    if r.is_empty() {
        return;
    }

    /* 既存と結合できるなら union する。 */
    let mut i = 0;
    while i < win.dirty.len {
        if near(&win.dirty.rects[i], &r) {
            let merged = win.dirty.rects[i].union(&r);
            /* 結合後の重複を畳むため、この要素を除いて再帰的に足し直す。 */
            remove_at(&mut win.dirty, i);
            add_dirty(win, merged);
            return;
        }
        i += 1;
    }

    if win.dirty.len < MAX_DMG {
        win.dirty.push(r);
    } else {
        /* 上限超過: 先頭と union して数を保つ (過剰申告=安全)。 */
        let merged = win.dirty.rects[0].union(&r);
        win.dirty.rects[0] = merged;
    }
}

/// dirty をクライアント全面 1 枚にする (露出計算の容量超過フォールバック等)。
pub fn set_dirty_full(win: &mut Win) {
    let (cw, ch) = win.client_size();
    win.dirty.clear();
    if cw > 0 && ch > 0 {
        win.dirty.push(Rect::new(0, 0, cw, ch));
    }
}

fn remove_at(set: &mut RectSet, i: usize) {
    if i >= set.len {
        return;
    }
    let mut j = i;
    while j + 1 < set.len {
        set.rects[j] = set.rects[j + 1];
        j += 1;
    }
    set.len -= 1;
}

/// dirty のうち index 番目を可視領域でクリップした断片 (Paint の矩形候補)。
pub fn clip_to_vis(win: &Win, dirty_rect: Rect) -> RectSet {
    let mut out = RectSet::EMPTY;
    let mut i = 0;
    while i < win.vis.len {
        let piece = dirty_rect.intersect(&win.vis.rects[i]);
        if !piece.is_empty() {
            out.push(piece);
        }
        i += 1;
    }
    out
}

/// dirty ∩ visible が空でない (= 配送できる Paint がある) か。契約 T3 の
/// `OP_WAIT` 起床条件。完全に隠れた窓 (vis 空) では常に false。
pub fn has_deliverable_paint(win: &Win) -> bool {
    if win.vis.is_empty() || win.dirty.is_empty() {
        return false;
    }
    let mut i = 0;
    while i < win.dirty.len {
        let mut k = 0;
        while k < win.vis.len {
            if win.dirty.rects[i].intersects(&win.vis.rects[k]) {
                return true;
            }
            k += 1;
        }
        i += 1;
    }
    false
}
