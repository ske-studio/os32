//! v12_api_test — GUI v1.2 デスクトップ client API の実機確認アプリ (票 C4 §7)
//!
//! gshell (WM) の下で動く**普通のアプリ**。v1.2 で足した 6 本の公開 API
//! (ジャンプ表 95〜100) を 1 枚の窓から全部叩き、結果を**ラベルとして窓の中に
//! 出す** — `/api/screenshot` だけで合否を判定できるようにするため。
//!
//! ```text
//!   キー / ボタン        試すもの
//!   1 / [MsgBox]        modal_open(OK)  → on_modal → modal_result (値は空)
//!   2 / [File]          file_open       → on_modal → modal_result (絶対パス)
//!   3 / [Input]         input_open      → on_modal → modal_result (UTF-8。FEP 可)
//!   4 / [Launch]        session_launch("/usr/bin/gui_demo.bin") の戻り値
//!   ESC                 終了
//! ```
//!
//! 窓の下端には Icon16 を 2 つ描く。左は **mask 全 1 (不透明)**、右は
//! **菱形 mask (角が透明)**。どちらもデスクトップ色 (`GUI_COLOR_DESKTOP`) の
//! 下地の上に描くので、右のアイコンの角に下地色が透けていれば mask が効いている。
//!
//! `GUI_EV_QUIT` は libos32gui の既定 `on_quit` (= `ui.quit()`) で抜ける
//! (`session_launch` が受理されると WM が Quit を投げてくる)。
#![no_std]
#![no_main]

extern crate libos32gui;
extern crate os32api;

use libos32gui::gapi::proto::{GUI_COLOR_DESKTOP, GUI_COLOR_TEXT, GUI_MODAL_OK};
use libos32gui::gapi::types::{Rect, Style};
use libos32gui::icon::{draw_icon16, GuiIcon16};
use libos32gui::widget::{self, WidgetId, SCAN_ESC};
use libos32gui::{modal, session, App, GuiErr, SizeSpec, Ui, Window, WindowSpec};
use os32api::gui::types::SurfaceId;
use os32api::KernelAPI;

/// 起動要求で差し替える相手 (v1.2 の LAUNCH は絶対パスのみ)。
const LAUNCH_PATH: &[u8] = b"/usr/bin/gui_demo.bin";

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8, api: *mut KernelAPI) -> i32 {
    if libos32gui::init(api).is_err() {
        return -1;
    }
    let mut app = match Test::build() {
        Ok(a) => a,
        Err(e) => return e.code(),
    };
    let _ = libos32gui::run(&mut app);
    0
}

/* ================================================================ */
/*  アプリの状態                                                     */
/* ================================================================ */

struct Test {
    win: Option<Window>,
    b_msg: WidgetId,
    b_file: WidgetId,
    b_input: WidgetId,
    b_launch: WidgetId,
    lbl_modal: WidgetId,
    lbl_file: WidgetId,
    lbl_input: WidgetId,
    lbl_sess: WidgetId,
    /// 発行した DialogId → どの試験か (1=msg / 2=file / 3=input)。0 = 未発行。
    pending: [u16; 4],
}

impl Test {
    fn build() -> libos32gui::GuiResult<Test> {
        let info = libos32gui::gapi::screen_info();
        let sw = info.width as i32;
        let sh = info.height as i32;
        let w = clamp(sw * 3 / 5, 300, 380);
        let h = clamp(sh * 3 / 5, 210, 260);

        let win = Window::create(
            &WindowSpec::new(b"v12 api test", Rect::new(24, 24, w as i16, h as i16))
                .min_size(300, 210),
        )?;

        let root = widget::column(8, 5)?;

        /* --- ボタン列 (PM の /api/mouse 台本はこの並びを叩く) --- */
        let btns = widget::row(0, 6)?;
        let b_msg = widget::button(b"1 MsgBox")?;
        let b_file = widget::button(b"2 File")?;
        let b_input = widget::button(b"3 Input")?;
        let b_launch = widget::button(b"4 Launch")?;
        widget::add(btns, b_msg, SizeSpec::Flex(1))?;
        widget::add(btns, b_file, SizeSpec::Flex(1))?;
        widget::add(btns, b_input, SizeSpec::Flex(1))?;
        widget::add(btns, b_launch, SizeSpec::Flex(1))?;
        widget::add(root, btns, SizeSpec::Fixed(26))?;

        /* --- 結果ラベル (screenshot で読む) --- */
        let lbl_modal = widget::label(b"msg: -")?;
        let lbl_file = widget::label(b"file: -")?;
        let lbl_input = widget::label(b"input: -")?;
        let lbl_sess = widget::label(b"launch: -")?;
        widget::add(root, lbl_modal, SizeSpec::Fixed(17))?;
        widget::add(root, lbl_file, SizeSpec::Fixed(17))?;
        widget::add(root, lbl_input, SizeSpec::Fixed(17))?;
        widget::add(root, lbl_sess, SizeSpec::Fixed(17))?;

        let hint = widget::label(b"ESC: quit  icons: opaque / masked")?;
        widget::add(root, hint, SizeSpec::Fixed(17))?;

        /* 残りは widget を置かない = 下地のまま。そこへ on_paint で icon16 を描く。 */
        win.set_root(root)?;
        win.set_focus()?;

        Ok(Test {
            win: Some(win),
            b_msg,
            b_file,
            b_input,
            b_launch,
            lbl_modal,
            lbl_file,
            lbl_input,
            lbl_sess,
            pending: [0; 4],
        })
    }

    fn win_id(&self) -> u32 {
        match self.win.as_ref() {
            Some(w) => w.id(),
            None => 0,
        }
    }

    /* ---- 3 つの open。DialogId を控えて on_modal で照合する ---- */

    fn do_msgbox(&mut self) {
        let r = modal::modal_open(self.win_id(), GUI_MODAL_OK, b"v1.2 MODAL_RESULT test");
        self.note_open(r, 1, self.lbl_modal, b"msg");
    }

    fn do_file(&mut self) {
        let r = modal::file_open(self.win_id(), b"Open a file");
        self.note_open(r, 2, self.lbl_file, b"file");
    }

    fn do_input(&mut self) {
        let r = modal::input_open(self.win_id(), b"Type text (SHIFT+SPACE = FEP)");
        self.note_open(r, 3, self.lbl_input, b"input");
    }

    /// `session_launch` は「受理」しか返さない。受理されれば WM が Quit を投げる。
    fn do_launch(&mut self) {
        let mut buf = [0u8; 64];
        let n = match session::session_launch(LAUNCH_PATH) {
            Ok(()) => fmt2(&mut buf, b"launch: accepted ", LAUNCH_PATH),
            Err(e) => fmt2(&mut buf, b"launch: ", e.name()),
        };
        let _ = widget::set_text(self.lbl_sess, &buf[..n]);
        let _ = self.invalidate_all();
    }

    fn note_open(&mut self, r: libos32gui::GuiResult<u16>, kind: usize, lbl: WidgetId, tag: &[u8]) {
        let mut buf = [0u8; 64];
        let n = match r {
            Ok(id) => {
                self.pending[kind] = id;
                fmt_num(&mut buf, tag, b": dialog #", id as i32)
            }
            Err(e) => fmt2(&mut buf, tag, e.name()),
        };
        let _ = widget::set_text(lbl, &buf[..n]);
        let _ = self.invalidate_all();
    }

    fn invalidate_all(&self) -> libos32gui::GuiResult<()> {
        match self.win.as_ref() {
            Some(w) => w.invalidate_all(),
            None => Err(GuiErr::STALE),
        }
    }
}

/* ================================================================ */
/*  ハンドラ                                                         */
/* ================================================================ */

impl App for Test {
    fn on_click(&mut self, _ui: &mut Ui, w: WidgetId) {
        if w == self.b_msg {
            self.do_msgbox();
        } else if w == self.b_file {
            self.do_file();
        } else if w == self.b_input {
            self.do_input();
        } else if w == self.b_launch {
            self.do_launch();
        }
    }

    /// 数字キーでも同じことをする (`/api/key` 台本用)。ESC で終了。
    fn on_key(&mut self, ui: &mut Ui, _window: u32, scan: u8, ch: u8, _mods: u8, down: bool) {
        if !down {
            return;
        }
        if scan == SCAN_ESC {
            ui.quit();
            return;
        }
        match ch {
            b'1' => self.do_msgbox(),
            b'2' => self.do_file(),
            b'3' => self.do_input(),
            b'4' => self.do_launch(),
            _ => {}
        }
    }

    /// **ここが票 C4 の要**: 値は `on_modal` の中で `modal_result` から取る。
    /// 非同期なので open が同期で path/text を返すことはない。
    fn on_modal(&mut self, _ui: &mut Ui, dialog: u16, result: i16) {
        let mut val = [0u8; 256];
        let r = modal::modal_result(dialog, &mut val);

        /* どの試験の結果かは控えた DialogId で決める。 */
        let (lbl, tag) = if dialog == self.pending[2] {
            (self.lbl_file, &b"file"[..])
        } else if dialog == self.pending[3] {
            (self.lbl_input, &b"input"[..])
        } else {
            (self.lbl_modal, &b"msg"[..])
        };

        let mut buf = [0u8; 64];
        let n = match r {
            Ok(m) => {
                if m.copied == 0 {
                    /* MessageBox は値が空 (契約 M1)。result だけ見せる。 */
                    fmt_num(&mut buf, tag, b": r=", m.result as i32)
                } else {
                    fmt2(&mut buf, tag, &val[..m.copied])
                }
            }
            Err(e) => fmt2(&mut buf, tag, e.name()),
        };
        let _ = widget::set_text(lbl, &buf[..n]);
        let _ = result; /* 値は modal_result 側で受ける (契約 M1) */
        let _ = self.invalidate_all();
    }

    fn on_close(&mut self, ui: &mut Ui, _window: u32) {
        self.win = None;
        ui.quit();
    }

    /// ウィジェットが使わなかった下端に Icon16 を 2 つ描く (契約 V12-I の目視確認)。
    ///
    /// 下地はデスクトップ色。左は mask 全 1、右は菱形 mask なので、右の角に
    /// 下地色が残っていれば「mask=0 は描かない」が効いている。
    fn on_paint(&mut self, _ui: &mut Ui, _window: u32, surface: SurfaceId, _rect: Rect) {
        let (cw, ch) = match self.win.as_ref() {
            Some(w) => w.client_size(),
            None => return,
        };
        let y = ch as i32 - 30;
        if y < 0 || cw < 96 {
            return;
        }
        /* 下地: 24x24 のデスクトップ色パッチ 2 枚 */
        let bg = Style::new(GUI_COLOR_TEXT, GUI_COLOR_DESKTOP);
        libos32gui::gapi::fill_rect(surface, Rect::new(10, y as i16, 24, 24), bg);
        libos32gui::gapi::fill_rect(surface, Rect::new(44, y as i16, 24, 24), bg);
        /* 左: 不透明 / 右: 菱形 mask */
        draw_icon16(surface, 14, y + 4, &ICON_OPAQUE);
        draw_icon16(surface, 48, y + 4, &ICON_MASKED);
    }
}

/* ================================================================ */
/*  Icon16 のデータ (契約 V12-I の論理形式をそのまま組む)             */
/* ================================================================ */

/// 枠 = 色 0 (TEXT)、内側 = 色 8 (ALERT)。両アイコン共通の画素。
static ICON_PIXELS: [u8; 128] = boxed_pixels(GUI_COLOR_TEXT, 8);

/// mask 全 1 (不透明)。
static ICON_OPAQUE: GuiIcon16 = GuiIcon16 { pixels: ICON_PIXELS, mask: [0xFF; 32] };
/// 菱形 mask (角が透明)。
static ICON_MASKED: GuiIcon16 = GuiIcon16 { pixels: ICON_PIXELS, mask: diamond_mask() };

/// 16x16 の「枠つき塗り」を 4bpp row-major (even x = 上位ニブル) で組む。
const fn boxed_pixels(border: u8, fill: u8) -> [u8; 128] {
    let mut p = [0u8; 128];
    let mut y = 0usize;
    while y < 16 {
        let mut x = 0usize;
        while x < 16 {
            let c = if y == 0 || y == 15 || x == 0 || x == 15 { border } else { fill };
            let i = y * 8 + (x >> 1);
            if x & 1 == 0 {
                p[i] = (p[i] & 0x0F) | ((c & 0x0F) << 4);
            } else {
                p[i] = (p[i] & 0xF0) | (c & 0x0F);
            }
            x += 1;
        }
        y += 1;
    }
    p
}

/// 菱形の 1bpp mask (bit7 が左端、1 = 不透明)。四隅が 0 になる。
const fn diamond_mask() -> [u8; 32] {
    let mut m = [0u8; 32];
    let mut y = 0usize;
    while y < 16 {
        let mut x = 0usize;
        while x < 16 {
            let dx = if x >= 8 { x - 8 } else { 7 - x };
            let dy = if y >= 8 { y - 8 } else { 7 - y };
            if dx + dy <= 6 {
                m[y * 2 + (x >> 3)] |= 1u8 << (7 - (x & 7));
            }
            x += 1;
        }
        y += 1;
    }
    m
}

/* ================================================================ */
/*  小道具 (alloc も core::fmt も使わない)                            */
/* ================================================================ */

fn clamp(v: i32, lo: i32, hi: i32) -> i32 {
    if v < lo {
        lo
    } else if v > hi {
        hi
    } else {
        v
    }
}

/// `a` + `": "` + `b` を詰める。返り値: 書いたバイト数。
fn fmt2(dst: &mut [u8], a: &[u8], b: &[u8]) -> usize {
    let mut n = put(dst, 0, a);
    n = put(dst, n, b": ");
    put(dst, n, b)
}

/// `a` + `b` + 10 進数を詰める。
fn fmt_num(dst: &mut [u8], a: &[u8], b: &[u8], v: i32) -> usize {
    let mut n = put(dst, 0, a);
    n = put(dst, n, b);
    let mut tmp = [0u8; 12];
    let mut k = 0usize;
    let neg = v < 0;
    let mut u = if neg { (-(v as i64)) as u32 } else { v as u32 };
    if u == 0 {
        tmp[k] = b'0';
        k += 1;
    }
    while u > 0 {
        tmp[k] = b'0' + (u % 10) as u8;
        u /= 10;
        k += 1;
    }
    if neg {
        n = put(dst, n, b"-");
    }
    while k > 0 {
        k -= 1;
        if n < dst.len() {
            dst[n] = tmp[k];
            n += 1;
        }
    }
    n
}

/// `dst[at..]` へ `src` を NUL 手前まで詰める。返り値: 新しい末尾。
fn put(dst: &mut [u8], at: usize, src: &[u8]) -> usize {
    let mut n = at;
    let mut i = 0usize;
    while i < src.len() && src[i] != 0 && n < dst.len() {
        dst[n] = src[i];
        n += 1;
        i += 1;
    }
    n
}
