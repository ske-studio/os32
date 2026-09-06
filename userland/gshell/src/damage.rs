//! damage.rs — 損傷矩形 (契約 G4 の dirty / issued、32px 境界、隣接結合)。
//!
//! 座標はすべてクライアントローカル。`add_dirty` は 32px グリッドへ丸めてから
//! 既存 dirty と重なる/隣接するものを結合する (既存 `gfx_add_dirty_rect` と同じ規則、
//! 上限 8/ウィンドウ)。上限を超える場合は外接矩形へ潰す (過剰申告は安全側)。

use crate::wm::{Rect, RectSet, Win, DAMAGE_SNAP, MAX_DMG, MAX_VIS};

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

/// dirty ∩ 可視領域 = **配送できる `Paint` の候補**。`(添字 = 元の dirty,
/// 断片)` を最大 `MAX_VIS` 件。
///
/// **起床判定 (`OP_WAIT`) も配送 (`OP_POLL`) も必ずこれを通す。** 2 か所で
/// 別々に書くと必ず食い違い、「起こされないと配れない / 配れないと起きられない」
/// で止まる (v1.2 G3 で実測: 打鍵の結果がマウスを動かすまで画面に出ない)。
pub fn deliverable_cand(win: &Win) -> ([(usize, Rect); MAX_VIS], usize) {
    let mut cand = [(0usize, Rect::EMPTY); MAX_VIS];
    let mut n = 0;
    if win.dirty.is_empty() || win.vis.is_empty() {
        return (cand, n);
    }
    let mut i = 0;
    while i < win.dirty.len && n < MAX_VIS {
        let pieces = clip_to_vis(win, win.dirty.rects[i]);
        let mut k = 0;
        while k < pieces.len && n < MAX_VIS {
            cand[n] = (i, pieces.rects[k]);
            n += 1;
            k += 1;
        }
        i += 1;
    }
    (cand, n)
}

/// 配送できる `Paint` があるか (契約 T3 の `OP_WAIT` 起床条件)。
/// 完全に隠れた窓 (可視領域が空) では常に false — 露出で初めて配送対象になる
/// (契約 G4)。
pub fn has_deliverable_paint(win: &Win) -> bool {
    if win.dirty.is_empty() {
        return false;
    }
    /* 可視領域が打ち切られている窓 (契約 G4「超過分は次の周」) の `vis` は
     * 真の可視領域の**部分集合**でしかなく、断片を入れ替えるのは `OP_POLL` の
     * 中の `page_vis` だけ。ここで部分集合だけを見て「配送できない」と決めると、
     * 入れ替えの機会そのものが来ない。dirty があるなら起こして `OP_POLL` に
     * 判断させる (そこで配れなければ `emit_paints_win` が dirty を掃除する)。 */
    if win.vis_capped {
        return true;
    }
    deliverable_cand(win).1 > 0
}
