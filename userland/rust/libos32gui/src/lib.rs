//! libos32gui — Win32 風 GUI ライブラリ (Rust, no_std)
//!
//! libos32gfx に「無い層」だけを実装する:
//!   ウィンドウ管理 / Z オーダー / フォーカス / メッセージディスパッチ / ウィジェット。
//! 描画プリミティブは in-repo の libos32gfx を extern "C" (os32api::gfx) 経由で呼ぶ。
//! Rust で描画を再実装しない (CONTRACTS C8)。
//!
//! - `#![no_std]`、alloc 不使用。状態はすべて固定長配列 (`GuiState`)。
//! - 状態は単一のグローバル `GUI` に持ち、単一スレッド前提で unsafe に触る。
//! - 公開 API は `#[no_mangle] extern "C"` (将来の C 製 gshell から直接叩ける)。
//!   Rust から使う場合も同じシンボルをそのまま呼べる。
//! - CPL=3 で動く (v2)。特権命令は使わず、全ハード操作は KAPI 経由。
#![no_std]

extern crate os32api;

pub mod types;

use core::cell::UnsafeCell;
use os32api::gfx;
use os32api::KernelAPI;
use types::*;

/* 公開型・定数の再エクスポート (Rust の利用側が libos32gui:: 直下で使える) */
pub use types::{
    GuiEvent, GUI_EV_BUTTON_CLICK, GUI_EV_CHECKBOX_TOGGLED, GUI_EV_FOCUS_CHANGED, GUI_EV_KEY,
    GUI_EV_LIST_SELECT, GUI_EV_MOUSE_DOWN, GUI_EV_MOUSE_MOVE, GUI_EV_MOUSE_UP, GUI_EV_NONE,
    GUI_EV_TEXT_CHANGED, GUI_EV_WIN_ACTIVATE, GUI_EV_WIN_CLOSE, GUI_EV_WIN_MOVE, GUI_WF_BORDER,
    GUI_WF_DEFAULT, GUI_WF_HAS_CLOSE, GUI_WF_MOVABLE, GUI_WF_VISIBLE,
};

/* 画面サイズ (libos32gfx 400 ライン GFX モード) */
const SCREEN_W: i32 = 640;
const SCREEN_H: i32 = 400;
const CHAR_W: i32 = 8; /* ANK 半角セル幅 (scale1) */

/* ================================================================ */
/*  グローバル状態                                                   */
/* ================================================================ */

struct GuiState {
    inited: bool,
    windows: [Window; MAX_WINDOWS],
    widgets: [Widget; MAX_WIDGETS],
    list_items: [ListItem; MAX_LIST_ITEMS],
    /* Z オーダー: window id を背面→前面で並べる。zorder[z_count-1] が最前面=フォーカス */
    zorder: [u32; MAX_WINDOWS],
    z_count: usize,
    next_win_id: u32,
    next_widget_id: u32,

    /* キーボードフォーカス中のウィジェット (0=なし)。最前面ウィンドウ内を指す */
    focus_widget: u32,

    /* イベントリングバッファ */
    events: [GuiEvent; EVENT_QUEUE_LEN],
    ev_head: usize,
    ev_count: usize,

    /* 入力トラッキング */
    prev_buttons: u8,
    mouse_x: i32,
    mouse_y: i32,

    /* タイトルバードラッグ中のウィンドウ (0=なし) と掴んだオフセット */
    drag_win: u32,
    drag_dx: i32,
    drag_dy: i32,

    /* 押下でアーム、離してまだ上にいれば click になるウィジェット (0=なし) */
    armed_widget: u32,
}

impl GuiState {
    const NEW: GuiState = GuiState {
        inited: false,
        windows: [Window::EMPTY; MAX_WINDOWS],
        widgets: [Widget::EMPTY; MAX_WIDGETS],
        list_items: [ListItem::EMPTY; MAX_LIST_ITEMS],
        zorder: [0; MAX_WINDOWS],
        z_count: 0,
        next_win_id: 1,
        next_widget_id: 1,
        focus_widget: 0,
        events: [GuiEvent::NONE; EVENT_QUEUE_LEN],
        ev_head: 0,
        ev_count: 0,
        prev_buttons: 0,
        mouse_x: SCREEN_W / 2,
        mouse_y: SCREEN_H / 2,
        drag_win: 0,
        drag_dx: 0,
        drag_dy: 0,
        armed_widget: 0,
    };
}

struct GuiCell(UnsafeCell<GuiState>);
unsafe impl Sync for GuiCell {}
static GUI: GuiCell = GuiCell(UnsafeCell::new(GuiState::NEW));

#[inline]
fn g() -> &'static mut GuiState {
    unsafe { &mut *GUI.0.get() }
}

/* ================================================================ */
/*  GuiState — 内部ヘルパ                                            */
/* ================================================================ */

impl GuiState {
    fn win_index(&self, id: u32) -> Option<usize> {
        if id == 0 {
            return None;
        }
        let mut i = 0;
        while i < MAX_WINDOWS {
            if self.windows[i].used && self.windows[i].id == id {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    fn widget_index(&self, id: u32) -> Option<usize> {
        if id == 0 {
            return None;
        }
        let mut i = 0;
        while i < MAX_WIDGETS {
            if self.widgets[i].used && self.widgets[i].id == id {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    fn front_win(&self) -> u32 {
        if self.z_count == 0 {
            0
        } else {
            self.zorder[self.z_count - 1]
        }
    }

    fn z_add_top(&mut self, id: u32) {
        if self.z_count < MAX_WINDOWS {
            self.zorder[self.z_count] = id;
            self.z_count += 1;
        }
    }

    fn z_remove(&mut self, id: u32) {
        let mut i = 0;
        while i < self.z_count {
            if self.zorder[i] == id {
                let mut j = i;
                while j + 1 < self.z_count {
                    self.zorder[j] = self.zorder[j + 1];
                    j += 1;
                }
                self.z_count -= 1;
                return;
            }
            i += 1;
        }
    }

    fn bring_to_front(&mut self, id: u32) {
        if self.front_win() != id {
            self.z_remove(id);
            self.z_add_top(id);
        }
        /* フォーカスウィジェットが前面ウィンドウ外なら先頭 focusable に移す */
        self.sync_focus_to_front();
    }

    /* focus_widget が最前面ウィンドウに属していなければ、そのウィンドウの
     * 先頭 focusable ウィジェットへ張り替える (無ければ 0)。 */
    fn sync_focus_to_front(&mut self) {
        let front = self.front_win();
        let ok = match self.widget_index(self.focus_widget) {
            Some(i) => self.widgets[i].win_id == front,
            None => false,
        };
        if !ok {
            self.focus_widget = self.first_focusable(front);
        }
    }

    fn first_focusable(&self, win_id: u32) -> u32 {
        let mut i = 0;
        while i < MAX_WIDGETS {
            let w = &self.widgets[i];
            if w.used && w.win_id == win_id && w.focusable() {
                return w.id;
            }
            i += 1;
        }
        0
    }

    /* 点 (px,py) を含む最前面の可視ウィンドウ id。無ければ 0。 */
    fn hit_window(&self, px: i32, py: i32) -> u32 {
        let mut z = self.z_count;
        while z > 0 {
            z -= 1;
            let id = self.zorder[z];
            if let Some(i) = self.win_index(id) {
                let w = &self.windows[i];
                if (w.flags & GUI_WF_VISIBLE) != 0 && point_in(px, py, w.x, w.y, w.w, w.h) {
                    return id;
                }
            }
        }
        0
    }

    /* ウィンドウ内クライアント点 (相対 cx,cy) にある任意ウィジェット id。無ければ 0。 */
    fn hit_widget(&self, win_id: u32, cx: i32, cy: i32) -> u32 {
        let mut i = 0;
        while i < MAX_WIDGETS {
            let wg = &self.widgets[i];
            if wg.used && wg.win_id == win_id && wg.kind != GUI_WT_LABEL {
                if point_in(cx, cy, wg.x, wg.y, wg.w, wg.h) {
                    return wg.id;
                }
            }
            i += 1;
        }
        0
    }

    fn push_event(&mut self, ev: GuiEvent) {
        if self.ev_count >= EVENT_QUEUE_LEN {
            self.ev_head = (self.ev_head + 1) % EVENT_QUEUE_LEN;
            self.ev_count -= 1;
        }
        let tail = (self.ev_head + self.ev_count) % EVENT_QUEUE_LEN;
        self.events[tail] = ev;
        self.ev_count += 1;
    }

    fn pop_event(&mut self) -> Option<GuiEvent> {
        if self.ev_count == 0 {
            return None;
        }
        let ev = self.events[self.ev_head];
        self.ev_head = (self.ev_head + 1) % EVENT_QUEUE_LEN;
        self.ev_count -= 1;
        Some(ev)
    }

    /* ---- listbox 項目プール ---- */
    fn list_item_slot(&self, widget_id: u32, index: i32) -> Option<usize> {
        let mut i = 0;
        while i < MAX_LIST_ITEMS {
            let it = &self.list_items[i];
            if it.used && it.widget_id == widget_id && it.index == index {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    fn list_visible_rows(&self, wi: usize) -> i32 {
        let h = self.widgets[wi].h;
        let r = h / LIST_ROW_H;
        if r < 1 {
            1
        } else {
            r
        }
    }

    fn list_ensure_visible(&mut self, wi: usize) {
        let vis = self.list_visible_rows(wi);
        let sel = self.widgets[wi].sel;
        if sel < 0 {
            return;
        }
        let mut top = self.widgets[wi].top;
        if sel < top {
            top = sel;
        } else if sel >= top + vis {
            top = sel - vis + 1;
        }
        if top < 0 {
            top = 0;
        }
        self.widgets[wi].top = top;
    }
}

/* ================================================================ */
/*  textbox 編集ヘルパ (ASCII 単一行。日本語入力は FEP 経路で別途)   */
/* ================================================================ */

fn tb_insert(wg: &mut Widget, ch: u8) -> bool {
    let len = cstr_len(&wg.text);
    if len >= MAX_TITLE - 1 {
        return false;
    }
    let mut c = wg.caret;
    if c < 0 {
        c = 0;
    }
    if c > len as i32 {
        c = len as i32;
    }
    let c = c as usize;
    let mut i = len;
    while i > c {
        wg.text[i] = wg.text[i - 1];
        i -= 1;
    }
    wg.text[c] = ch;
    wg.text[len + 1] = 0;
    wg.caret = (c + 1) as i32;
    true
}

fn tb_remove_at(wg: &mut Widget, p: usize) {
    let len = cstr_len(&wg.text);
    if p >= len {
        return;
    }
    let mut i = p;
    while i + 1 < len {
        wg.text[i] = wg.text[i + 1];
        i += 1;
    }
    wg.text[len - 1] = 0;
}

fn tb_backspace(wg: &mut Widget) -> bool {
    if wg.caret <= 0 {
        return false;
    }
    let p = (wg.caret - 1) as usize;
    tb_remove_at(wg, p);
    wg.caret -= 1;
    true
}

fn tb_delete(wg: &mut Widget) -> bool {
    let len = cstr_len(&wg.text) as i32;
    if wg.caret >= len {
        return false;
    }
    tb_remove_at(wg, wg.caret as usize);
    true
}

/* ================================================================ */
/*  入力ポンプ (Win32 の GetMessage 相当の生成側)                    */
/* ================================================================ */

fn pump_mouse(st: &mut GuiState) {
    let mut mi = MouseInfo::ZERO;
    unsafe {
        let a = os32api::api();
        (a.mouse_poll)(&mut mi as *mut MouseInfo as *mut u8);
    }
    let mx = mi.x as i32;
    let my = mi.y as i32;
    st.mouse_x = mx;
    st.mouse_y = my;

    let btn = mi.buttons;
    let down_edge = (btn & MOUSE_BTN_LEFT) != 0 && (st.prev_buttons & MOUSE_BTN_LEFT) == 0;
    let up_edge = (btn & MOUSE_BTN_LEFT) == 0 && (st.prev_buttons & MOUSE_BTN_LEFT) != 0;

    /* ---- ドラッグ中はウィンドウ追従 ---- */
    if st.drag_win != 0 {
        if let Some(i) = st.win_index(st.drag_win) {
            let mut nx = mx - st.drag_dx;
            let mut ny = my - st.drag_dy;
            let w = st.windows[i].w;
            if nx < -(w - 40) {
                nx = -(w - 40);
            }
            if nx > SCREEN_W - 40 {
                nx = SCREEN_W - 40;
            }
            if ny < 0 {
                ny = 0;
            }
            if ny > SCREEN_H - TITLEBAR_H {
                ny = SCREEN_H - TITLEBAR_H;
            }
            st.windows[i].x = nx;
            st.windows[i].y = ny;
        }
    }

    /* ---- 押下エッジ ---- */
    if down_edge {
        let win = st.hit_window(mx, my);
        if win != 0 {
            let changed = st.front_win() != win;
            st.bring_to_front(win);
            if changed {
                st.push_event(GuiEvent {
                    kind: GUI_EV_WIN_ACTIVATE,
                    win_id: win,
                    ..GuiEvent::NONE
                });
            }
            let i = st.win_index(win).unwrap();
            let w = st.windows[i];
            st.push_event(GuiEvent {
                kind: GUI_EV_MOUSE_DOWN,
                win_id: win,
                x: mx,
                y: my,
                button: (btn & MOUSE_BTN_LEFT) as u32,
                ..GuiEvent::NONE
            });

            let (crx, cry, crw, crh) = w.close_rect();
            if (w.flags & GUI_WF_HAS_CLOSE) != 0 && point_in(mx, my, crx, cry, crw, crh) {
                st.push_event(GuiEvent {
                    kind: GUI_EV_WIN_CLOSE,
                    win_id: win,
                    ..GuiEvent::NONE
                });
            } else {
                let (tx, ty, tw, th) = w.titlebar_rect();
                if (w.flags & GUI_WF_MOVABLE) != 0 && point_in(mx, my, tx, ty, tw, th) {
                    st.drag_win = win;
                    st.drag_dx = mx - w.x;
                    st.drag_dy = my - w.y;
                } else {
                    let (cox, coy) = w.client_origin();
                    let cx = mx - cox;
                    let cy = my - coy;
                    let wid = st.hit_widget(win, cx, cy);
                    if wid != 0 {
                        mouse_down_widget(st, wid, cx, cy);
                    }
                }
            }
        }
    }

    /* ---- 解放エッジ ---- */
    if up_edge {
        if st.drag_win != 0 {
            let win = st.drag_win;
            st.drag_win = 0;
            if let Some(i) = st.win_index(win) {
                let (x, y) = (st.windows[i].x, st.windows[i].y);
                st.push_event(GuiEvent {
                    kind: GUI_EV_WIN_MOVE,
                    win_id: win,
                    x,
                    y,
                    ..GuiEvent::NONE
                });
            }
        }
        if st.armed_widget != 0 {
            let wid = st.armed_widget;
            st.armed_widget = 0;
            mouse_up_armed(st, wid, mx, my);
        }
        st.push_event(GuiEvent {
            kind: GUI_EV_MOUSE_UP,
            win_id: st.hit_window(mx, my),
            x: mx,
            y: my,
            ..GuiEvent::NONE
        });
    }

    st.prev_buttons = btn;
}

/* クライアント領域内のウィジェットを押下したときの処理 */
fn mouse_down_widget(st: &mut GuiState, wid: u32, cx: i32, cy: i32) {
    let wi = match st.widget_index(wid) {
        Some(i) => i,
        None => return,
    };
    st.focus_widget = wid; /* クリックでキーボードフォーカスも移る */
    match st.widgets[wi].kind {
        GUI_WT_BUTTON | GUI_WT_CHECKBOX => {
            st.armed_widget = wid;
            st.widgets[wi].pressed = true;
        }
        GUI_WT_LISTBOX => {
            let rel = cy - st.widgets[wi].y;
            let idx = st.widgets[wi].top + rel / LIST_ROW_H;
            if idx >= 0 && idx < st.widgets[wi].item_count {
                st.widgets[wi].sel = idx;
                st.list_ensure_visible(wi);
                let win = st.widgets[wi].win_id;
                st.push_event(GuiEvent {
                    kind: GUI_EV_LIST_SELECT,
                    win_id: win,
                    widget_id: wid,
                    x: idx,
                    ..GuiEvent::NONE
                });
            }
        }
        GUI_WT_TEXTBOX => {
            /* クリック位置におおよそキャレットを置く */
            let rel = cx - 4;
            let mut caret = if rel < 0 { 0 } else { rel / CHAR_W };
            let len = cstr_len(&st.widgets[wi].text) as i32;
            if caret > len {
                caret = len;
            }
            st.widgets[wi].caret = caret;
        }
        _ => {}
    }
}

/* アーム済みウィジェットを離したときの処理 (ボタン/チェックボックス) */
fn mouse_up_armed(st: &mut GuiState, wid: u32, mx: i32, my: i32) {
    let wi = match st.widget_index(wid) {
        Some(i) => i,
        None => return,
    };
    st.widgets[wi].pressed = false;
    let win = st.widgets[wi].win_id;
    let kind = st.widgets[wi].kind;
    let (wx, wy, ww, wh) = (
        st.widgets[wi].x,
        st.widgets[wi].y,
        st.widgets[wi].w,
        st.widgets[wi].h,
    );
    let pi = match st.win_index(win) {
        Some(i) => i,
        None => return,
    };
    let (cox, coy) = st.windows[pi].client_origin();
    if !point_in(mx, my, cox + wx, coy + wy, ww, wh) {
        return; /* ボタン外で離した → キャンセル */
    }
    match kind {
        GUI_WT_BUTTON => {
            st.push_event(GuiEvent {
                kind: GUI_EV_BUTTON_CLICK,
                win_id: win,
                widget_id: wid,
                ..GuiEvent::NONE
            });
        }
        GUI_WT_CHECKBOX => {
            let nc = !st.widgets[wi].checked;
            st.widgets[wi].checked = nc;
            st.push_event(GuiEvent {
                kind: GUI_EV_CHECKBOX_TOGGLED,
                win_id: win,
                widget_id: wid,
                button: nc as u32,
                ..GuiEvent::NONE
            });
        }
        _ => {}
    }
}

fn pump_keyboard(st: &mut GuiState) {
    let shift;
    unsafe {
        let a = os32api::api();
        shift = ((a.kbd_get_modifiers)() & SHIFT_MASK) != 0;
    }
    loop {
        let ch = unsafe {
            let a = os32api::api();
            (a.kbd_trygetchar)()
        };
        if ch <= 0 {
            break;
        }
        let focus = st.front_win();

        if ch == KEY_TAB {
            advance_focus(st, shift);
        } else {
            route_key_to_focus(st, ch);
        }

        /* 生キーは従来どおり最前面ウィンドウへ配送 (既存経路を壊さない) */
        st.push_event(GuiEvent {
            kind: GUI_EV_KEY,
            win_id: focus,
            key: ch,
            ..GuiEvent::NONE
        });
    }
}

/* Tab / Shift+Tab で最前面ウィンドウの focusable を巡回 */
fn advance_focus(st: &mut GuiState, backward: bool) {
    let front = st.front_win();
    if front == 0 {
        return;
    }
    /* focusable の slot を作成順 (slot 順) に集める */
    let mut order = [0u32; MAX_WIDGETS];
    let mut n = 0usize;
    let mut i = 0;
    while i < MAX_WIDGETS {
        let w = &st.widgets[i];
        if w.used && w.win_id == front && w.focusable() {
            order[n] = w.id;
            n += 1;
        }
        i += 1;
    }
    if n == 0 {
        st.focus_widget = 0;
        return;
    }
    /* 現在位置 */
    let mut cur = usize::MAX;
    let mut k = 0;
    while k < n {
        if order[k] == st.focus_widget {
            cur = k;
            break;
        }
        k += 1;
    }
    let next = if cur == usize::MAX {
        0
    } else if backward {
        (cur + n - 1) % n
    } else {
        (cur + 1) % n
    };
    st.focus_widget = order[next];
    st.push_event(GuiEvent {
        kind: GUI_EV_FOCUS_CHANGED,
        win_id: front,
        widget_id: st.focus_widget,
        ..GuiEvent::NONE
    });
}

/* フォーカスウィジェットへキーを配送 */
fn route_key_to_focus(st: &mut GuiState, ch: i32) {
    let wid = st.focus_widget;
    let wi = match st.widget_index(wid) {
        Some(i) => i,
        None => return,
    };
    let win = st.widgets[wi].win_id;
    match st.widgets[wi].kind {
        GUI_WT_TEXTBOX => {
            let mut changed = false;
            if ch == KEY_BS {
                changed = tb_backspace(&mut st.widgets[wi]);
            } else if ch == KEY_DEL {
                changed = tb_delete(&mut st.widgets[wi]);
            } else if ch == KEY_LEFT {
                if st.widgets[wi].caret > 0 {
                    st.widgets[wi].caret -= 1;
                }
            } else if ch == KEY_RIGHT {
                let len = cstr_len(&st.widgets[wi].text) as i32;
                if st.widgets[wi].caret < len {
                    st.widgets[wi].caret += 1;
                }
            } else if ch == KEY_HOME {
                st.widgets[wi].caret = 0;
            } else if (0x20..=0x7E).contains(&ch) {
                changed = tb_insert(&mut st.widgets[wi], ch as u8);
            }
            if changed {
                st.push_event(GuiEvent {
                    kind: GUI_EV_TEXT_CHANGED,
                    win_id: win,
                    widget_id: wid,
                    ..GuiEvent::NONE
                });
            }
        }
        GUI_WT_LISTBOX => {
            let cnt = st.widgets[wi].item_count;
            if cnt > 0 {
                let mut sel = st.widgets[wi].sel;
                if ch == KEY_UP {
                    sel = if sel <= 0 { 0 } else { sel - 1 };
                } else if ch == KEY_DOWN {
                    sel = if sel < 0 { 0 } else { sel + 1 };
                    if sel >= cnt {
                        sel = cnt - 1;
                    }
                } else {
                    return;
                }
                if sel != st.widgets[wi].sel {
                    st.widgets[wi].sel = sel;
                    st.list_ensure_visible(wi);
                    st.push_event(GuiEvent {
                        kind: GUI_EV_LIST_SELECT,
                        win_id: win,
                        widget_id: wid,
                        x: sel,
                        ..GuiEvent::NONE
                    });
                }
            }
        }
        GUI_WT_CHECKBOX => {
            if ch == KEY_ENTER || ch == KEY_ENTER2 || ch == 0x20 {
                let nc = !st.widgets[wi].checked;
                st.widgets[wi].checked = nc;
                st.push_event(GuiEvent {
                    kind: GUI_EV_CHECKBOX_TOGGLED,
                    win_id: win,
                    widget_id: wid,
                    button: nc as u32,
                    ..GuiEvent::NONE
                });
            }
        }
        GUI_WT_BUTTON => {
            if ch == KEY_ENTER || ch == KEY_ENTER2 || ch == 0x20 {
                st.push_event(GuiEvent {
                    kind: GUI_EV_BUTTON_CLICK,
                    win_id: win,
                    widget_id: wid,
                    ..GuiEvent::NONE
                });
            }
        }
        _ => {}
    }
}

/* ================================================================ */
/*  描画 (フルシーン再描画。背後の save/restore は将来の最適化)      */
/* ================================================================ */

fn draw_scene(st: &GuiState) {
    unsafe {
        gfx::gfx_clear(COL_DESKTOP);
    }
    let mut z = 0;
    while z < st.z_count {
        let id = st.zorder[z];
        if let Some(i) = st.win_index(id) {
            let w = st.windows[i];
            if (w.flags & GUI_WF_VISIBLE) != 0 {
                let active = st.front_win() == id;
                draw_window(&w, active);
                draw_widgets(st, id, active);
            }
        }
        z += 1;
    }
    draw_cursor(st.mouse_x, st.mouse_y);
    unsafe {
        let a = os32api::api();
        (a.gfx_add_dirty_rect)(0, 0, SCREEN_W, SCREEN_H);
        (a.gfx_present_dirty)();
    }
}

fn draw_window(w: &Window, active: bool) {
    unsafe {
        if (w.flags & GUI_WF_BORDER) != 0 {
            gfx::gfx_fill_rect(w.x, w.y, w.w, w.h, COL_BORDER);
        }
        let (cox, coy) = w.client_origin();
        let (cw, ch) = w.client_size();
        gfx::gfx_fill_rect(cox, coy, cw, ch, COL_CLIENT);
        let (tx, ty, tw, th) = w.titlebar_rect();
        let tcol = if active { COL_TITLE_ACT } else { COL_TITLE_INACT };
        gfx::gfx_fill_rect(tx, ty, tw, th, tcol);
        gfx::kcg_set_scale(1);
        gfx::kcg_draw_utf8(tx + 4, ty + 1, w.title.as_ptr(), COL_TITLE_TEXT, tcol);
        if (w.flags & GUI_WF_HAS_CLOSE) != 0 {
            let (crx, cry, crw, crh) = w.close_rect();
            gfx::gfx_fill_rect(crx, cry, crw, crh, COL_CLOSE_FACE);
            gfx::gfx_rect(crx, cry, crw, crh, COL_BORDER);
            gfx::gfx_line(crx + 3, cry + 3, crx + crw - 4, cry + crh - 4, COL_CLOSE_X);
            gfx::gfx_line(crx + crw - 4, cry + 3, crx + 3, cry + crh - 4, COL_CLOSE_X);
        }
    }
}

fn draw_widgets(st: &GuiState, win_id: u32, active: bool) {
    let i = match st.win_index(win_id) {
        Some(i) => i,
        None => return,
    };
    let (cox, coy) = st.windows[i].client_origin();
    let focus = st.focus_widget;
    let mut k = 0;
    while k < MAX_WIDGETS {
        let wg = &st.widgets[k];
        if wg.used && wg.win_id == win_id {
            let sx = cox + wg.x;
            let sy = coy + wg.y;
            let focused = active && wg.id == focus;
            match wg.kind {
                GUI_WT_BUTTON => draw_button(wg, sx, sy, focused),
                GUI_WT_LABEL => unsafe {
                    gfx::kcg_set_scale(1);
                    gfx::kcg_draw_utf8(sx, sy, wg.text.as_ptr(), COL_LABEL_TEXT, COL_CLIENT);
                },
                GUI_WT_CHECKBOX => draw_checkbox(wg, sx, sy, focused),
                GUI_WT_TEXTBOX => draw_textbox(wg, sx, sy, focused),
                GUI_WT_LISTBOX => draw_listbox(st, wg, sx, sy, focused),
                _ => {}
            }
        }
        k += 1;
    }
}

fn focus_ring(sx: i32, sy: i32, w: i32, h: i32) {
    unsafe {
        gfx::gfx_rect(sx - 1, sy - 1, w + 2, h + 2, COL_FOCUS);
    }
}

fn draw_button(wg: &Widget, sx: i32, sy: i32, focused: bool) {
    unsafe {
        let face = if wg.pressed { COL_BTN_FACE_DN } else { COL_BTN_FACE };
        gfx::gfx_fill_rect(sx, sy, wg.w, wg.h, face);
        gfx::gfx_rect(sx, sy, wg.w, wg.h, COL_BORDER);
        gfx::kcg_set_scale(1);
        let ty = sy + (wg.h - 16) / 2;
        gfx::kcg_draw_utf8(sx + 5, ty, wg.text.as_ptr(), COL_BTN_TEXT, face);
    }
    if focused {
        focus_ring(sx, sy, wg.w, wg.h);
    }
}

fn draw_checkbox(wg: &Widget, sx: i32, sy: i32, focused: bool) {
    unsafe {
        let by = sy + (wg.h - CHECK_BOX) / 2;
        gfx::gfx_fill_rect(sx, by, CHECK_BOX, CHECK_BOX, COL_EDIT_BG);
        gfx::gfx_rect(sx, by, CHECK_BOX, CHECK_BOX, COL_BORDER);
        if wg.checked {
            /* チェック印 (レ) */
            gfx::gfx_line(sx + 3, by + 7, sx + 6, by + 10, COL_CHECK_MARK);
            gfx::gfx_line(sx + 6, by + 10, sx + 11, by + 3, COL_CHECK_MARK);
            gfx::gfx_line(sx + 3, by + 8, sx + 6, by + 11, COL_CHECK_MARK);
            gfx::gfx_line(sx + 6, by + 11, sx + 11, by + 4, COL_CHECK_MARK);
        }
        gfx::kcg_set_scale(1);
        let ty = sy + (wg.h - 16) / 2;
        gfx::kcg_draw_utf8(sx + CHECK_BOX + 4, ty, wg.text.as_ptr(), COL_LABEL_TEXT, COL_CLIENT);
    }
    if focused {
        focus_ring(sx, sy, wg.w, wg.h);
    }
}

fn draw_textbox(wg: &Widget, sx: i32, sy: i32, focused: bool) {
    unsafe {
        gfx::gfx_fill_rect(sx, sy, wg.w, wg.h, COL_EDIT_BG);
        gfx::gfx_rect(sx, sy, wg.w, wg.h, COL_BORDER);
        gfx::kcg_set_scale(1);
        let ty = sy + (wg.h - 16) / 2;
        gfx::kcg_draw_utf8(sx + 4, ty, wg.text.as_ptr(), COL_EDIT_TEXT, COL_EDIT_BG);
        if focused {
            let caret_x = sx + 4 + wg.caret * CHAR_W;
            gfx::gfx_vline(caret_x, sy + 3, wg.h - 6, COL_CARET);
        }
    }
    if focused {
        focus_ring(sx, sy, wg.w, wg.h);
    }
}

fn draw_listbox(st: &GuiState, wg: &Widget, sx: i32, sy: i32, focused: bool) {
    unsafe {
        gfx::gfx_fill_rect(sx, sy, wg.w, wg.h, COL_EDIT_BG);
        gfx::gfx_rect(sx, sy, wg.w, wg.h, COL_BORDER);
        gfx::kcg_set_scale(1);
    }
    let vis = wg.h / LIST_ROW_H;
    let mut row = 0;
    while row < vis {
        let idx = wg.top + row;
        if idx >= wg.item_count {
            break;
        }
        let ry = sy + row * LIST_ROW_H;
        let selected = idx == wg.sel;
        let (bg, fg) = if selected {
            (COL_SEL_BG, COL_SEL_TEXT)
        } else {
            (COL_EDIT_BG, COL_EDIT_TEXT)
        };
        unsafe {
            if selected {
                gfx::gfx_fill_rect(sx + 1, ry, wg.w - 2, LIST_ROW_H, bg);
            }
            if let Some(si) = st.list_item_slot(wg.id, idx) {
                gfx::kcg_draw_utf8(sx + 4, ry + 1, st.list_items[si].text.as_ptr(), fg, bg);
            }
        }
        row += 1;
    }
    if focused {
        focus_ring(sx, sy, wg.w, wg.h);
    }
}

/* 小さな矢印カーソル (自前描画。present 後も残るよう最後に描く) */
fn draw_cursor(mx: i32, my: i32) {
    unsafe {
        gfx::gfx_line(mx, my, mx, my + 12, COL_BORDER);
        gfx::gfx_line(mx, my, mx + 8, my + 8, COL_BORDER);
        gfx::gfx_line(mx, my + 12, mx + 8, my + 8, COL_BORDER);
        gfx::gfx_line(mx + 1, my + 2, mx + 1, my + 9, 7);
    }
}

/* 呼び出し側バッファへ NUL 終端でコピー。返り値: コピーしたバイト数。 */
unsafe fn copy_out(src: &[u8; MAX_TITLE], out: *mut u8, cap: i32) -> i32 {
    if out.is_null() || cap <= 0 {
        return 0;
    }
    let len = cstr_len(src);
    let max = (cap - 1) as usize;
    let n = if len < max { len } else { max };
    let mut i = 0;
    while i < n {
        *out.add(i) = src[i];
        i += 1;
    }
    *out.add(n) = 0;
    n as i32
}

/* ================================================================ */
/*  公開 API (extern "C")                                            */
/* ================================================================ */

/// GUI サブシステムを初期化する。libos32gfx を GFX モードにし、マウスを有効化する。
#[no_mangle]
pub extern "C" fn gui_init(api: *mut KernelAPI) -> i32 {
    if api.is_null() {
        return -1;
    }
    os32api::os32_init(api);
    let st = g();
    *st = GuiState::NEW;
    st.inited = true;
    unsafe {
        gfx::libos32gfx_init(api);
        let a = os32api::api();
        (a.mouse_set_bounds)(0, 0, (SCREEN_W - 1) as i16, (SCREEN_H - 1) as i16);
        (a.mouse_cursor_hide)();
    }
    0
}

/// GUI を終了し、テキスト画面へ戻す。
#[no_mangle]
pub extern "C" fn gui_shutdown() {
    let st = g();
    st.inited = false;
    unsafe {
        let a = os32api::api();
        (a.gfx_shutdown)();
        (a.tvram_clear)();
    }
}

/// ウィンドウを作成する。返り値: ハンドル (1 以上)、0=失敗。
#[no_mangle]
pub extern "C" fn gui_create_window(
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    title: *const u8,
    flags: u32,
) -> u32 {
    let st = g();
    let mut i = 0;
    while i < MAX_WINDOWS {
        if !st.windows[i].used {
            let id = st.next_win_id;
            st.next_win_id += 1;
            let mut win = Window::EMPTY;
            win.used = true;
            win.id = id;
            win.x = x;
            win.y = y;
            win.w = if w < 60 { 60 } else { w };
            win.h = if h < TITLEBAR_H + 8 { TITLEBAR_H + 8 } else { h };
            win.flags = if flags == 0 { GUI_WF_DEFAULT } else { flags };
            copy_cstr(&mut win.title, title);
            st.windows[i] = win;
            st.z_add_top(id);
            st.sync_focus_to_front();
            return id;
        }
        i += 1;
    }
    0
}

/// ウィンドウを破棄する (所属ウィジェット・リスト項目もまとめて解放)。
#[no_mangle]
pub extern "C" fn gui_destroy_window(hwnd: u32) -> i32 {
    let st = g();
    match st.win_index(hwnd) {
        Some(i) => {
            st.windows[i] = Window::EMPTY;
            st.z_remove(hwnd);
            let mut k = 0;
            while k < MAX_WIDGETS {
                if st.widgets[k].used && st.widgets[k].win_id == hwnd {
                    free_widget_items(st, st.widgets[k].id);
                    st.widgets[k] = Widget::EMPTY;
                }
                k += 1;
            }
            if st.drag_win == hwnd {
                st.drag_win = 0;
            }
            st.sync_focus_to_front();
            0
        }
        None => -1,
    }
}

fn free_widget_items(st: &mut GuiState, widget_id: u32) {
    let mut i = 0;
    while i < MAX_LIST_ITEMS {
        if st.list_items[i].used && st.list_items[i].widget_id == widget_id {
            st.list_items[i] = ListItem::EMPTY;
        }
        i += 1;
    }
}

/// ウィンドウを移動する。
#[no_mangle]
pub extern "C" fn gui_move_window(hwnd: u32, x: i32, y: i32) -> i32 {
    let st = g();
    match st.win_index(hwnd) {
        Some(i) => {
            st.windows[i].x = x;
            st.windows[i].y = y;
            0
        }
        None => -1,
    }
}

/// ウィンドウを最前面へ (= フォーカス)。
#[no_mangle]
pub extern "C" fn gui_bring_to_front(hwnd: u32) -> i32 {
    let st = g();
    if st.win_index(hwnd).is_none() {
        return -1;
    }
    st.bring_to_front(hwnd);
    0
}

/// フォーカス (最前面) を設定する。gui_bring_to_front と同義。
#[no_mangle]
pub extern "C" fn gui_set_focus(hwnd: u32) -> i32 {
    gui_bring_to_front(hwnd)
}

/// 現在フォーカス中 (最前面) のウィンドウハンドル。
#[no_mangle]
pub extern "C" fn gui_get_focus() -> u32 {
    g().front_win()
}

/// 現在のウィンドウ数。
#[no_mangle]
pub extern "C" fn gui_window_count() -> i32 {
    g().z_count as i32
}

/// ウィンドウの表示/非表示を切り替える。
#[no_mangle]
pub extern "C" fn gui_show_window(hwnd: u32, visible: i32) -> i32 {
    let st = g();
    match st.win_index(hwnd) {
        Some(i) => {
            if visible != 0 {
                st.windows[i].flags |= GUI_WF_VISIBLE;
            } else {
                st.windows[i].flags &= !GUI_WF_VISIBLE;
            }
            0
        }
        None => -1,
    }
}

fn add_widget(
    hwnd: u32,
    kind: u8,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    text: *const u8,
) -> u32 {
    let st = g();
    if st.win_index(hwnd).is_none() {
        return 0;
    }
    let mut i = 0;
    while i < MAX_WIDGETS {
        if !st.widgets[i].used {
            let id = st.next_widget_id;
            st.next_widget_id += 1;
            let mut wg = Widget::EMPTY;
            wg.used = true;
            wg.id = id;
            wg.win_id = hwnd;
            wg.kind = kind;
            wg.x = x;
            wg.y = y;
            wg.w = w;
            wg.h = h;
            copy_cstr(&mut wg.text, text);
            st.widgets[i] = wg;
            st.sync_focus_to_front();
            return id;
        }
        i += 1;
    }
    0
}

/// ボタンを追加する。返り値: ウィジェット id、0=失敗。
#[no_mangle]
pub extern "C" fn gui_add_button(hwnd: u32, x: i32, y: i32, w: i32, h: i32, text: *const u8) -> u32 {
    add_widget(hwnd, GUI_WT_BUTTON, x, y, w, h, text)
}

/// ラベルを追加する (テキストのみ)。
#[no_mangle]
pub extern "C" fn gui_add_label(hwnd: u32, x: i32, y: i32, text: *const u8) -> u32 {
    add_widget(hwnd, GUI_WT_LABEL, x, y, 0, 16, text)
}

/// チェックボックスを追加する。
#[no_mangle]
pub extern "C" fn gui_add_checkbox(hwnd: u32, x: i32, y: i32, text: *const u8) -> u32 {
    add_widget(hwnd, GUI_WT_CHECKBOX, x, y, 140, 16, text)
}

/// テキストボックス (単一行) を追加する。初期テキストは空。
#[no_mangle]
pub extern "C" fn gui_add_textbox(hwnd: u32, x: i32, y: i32, w: i32, h: i32) -> u32 {
    add_widget(hwnd, GUI_WT_TEXTBOX, x, y, w, h, core::ptr::null())
}

/// リストボックスを追加する。
#[no_mangle]
pub extern "C" fn gui_add_listbox(hwnd: u32, x: i32, y: i32, w: i32, h: i32) -> u32 {
    add_widget(hwnd, GUI_WT_LISTBOX, x, y, w, h, core::ptr::null())
}

/// ウィジェットのテキストを差し替える (textbox はキャレットを末尾へ)。
#[no_mangle]
pub extern "C" fn gui_widget_set_text(widget_id: u32, text: *const u8) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) => {
            let mut buf = [0u8; MAX_TITLE];
            copy_cstr(&mut buf, text);
            st.widgets[i].text = buf;
            st.widgets[i].caret = cstr_len(&buf) as i32;
            0
        }
        None => -1,
    }
}

/// ボタン/チェックボックスが押下中 (armed) か。1/0。
#[no_mangle]
pub extern "C" fn gui_button_is_pressed(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].pressed => 1,
        _ => 0,
    }
}

/// チェックボックスの状態を取得。1=チェック, 0=非/不明。
#[no_mangle]
pub extern "C" fn gui_checkbox_is_checked(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_CHECKBOX && st.widgets[i].checked => 1,
        _ => 0,
    }
}

/// チェックボックスの状態を設定。
#[no_mangle]
pub extern "C" fn gui_checkbox_set_checked(widget_id: u32, checked: i32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_CHECKBOX => {
            st.widgets[i].checked = checked != 0;
            0
        }
        _ => -1,
    }
}

/// テキストボックスの内容を呼び出し側バッファへ取得。返り値: バイト数。
///
/// # Safety
/// `out` は `cap` バイト以上の書込み可能領域を指すこと。
#[no_mangle]
pub unsafe extern "C" fn gui_textbox_get_text(widget_id: u32, out: *mut u8, cap: i32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) => copy_out(&st.widgets[i].text, out, cap),
        None => {
            if !out.is_null() && cap > 0 {
                *out = 0;
            }
            0
        }
    }
}

/// リストボックスへ項目を追加する。返り値: 追加した項目のインデックス、-1=失敗。
#[no_mangle]
pub extern "C" fn gui_listbox_add_item(widget_id: u32, text: *const u8) -> i32 {
    let st = g();
    let wi = match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_LISTBOX => i,
        _ => return -1,
    };
    let mut s = 0;
    while s < MAX_LIST_ITEMS {
        if !st.list_items[s].used {
            let idx = st.widgets[wi].item_count;
            let mut it = ListItem::EMPTY;
            it.used = true;
            it.widget_id = widget_id;
            it.index = idx;
            copy_cstr(&mut it.text, text);
            st.list_items[s] = it;
            st.widgets[wi].item_count = idx + 1;
            if st.widgets[wi].sel < 0 {
                st.widgets[wi].sel = 0;
            }
            return idx;
        }
        s += 1;
    }
    -1
}

/// リストボックスの全項目を消す。
#[no_mangle]
pub extern "C" fn gui_listbox_clear(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_LISTBOX => {
            free_widget_items(st, widget_id);
            st.widgets[i].item_count = 0;
            st.widgets[i].sel = -1;
            st.widgets[i].top = 0;
            0
        }
        _ => -1,
    }
}

/// リストボックスの選択インデックスを取得 (-1=なし/不明)。
#[no_mangle]
pub extern "C" fn gui_listbox_get_selection(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_LISTBOX => st.widgets[i].sel,
        _ => -1,
    }
}

/// リストボックスの選択を設定する。
#[no_mangle]
pub extern "C" fn gui_listbox_set_selection(widget_id: u32, index: i32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].kind == GUI_WT_LISTBOX => {
            if index < -1 || index >= st.widgets[i].item_count {
                return -1;
            }
            st.widgets[i].sel = index;
            st.list_ensure_visible(i);
            0
        }
        _ => -1,
    }
}

/// リストボックス項目のテキストを取得。返り値: バイト数。
///
/// # Safety
/// `out` は `cap` バイト以上の書込み可能領域を指すこと。
#[no_mangle]
pub unsafe extern "C" fn gui_listbox_get_item_text(
    widget_id: u32,
    index: i32,
    out: *mut u8,
    cap: i32,
) -> i32 {
    let st = g();
    match st.list_item_slot(widget_id, index) {
        Some(si) => copy_out(&st.list_items[si].text, out, cap),
        None => {
            if !out.is_null() && cap > 0 {
                *out = 0;
            }
            0
        }
    }
}

/// キーボードフォーカス中のウィジェット id を取得。
#[no_mangle]
pub extern "C" fn gui_get_focus_widget() -> u32 {
    g().focus_widget
}

/// キーボードフォーカスをウィジェットへ設定する。所属ウィンドウを最前面化する。
#[no_mangle]
pub extern "C" fn gui_set_focus_widget(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) => {
            let win = st.widgets[i].win_id;
            st.bring_to_front(win);
            st.focus_widget = widget_id;
            0
        }
        None => -1,
    }
}

/// 入力 (マウス/キー) をポーリングし、イベントキューを更新する。毎フレーム 1 回。
#[no_mangle]
pub extern "C" fn gui_pump() {
    let st = g();
    if !st.inited {
        return;
    }
    pump_mouse(st);
    pump_keyboard(st);
}

/// イベントを 1 個取り出す (Win32 の PeekMessage 相当)。1=あり, 0=空。
///
/// # Safety
/// `out` は有効な GuiEvent 1 個分を指していること。
#[no_mangle]
pub unsafe extern "C" fn gui_poll_event(out: *mut GuiEvent) -> i32 {
    if out.is_null() {
        return 0;
    }
    match g().pop_event() {
        Some(ev) => {
            *out = ev;
            1
        }
        None => 0,
    }
}

/// 全ウィンドウを Z オーダー順に再描画して VRAM へ転送する。毎フレーム 1 回。
#[no_mangle]
pub extern "C" fn gui_draw() {
    let st = g();
    if !st.inited {
        return;
    }
    draw_scene(st);
}

/// マウスの現在座標を取得する (画面座標)。
///
/// # Safety
/// `out_x` / `out_y` は有効な i32 を指すこと。NULL は無視。
#[no_mangle]
pub unsafe extern "C" fn gui_mouse_pos(out_x: *mut i32, out_y: *mut i32) {
    let st = g();
    if !out_x.is_null() {
        *out_x = st.mouse_x;
    }
    if !out_y.is_null() {
        *out_y = st.mouse_y;
    }
}
