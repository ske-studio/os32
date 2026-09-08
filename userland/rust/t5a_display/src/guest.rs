//! Guest-only glue. Host tests never call KAPI or the shared GUI library.
use crate::{
    boundary,
    input::{self, Action},
    paint,
    session::Session,
    state::Fixture,
    status,
    storage::Storage,
    view::{Layout, MARGIN},
};
use libos32gui::gapi::{
    self,
    proto::{GUI_COLOR_EDIT_BG, GUI_COLOR_TEXT},
    types::{Rect, Style, SurfaceId},
};
use libos32gui::{App, GuiErr, GuiResult, Ui, Window, WindowSpec};
use libos32term_render::{Glyphs, Rect as PixelRect, Sink, CELL_HEIGHT, CELL_WIDTH};
use os32api::KernelAPI;

static STORAGE: Storage = Storage::new();

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
    let windows = match Windows::build() {
        Ok(w) => w,
        Err(e) => return e.code(),
    };
    let session = match Session::new(cells, Fixture::Normal) {
        Ok(s) => s,
        Err(_) => return GuiErr::INVAL.code(),
    };
    let mut app = DisplayApp {
        windows,
        session,
        runs: 0,
        paint_error: false,
    };
    match libos32gui::run(&mut app) {
        Ok(()) => 0,
        Err(e) => e.code(),
    }
}

struct Windows {
    main: Option<Window>,
    cover: Option<Window>,
}
impl Windows {
    fn build() -> GuiResult<Self> {
        let info = gapi::screen_info();
        let plan = boundary::windows(info.width as i64, info.height as i64).ok_or(GuiErr::INVAL)?;
        // Validate both conversions before the first window creation.
        let first = gui_rect(plan[0]).ok_or(GuiErr::INVAL)?;
        let second = gui_rect(plan[1]).ok_or(GuiErr::INVAL)?;
        let main = Window::create(&WindowSpec::new(b"T5a 1-5 fixture / j k g e / q", first))?;
        let cover = Window::create(&WindowSpec::new(b"T5a cover: drag me", second))?;
        // Cover starts in front, deliberately occluding the display. Either
        // window accepts keys. Ownership remains here across fixture switches.
        cover.set_focus()?;
        Ok(Self {
            main: Some(main),
            cover: Some(cover),
        })
    }
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
    windows: Windows,
    session: Session<'a>,
    runs: u64,
    paint_error: bool,
}
impl DisplayApp<'_> {
    fn layout(&self) -> Option<Layout> {
        let (w, h) = self.windows.main.as_ref()?.client_size();
        Layout::new(w as i64, h as i64)
    }
    fn fail(&mut self, ui: &mut Ui) {
        ui.quit();
    }
}
impl App for DisplayApp<'_> {
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
            Action::Select(fixture) => {
                if let Some(w) = self.windows.main.as_ref() {
                    if self.session.select(fixture).is_err() {
                        self.fail(ui);
                        return;
                    }
                    self.runs = 0;
                    self.paint_error = false;
                    if w.invalidate_all().is_err() {
                        self.fail(ui);
                    }
                }
            }
            Action::Move(movement) => {
                if let Some(layout) = self.layout() {
                    self.session.move_top(movement, layout.rows());
                    if let Some(w) = self.windows.main.as_ref() {
                        if w.invalidate_all().is_err() {
                            self.fail(ui);
                        }
                    }
                }
            }
            Action::None => {}
        }
    }
    fn on_close(&mut self, ui: &mut Ui, window: u32) {
        if self.windows.main.as_ref().map(Window::id) == Some(window) {
            self.windows.main = None;
        }
        if self.windows.cover.as_ref().map(Window::id) == Some(window) {
            self.windows.cover = None;
        }
        if self.windows.main.is_none() && self.windows.cover.is_none() {
            self.fail(ui);
        }
    }
    fn on_quit(&mut self, ui: &mut Ui, _reason: u8) {
        self.fail(ui);
    }
    fn on_paint(&mut self, _ui: &mut Ui, window: u32, surface: SurfaceId, rect: Rect) {
        let is_main = self.windows.main.as_ref().map(Window::id) == Some(window);
        let owner = if is_main {
            self.windows.main.as_ref()
        } else {
            self.windows.cover.as_ref().filter(|w| w.id() == window)
        };
        let Some(owner) = owner else {
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
        if !is_main {
            // Small ASCII help; GUI base clip and client surface clip remain active.
            if cw > 0 && ch as i64 >= CELL_HEIGHT {
                let text = b"1-5 fixture  q quit";
                let count = text.len().min(cw as usize / CELL_WIDTH as usize);
                gapi::text(
                    surface,
                    0,
                    0,
                    &text[..count],
                    Style::new(GUI_COLOR_TEXT, GUI_COLOR_EDIT_BG),
                );
            }
            return;
        }
        let Some(layout) = Layout::new(cw as i64, ch as i64) else {
            self.paint_error = true;
            let _ = owner.set_title(b"T5a: client too small");
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
                let lines = status::lines(self.session.display(), self.runs, self.paint_error);
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
                let _ = owner.set_title(b"T5a: PAINT RANGE/RENDER ERROR");
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
