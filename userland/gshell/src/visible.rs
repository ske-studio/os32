//! visible.rs — 可視領域の計算 (契約 G4)。
//!
//! ウィンドウごとに「クライアント矩形 − 上にあるウィンドウの外形」を **互いに素な
//! 矩形の集合** (最大 16、クライアントローカル座標) として持つ。Z 順・移動・表示・
//! 破棄のたびに再計算し、露出した分をそのウィンドウの `dirty` に足して `Paint` を
//! 出させる。`Paint` の矩形は常に可視領域の内側 (= 背面が前面を上書きしない)。

use crate::damage;
use crate::wm::{GuiState, Rect, RectSet, MAX_VIS};
use os32api::gui::proto::GUI_MAX_WINDOWS;

/// `a` から `b` を引いた最大 4 個の矩形を out へ push する。
pub fn rect_subtract(a: Rect, b: Rect, out: &mut RectSet) {
    let c = a.intersect(&b);
    if c.is_empty() {
        out.push(a);
        return;
    }
    /* 上 */
    if c.y > a.y {
        out.push(Rect::new(a.x, a.y, a.w, c.y - a.y));
    }
    /* 下 */
    if c.bottom() < a.bottom() {
        out.push(Rect::new(a.x, c.bottom(), a.w, a.bottom() - c.bottom()));
    }
    /* 左 (交差の高さ分だけ) */
    if c.x > a.x {
        out.push(Rect::new(a.x, c.y, c.x - a.x, c.h));
    }
    /* 右 */
    if c.right() < a.right() {
        out.push(Rect::new(c.right(), c.y, a.right() - c.right(), c.h));
    }
}

/// region 全体から穴 `hole` を引く。返り値 false = 容量 (16) 超過で打ち切り。
pub fn region_subtract_rect(region: &RectSet, hole: Rect) -> (RectSet, bool) {
    let mut out = RectSet::EMPTY;
    let mut ok = true;
    let mut i = 0;
    while i < region.len {
        let r = region.rects[i];
        if !r.intersects(&hole) {
            if !out.push(r) {
                ok = false;
            }
        } else {
            let mut pieces = RectSet::EMPTY;
            rect_subtract(r, hole, &mut pieces);
            let mut k = 0;
            while k < pieces.len {
                if !out.push(pieces.rects[k]) {
                    ok = false;
                }
                k += 1;
            }
        }
        i += 1;
    }
    (out, ok)
}

/// region_a から region_b を丸ごと引く (a − b)。false = 容量超過。
pub fn region_subtract_region(a: &RectSet, b: &RectSet) -> (RectSet, bool) {
    let mut cur = *a;
    let mut ok = true;
    let mut i = 0;
    while i < b.len {
        let (next, o) = region_subtract_rect(&cur, b.rects[i]);
        cur = next;
        if !o {
            ok = false;
        }
        i += 1;
    }
    (cur, ok)
}

/// 1 ウィンドウの可視領域を計算する (クライアントローカル座標)。
/// 容量超過時は「全体を不可視」ではなく可視のまま (安全側) にするため、
/// 打ち切りが起きたら full-client 1 枚で近似する。
fn compute_vis(st: &GuiState, index: usize) -> RectSet {
    let w = &st.windows[index];
    let mut out = RectSet::EMPTY;
    if !w.visible {
        return out;
    }
    let (cox, coy) = w.client_origin();
    let (cw, ch) = w.client_size();
    if cw <= 0 || ch <= 0 {
        return out;
    }
    let mut region = RectSet::EMPTY;
    region.push(Rect::new(cox, coy, cw, ch)); /* 画面座標で計算 */

    /* 自分より前面 (Z が上) の可視ウィンドウの外形を引く。 */
    let myz = match st.z_of(index) {
        Some(z) => z,
        None => return out,
    };
    let mut z = myz + 1;
    let mut capped = false;
    while z < st.z_count {
        let above = st.zorder[z];
        let wa = &st.windows[above];
        if wa.used && wa.visible {
            let (next, ok) = region_subtract_rect(&region, wa.outer());
            region = next;
            if !ok {
                capped = true;
            }
        }
        z += 1;
    }

    /* モーダルダイアログは WM 自身の窓で Z 順の最前面 (契約 U4)。ここで穴を
     * 開けておくと、下のアプリはダイアログの下を描かず、閉じたときに露出分の
     * `Paint` を受け取る (入れ子ループを作らずに再描画が回る)。 */
    let dlg = crate::modal::rect();
    if !dlg.is_empty() {
        let (next, ok) = region_subtract_rect(&region, dlg);
        region = next;
        if !ok {
            capped = true;
        }
    }

    if capped {
        /* 近似: クライアント全面を可視扱い (次周で正確化される)。 */
        out.push(Rect::new(0, 0, cw, ch));
        return out;
    }
    /* 画面座標 → クライアントローカルへ移して格納。 */
    let mut i = 0;
    while i < region.len && out.len < MAX_VIS {
        let r = region.rects[i];
        out.push(r.translate(-cox, -coy));
        i += 1;
    }
    out
}

/// 全ウィンドウの可視領域を再計算し、**露出した分を dirty に足す** (契約 G4)。
/// 呼ぶ側は Z 順・移動・表示・破棄の直後に 1 回呼ぶ。
pub fn recompute_and_expose(st: &mut GuiState) {
    let mut idx = 0;
    while idx < GUI_MAX_WINDOWS {
        if !st.windows[idx].used {
            idx += 1;
            continue;
        }
        let old_vis = st.windows[idx].vis;
        let new_vis = compute_vis(st, idx);
        /* 露出 = new − old。露出分のみ dirty に足す (完全に隠れた窓は空)。 */
        let (exposed, _ok) = region_subtract_region(&new_vis, &old_vis);
        st.windows[idx].vis = new_vis;
        let mut e = 0;
        while e < exposed.len {
            damage::add_dirty(&mut st.windows[idx], exposed.rects[e]);
            e += 1;
        }
        idx += 1;
    }
}
