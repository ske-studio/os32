//! input.rs — 入力の取り込み (契約 T3 / U2a)。
//!
//! `kbd_trygetrawkey` の生 make/break を `Key` (down 0/1) に、印字可能キーは加えて
//! `Text` にして、フォーカス窓の所有者スロットのリングへ積む。`mouse_poll` から
//! `Pointer` (最新 1 件へ畳む) と `Button` を作る。修飾は `kbd_get_modifiers`。
//! `kbd_dropped_count` の差分を `dropped` に足して `OVERFLOW` (契約 T3)。
//!
//! WM 自身の UI (ドラッグ / 閉じる / フォーカス切替) は `wm_ui == true` の文脈
//! (= OP_WAIT の中 / gshell 単独ループ、契約 X3) でだけ進める。ポンプ (X4) は
//! `wm_ui == false` で呼び、入力のリング追記とカーソルだけを行う (状態機械を
//! 進めない)。FEP は W2。

use crate::ring;
use crate::wm::{GuiState, Rect};
use crate::{visible, wm};
use os32api::gui::proto::{GuiRect16, GUI_EV_CONFIGURE, GUI_EV_FOCUS};

/* 修飾ビット (drivers/kbd.h の SHIFT_* と一致)。 */
const MOD_SHIFT: u32 = 0x01;
const MOD_CAPS: u32 = 0x02;
const MOD_CTRL: u32 = 0x10;

/* 修飾キーのスキャンコード (KEY_SHIFT..KEY_CTRL)。Key として配送しない。 */
const SC_SHIFT: u8 = 0x70;
const SC_CTRL: u8 = 0x74;
/* WM が単独時に横取りするキー (KEY_ESC / KEY_F1)。 */
const SC_ESC: u8 = 0x00;
const SC_F1: u8 = 0x62;

const MOUSE_BTN_LEFT: u8 = 0x01;

/// mouse_poll(*mut u8) が書き込む構造体 (os32_kapi_shared.h MouseInfo と同一)。
#[repr(C)]
#[derive(Clone, Copy)]
struct MouseInfo {
    x: i16,
    y: i16,
    dx: i16,
    dy: i16,
    buttons: u8,
    mode: u8,
}

/* drivers/kbd.c の scancode_to_ascii[128] の写し。 */
static SC2A: [u8; 128] = [
    0x1B, b'1', b'2', b'3', b'4', b'5', b'6', b'7', b'8', b'9', b'0', b'-', b'^', b'\\', 0x08, 0x09,
    b'q', b'w', b'e', b'r', b't', b'y', b'u', b'i', b'o', b'p', b'@', b'[', 0x0D, b'a', b's', b'd',
    b'f', b'g', b'h', b'j', b'k', b'l', b';', b':', b']', b'z', b'x', b'c', b'v', b'b', b'n', b'm',
    b',', b'.', b'/', 0, b' ', 0, 0x12, 0x03, 0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,
    b'-', b'/', b'7', b'8', b'9', b'*', b'4', b'5', b'6', b'+', b'1', b'2', b'3', b'=', b'0', b',',
    b'.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];

/* drivers/kbd.c の scancode_to_ascii_shift[128] の写し。 */
static SC2A_SHIFT: [u8; 128] = [
    0x1B, b'!', b'"', b'#', b'$', b'%', b'&', b'\'', b'(', b')', 0, b'=', b'`', b'|', 0x08, 0x09,
    b'Q', b'W', b'E', b'R', b'T', b'Y', b'U', b'I', b'O', b'P', b'~', b'{', 0x0D, b'A', b'S', b'D',
    b'F', b'G', b'H', b'J', b'K', b'L', b'+', b'*', b'}', b'Z', b'X', b'C', b'V', b'B', b'N', b'M',
    b'<', b'>', b'?', b'_', b' ', 0, 0x12, 0x03, 0x16, 0x7F, 0x1E, 0x1D, 0x1C, 0x1F, 0x01, 0x05,
    b'-', b'/', b'7', b'8', b'9', b'*', b'4', b'5', b'6', b'+', b'1', b'2', b'3', b'=', b'0', b',',
    b'.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];

/// スキャンコード + 修飾 → ASCII (drivers/kbd.c と同じ規則。無ければ 0)。
fn translate(scan: u8, mods: u32) -> u8 {
    let i = (scan & 0x7F) as usize;
    let mut a = if (mods & MOD_SHIFT) != 0 { SC2A_SHIFT[i] } else { SC2A[i] };
    if (mods & MOD_CAPS) != 0 {
        if a >= b'a' && a <= b'z' {
            a -= 32;
        } else if a >= b'A' && a <= b'Z' {
            a += 32;
        }
    }
    if (mods & MOD_CTRL) != 0 && a >= b'a' && a <= b'z' {
        a = a - b'a' + 1;
    }
    a
}

/// フォーカス窓 (最前面) の配送先。無ければ None。
struct Target {
    slot: usize,
    win_id: u32,
    cox: i32,
    coy: i32,
    index: usize,
}

fn focus_target(st: &GuiState) -> Option<Target> {
    let index = st.front_index()?;
    let owner = st.windows[index].owner;
    let slot = st.slot_of_owner(owner)?;
    let (cox, coy) = st.windows[index].client_origin();
    Some(Target { slot, win_id: st.windows[index].id(index), cox, coy, index })
}

/* ================================================================ */
/*  入力取り込み本体                                                 */
/* ================================================================ */

/// 入力を取り込みリングへ流す。`wm_ui` = WM の UI 状態機械も進めるか
/// (X3 で true、X4 ポンプで false)。
pub fn capture(st: &mut GuiState, wm_ui: bool) {
    capture_keyboard(st, wm_ui);
    capture_mouse(st, wm_ui);
}

fn capture_keyboard(st: &mut GuiState, wm_ui: bool) {
    let mods = unsafe { (os32api::api().kbd_get_modifiers)() };

    /* 取りこぼしの差分を dropped に加算 (契約 T3)。 */
    let cur_drop = unsafe { (os32api::api().kbd_dropped_count)() };
    let delta = cur_drop.wrapping_sub(st.last_kbd_dropped);
    st.last_kbd_dropped = cur_drop;
    if delta > 0 {
        if let Some(t) = focus_target(st) {
            let n = if delta > 0xFFFF { 0xFFFF } else { delta as u16 };
            ring::add_dropped(st, t.slot, n);
        }
    }

    loop {
        /* 満杯に近ければ取り込まない (カーネル待ち行列に残す。契約 T3)。 */
        let space_ok = match focus_target(st) {
            Some(t) => ring::space(st, t.slot) >= 2,
            None => true, /* 宛先無し: 取り込んでも捨てるだけなので読む (キューを空ける) */
        };
        if !space_ok {
            break;
        }
        let raw = unsafe { (os32api::api().kbd_trygetrawkey)() };
        if raw < 0 {
            break;
        }
        let scan = (raw & 0x7F) as u8;
        let down = ((raw >> 8) & 1) != 0;

        /* 修飾キー自体は Key として配送しない (状態は mods で見る)。 */
        if scan >= SC_SHIFT && scan <= SC_CTRL {
            continue;
        }

        /* WM 単独時 (フォーカス窓なし) の横取り: ESC=終了 / F1=起動。 */
        if wm_ui && st.front_index().is_none() {
            if down && scan == SC_ESC {
                st.quit = true;
            } else if down && scan == SC_F1 {
                st.launch_pending = true;
            }
            continue;
        }

        let t = match focus_target(st) {
            Some(t) => t,
            None => continue,
        };
        let ch = translate(scan, mods);
        let serial = next_serial(st, t.slot);
        let ev = ring::ev_key(down, t.win_id, scan, ch, mods as u8, serial);
        ring::append(st, t.slot, &ev);

        /* 印字可能キーは Text も配送 (FEP オフ時。FEP は W2)。 */
        if down && ch >= 0x20 && ch <= 0x7E {
            let mut utf8 = [0u8; 8];
            utf8[0] = ch;
            let s2 = next_serial(st, t.slot);
            let evt = ring::ev_text(t.win_id, utf8, 1, s2);
            ring::append(st, t.slot, &evt);
        }
    }
}

fn capture_mouse(st: &mut GuiState, wm_ui: bool) {
    let mut mi = MouseInfo { x: 0, y: 0, dx: 0, dy: 0, buttons: 0, mode: 0 };
    unsafe {
        (os32api::api().mouse_poll)(&mut mi as *mut MouseInfo as *mut u8);
    }
    let mx = mi.x as i32;
    let my = mi.y as i32;
    st.mouse_x = mx;
    st.mouse_y = my;
    let btn = mi.buttons;
    let down_edge = (btn & MOUSE_BTN_LEFT) != 0 && (st.prev_buttons & MOUSE_BTN_LEFT) == 0;
    let up_edge = (btn & MOUSE_BTN_LEFT) == 0 && (st.prev_buttons & MOUSE_BTN_LEFT) != 0;

    if wm_ui {
        /* ---- ドラッグ追従 (XOR 枠だけ動かす。実体は drop で移す) ---- */
        if st.drag_index >= 0 {
            update_drag(st, mx, my);
        }
        if down_edge {
            wm_button_down(st, mx, my);
        } else if up_edge {
            wm_button_up(st, mx, my);
        } else {
            /* 移動: フォーカス窓へ Pointer (畳み込み) */
            forward_pointer(st, mx, my, btn);
        }
    } else {
        /* ポンプ: 状態機械を進めず、入力だけ流す。 */
        if down_edge {
            forward_button(st, mx, my, btn, true);
        } else if up_edge {
            forward_button(st, mx, my, btn, false);
        } else {
            forward_pointer(st, mx, my, btn);
        }
    }
    st.prev_buttons = btn;
}

/* ---- WM 状態機械 (X3 のみ) ---- */

fn wm_button_down(st: &mut GuiState, mx: i32, my: i32) {
    let hit = st.hit_window(mx, my);
    let idx = match hit {
        Some(i) => i,
        None => return,
    };
    /* 前面化 + フォーカス切替 (Focus イベント)。 */
    let old_front = st.front_id();
    let changed = st.front_index() != Some(idx);
    if changed {
        st.bring_to_front(idx);
        visible::recompute_and_expose(st);
        let vac = st.windows[idx].outer();
        st.screen_dirty.push(vac);
        let new_front = st.windows[idx].id(idx);
        emit_focus_change(st, old_front, new_front);
    }

    /* 閉じるボタン? */
    let w = st.windows[idx];
    if w.has_close() && w.close_rect().contains(mx, my) {
        if let Some(slot) = st.slot_of_owner(w.owner) {
            let ev = ring::ev_simple(os32api::gui::proto::GUI_EV_CLOSE, 0, w.id(idx));
            ring::append(st, slot, &ev);
        }
        return;
    }
    /* タイトルバー? → ドラッグ開始 (実体は動かさない)。 */
    if w.movable() && w.titlebar_rect().contains(mx, my) {
        st.drag_index = idx as i32;
        st.drag_dx = mx - w.x;
        st.drag_dy = my - w.y;
        st.drag_frame = w.outer();
        crate::chrome::draw_drag_outline(w.x, w.y, w.w, w.h);
        present_frame_edges(st, w.outer());
        return;
    }
    /* それ以外 (クライアント / 枠) → Button をアプリへ。 */
    forward_button(st, mx, my, MOUSE_BTN_LEFT, true);
}

fn wm_button_up(st: &mut GuiState, mx: i32, my: i32) {
    if st.drag_index >= 0 {
        let idx = st.drag_index as usize;
        let old_outer = st.windows[idx].outer();
        /* 枠の最終位置へ実体を移す。 */
        let nf = st.drag_frame;
        st.windows[idx].x = nf.x;
        st.windows[idx].y = nf.y;
        st.drag_index = -1;
        st.drag_frame = Rect::EMPTY;

        /* 露出計算 + 旧位置と新位置を画面損傷に。 */
        st.screen_dirty.push(old_outer);
        st.screen_dirty.push(st.windows[idx].outer());
        visible::recompute_and_expose(st);
        /* 移動した窓のクライアントは全面再描画が要る (中身は同じでも位置が変わる)。 */
        crate::damage::set_dirty_full(&mut st.windows[idx]);
        /* Configure を通知 (座標確定、R2)。 */
        st.windows[idx].configure_pending = true;
        emit_configure(st, idx);
        return;
    }
    forward_button(st, mx, my, 0, false);
}

fn update_drag(st: &mut GuiState, mx: i32, my: i32) {
    let idx = st.drag_index as usize;
    let w = st.windows[idx];
    let mut nx = mx - st.drag_dx;
    let mut ny = my - st.drag_dy;
    /* 画面内にクランプ (タイトルバーが掴める範囲を残す)。 */
    if nx < -(w.w - 40) {
        nx = -(w.w - 40);
    }
    if nx > st.screen_w - 40 {
        nx = st.screen_w - 40;
    }
    if ny < 0 {
        ny = 0;
    }
    if ny > st.screen_h - crate::wm::TITLEBAR_H {
        ny = st.screen_h - crate::wm::TITLEBAR_H;
    }
    let old_frame = st.drag_frame;
    let new_frame = Rect::new(nx, ny, w.w, w.h);
    if new_frame == old_frame {
        return;
    }
    st.drag_frame = new_frame;
    /* 旧枠を下地で消し、新枠を描いて、両者の縁だけ present。 */
    erase_frame_edges(st, old_frame);
    crate::chrome::draw_drag_outline(new_frame.x, new_frame.y, new_frame.w, new_frame.h);
    present_frame_edges(st, old_frame);
    present_frame_edges(st, new_frame);
}

/* ---- アプリへの配送 ---- */

fn forward_pointer(st: &mut GuiState, mx: i32, my: i32, btn: u8) {
    let t = match focus_target(st) {
        Some(t) => t,
        None => return,
    };
    let cx = (mx - t.cox) as i16;
    let cy = (my - t.coy) as i16;
    let serial = next_serial(st, t.slot);
    let ev = ring::ev_pointer(t.win_id, cx, cy, btn, serial);
    ring::append(st, t.slot, &ev);
}

fn forward_button(st: &mut GuiState, mx: i32, my: i32, btn: u8, down: bool) {
    let t = match focus_target(st) {
        Some(t) => t,
        None => return,
    };
    let cx = (mx - t.cox) as i16;
    let cy = (my - t.coy) as i16;
    let serial = next_serial(st, t.slot);
    let ev = ring::ev_button(down, t.win_id, cx, cy, MOUSE_BTN_LEFT, serial);
    ring::append(st, t.slot, &ev);
    let _ = btn;
}

/* ---- Focus / Configure ---- */

fn emit_focus_change(st: &mut GuiState, old_id: u32, new_id: u32) {
    if old_id != 0 {
        if let Some(oi) = st.win_by_id(old_id) {
            if let Some(slot) = st.slot_of_owner(st.windows[oi].owner) {
                let ev = ring::ev_simple(GUI_EV_FOCUS, 0, old_id);
                ring::append(st, slot, &ev);
            }
        }
    }
    if new_id != 0 {
        if let Some(ni) = st.win_by_id(new_id) {
            if let Some(slot) = st.slot_of_owner(st.windows[ni].owner) {
                let ev = ring::ev_simple(GUI_EV_FOCUS, 1, new_id);
                ring::append(st, slot, &ev);
            }
        }
    }
}

fn emit_configure(st: &mut GuiState, index: usize) {
    let w = &st.windows[index];
    if !w.configure_pending {
        return;
    }
    let owner = w.owner;
    let id = w.id(index);
    let (cw, ch) = w.client_size();
    let (cox, coy) = w.client_origin();
    let rect = GuiRect16 { x: cox as i16, y: coy as i16, w: cw as i16, h: ch as i16 };
    if let Some(slot) = st.slot_of_owner(owner) {
        let ev = ring::ev_rect(GUI_EV_CONFIGURE, id, rect);
        if ring::append(st, slot, &ev) {
            st.windows[index].configure_pending = false;
        }
    }
}

#[inline]
fn next_serial(st: &mut GuiState, slot: usize) -> u16 {
    st.slots[slot].serial = st.slots[slot].serial.wrapping_add(1);
    st.slots[slot].serial
}

/* ---- ドラッグ枠の縁の present / 消去 (wm compositor へ委譲) ---- */

fn frame_edges(f: Rect) -> [Rect; 4] {
    let t = 2; /* 枠の太さ */
    [
        Rect::new(f.x, f.y, f.w, t),                       /* 上 */
        Rect::new(f.x, f.y + f.h - t, f.w, t),             /* 下 */
        Rect::new(f.x, f.y, t, f.h),                       /* 左 */
        Rect::new(f.x + f.w - t, f.y, t, f.h),             /* 右 */
    ]
}

fn present_frame_edges(st: &GuiState, f: Rect) {
    for e in frame_edges(f).iter() {
        wm::present_rect(st, *e);
    }
}

fn erase_frame_edges(st: &mut GuiState, f: Rect) {
    for e in frame_edges(f).iter() {
        wm::composite_rect(st, *e);
    }
}
