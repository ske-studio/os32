//! T5b display-only: parent heap storage, scoped model, no exec/FD operations.
use libos32term::model::{Cell, BLANK};
const COLS: usize = 40;
const ROWS: usize = 64;
const CELLS: usize = COLS * ROWS * 2;
const BYTES: usize = CELLS * core::mem::size_of::<Cell>();
const _: () = assert!(core::mem::align_of::<Cell>() <= 4 && BYTES <= u32::MAX as usize);

/// Private allocator boundary.
/// # Safety
/// alloc returns null or BYTES writable bytes aligned for Cell; free accepts
/// exactly that allocation once, in the same restored parent heap context.
unsafe trait Heap {
    fn alloc(&mut self, bytes: usize) -> *mut Cell;
    unsafe fn free(&mut self, ptr: *mut Cell);
}
fn with_cells<R>(heap: &mut impl Heap, f: impl FnOnce(&mut [Cell]) -> R) -> Option<R> {
    let p = heap.alloc(BYTES);
    if p.is_null() {
        return None;
    }
    // Heap guarantees alignment and BYTES writable bytes. Initialize enum/char
    // values BEFORE making a slice; zeroed bytes are not a Cell constructor.
    unsafe {
        for i in 0..CELLS {
            p.add(i).write(BLANK);
        }
        let result = f(core::slice::from_raw_parts_mut(p, CELLS));
        // HRTB closure cannot return a borrow of the cells. Every Terminal has
        // ended before this free. Guest panic=abort never resumes this scope.
        heap.free(p);
        Some(result)
    }
}

use libos32term::{
    model::{Error, Model},
    stream::Terminal,
};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fixture {
    Normal,
    Exact,
    Full,
    TooWide,
    Finish,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Stop {
    Complete,
    Feed(Error),
    Finish(Error),
}
struct Display<'a> {
    term: Terminal<'a>,
    top: usize,
    consumed: usize,
    total: usize,
    stop: Stop,
}
impl<'a> Display<'a> {
    fn load(cells: &'a mut [Cell], fixture: Fixture, stderr: bool) -> Self {
        let (cols, rows) = if fixture == Fixture::TooWide {
            (1, 1)
        } else {
            (COLS, ROWS)
        };
        let mut s = Self {
            term: Terminal::new(Model::new(cells, cols, rows, libos32term_render::width).unwrap()),
            top: 0,
            consumed: 0,
            total: 0,
            stop: Stop::Complete,
        };
        if fixture == Fixture::TooWide {
            s.feed("日!".as_bytes());
        } else if fixture != Fixture::Normal {
            for _ in 0..COLS * ROWS {
                s.feed(b"A");
            }
            if fixture == Fixture::Full {
                s.feed(b"XY");
            }
            if fixture == Fixture::Finish {
                s.feed(b"\xe3\x81");
            }
        } else if stderr {
            s.feed(b"stderr fixture (not captured)\n");
        } else {
            s.feed("ASCII 0123\n日本語 あいう\nＡＢ１２！\nｱｲｳ ¥\nNUL:\0 ESC:\u{1b}\n".as_bytes());
            s.feed(b"invalid:\xff\nTAB:a\tb\nBS:ab\x08Z\nCR:old\rNEW\n");
            for _ in 0..40 {
                s.feed(b".\r\n");
            }
        }
        if s.stop == Stop::Complete {
            if let Some(e) = s.term.finish().error {
                s.stop = Stop::Finish(e);
            }
        }
        s
    }
    fn feed(&mut self, bytes: &[u8]) {
        self.total += bytes.len();
        if self.stop != Stop::Complete {
            return;
        }
        let r = self.term.feed(bytes);
        self.consumed += r.consumed;
        if let Some(e) = r.error {
            self.stop = Stop::Feed(e);
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Request {
    Open,
    Close,
    Select(Fixture),
    Tab,
    First,
    Up,
    Down,
    Last,
}
struct Panel<'a> {
    streams: [Display<'a>; 2],
    tab: usize,
}
struct Registry {
    ptr: core::cell::Cell<*mut ()>,
    borrowed: core::cell::Cell<bool>,
    driving: core::cell::Cell<bool>,
    pending: core::cell::Cell<Option<Request>>,
}
// Single CPU, synchronous WM callbacks only; IRQ handlers never access this.
unsafe impl Sync for Registry {}
static REGISTRY: Registry = Registry {
    ptr: core::cell::Cell::new(core::ptr::null_mut()),
    borrowed: core::cell::Cell::new(false),
    driving: core::cell::Cell::new(false),
    pending: core::cell::Cell::new(None),
};
fn request(r: Request) -> bool {
    if REGISTRY.pending.get().is_some() {
        return false;
    }
    REGISTRY.pending.set(Some(r));
    true
}
struct BorrowGuard;
impl Drop for BorrowGuard {
    fn drop(&mut self) {
        REGISTRY.borrowed.set(false);
    }
}
fn read<R>(f: impl FnOnce(&Panel<'_>) -> R) -> Option<R> {
    let p = REGISTRY.ptr.get();
    if p.is_null() || REGISTRY.borrowed.replace(true) {
        return None;
    }
    let _guard = BorrowGuard;
    // scoped() publishes only a live stack Panel. No reference escapes this
    // HRTB callback. The guard excludes recursive glyph/KAPI redraw readers
    // and every writer. No static Terminal lifetime is constructed.
    Some(f(unsafe { &*p.cast::<Panel<'_>>() }))
}
fn scoped<R>(panel: &mut Panel<'_>, f: impl FnOnce() -> R) -> R {
    assert!(REGISTRY.ptr.get().is_null());
    REGISTRY.ptr.set((panel as *mut Panel<'_>).cast());
    struct Clear;
    impl Drop for Clear {
        fn drop(&mut self) {
            REGISTRY.ptr.set(core::ptr::null_mut());
        }
    }
    let _clear = Clear;
    f()
}
fn control(action: Request, rows: usize) {
    assert!(!REGISTRY.borrowed.replace(true));
    let _guard = BorrowGuard;
    let p = unsafe { &mut *REGISTRY.ptr.get().cast::<Panel<'_>>() };
    let s = &mut p.streams[p.tab];
    let last = s
        .term
        .model()
        .state()
        .retained_rows
        .end
        .saturating_sub(rows.max(1));
    match action {
        Request::Last => s.top = last,
        Request::First => s.top = 0,
        Request::Up => s.top = s.top.saturating_sub(1).min(last),
        Request::Down => s.top = s.top.saturating_add(1).min(last),
        Request::Tab => p.tab = 1 - p.tab,
        _ => {}
    }
}
trait Driver {
    fn step(&mut self) -> bool;
    fn changed(&mut self);
    fn refused(&mut self);
    fn rows(&self) -> usize;
}
fn drive(heap: &mut impl Heap, driver: &mut impl Driver) {
    // Top-level scope remains claimed during synchronous external execution,
    // even when no panel/GUI slot exists. Recursive entry cannot allocate,
    // consume pending input, or replace the published model.
    if REGISTRY.driving.replace(true) {
        return;
    }
    struct Driving;
    impl Drop for Driving {
        fn drop(&mut self) {
            REGISTRY.driving.set(false);
        }
    }
    let _driving = Driving;
    loop {
        if REGISTRY.pending.take() == Some(Request::Open) {
            let keep = if driver.rows() == 0 {
                None
            } else {
                with_cells(heap, |cells| {
                    let mut fixture = Fixture::Normal;
                    loop {
                        // Previous Panel/Terminals have ended before cells are
                        // reborrowed for a new fixture. No self-referential struct.
                        let (a, b) = cells.split_at_mut(COLS * ROWS);
                        let mut panel = Panel {
                            streams: [
                                Display::load(a, fixture, false),
                                Display::load(b, fixture, true),
                            ],
                            tab: 0,
                        };
                        let (keep, next) = scoped(&mut panel, || {
                            driver.changed();
                            loop {
                                match REGISTRY.pending.take() {
                                    Some(Request::Close) => break (true, None),
                                    Some(Request::Select(f)) => break (true, Some(f)),
                                    Some(action) => {
                                        control(action, driver.rows());
                                        driver.changed();
                                    }
                                    None => {}
                                }
                                if !driver.step() {
                                    break (false, None);
                                }
                            }
                        });
                        if let Some(f) = next {
                            fixture = f;
                            continue;
                        }
                        driver.changed();
                        break keep;
                    }
                })
            };
            match keep {
                Some(false) => return,
                None => driver.refused(),
                _ => {}
            }
        }
        if !driver.step() {
            return;
        }
    }
}

use libos32term_render::{Glyphs, Rect as PixelRect, Sink, View};
const MARGIN: i64 = 8;
const PANEL_X: i64 = 16;
const PANEL_Y: i64 = 16;
const PANEL_W: i64 = COLS as i64 * 8 + MARGIN * 2;
const HEADER_H: i64 = 80;
const PANEL_MAX_H: i64 = 368;
#[derive(Clone, Copy)]
struct Layout {
    panel: PixelRect,
    body: PixelRect,
    rows: usize,
}
fn layout(w: i64, h: i64, bar: i64) -> Option<Layout> {
    if w < PANEL_X + PANEL_W + MARGIN
        || h < 0
        || bar < 0
        || w > i16::MAX as i64
        || h > i16::MAX as i64
    {
        return None;
    }
    let height = h
        .checked_sub(bar)?
        .checked_sub(PANEL_Y * 2)?
        .min(PANEL_MAX_H);
    if height < HEADER_H + MARGIN * 2 + 16 {
        return None;
    }
    let panel = PixelRect {
        x0: PANEL_X,
        y0: PANEL_Y,
        x1: PANEL_X + PANEL_W,
        y1: PANEL_Y + height,
    };
    let rows = ((height - HEADER_H - MARGIN * 2) / 16) as usize;
    let body = PixelRect {
        x0: panel.x0 + MARGIN,
        y0: panel.y0 + MARGIN + HEADER_H,
        x1: panel.x1 - MARGIN,
        y1: panel.y0 + MARGIN + HEADER_H + rows as i64 * 16,
    };
    Some(Layout { panel, body, rows })
}
fn paint(
    p: &Panel<'_>,
    l: Layout,
    clip: PixelRect,
    holes: &[PixelRect],
    g: &mut impl Glyphs,
    s: &mut impl Sink,
) -> bool {
    if p.tab >= p.streams.len()
        || l.rows == 0
        || p.streams[p.tab].top.checked_add(l.rows).is_none()
        || p.streams[p.tab].top > p.streams[p.tab].term.model().state().retained_rows.end
    {
        return false;
    }
    // Preflight the complete write plan, not just the first surviving piece.
    for r in [l.panel, l.body, clip].iter().chain(holes.iter()) {
        if r.x1.checked_sub(r.x0).is_none()
            || r.y1.checked_sub(r.y0).is_none()
            || r.x0 > r.x1
            || r.y0 > r.y1
        {
            return false;
        }
    }
    for r in [l.panel, l.body] {
        if r.x0 < i32::MIN as i64
            || r.y0 < i32::MIN as i64
            || r.x1 > i32::MAX as i64
            || r.y1 > i32::MAX as i64
        {
            return false;
        }
    }
    let Some(regions) = regions(l.panel, clip, holes) else {
        return false;
    };
    let lines = labels(p);
    for r in regions.as_slice() {
        for y in r.y0..r.y1 {
            s.run(r.x0 as i32, y as i32, (r.x1 - r.x0) as i32, false);
        }
        for (row, line) in lines.iter().enumerate() {
            label(
                line,
                l.panel.x0 + MARGIN,
                l.panel.y0 + MARGIN + row as i64 * 16,
                *r,
                g,
                s,
            );
        }
        if let Ok(Some(body)) = r.intersection(l.body) {
            let view = View {
                top: p.streams[p.tab].top,
                height: l.rows,
                origin: (l.body.x0, l.body.y0),
                clip: body,
            };
            if libos32term_render::render(p.streams[p.tab].term.model(), view, g, s).is_err() {
                return false;
            }
        }
    }
    true
}
const MAX_REGIONS: usize = 16;
#[derive(Clone, Copy)]
struct Regions {
    rects: [PixelRect; MAX_REGIONS],
    len: usize,
}
impl Regions {
    const EMPTY: Self = Self {
        rects: [PixelRect {
            x0: 0,
            y0: 0,
            x1: 0,
            y1: 0,
        }; MAX_REGIONS],
        len: 0,
    };
    fn push(&mut self, r: PixelRect) -> Option<()> {
        if r.x0 >= r.x1 || r.y0 >= r.y1 {
            return Some(());
        }
        if self.len == MAX_REGIONS {
            return None;
        }
        self.rects[self.len] = r;
        self.len += 1;
        Some(())
    }
    fn as_slice(&self) -> &[PixelRect] {
        &self.rects[..self.len]
    }
}
fn regions(panel: PixelRect, clip: PixelRect, holes: &[PixelRect]) -> Option<Regions> {
    let mut result = Regions::EMPTY;
    if let Some(r) = panel.intersection(clip).ok()? {
        result.push(r)?;
    }
    for hole in holes {
        let mut next = Regions::EMPTY;
        for a in result.as_slice() {
            if let Some(c) = a.intersection(*hole).ok()? {
                next.push(PixelRect {
                    x0: a.x0,
                    y0: a.y0,
                    x1: a.x1,
                    y1: c.y0,
                })?;
                next.push(PixelRect {
                    x0: a.x0,
                    y0: c.y1,
                    x1: a.x1,
                    y1: a.y1,
                })?;
                next.push(PixelRect {
                    x0: a.x0,
                    y0: c.y0,
                    x1: c.x0,
                    y1: c.y1,
                })?;
                next.push(PixelRect {
                    x0: c.x1,
                    y0: c.y0,
                    x1: a.x1,
                    y1: c.y1,
                })?;
            } else {
                next.push(*a)?;
            }
        }
        result = next;
    }
    Some(result)
}

#[derive(Clone, Copy)]
struct Line {
    bytes: [u8; 80],
    len: usize,
}
impl Line {
    const EMPTY: Self = Self {
        bytes: [0; 80],
        len: 0,
    };
}
impl core::fmt::Write for Line {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        if self.len + s.len() > self.bytes.len() {
            return Err(core::fmt::Error);
        }
        self.bytes[self.len..self.len + s.len()].copy_from_slice(s.as_bytes());
        self.len += s.len();
        Ok(())
    }
}
fn error_name(e: Option<Error>) -> &'static str {
    match e {
        None => "none",
        Some(Error::Full) => "Full",
        Some(Error::TooWide) => "TooWide",
        _ => "error",
    }
}
fn labels(p: &Panel<'_>) -> [Line; 5] {
    use core::fmt::Write;
    let mut lines = [Line::EMPTY; 5];
    let s = &p.streams[p.tab];
    let _ = lines[0].write_str(if p.tab == 0 {
        "stdout* stderr   fixture   Close"
    } else {
        "stdout  stderr*  fixture   Close"
    });
    let _ = lines[1].write_str("1Norm  2Exact 3Full  4Wide  5Finish");
    let _ = lines[2].write_str("First     Up        Down      Last");
    let pending = s.term.pending().iter().filter(|c| c.is_some()).count();
    let _ = write!(
        lines[3],
        "in {}/{} top {} pending {}",
        s.consumed, s.total, s.top, pending
    );
    let (phase, error) = match s.stop {
        Stop::Complete => ("Done", None),
        Stop::Feed(e) => ("Feed", Some(e)),
        Stop::Finish(e) => ("Finish", Some(e)),
    };
    let _ = write!(
        lines[4],
        "{} {} tail {} limit {}",
        phase,
        error_name(error),
        s.total - s.consumed,
        error_name(s.term.model().state().limit)
    );
    lines
}
fn label(line: &Line, x: i64, y: i64, clip: PixelRect, g: &mut impl Glyphs, s: &mut impl Sink) {
    for (i, ch) in line.bytes[..line.len.min(COLS)].iter().enumerate() {
        let x = x + i as i64 * 8;
        if x >= clip.x1 || x + 8 <= clip.x0 || y >= clip.y1 || y + 16 <= clip.y0 {
            continue;
        }
        let bits = g.ank(*ch);
        for yy in y.max(clip.y0)..(y + 16).min(clip.y1) {
            for xx in x.max(clip.x0)..(x + 8).min(clip.x1) {
                if bits[(yy - y) as usize] & (0x80 >> (xx - x)) != 0 {
                    s.run(xx as i32, yy as i32, 1, true);
                }
            }
        }
    }
}
#[derive(Clone, Copy)]
struct PointerGate {
    held: u8,
}
impl PointerGate {
    fn sample(&mut self, hit: bool, buttons: u8, previous: u8) -> bool {
        if hit {
            self.held |= buttons & !previous;
        }
        let consume = hit || self.held != 0;
        self.held &= buttons;
        consume
    }
}
#[cfg(not(t5b_core_test))]
pub(crate) use guest::*;
#[cfg(not(t5b_core_test))]
mod guest {
    use super::*;
    use crate::wm::{GuiState, Rect};
    fn plan(st: &GuiState) -> Option<Layout> {
        layout(
            st.screen_w as i64,
            st.screen_h as i64,
            crate::taskbar::TASKBAR_H as i64,
        )
    }
    fn wm_rect(r: PixelRect) -> Rect {
        Rect::new(
            r.x0 as i32,
            r.y0 as i32,
            (r.x1 - r.x0) as i32,
            (r.y1 - r.y0) as i32,
        )
    }
    pub fn rect(st: &GuiState) -> Rect {
        if REGISTRY.ptr.get().is_null() {
            return Rect::EMPTY;
        }
        plan(st).map(|l| wm_rect(l.panel)).unwrap_or(Rect::EMPTY)
    }
    pub fn request_open() -> bool {
        request(Request::Open)
    }
    struct ParentHeap;
    // Existing mem_alloc is 4-byte aligned; Cell alignment is asserted below.
    unsafe impl Heap for ParentHeap {
        fn alloc(&mut self, n: usize) -> *mut Cell {
            unsafe { (os32api::api().mem_alloc)(n as u32).cast() }
        }
        unsafe fn free(&mut self, p: *mut Cell) {
            (os32api::api().mem_free)(p.cast());
        }
    }
    /// Only main's outer loop calls this. step may synchronously execute a child;
    /// no Terminal borrow/allocator operation is held across step. Child X3
    /// redraws use read(), X4 input only changes independent private flags.
    pub fn run(st: &mut GuiState, step: impl FnMut(&mut GuiState) -> bool) {
        struct Desktop<'a, F> {
            st: &'a mut GuiState,
            step: F,
        }
        impl<F: FnMut(&mut GuiState) -> bool> Driver for Desktop<'_, F> {
            fn step(&mut self) -> bool {
                (self.step)(self.st)
            }
            fn rows(&self) -> usize {
                plan(self.st).map(|l| l.rows).unwrap_or(0)
            }
            fn changed(&mut self) {
                if let Some(l) = plan(self.st) {
                    let r = wm_rect(l.panel);
                    self.st.dirty_screen(r);
                    // On close, force underlay dirty even if exposure fragments
                    // overflow; the existing dirty paging remains fail-closed.
                    if REGISTRY.ptr.get().is_null() {
                        for w in &mut self.st.windows {
                            if w.used && w.visible {
                                let (x, y) = w.client_origin();
                                crate::damage::add_dirty(w, r.translate(-x, -y));
                            }
                        }
                    }
                    crate::visible::recompute_and_expose(self.st);
                }
            }
            fn refused(&mut self) {
                crate::modal::open_wm_message(
                    self.st,
                    os32api::gui::proto::GUI_MODAL_OK,
                    b"Display refused: geometry or parent heap\0",
                    crate::modal::WM_PURPOSE_NOTIFY,
                );
            }
        }
        drive(&mut ParentHeap, &mut Desktop { st, step });
    }
    fn pixels(r: Rect) -> PixelRect {
        PixelRect {
            x0: r.x as i64,
            y0: r.y as i64,
            x1: r.x as i64 + r.w as i64,
            y1: r.y as i64 + r.h as i64,
        }
    }
    struct GuestGlyphs;
    impl Glyphs for GuestGlyphs {
        fn jis(&mut self, c: char) -> Option<u16> {
            let j = unsafe { crate::ffi::unicode_to_jis(c as u32) };
            if (0x21..=0x7e).contains(&(j >> 8)) && (0x21..=0x7e).contains(&(j & 255)) {
                Some(j)
            } else {
                None
            }
        }
        fn ank(&mut self, c: u8) -> [u8; 16] {
            let mut b = [0; 16];
            unsafe {
                (os32api::api().kcg_read_ank)(c, b.as_mut_ptr());
            }
            b
        }
        fn kanji(&mut self, c: u16) -> [u8; 32] {
            let mut b = [0; 32];
            unsafe {
                (os32api::api().kcg_read_kanji)(c, b.as_mut_ptr());
            }
            b
        }
    }
    struct WmSink;
    impl Sink for WmSink {
        fn run(&mut self, x: i32, y: i32, n: i32, fg: bool) {
            let color = if fg {
                os32api::gui::proto::GUI_COLOR_TEXT
            } else {
                os32api::gui::proto::GUI_COLOR_WINDOW
            };
            unsafe {
                os32api::gfx::gfx_hline(x, y, n, color);
            }
        }
    }
    /// Saved-model read only. Only the non-X4 compositor calls this. The
    /// read guard remains claimed through every font/sink KAPI callback.
    pub fn draw(st: &GuiState, clip: Rect) {
        if st.in_pump {
            return;
        }
        let Some(l) = plan(st) else {
            return;
        };
        let cursor = if st.cursor.shown {
            crate::cursor::rect(st)
        } else {
            Rect::EMPTY
        };
        let holes = [
            pixels(crate::modal::rect()),
            pixels(crate::taskbar::rect(st)),
            pixels(crate::startmenu::rect()),
            pixels(crate::fep::rect()),
            pixels(cursor),
        ];
        let _ = read(|p| paint(p, l, pixels(clip), &holes, &mut GuestGlyphs, &mut WmSink));
    }
    struct GateCell(core::cell::Cell<PointerGate>);
    unsafe impl Sync for GateCell {}
    static GATE: GateCell = GateCell(core::cell::Cell::new(PointerGate { held: 0 }));
    /// Upper UI bypasses delivery, not physical release bookkeeping. Never
    /// acquire capture here or advance the WM's deferred button-edge state.
    pub fn release_buttons(buttons: u8) {
        let mut gate = GATE.0.get();
        gate.held &= buttons;
        GATE.0.set(gate);
    }
    pub fn mouse(st: &GuiState, mx: i32, my: i32, buttons: u8, previous: u8) -> bool {
        // Upper WM UI keeps priority. An ongoing WM window drag must reach
        // its drop path, even when its outline crosses the resident panel.
        if crate::startmenu::is_open() || crate::taskbar::hit(st, mx, my) || st.drag_index >= 0 {
            release_buttons(buttons);
            return false;
        }
        let hit = rect(st).contains(mx, my) && !crate::fep::rect().contains(mx, my);
        if hit && buttons & 1 != 0 && previous & 1 == 0 {
            let x = mx as i64 - PANEL_X - MARGIN;
            let y = my as i64 - PANEL_Y - MARGIN;
            if (16..32).contains(&y) && (0..35 * 8).contains(&x) {
                const FIXTURES: [Fixture; 5] = [
                    Fixture::Normal,
                    Fixture::Exact,
                    Fixture::Full,
                    Fixture::TooWide,
                    Fixture::Finish,
                ];
                request(Request::Select(FIXTURES[(x / (7 * 8)) as usize]));
            } else if (32..48).contains(&y) && (0..40 * 8).contains(&x) {
                const MOVES: [Request; 4] =
                    [Request::First, Request::Up, Request::Down, Request::Last];
                request(MOVES[(x / (10 * 8)) as usize]);
            } else if (0..16).contains(&y) {
                if x >= 27 * 8 {
                    request(Request::Close);
                } else if (0..14 * 8).contains(&x) {
                    let tab = if x < 8 * 8 { 0 } else { 1 };
                    if read(|p| p.tab != tab) == Some(true) {
                        request(Request::Tab);
                    }
                }
            }
        }
        let mut gate = GATE.0.get();
        let consume = gate.sample(hit, buttons, previous);
        GATE.0.set(gate);
        consume
    }
}
#[cfg(test)]
#[path = "../host/terminal_tests.rs"]
mod tests;
