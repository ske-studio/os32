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
#![no_std]

/* os32api: KernelAPI バインディング + libos32gfx の extern "C" 宣言。
 * panic_handler / global_allocator もここが提供する (単一実体)。 */
extern crate os32api;

pub mod types;

use core::cell::UnsafeCell;
use os32api::gfx;
use os32api::KernelAPI;
use types::*;

/* 公開型・定数の再エクスポート (Rust の利用側が libos32gui:: 直下で使える) */
pub use types::{
    GuiEvent, GUI_EV_BUTTON_CLICK, GUI_EV_KEY, GUI_EV_MOUSE_DOWN, GUI_EV_MOUSE_MOVE,
    GUI_EV_MOUSE_UP, GUI_EV_NONE, GUI_EV_WIN_ACTIVATE, GUI_EV_WIN_CLOSE, GUI_EV_WIN_MOVE,
    GUI_WF_BORDER, GUI_WF_DEFAULT, GUI_WF_HAS_CLOSE, GUI_WF_MOVABLE, GUI_WF_VISIBLE,
};

/* 画面サイズ (libos32gfx 400 ライン GFX モード) */
const SCREEN_W: i32 = 640;
const SCREEN_H: i32 = 400;

/* ================================================================ */
/*  グローバル状態                                                   */
/* ================================================================ */

struct GuiState {
    inited: bool,
    windows: [Window; MAX_WINDOWS],
    widgets: [Widget; MAX_WIDGETS],
    /* Z オーダー: window id を背面→前面で並べる。zorder[z_count-1] が最前面=フォーカス */
    zorder: [u32; MAX_WINDOWS],
    z_count: usize,
    next_win_id: u32,
    next_widget_id: u32,

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

    /* 押下でアーム、離してまだ上にいれば click になるボタン (0=なし) */
    armed_widget: u32,
}

impl GuiState {
    const NEW: GuiState = GuiState {
        inited: false,
        windows: [Window::EMPTY; MAX_WINDOWS],
        widgets: [Widget::EMPTY; MAX_WIDGETS],
        zorder: [0; MAX_WINDOWS],
        z_count: 0,
        next_win_id: 1,
        next_widget_id: 1,
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

/* UnsafeCell ラッパー (static mut を避けつつ単一グローバルを持つ) */
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
        if self.front_win() == id {
            return;
        }
        self.z_remove(id);
        self.z_add_top(id);
    }

    /* 点 (px,py) を含む最前面の可視ウィンドウ id。無ければ 0。 */
    fn hit_window(&self, px: i32, py: i32) -> u32 {
        let mut z = self.z_count;
        while z > 0 {
            z -= 1;
            let id = self.zorder[z];
            if let Some(i) = self.win_index(id) {
                let w = &self.windows[i];
                if (w.flags & GUI_WF_VISIBLE) != 0
                    && point_in(px, py, w.x, w.y, w.w, w.h)
                {
                    return id;
                }
            }
        }
        0
    }

    /* ウィンドウ内クライアント点 (相対 cx,cy) にあるボタン widget id。無ければ 0。 */
    fn hit_widget(&self, win_id: u32, cx: i32, cy: i32) -> u32 {
        let mut i = 0;
        while i < MAX_WIDGETS {
            let wg = &self.widgets[i];
            if wg.used && wg.win_id == win_id && wg.kind == GUI_WT_BUTTON {
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
            /* あふれたら最古を捨てる */
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
            /* 画面内にクランプ (タイトルバーが最低限つかめるよう緩め) */
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

            /* 閉じるボタン */
            let (crx, cry, crw, crh) = w.close_rect();
            if (w.flags & GUI_WF_HAS_CLOSE) != 0 && point_in(mx, my, crx, cry, crw, crh) {
                st.push_event(GuiEvent {
                    kind: GUI_EV_WIN_CLOSE,
                    win_id: win,
                    ..GuiEvent::NONE
                });
            } else {
                /* タイトルバー → ドラッグ開始 */
                let (tx, ty, tw, th) = w.titlebar_rect();
                if (w.flags & GUI_WF_MOVABLE) != 0 && point_in(mx, my, tx, ty, tw, th) {
                    st.drag_win = win;
                    st.drag_dx = mx - w.x;
                    st.drag_dy = my - w.y;
                } else {
                    /* クライアント領域 → ウィジェット判定 */
                    let (cox, coy) = w.client_origin();
                    let cx = mx - cox;
                    let cy = my - coy;
                    let wid = st.hit_widget(win, cx, cy);
                    if wid != 0 {
                        st.armed_widget = wid;
                        if let Some(wi) = st.widget_index(wid) {
                            st.widgets[wi].pressed = true;
                        }
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
            if let Some(wi) = st.widget_index(wid) {
                st.widgets[wi].pressed = false;
                let win = st.widgets[wi].win_id;
                let (wx, wy, ww, wh) =
                    (st.widgets[wi].x, st.widgets[wi].y, st.widgets[wi].w, st.widgets[wi].h);
                /* まだボタンの上か? */
                if let Some(pi) = st.win_index(win) {
                    let (cox, coy) = st.windows[pi].client_origin();
                    if point_in(mx, my, cox + wx, coy + wy, ww, wh) {
                        st.push_event(GuiEvent {
                            kind: GUI_EV_BUTTON_CLICK,
                            win_id: win,
                            widget_id: wid,
                            ..GuiEvent::NONE
                        });
                    }
                }
            }
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

fn pump_keyboard(st: &mut GuiState) {
    unsafe {
        let a = os32api::api();
        loop {
            let ch = (a.kbd_trygetchar)();
            if ch <= 0 {
                break;
            }
            let focus = st.front_win();
            st.push_event(GuiEvent {
                kind: GUI_EV_KEY,
                win_id: focus,
                key: ch,
                ..GuiEvent::NONE
            });
        }
    }
}

/* ================================================================ */
/*  描画 (フルシーン再描画。背後の save/restore は将来の最適化)      */
/* ================================================================ */

fn draw_scene(st: &GuiState) {
    unsafe {
        gfx::gfx_clear(COL_DESKTOP);
    }
    /* 背面→前面の順に描く */
    let mut z = 0;
    while z < st.z_count {
        let id = st.zorder[z];
        if let Some(i) = st.win_index(id) {
            let w = st.windows[i];
            if (w.flags & GUI_WF_VISIBLE) != 0 {
                let active = st.front_win() == id;
                draw_window(&w, active);
                draw_widgets(st, id);
            }
        }
        z += 1;
    }
    draw_cursor(st.mouse_x, st.mouse_y);
    /* VRAM へ転送 (全画面ダーティ) */
    unsafe {
        let a = os32api::api();
        (a.gfx_add_dirty_rect)(0, 0, SCREEN_W, SCREEN_H);
        (a.gfx_present_dirty)();
    }
}

fn draw_window(w: &Window, active: bool) {
    unsafe {
        /* 枠 (背景) */
        if (w.flags & GUI_WF_BORDER) != 0 {
            gfx::gfx_fill_rect(w.x, w.y, w.w, w.h, COL_BORDER);
        }
        /* クライアント面 */
        let (cox, coy) = w.client_origin();
        let (cw, ch) = w.client_size();
        gfx::gfx_fill_rect(cox, coy, cw, ch, COL_CLIENT);
        /* タイトルバー */
        let (tx, ty, tw, th) = w.titlebar_rect();
        let tcol = if active { COL_TITLE_ACT } else { COL_TITLE_INACT };
        gfx::gfx_fill_rect(tx, ty, tw, th, tcol);
        gfx::kcg_set_scale(1);
        gfx::kcg_draw_utf8(tx + 4, ty + 1, w.title.as_ptr(), COL_TITLE_TEXT, tcol);
        /* 閉じるボタン */
        if (w.flags & GUI_WF_HAS_CLOSE) != 0 {
            let (crx, cry, crw, crh) = w.close_rect();
            gfx::gfx_fill_rect(crx, cry, crw, crh, COL_CLOSE_FACE);
            gfx::gfx_rect(crx, cry, crw, crh, COL_BORDER);
            /* × 印 */
            gfx::gfx_line(crx + 3, cry + 3, crx + crw - 4, cry + crh - 4, COL_CLOSE_X);
            gfx::gfx_line(crx + crw - 4, cry + 3, crx + 3, cry + crh - 4, COL_CLOSE_X);
        }
    }
}

fn draw_widgets(st: &GuiState, win_id: u32) {
    let i = match st.win_index(win_id) {
        Some(i) => i,
        None => return,
    };
    let (cox, coy) = st.windows[i].client_origin();
    let mut k = 0;
    while k < MAX_WIDGETS {
        let wg = &st.widgets[k];
        if wg.used && wg.win_id == win_id {
            let sx = cox + wg.x;
            let sy = coy + wg.y;
            unsafe {
                match wg.kind {
                    GUI_WT_BUTTON => {
                        let face = if wg.pressed { COL_BTN_FACE_DN } else { COL_BTN_FACE };
                        gfx::gfx_fill_rect(sx, sy, wg.w, wg.h, face);
                        gfx::gfx_rect(sx, sy, wg.w, wg.h, COL_BORDER);
                        gfx::kcg_set_scale(1);
                        let ty = sy + (wg.h - 16) / 2;
                        gfx::kcg_draw_utf8(sx + 5, ty, wg.text.as_ptr(), COL_BTN_TEXT, face);
                    }
                    GUI_WT_LABEL => {
                        gfx::kcg_set_scale(1);
                        gfx::kcg_draw_utf8(sx, sy, wg.text.as_ptr(), COL_LABEL_TEXT, COL_CLIENT);
                    }
                    _ => {}
                }
            }
        }
        k += 1;
    }
}

/* 小さな矢印カーソル (自前描画。present 後も残るよう最後に描く) */
fn draw_cursor(mx: i32, my: i32) {
    unsafe {
        /* 黒アウトライン + 白 の簡易矢印 */
        gfx::gfx_line(mx, my, mx, my + 12, COL_BORDER);
        gfx::gfx_line(mx, my, mx + 8, my + 8, COL_BORDER);
        gfx::gfx_line(mx, my + 12, mx + 8, my + 8, COL_BORDER);
        gfx::gfx_line(mx + 1, my + 2, mx + 1, my + 9, 7);
    }
}

/* ================================================================ */
/*  公開 API (extern "C")                                            */
/* ================================================================ */

/// GUI サブシステムを初期化する。libos32gfx を GFX モードにし、マウスを有効化する。
/// 返り値: 0=成功, 負=失敗。
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
        /* HW カーソルは自前描画するので出さない (present で消えるため) */
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

/// ウィンドウを作成する。返り値: ウィンドウハンドル (1 以上)、0=失敗。
/// `title` は NUL 終端 UTF-8。`flags` は GUI_WF_*。0 を渡すと GUI_WF_DEFAULT。
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
            st.z_add_top(id); /* 新規は最前面 */
            return id;
        }
        i += 1;
    }
    0
}

/// ウィンドウを破棄する (所属ウィジェットもまとめて解放)。0=成功, 負=不明なハンドル。
#[no_mangle]
pub extern "C" fn gui_destroy_window(hwnd: u32) -> i32 {
    let st = g();
    match st.win_index(hwnd) {
        Some(i) => {
            st.windows[i] = Window::EMPTY;
            st.z_remove(hwnd);
            /* ウィジェット解放 */
            let mut k = 0;
            while k < MAX_WIDGETS {
                if st.widgets[k].used && st.widgets[k].win_id == hwnd {
                    st.widgets[k] = Widget::EMPTY;
                }
                k += 1;
            }
            if st.drag_win == hwnd {
                st.drag_win = 0;
            }
            0
        }
        None => -1,
    }
}

/// ウィンドウを移動する。0=成功。
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

/// ウィンドウを最前面へ (= フォーカス)。0=成功。
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

/// 現在フォーカス中 (最前面) のウィンドウハンドル。無ければ 0。
#[no_mangle]
pub extern "C" fn gui_get_focus() -> u32 {
    g().front_win()
}

/// 現在のウィンドウ数。
#[no_mangle]
pub extern "C" fn gui_window_count() -> i32 {
    g().z_count as i32
}

/// ウィンドウの表示/非表示を切り替える。0=成功。
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

/// ボタンウィジェットを追加する。座標は所属ウィンドウのクライアント相対。
/// 返り値: ウィジェット id (1 以上)、0=失敗。
#[no_mangle]
pub extern "C" fn gui_add_button(
    hwnd: u32,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    text: *const u8,
) -> u32 {
    add_widget(hwnd, GUI_WT_BUTTON, x, y, w, h, text)
}

/// ラベルウィジェットを追加する (テキストのみ)。返り値: ウィジェット id、0=失敗。
#[no_mangle]
pub extern "C" fn gui_add_label(hwnd: u32, x: i32, y: i32, text: *const u8) -> u32 {
    add_widget(hwnd, GUI_WT_LABEL, x, y, 0, 16, text)
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
            return id;
        }
        i += 1;
    }
    0
}

/// ウィジェットのテキストを差し替える。0=成功。
#[no_mangle]
pub extern "C" fn gui_widget_set_text(widget_id: u32, text: *const u8) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) => {
            let mut buf = [0u8; MAX_TITLE];
            copy_cstr(&mut buf, text);
            st.widgets[i].text = buf;
            0
        }
        None => -1,
    }
}

/// ボタンが現在押下中 (armed) か。1=押下中, 0=非押下/不明。
#[no_mangle]
pub extern "C" fn gui_button_is_pressed(widget_id: u32) -> i32 {
    let st = g();
    match st.widget_index(widget_id) {
        Some(i) if st.widgets[i].pressed => 1,
        _ => 0,
    }
}

/// 入力 (マウス/キー) をポーリングし、イベントキューを更新する。
/// Win32 の「メッセージポンプの生成側」。毎フレーム 1 回呼ぶ。
#[no_mangle]
pub extern "C" fn gui_pump() {
    let st = g();
    if !st.inited {
        return;
    }
    pump_mouse(st);
    pump_keyboard(st);
}

/// イベントを 1 個取り出す (Win32 の PeekMessage 相当)。
/// `out` に書き込み 1 を返す。キューが空なら 0。
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
