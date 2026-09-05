//! app.rs — U3 のループ (GetMessage の代替、入れ子なし)。票 C2 作業 3。
//!
//! ```text
//! loop {
//!     n = OP_POLL                  // リングから一括で取り出す
//!     for ev in events[..n] { handle(ev) }   // 状態更新 + invalidate だけ
//!     paint_damaged()              // Paint を受けた窓を描く
//!     commit_all()                 // 1 周 1 回
//!     OP_WAIT(next_timer)          // 何も無ければ sys_halt で待つ
//! }
//! ```
//!
//! - ハンドラの中から `OP_WAIT` を呼ばない (`client::wait` の `debug_assert`)。
//! - `Quit` を受けたら速やかに戻る。
//! - 1 周の `gui_call` は `POLL` + `COMMIT` + `WAIT` + 状態変更だけ (契約 P)。

use crate::client::{self, GuiResult};
use crate::uistate::{s, GUI_NONE};
use crate::widget::{self, WidgetId, WEV_CLICK, WEV_FOCUS, WEV_SELECT, WEV_TEXT_CHANGED, WEV_TOGGLED};
use crate::window;
use os32api::gui::proto::{
    GuiEvent, GUI_COLOR_TEXT, GUI_COLOR_WINDOW, GUI_EV_BUTTON, GUI_EV_CLOSE, GUI_EV_CONFIGURE,
    GUI_EV_FOCUS, GUI_EV_KEY, GUI_EV_MODAL, GUI_EV_PAINT, GUI_EV_PALETTE, GUI_EV_POINTER,
    GUI_EV_QUIT, GUI_EV_TEXT, GUI_EV_TIMER, GUI_RING_CAPACITY,
};
use os32api::gui::types::{Rect, Style, SurfaceId};

/* ================================================================ */
/*  Ui — ループからハンドラへ渡す取っ手                              */
/* ================================================================ */

/// アプリのハンドラが触るループ側の状態。実体はグローバル (単一アプリ)。
pub struct Ui {
    /// 次の `OP_WAIT` のタイムアウト (tick)。0 = 期限なし (WM のタイマで起きる)。
    pub timeout_ticks: u32,
}

impl Ui {
    pub fn new() -> Ui {
        Ui { timeout_ticks: 0 }
    }

    /// ループを終える。
    #[inline]
    pub fn quit(&mut self) {
        s().quit = true;
    }

    #[inline]
    pub fn is_quitting(&self) -> bool {
        s().quit
    }

    /// 生きているウィンドウ枚数 (0 になったらループは戻る)。
    #[inline]
    pub fn window_count(&self) -> usize {
        window::count()
    }

    /// `OVERFLOW` を受けた直後か (入力状態を未知として扱う。契約 T3)。
    #[inline]
    pub fn input_unknown(&self) -> bool {
        s().input_unknown
    }

    /// 押下状態を読み直す (`OVERFLOW` からの復帰。契約 T3)。
    pub fn key_is_pressed(&self, scan: u8) -> bool {
        unsafe { (os32api::api().kbd_is_pressed)(scan as i32) != 0 }
    }
}

impl Default for Ui {
    fn default() -> Ui {
        Ui::new()
    }
}

/* ================================================================ */
/*  App — 種別ごとのハンドラ (契約 U6: 巨大 switch を書かせない)      */
/* ================================================================ */

#[allow(unused_variables)]
pub trait App {
    /* --- ウィジェット (クライアント側で合成) --- */
    fn on_click(&mut self, ui: &mut Ui, w: WidgetId) {}
    fn on_toggled(&mut self, ui: &mut Ui, w: WidgetId, on: bool) {}
    fn on_text_changed(&mut self, ui: &mut Ui, w: WidgetId) {}
    fn on_select(&mut self, ui: &mut Ui, w: WidgetId, index: i32) {}
    fn on_widget_focus(&mut self, ui: &mut Ui, w: WidgetId) {}

    /* --- WM 由来 --- */
    /// 生イベント (種別ごとのハンドラの前に 1 回)。`serial` を見たい計測器
    /// (`gui_bench`、契約 P2) 用。ふつうのアプリは実装しない。
    fn on_raw(&mut self, ui: &mut Ui, ev: &GuiEvent) {}

    /// 閉じるボタン。既定はループ終了 (破棄はアプリが決める。契約 U1)。
    fn on_close(&mut self, ui: &mut Ui, window: u32) {
        ui.quit();
    }
    fn on_focus(&mut self, ui: &mut Ui, window: u32, focused: bool) {}
    /// 生キー (契約 U2a: `scan` は PC-98 スキャンコード)。`down=false` も来る。
    fn on_key(&mut self, ui: &mut Ui, window: u32, scan: u8, ch: u8, mods: u8, down: bool) {}
    fn on_timer(&mut self, ui: &mut Ui, window: u32, id: u8) {}
    fn on_configure(&mut self, ui: &mut Ui, window: u32) {}
    fn on_palette(&mut self, ui: &mut Ui, window: u32, active: bool) {}
    fn on_modal(&mut self, ui: &mut Ui, dialog: u16, result: i16) {}
    /// WM からの終了要求。既定はループ終了。
    fn on_quit(&mut self, ui: &mut Ui, reason: u8) {
        ui.quit();
    }
    /// 取りこぼし通知 (契約 T3)。押しっぱなしの前提を捨てる。
    fn on_overflow(&mut self, ui: &mut Ui, dropped: u16) {}

    /* --- 描画 --- */
    /// ウィジェット木の後に呼ばれる。基底クリップは `rect` に固定済み (契約 G2)。
    fn on_paint(&mut self, ui: &mut Ui, window: u32, surface: SurfaceId, rect: Rect) {}

    /// `commit` の直後 (`OP_WAIT` の前)。計測 (`gui_bench`) 用の締めの場所。
    fn after_commit(&mut self, ui: &mut Ui) {}
}

/* ================================================================ */
/*  U3 ループ                                                        */
/* ================================================================ */

/// 1 アプリ 1 本のイベントループ。`Quit` / 全ウィンドウ消滅 / `Ui::quit` で戻る。
pub fn run(app: &mut impl App) -> GuiResult<()> {
    let mut ui = Ui::new();
    run_with(app, &mut ui)
}

/// `Ui` を呼び出し側が用意する版 (タイムアウトを最初から与えたいとき)。
pub fn run_with(app: &mut impl App, ui: &mut Ui) -> GuiResult<()> {
    let zero = GuiEvent { kind: 0, sub: 0, serial: 0, window: 0, payload: [0; 8] };
    let mut buf = [zero; GUI_RING_CAPACITY];

    loop {
        let p = client::poll(&mut buf)?;

        /* 取りこぼしは受け取った周だけ「入力状態を未知」にする (契約 T3)。 */
        s().input_unknown = p.overflow;
        if p.overflow {
            client::dbg_print_num(b"[gui] input OVERFLOW, dropped =", p.dropped as i32);
            client::enter_handler();
            app.on_overflow(ui, p.dropped);
            client::leave_handler();
        }

        /* (1) handle: 状態更新 + invalidate だけ。ここで描かない。 */
        client::enter_handler();
        let mut i = 0;
        while i < p.count {
            dispatch(app, ui, &buf[i]);
            i += 1;
        }
        client::leave_handler();

        /* (1b) 1 周分に溜まった損傷をまとめて申告する (契約 P: gui_call を減らす)。 */
        flush_damage();

        /* (2) Paint を受けた窓を描く。 */
        paint_damaged(app, ui);

        /* (3) commit は 1 周 1 回 (契約 P / G4)。 */
        client::commit(0)?;
        client::enter_handler();
        app.after_commit(ui);
        client::leave_handler();

        if s().quit || window::count() == 0 {
            break;
        }

        /* (4) 何も無ければ sys_halt で待つ。`after_commit` が出した損傷も先に流す。 */
        flush_damage();
        client::wait(ui.timeout_ticks)?;
    }
    Ok(())
}

/* ================================================================ */
/*  1 件の配送 (契約 T3 / U2)                                        */
/* ================================================================ */

/// 溜まった損傷を `OP_INVALIDATE` で送る (窓あたり最大 8 本)。
pub fn flush_damage() {
    let n = s().damage.len;
    let mut i = 0;
    while i < n {
        let d = s().damage.items[i];
        let _ = client::invalidate(d.window, d.rect);
        i += 1;
    }
    s().damage.clear();
}

#[inline]
fn to_rect(r: os32api::gui::proto::GuiRect16) -> Rect {
    Rect::new(r.x, r.y, r.w, r.h)
}

fn dispatch(app: &mut impl App, ui: &mut Ui, ev: &GuiEvent) {
    /* generation が自分の表と一致しないイベントは捨てる (契約 U2)。 */
    let slot = s().win_slot(ev.window);
    if slot.is_none() && ev.kind != GUI_EV_QUIT {
        return;
    }
    let win = ev.window;
    app.on_raw(ui, ev);

    match ev.kind {
        GUI_EV_PAINT => {
            s().paint.push(win, to_rect(ev.rect()));
        }
        GUI_EV_CONFIGURE => {
            window::on_configure(win, to_rect(ev.rect()));
            app.on_configure(ui, win);
        }
        GUI_EV_CLOSE => app.on_close(ui, win),
        GUI_EV_FOCUS => {
            let i = slot.unwrap();
            s().windows[i].focused = ev.sub != 0;
            /* フォーカスリングの見た目が変わるのはフォーカス中のウィジェットだけ。 */
            let f = s().windows[i].focus;
            if f != GUI_NONE {
                widget::invalidate(f as usize - 1);
            }
            app.on_focus(ui, win, ev.sub != 0);
        }
        GUI_EV_KEY => {
            let k = ev.key();
            if ev.sub != 0 {
                let out = widget::on_key(slot.unwrap(), k.scan, k.mods);
                emit(app, ui, &out);
            }
            app.on_key(ui, win, k.scan, k.ch, k.mods, ev.sub != 0);
        }
        GUI_EV_TEXT => {
            let t = ev.text();
            let len = (ev.sub & 0x7F) as usize;
            let n = if len > 8 { 8 } else { len };
            let out = widget::on_text(slot.unwrap(), &t.utf8[..n]);
            emit(app, ui, &out);
        }
        GUI_EV_POINTER => {
            let p = ev.pointer();
            s().ptr_x = p.x;
            s().ptr_y = p.y;
            widget::on_pointer(slot.unwrap(), p.x as i32, p.y as i32);
        }
        GUI_EV_BUTTON => {
            let b = ev.button();
            let i = slot.unwrap();
            let out = if ev.sub != 0 {
                widget::on_button_down(i, b.x as i32, b.y as i32)
            } else {
                widget::on_button_up(i, b.x as i32, b.y as i32)
            };
            emit(app, ui, &out);
        }
        GUI_EV_TIMER => {
            crate::timer::fired(win, ev.sub as u16);
            app.on_timer(ui, win, ev.sub);
        }
        GUI_EV_MODAL => {
            let m = ev.modal();
            app.on_modal(ui, m.dialog, m.result);
        }
        GUI_EV_QUIT => app.on_quit(ui, ev.sub),
        GUI_EV_PALETTE => app.on_palette(ui, win, ev.sub != 0),
        _ => {}
    }
}

/// 合成したウィジェットイベントを種別ごとのハンドラへ渡す。
fn emit(app: &mut impl App, ui: &mut Ui, out: &widget::WidgetOut) {
    let evs = out.as_slice();
    let mut i = 0;
    while i < evs.len() {
        let e = evs[i];
        match e.kind {
            WEV_CLICK => app.on_click(ui, e.widget),
            WEV_TOGGLED => app.on_toggled(ui, e.widget, e.value != 0),
            WEV_TEXT_CHANGED => app.on_text_changed(ui, e.widget),
            WEV_SELECT => app.on_select(ui, e.widget, e.value),
            WEV_FOCUS => app.on_widget_focus(ui, e.widget),
            _ => {}
        }
        i += 1;
    }
}

/* ================================================================ */
/*  描画 (契約 G2: 基底クリップ = 処理中の Paint 矩形)                */
/* ================================================================ */

fn paint_damaged(app: &mut impl App, ui: &mut Ui) {
    let n = s().paint.len;
    let mut i = 0;
    while i < n {
        let req = s().paint.items[i];
        i += 1;
        let slot = match s().win_slot(req.window) {
            Some(k) => k,
            None => continue, /* 破棄済みの窓宛 (契約 U2) */
        };
        let surface = s().windows[slot].surface;
        if surface.is_null() {
            continue;
        }
        /* 基底クリップを Paint 矩形に固定する。この外へは 1 画素も出ない。 */
        if crate::clip::set_base_clip(surface, req.rect) < 0 {
            continue;
        }
        /* クライアント面の下地 (WM は枠しか描かない)。 */
        crate::draw::fill_rect(
            surface,
            req.rect,
            Style::new(GUI_COLOR_TEXT, GUI_COLOR_WINDOW),
        );
        let root = s().windows[slot].root;
        let focus = s().windows[slot].focus;
        let focused = s().windows[slot].focused;
        widget::draw_tree(surface, root, focus, focused, req.rect);
        app.on_paint(ui, req.window, surface, req.rect);
        crate::clip::clear_base_clip();
    }
    s().paint.clear();
}
