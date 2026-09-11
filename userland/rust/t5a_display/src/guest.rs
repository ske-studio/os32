//! Guest-only glue. Host tests never call KAPI or the shared GUI library.
//!
//! 票 K6C-A: fixture 供給を **con_sink 供給**に置き換えた端末窓。待ちは
//! GetMessage 方式 (`libos32gui::run` の U3 ループ) のままで、吸い出しは
//! 反復タイマの中だけで行う。ここに busy loop は無い — 協調型なので回し
//! 続けると他のアプリが飢える。
use crate::{
    boundary,
    input::{self, Action},
    paint,
    session::Session,
    sink::{self, Stop as SinkStop},
    state::{Fixture, Movement},
    status::{self, SinkStatus},
    storage::Storage,
    view::{Layout, MARGIN},
};
use libos32gui::gapi::{
    self,
    proto::{GUI_COLOR_EDIT_BG, GUI_COLOR_TEXT},
    types::{Rect, Style, SurfaceId},
};
use libos32gui::{App, GuiErr, GuiResult, Timer, Ui, Window, WindowSpec};
use libos32term_render::{Glyphs, Rect as PixelRect, Sink, CELL_HEIGHT, CELL_WIDTH};
use os32api::KernelAPI;

static STORAGE: Storage = Storage::new();

/// 吸い出しタイマ。10ms 刻みなので 10 = 100ms (票 §1)。
const TIMER_SINK: u8 = 1;
const TIMER_TICKS: u16 = 10;

/// con_sink_read へ渡す私有バッファ。`cap >= CON_SINK_REC_MAX` (203) が
/// KAPI の要求で、下回ると `OS32_ERR_INVAL`。1KB あれば 1 回で 5 本以上入る。
const SINK_BUF: usize = 1024;
const _: () = assert!(SINK_BUF >= sink::REC_MAX);

/// タイマ 1 周で吸う上限 (票 §2-1)。リングは 8KB なので 1 周で汲み切れる。
/// 上限を置くのは、際限なく読み続けて Paint と他アプリを待たせないため。
const SINK_BUDGET: usize = 8 * 1024;

pub fn run(api: *mut KernelAPI) -> i32 {
    if api.is_null() {
        return GuiErr::INVAL.code();
    }
    // Claim before GUI/KAPI init: reentrant or repeated main cannot replace API
    // globals or construct a second mutable reference. Claim is never released.
    let cells = match STORAGE.take() {
        Some(c) => c,
        None => return GuiErr::INVAL.code(),
    };
    if let Err(e) = libos32gui::init(api) {
        return e.code();
    }
    let window = match build_window() {
        Ok(w) => w,
        Err(e) => return e.code(),
    };
    /* 空の画面で始める。以後は con_sink のレコードだけが入る (票 §2-4)。 */
    let session = match Session::new(cells, Fixture::Live) {
        Ok(s) => s,
        Err(_) => return GuiErr::INVAL.code(),
    };
    /* タイマが張れなければ何も吸えない。同期で回す代案は取らない
     * (協調型なので他のアプリが止まる)。 */
    let timer = match Timer::repeating(&window, TIMER_SINK, TIMER_TICKS) {
        Ok(t) => t,
        Err(e) => return e.code(),
    };
    let mut app = DisplayApp {
        window: Some(window),
        _timer: Some(timer),
        session,
        buf: [0; SINK_BUF],
        sink: SinkStatus::default(),
        follow: true,
        runs: 0,
        paint_error: false,
    };
    match libos32gui::run(&mut app) {
        Ok(()) => 0,
        Err(e) => e.code(),
    }
}

fn build_window() -> GuiResult<Window> {
    let info = gapi::screen_info();
    let plan = boundary::windows(info.width as i64, info.height as i64).ok_or(GuiErr::INVAL)?;
    let rect = gui_rect(plan[0]).ok_or(GuiErr::INVAL)?;
    Window::create(&WindowSpec::new(b"Terminal (con_sink)  j k g e / q", rect))
}

fn gui_rect(r: PixelRect) -> Option<Rect> {
    Some(Rect::new(
        i16::try_from(r.x0).ok()?,
        i16::try_from(r.y0).ok()?,
        i16::try_from(r.x1.checked_sub(r.x0)?).ok()?,
        i16::try_from(r.y1.checked_sub(r.y0)?).ok()?,
    ))
}
fn pixels(r: Rect) -> PixelRect {
    PixelRect {
        x0: r.x as i64,
        y0: r.y as i64,
        x1: r.x as i64 + r.w as i64,
        y1: r.y as i64 + r.h as i64,
    }
}

struct DisplayApp<'a> {
    window: Option<Window>,
    /// 持っているだけ。`Drop` が `kill_timer` を出す。
    _timer: Option<Timer>,
    session: Session<'a>,
    /// con_sink_read の行き先。毎周スタックに 1KB 積まないよう持ち回す。
    buf: [u8; SINK_BUF],
    sink: SinkStatus,
    /// 末尾追従。スクロール操作で切れ、`e` (Last) で戻る。
    follow: bool,
    runs: u64,
    paint_error: bool,
}

impl DisplayApp<'_> {
    fn layout(&self) -> Option<Layout> {
        let (w, h) = self.window.as_ref()?.client_size();
        Layout::new(w as i64, h as i64)
    }
    fn fail(&mut self, ui: &mut Ui) {
        ui.quit();
    }
    fn repaint(&mut self, ui: &mut Ui) {
        if let Some(w) = self.window.as_ref() {
            if w.invalidate_all().is_err() {
                self.fail(ui);
            }
        }
    }

    /// リングを空になるまで (1 周の予算まで) 吸って T4 モデルへ流す。
    /// 戻り値は「描き直す必要があるか」。
    fn pump(&mut self) -> bool {
        let mut changed = false;
        let mut budget = SINK_BUDGET;
        while !self.sink.stopped && budget >= SINK_BUF {
            // SAFETY: libos32gui::init initialized os32api. buf is a private,
            // writable SINK_BUF-byte array and cap matches its true length.
            let rc =
                unsafe { (os32api::api().con_sink_read)(self.buf.as_mut_ptr(), SINK_BUF as u32) };
            if rc < 0 {
                /* 黙って諦めない: 理由を状態行に出す。読み手拒否だけは先客が
                 * 畳めば直るので次の周も試す (票 §3 A4)。
                 * 描き直すのは理由が**変わった**ときだけ — 同じ拒否のたびに
                 * 全面 invalidate すると 10Hz で他のアプリを待たせる。 */
                let first = self.sink.error != Some(rc);
                self.sink.error = Some(rc);
                self.sink.stopped = !sink::retryable(rc);
                return changed || first;
            }
            if rc == 0 {
                if self.sink.error.is_some() {
                    /* 読めるようになった (先客が終わった)。 */
                    self.sink.error = None;
                    changed = true;
                }
                break;
            }
            let n = (rc as usize).min(SINK_BUF);
            self.sink.error = None;
            self.sink.bytes += n as u64;
            budget -= n;
            changed = true;

            let mut it = sink::records(&self.buf[..n]);
            /* `it` は self.buf を、apply は self.session と self.sink を触る。
             * 借りる先が別のフィールドなので同時に持てる。 */
            for record in it.by_ref() {
                self.sink.records += 1;
                match self.session.apply(record) {
                    Ok(a) => {
                        self.sink.wraps += a.wraps;
                        self.sink.lost += a.lost;
                    }
                    Err(_) => {
                        /* モデルを作り直せない = 画面を持てない。以後読まない。 */
                        self.sink.stopped = true;
                        self.sink.error = Some(sink::ERR_INVAL);
                        break;
                    }
                }
            }
            if it.stop() != SinkStop::Done {
                self.sink.malformed += it.discarded() as u32;
            }
        }
        changed
    }

    /// 溜まり具合と取りこぼしを読む (所有権は要らない)。
    fn refresh_stat(&mut self) -> bool {
        let mut ring: u32 = 0;
        let mut dropped: u32 = 0;
        // SAFETY: initialized KAPI; both pointers are to local u32s.
        unsafe {
            (os32api::api().con_sink_stat)(&mut ring, &mut dropped);
        }
        /* 描き直しを迫るのは dropped が増えたときだけ (票 §2-2)。ring は
         * 読めていない間ずっと動くので、これで描き直すと 10Hz で回り続ける。
         * 値は毎周更新してあるので、次の Paint で最新が出る。 */
        let changed = dropped != self.sink.dropped;
        self.sink.ring = ring;
        self.sink.dropped = dropped;
        changed
    }
}

impl App for DisplayApp<'_> {
    fn on_timer(&mut self, ui: &mut Ui, _window: u32, id: u8) {
        if id != TIMER_SINK || ui.is_quitting() {
            return;
        }
        let mut changed = self.pump();
        /* dropped が増えたら状態行に出す (票 §2-2)。増えた周は必ず描き直す。 */
        changed |= self.refresh_stat();
        if !changed {
            return;
        }
        if self.follow {
            if let Some(layout) = self.layout() {
                self.session.move_top(Movement::Last, layout.rows());
            }
        }
        self.repaint(ui);
    }

    fn on_key(&mut self, ui: &mut Ui, _window: u32, scan: u8, ch: u8, _mods: u8, down: bool) {
        if down && scan == libos32gui::widget::SCAN_ESC {
            self.fail(ui);
            return;
        }
        if ui.is_quitting() {
            return;
        }
        match input::key(ch, down) {
            Action::Quit => self.fail(ui),
            /* fixture の切り替えは live では意味を持たない (受け取った出力を
             * 捨てることになる)。ホスト試験だけが Select を使う。 */
            Action::Select(_) => {}
            Action::Move(movement) => {
                if let Some(layout) = self.layout() {
                    self.follow = matches!(movement, Movement::Last);
                    self.session.move_top(movement, layout.rows());
                    self.repaint(ui);
                }
            }
            Action::None => {}
        }
    }

    fn on_close(&mut self, ui: &mut Ui, window: u32) {
        if self.window.as_ref().map(Window::id) == Some(window) {
            self.window = None;
            self.fail(ui);
        }
    }

    fn on_quit(&mut self, ui: &mut Ui, _reason: u8) {
        self.fail(ui);
    }

    fn on_paint(&mut self, _ui: &mut Ui, window: u32, surface: SurfaceId, rect: Rect) {
        let Some(owner) = self.window.as_ref().filter(|w| w.id() == window) else {
            return;
        };
        if owner.surface() != surface {
            return;
        }
        // libos32gui paint_damaged has installed a surface-specific base clip.
        // Intersect its current effective clip with this event; never install or
        // retain base clips ourselves. GUI coordinates are checked before calls.
        let clip = match pixels(rect).intersection(pixels(gapi::current_clip())) {
            Ok(Some(c)) => c,
            _ => return,
        };
        let (cw, ch) = owner.client_size();
        let Some(layout) = Layout::new(cw as i64, ch as i64) else {
            self.paint_error = true;
            let _ = owner.set_title(b"Terminal: client too small");
            return;
        };
        // This sink can only be constructed in this callback and never escapes.
        // paint::body preflights the full view/write extent before either trait
        // callback. hline itself takes i32; client/Rect coordinates fit i16.
        struct PaintSink {
            surface: SurfaceId,
        }
        impl Sink for PaintSink {
            fn run(&mut self, x: i32, y: i32, length: i32, foreground: bool) {
                let color = if foreground {
                    GUI_COLOR_TEXT
                } else {
                    GUI_COLOR_EDIT_BG
                };
                gapi::hline(self.surface, x, y, length, Style::pen(color));
            }
        }
        let mut sink = PaintSink { surface };
        match paint::body(
            self.session.display(),
            layout,
            clip,
            &mut GuestGlyphs,
            &mut sink,
        ) {
            Ok(stats) => {
                let lines = status::lines(
                    self.session.display(),
                    self.runs,
                    self.paint_error,
                    &self.sink,
                );
                let max_chars = ((cw as i64 - 2 * MARGIN) / CELL_WIDTH) as usize;
                for (row, line) in lines.iter().enumerate() {
                    let bytes = line.bytes();
                    gapi::text(
                        surface,
                        MARGIN as i32,
                        (MARGIN + row as i64 * CELL_HEIGHT) as i32,
                        &bytes[..bytes.len().min(max_chars)],
                        Style::new(GUI_COLOR_TEXT, GUI_COLOR_EDIT_BG),
                    );
                }
                self.runs = stats.runs;
            }
            Err(_) => {
                self.paint_error = true;
                let _ = owner.set_title(b"Terminal: PAINT RANGE/RENDER ERROR");
            }
        }
    }
}

extern "C" {
    // lib/utf8.h: u16 unicode_to_jis(u32 codepoint).
    // Final link MUST use lib/utf8_prog.o. It probes four known table entries
    // on first table use; do not call utf8_set_jis_table_ready(1).
    fn unicode_to_jis(codepoint: u32) -> u16;
}
struct GuestGlyphs;
impl Glyphs for GuestGlyphs {
    fn jis(&mut self, c: char) -> Option<u16> {
        // SAFETY: guest mapping and C ABI supplied by existing OS32 program path.
        boundary::valid_jis(unsafe { unicode_to_jis(c as u32) })
    }
    fn ank(&mut self, code: u8) -> [u8; 16] {
        let mut out = [0; 16];
        // SAFETY: libos32gui::init initialized os32api; writable 16-byte buffer
        // matches kapi_generated.rs. Void API cannot report missing glyphs.
        unsafe {
            (os32api::api().kcg_read_ank)(code, out.as_mut_ptr());
        }
        out
    }
    fn kanji(&mut self, code: u16) -> [u8; 32] {
        let mut out = [0; 32];
        if boundary::valid_jis(code).is_some() {
            // SAFETY: initialized KAPI, independently checked JIS bytes, writable
            // 32-byte buffer. [row*2]=left, [+1]=right; both MSB-left.
            unsafe {
                (os32api::api().kcg_read_kanji)(code, out.as_mut_ptr());
            }
        }
        // All-zero patterns are retained; the void API has no missing indicator.
        out
    }
}
