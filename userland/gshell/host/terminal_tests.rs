use super::*;
#[test]
fn normal_fixture_streams_are_independent_and_saved() {
    let mut cells = vec![BLANK; CELLS];
    let (a, b) = cells.split_at_mut(COLS * ROWS);
    let out = Display::load(a, Fixture::Normal, false);
    let err = Display::load(b, Fixture::Normal, true);
    assert_eq!(out.term.model().cells()[COLS], Cell::Wide('日'));
    assert_eq!(err.term.model().cells()[0], Cell::Single('s'));
    assert_eq!(out.term.model().state().retained_rows.end, 50);
    assert_eq!(out.consumed, out.total);
    assert_eq!(out.stop, Stop::Complete);
    assert_eq!(out.term.pending(), [None; 2]);
}
#[test]
fn boundary_fixtures_report_stop_pending_and_unconsumed_separately() {
    let mut cells = vec![BLANK; CELLS];
    let mut got = Vec::new();
    for f in [
        Fixture::Exact,
        Fixture::Full,
        Fixture::TooWide,
        Fixture::Finish,
    ] {
        let s = Display::load(&mut cells, f, false);
        got.push((s.stop, s.consumed, s.total, s.term.pending()));
    }
    assert_eq!(
        got,
        vec![
            (Stop::Complete, 2560, 2560, [None; 2]),
            (Stop::Feed(Error::Full), 2561, 2562, [Some('X'), None]),
            (Stop::Feed(Error::TooWide), 3, 4, [Some('日'), None]),
            (
                Stop::Finish(Error::Full),
                2562,
                2562,
                [Some('\u{fffd}'), None]
            ),
        ]
    );
    let s = Display::load(&mut cells, Fixture::Normal, false);
    assert_eq!(s.term.pending(), [None; 2]);
    assert_eq!(s.term.model().state().limit, None);
    assert_eq!(s.top, 0);
    assert!(s.term.model().cells()[50 * COLS..]
        .iter()
        .all(|c| *c == BLANK));
}
#[test]
fn top_level_retains_across_child_callbacks_and_cui_failure_then_drops() {
    REGISTRY.pending.set(None);
    struct D {
        step: usize,
        changes: usize,
    }
    impl Driver for D {
        fn rows(&self) -> usize {
            10
        }
        fn changed(&mut self) {
            self.changes += 1;
        }
        fn refused(&mut self) {
            panic!("unexpected refusal");
        }
        fn step(&mut self) -> bool {
            self.step += 1;
            match self.step {
                1 => {
                    assert!(request(Request::Open));
                }
                2 => {
                    let saved = read(|p| p.streams[0].term.model().cells().to_vec());
                    assert!(saved.is_some());
                    // Synchronous child: no GUI slot needed. Requests do not
                    // mutate/free while this top-level step is suspended.
                    assert!(request(Request::Last));
                    assert!(!request(Request::Close));
                    assert_eq!(read(|p| p.streams[0].top), Some(0));
                    assert_eq!(read(|p| p.streams[0].term.model().cells().to_vec()), saved);
                    assert_eq!(read(|_| read(|_| 1)), Some(None));
                }
                3 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(40));
                    // Failed CUI handoff returns true: same saved model.
                }
                4 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(40));
                    return false;
                }
                _ => panic!("extra step"),
            }
            true
        }
    }
    let mut d = D {
        step: 0,
        changes: 0,
    };
    let mut h = TestHeap {
        fail: false,
        allocs: 0,
        frees: 0,
    };
    drive(&mut h, &mut d);
    assert_eq!(d.step, 4);
    assert_eq!((h.allocs, h.frees), (1, 1));
    assert_eq!(read(|_| 1), None);
    assert_eq!(d.changes, 3);
}
#[test]
fn controls_reset_scopes_close_reopen_and_refusal_is_not_retried() {
    REGISTRY.pending.set(None);
    struct D {
        n: usize,
    }
    impl Driver for D {
        fn rows(&self) -> usize {
            10
        }
        fn changed(&mut self) {}
        fn refused(&mut self) {
            panic!("unexpected");
        }
        fn step(&mut self) -> bool {
            self.n += 1;
            let action = match self.n {
                1 => Request::Open,
                2 => Request::Down,
                3 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(1));
                    Request::Last
                }
                4 => Request::Up,
                5 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(39));
                    Request::Tab
                }
                6 => {
                    assert_eq!(read(|p| p.streams[p.tab].top), Some(0));
                    Request::Tab
                }
                7 => Request::First,
                8 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(0));
                    Request::Select(Fixture::Finish)
                }
                9 => {
                    assert_eq!(read(|p| p.streams[0].stop), Some(Stop::Finish(Error::Full)));
                    Request::Select(Fixture::Normal)
                }
                10 => {
                    assert_eq!(read(|p| p.streams[0].term.pending()), Some([None; 2]));
                    Request::Close
                }
                11 => {
                    assert_eq!(read(|_| 1), None);
                    Request::Open
                }
                12 => {
                    assert_eq!(read(|p| p.streams[0].top), Some(0));
                    return false;
                }
                _ => panic!("unbounded loop"),
            };
            assert!(request(action));
            true
        }
    }
    let mut h = TestHeap {
        fail: false,
        allocs: 0,
        frees: 0,
    };
    drive(&mut h, &mut D { n: 0 });
    assert_eq!((h.allocs, h.frees), (2, 2));
    struct Fail {
        n: usize,
        refusals: usize,
    }
    impl Driver for Fail {
        fn rows(&self) -> usize {
            10
        }
        fn changed(&mut self) {
            panic!("visible on allocation failure");
        }
        fn refused(&mut self) {
            self.refusals += 1;
        }
        fn step(&mut self) -> bool {
            self.n += 1;
            if self.n == 1 {
                request(Request::Open);
            }
            self.n < 4
        }
    }
    h.fail = true;
    let mut d = Fail { n: 0, refusals: 0 };
    drive(&mut h, &mut d);
    assert_eq!(d.refusals, 1);
    assert_eq!((h.allocs, h.frees), (3, 2));
}
#[derive(Default)]
struct Font {
    reads: usize,
}
impl Glyphs for Font {
    fn jis(&mut self, _: char) -> Option<u16> {
        Some(0x467c)
    }
    fn ank(&mut self, _: u8) -> [u8; 16] {
        self.reads += 1;
        [0xa5; 16]
    }
    fn kanji(&mut self, _: u16) -> [u8; 32] {
        self.reads += 1;
        [0xa5; 32]
    }
}
#[derive(Default)]
struct Canvas {
    pixels: std::collections::BTreeMap<(i32, i32), bool>,
}
impl Sink for Canvas {
    fn run(&mut self, x: i32, y: i32, n: i32, f: bool) {
        assert!(n > 0);
        for xx in x..x + n {
            self.pixels.insert((xx, y), f);
        }
    }
}
#[test]
fn saved_panel_clip_holes_and_half_japanese_reexposure() {
    let mut cells = vec![BLANK; CELLS];
    let (a, b) = cells.split_at_mut(COLS * ROWS);
    let mut p = Panel {
        streams: [
            Display::load(a, Fixture::Normal, false),
            Display::load(b, Fixture::Normal, true),
        ],
        tab: 0,
    };
    p.streams[0].top = 1;
    let l = layout(640, 400, 24);
    assert!(l.is_some());
    let l = l.unwrap();
    let mut g = Font::default();
    let mut full = Canvas::default();
    assert!(paint(&p, l, l.panel, &[], &mut g, &mut full));
    assert!(!full.pixels.is_empty());
    let before = p.streams[0].term.model().cells().to_vec();
    for cut in [
        PixelRect {
            x0: 24,
            y0: 104,
            x1: 32,
            y1: 120,
        },
        PixelRect {
            x0: 32,
            y0: 104,
            x1: 40,
            y1: 120,
        },
        PixelRect {
            x0: 25,
            y0: 105,
            x1: 39,
            y1: 106,
        },
        PixelRect {
            x0: 24,
            y0: 119,
            x1: 40,
            y1: 120,
        },
    ] {
        let holes = [PixelRect {
            x0: 29,
            y0: 108,
            x1: 35,
            y1: 115,
        }];
        let mut partial = Canvas::default();
        assert!(paint(&p, l, cut, &holes, &mut g, &mut partial));
        let expected = full
            .pixels
            .iter()
            .filter(|((x, y), _)| {
                let (x, y) = (*x as i64, *y as i64);
                x >= cut.x0
                    && x < cut.x1
                    && y >= cut.y0
                    && y < cut.y1
                    && !(x >= 29 && x < 35 && y >= 108 && y < 115)
            })
            .map(|(k, v)| (*k, *v))
            .collect();
        assert_eq!(partial.pixels, expected);
    }
    assert_eq!(p.streams[0].term.model().cells(), before);
}
#[test]
fn pointer_and_both_edges_do_not_leak_after_leaving_or_closing_panel() {
    let mut gate = PointerGate { held: 0 };
    assert!(gate.sample(true, 0, 0)); // hover, X3 or X4
    assert!(gate.sample(true, 3, 0)); // both downs
    assert!(gate.sample(false, 3, 3)); // drag outside
    assert!(gate.sample(false, 2, 3)); // left up outside
    assert!(gate.sample(false, 0, 2)); // right up after close
    assert!(!gate.sample(false, 0, 0));
    assert!(!gate.sample(false, 1, 0)); // app-owned down
    assert!(gate.sample(true, 0, 1)); // up over panel never goes below
}
#[test]
fn invalid_view_and_region_overflow_fail_before_any_write_or_glyph() {
    let mut cells = vec![BLANK; CELLS];
    let (a, b) = cells.split_at_mut(COLS * ROWS);
    let mut p = Panel {
        streams: [
            Display::load(a, Fixture::Normal, false),
            Display::load(b, Fixture::Normal, true),
        ],
        tab: 0,
    };
    let l = layout(640, 480, 24).unwrap();
    let mut g = Font::default();
    let mut c = Canvas::default();
    p.streams[0].top = usize::MAX;
    assert!(!paint(&p, l, l.panel, &[], &mut g, &mut c));
    assert!(c.pixels.is_empty());
    assert_eq!(g.reads, 0);
    p.streams[0].top = 0;
    let holes: Vec<_> = (0..20)
        .map(|n| PixelRect {
            x0: 30 + n * 12,
            y0: 120,
            x1: 34 + n * 12,
            y1: 180,
        })
        .collect();
    assert!(!paint(&p, l, l.panel, &holes, &mut g, &mut c));
    assert!(c.pixels.is_empty());
    assert_eq!(g.reads, 0);
    for (w, h, b) in [
        (i64::MAX, 400, 24),
        (640, i64::MAX, 24),
        (0, 400, 24),
        (640, 400, i64::MIN),
    ] {
        assert!(layout(w, h, b).is_none());
    }
    let invalid = PixelRect {
        x0: i64::MIN,
        y0: 0,
        x1: i64::MAX,
        y1: 10,
    };
    assert!(!paint(&p, l, invalid, &[], &mut g, &mut c));
    assert!(c.pixels.is_empty());
    assert_eq!(g.reads, 0);
}
#[test]
fn headers_show_controls_and_stop_outside_body() {
    let mut cells = vec![BLANK; CELLS];
    let (a, b) = cells.split_at_mut(COLS * ROWS);
    let p = Panel {
        streams: [
            Display::load(a, Fixture::Full, false),
            Display::load(b, Fixture::Normal, true),
        ],
        tab: 0,
    };
    let lines = labels(&p);
    let text: Vec<_> = lines
        .iter()
        .map(|l| core::str::from_utf8(&l.bytes[..l.len]).unwrap())
        .collect();
    assert_eq!(text[0], "stdout* stderr   fixture   Close");
    assert_eq!(text[1], "1Norm  2Exact 3Full  4Wide  5Finish");
    assert_eq!(text[2], "First     Up        Down      Last");
    assert_eq!(text[3], "in 2561/2562 top 0 pending 1");
    assert_eq!(text[4], "Feed Full tail 1 limit Full");
    let l = layout(640, 400, 24).unwrap();
    let mut g = Font::default();
    let mut c = Canvas::default();
    assert!(paint(&p, l, l.panel, &[], &mut g, &mut c));
    assert!(c
        .pixels
        .iter()
        .any(|((_, y), f)| *y < l.body.y0 as i32 && *f));
}
#[cfg(not(t5b_core_test))]
#[test]
fn guest_top_level_open_close_uses_parent_heap_and_invalidates() {
    use crate::{mocks, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    st.screen_w = 640;
    st.screen_h = 400;
    let before = mocks::FREES.load(Ordering::SeqCst);
    assert!(request_open());
    let mut n = 0;
    run(&mut st, |st| {
        n += 1;
        if n == 1 {
            assert!(!rect(st).is_empty());
            assert!(!st.screen_dirty.is_empty());
            assert_eq!(mocks::FREES.load(Ordering::SeqCst), before);
            assert!(request(Request::Close));
            true
        } else {
            assert!(rect(st).is_empty());
            false
        }
    });
    assert_eq!(n, 2);
    assert_eq!(mocks::FREES.load(Ordering::SeqCst), before + 1);
}
#[cfg(not(t5b_core_test))]
#[test]
fn compositor_occludes_app_and_repairs_actual_full_chrome_writes() {
    use crate::{mocks, visible, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    st.screen_w = 640;
    st.screen_h = 400;
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.x = 0;
    w.y = 100;
    w.w = 600;
    w.h = 270;
    w.flags = os32api::gui::proto::GUI_WF_BORDER;
    w.gen = 1;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    assert!(request_open());
    let mut n = 0;
    run(&mut st, |st| {
        n += 1;
        if n == 1 {
            wm::composite_rect(st, wm::Rect::new(0, 0, 640, 400));
            assert_eq!(
                mocks::pixels()[104 * mocks::W + 25],
                7,
                "panel body was not composed"
            );
            let panel = rect(st);
            let (ox, oy) = st.windows[0].client_origin();
            for r in st.windows[0].vis.as_slice() {
                assert!(!r.translate(ox, oy).intersects(&panel));
            }
            let before = mocks::pixels();
            // Requested clip misses panel, but chrome redraw writes the
            // full title across it. Repair must cover actual write area.
            wm::composite_rect(st, wm::Rect::new(580, 110, 1, 1));
            let after = mocks::pixels();
            for y in panel.y..panel.bottom() {
                for x in panel.x..panel.right() {
                    let i = y as usize * mocks::W + x as usize;
                    assert_eq!(after[i], before[i], "chrome damaged panel at {x},{y}");
                }
            }
            assert!(request(Request::Close));
            true
        } else {
            assert!(rect(st).is_empty());
            visible::recompute_and_expose(st);
            assert!(st.windows[0]
                .vis
                .as_slice()
                .iter()
                .any(|r| r.translate(2, 120).contains(100, 150)));
            assert!(!st.windows[0].dirty.is_empty());
            wm::composite_rect(st, wm::Rect::new(0, 0, 640, 400));
            assert_ne!(
                mocks::pixels()[30 * mocks::W + 30],
                7,
                "desktop not restored"
            );
            false
        }
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn actual_wait_taskbar_release_allows_immediate_app_press() {
    check_wait_release_allows_immediate_app_press("taskbar");
}
#[cfg(not(t5b_core_test))]
#[test]
fn actual_wait_modal_release_allows_immediate_app_press() {
    check_wait_release_allows_immediate_app_press("modal");
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_wait_menu_release_allows_immediate_app_press() {
    check_wait_release_allows_immediate_app_press("menu");
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_wait_fep_release_allows_immediate_app_press() {
    check_wait_release_allows_immediate_app_press("fep");
}
#[cfg(not(t5b_core_test))]
fn check_wait_release_allows_immediate_app_press(upper: &str) {
    use crate::{fep, input, mocks, modal, ring, slot, startmenu, wm};
    unsafe extern "C" fn active() -> i32 {
        1
    }
    unsafe extern "C" fn inactive() -> i32 {
        0
    }
    let modal_release = upper == "modal";
    mocks::init();
    REGISTRY.pending.set(None);
    let shm = mocks::Shm::new();
    let mut st = wm::GuiState::NEW;
    st.shm_base = shm.base();
    st.slots[0].used = true;
    st.slots[0].owner = 2;
    slot::init_header(&st, 0);
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.owner = 2;
    w.gen = 1;
    w.w = 600;
    w.h = 370;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    assert!(request_open());
    run(&mut st, |st| {
        *mocks::MOUSE.lock().unwrap() = (100, 150, 1);
        input::capture(st, input::Ctx::Wait);
        assert_eq!(ring::pending(st, 0), 0, "panel press leaked");
        if modal_release {
            modal::open_wm_message(
                st,
                os32api::gui::proto::GUI_MODAL_OK,
                b"Modal\0",
                modal::WM_PURPOSE_NOTIFY,
            );
            assert!(modal::is_open());
        }
        let release = if upper == "menu" {
            startmenu::open_context(st, 40, 130);
            assert!(startmenu::is_open());
            (100, 150, 0)
        } else if upper == "fep" {
            st.windows[0].tc_visible = true;
            st.windows[0].tc_x = 60;
            st.windows[0].tc_y = 180;
            unsafe {
                (*os32api::api_ptr()).ime_is_active = active;
            }
            fep::install();
            fep::pre_cycle(st);
            fep::post_cycle(st);
            let r = fep::rect();
            assert!(!r.is_empty());
            assert!(rect(st).contains(r.x, r.y));
            (r.x as i16, r.y as i16, 0)
        } else {
            (500, st.screen_h as i16 - 1, 0)
        };
        *mocks::MOUSE.lock().unwrap() = release;
        input::capture(st, input::Ctx::Wait);
        assert_eq!(ring::pending(st, 0), 0, "upper UI release leaked");
        if upper == "menu" {
            assert!(startmenu::is_open());
            startmenu::close(st);
        } else if upper == "fep" {
            unsafe {
                (*os32api::api_ptr()).ime_is_active = inactive;
            }
            fep::install();
            fep::pre_cycle(st);
            fep::post_cycle(st);
        }
        if modal_release {
            assert!(modal::is_open());
            modal::on_key(st, 0, 0x1b, 0);
            assert!(!modal::is_open());
        }
        // No idle sample between the upper UI release and this new press.
        *mocks::MOUSE.lock().unwrap() = (500, 200, 1);
        input::capture(st, input::Ctx::Wait);
        assert_eq!(
            ring::pending(st, 0),
            1,
            "new app press swallowed after upper UI release (modal={modal_release})"
        );
        *mocks::MOUSE.lock().unwrap() = (500, 200, 0);
        input::capture(st, input::Ctx::Wait);
        false
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn actual_x3_x4_mouse_capture_has_no_panel_to_app_leaks_or_glyph_work() {
    use crate::{input, mocks, ring, slot, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    REGISTRY.pending.set(None);
    let shm = mocks::Shm::new();
    let mut st = wm::GuiState::NEW;
    st.shm_base = shm.base();
    st.slots[0].used = true;
    st.slots[0].owner = 2;
    slot::init_header(&st, 0);
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.owner = 2;
    w.gen = 1;
    w.x = 0;
    w.y = 0;
    w.w = 600;
    w.h = 370;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    assert!(request_open());
    let mut n = 0;
    run(&mut st, |st| {
        n += 1;
        if n == 1 {
            for ctx in [input::Ctx::Pump, input::Ctx::Wait] {
                for (x, y, b) in [
                    (100, 150, 0),
                    (100, 150, 3),
                    (500, 200, 3),
                    (500, 200, 2),
                    (500, 200, 0),
                ] {
                    *mocks::MOUSE.lock().unwrap() = (x, y, b);
                    let reads = mocks::READS.load(Ordering::SeqCst);
                    let allocs = mocks::ALLOCS.load(Ordering::SeqCst);
                    input::capture(st, ctx);
                    assert_eq!(ring::pending(st, 0), 0, "pointer/down/up leaked");
                    assert_eq!(mocks::READS.load(Ordering::SeqCst), reads);
                    assert_eq!(mocks::ALLOCS.load(Ordering::SeqCst), allocs);
                }
            }
            st.drag_index = 0;
            *mocks::MOUSE.lock().unwrap() = (100, 150, 0);
            input::capture(st, input::Ctx::Pump);
            assert_eq!(
                ring::pending(st, 0),
                0,
                "WM drag pointer leaked through panel"
            );
            st.drag_index = -1;
            // A real app point still receives input; suppression isn't global.
            *mocks::MOUSE.lock().unwrap() = (510, 200, 0);
            input::capture(st, input::Ctx::Pump);
            assert_eq!(ring::pending(st, 0), 1);
            false
        } else {
            panic!("extra step")
        }
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn app_drag_outline_stays_below_fixed_panel() {
    use crate::{mocks, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    assert!(request_open());
    run(&mut st, |st| {
        wm::composite_rect(st, wm::Rect::new(0, 0, 640, 400));
        let before = mocks::pixels();
        let r = rect(st);
        st.drag_index = 0;
        st.drag_frame = wm::Rect::new(30, 110, 100, 50);
        wm::flush_screen_dirty(st);
        let after = mocks::pixels();
        for y in r.y..r.bottom() {
            for x in r.x..r.right() {
                let i = y as usize * mocks::W + x as usize;
                assert_eq!(after[i], before[i], "drag above panel {x},{y}");
            }
        }
        st.drag_index = -1;
        false
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn mouse_controls_defer_model_mutation_and_preserve_first_display_request() {
    use crate::{mocks, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    assert!(request_open());
    let mut n = 0;
    run(&mut st, |st| {
        n += 1;
        match n {
            1 => {
                assert!(mouse(st, 105, 28, 1, 0)); // stderr tab
                assert_eq!(read(|p| p.tab), Some(0));
                assert_eq!(REGISTRY.pending.get(), Some(Request::Tab));
                assert!(mouse(st, 245, 28, 0, 1));
                assert!(mouse(st, 245, 28, 1, 0)); // close cannot replace tab
                assert_eq!(REGISTRY.pending.get(), Some(Request::Tab));
                mouse(st, 245, 28, 0, 1);
            }
            2 => {
                assert_eq!(read(|p| p.tab), Some(1));
                mouse(st, 245, 28, 1, 0);
            }
            3 => {
                assert!(rect(st).is_empty());
                mouse(st, 500, 200, 0, 1);
                return false;
            }
            _ => panic!("loop"),
        }
        true
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn fixture_and_top_mouse_rows_match_labels() {
    use crate::{mocks, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    assert!(request_open());
    run(&mut st, |st| {
        for (x, y, want) in [
            (25, 44, Request::Select(Fixture::Normal)),
            (81, 44, Request::Select(Fixture::Exact)),
            (137, 44, Request::Select(Fixture::Full)),
            (193, 44, Request::Select(Fixture::TooWide)),
            (249, 44, Request::Select(Fixture::Finish)),
            (25, 60, Request::First),
            (105, 60, Request::Up),
            (185, 60, Request::Down),
            (265, 60, Request::Last),
        ] {
            mouse(st, x, y, 1, 0);
            assert_eq!(REGISTRY.pending.take(), Some(want));
            mouse(st, x, y, 0, 1);
        }
        false
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn z_start_menu_display_never_overwrites_pending_session_action() {
    use crate::{mocks, session, startmenu, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    session::clear();
    let mut st = wm::GuiState::NEW;
    assert_eq!(
        session::set_wm_launch(&mut st, b"/usr/bin/gui_demo.bin\0"),
        0
    );
    let before = session::pending_action();
    let mut path = [0; session::PATH_MAX + 1];
    let len = session::copy_path(&mut path);
    startmenu::toggle_start(&mut st);
    for _ in 0..5 {
        startmenu::on_key(&mut st, 0x3d);
    }
    startmenu::on_key(&mut st, 0x1c);
    assert_eq!(REGISTRY.pending.get(), Some(Request::Open));
    assert_eq!(session::pending_action(), before);
    let mut after = [0; session::PATH_MAX + 1];
    assert_eq!(session::copy_path(&mut after), len);
    assert_eq!(after, path);
    assert!(!request(Request::Close));
    assert_eq!(REGISTRY.pending.take(), Some(Request::Open));
    session::clear();
}
#[cfg(not(t5b_core_test))]
#[test]
fn resident_standalone_loop_actually_services_display_and_drops_on_exit() {
    use crate::{mocks, session, wm};
    use std::sync::atomic::{AtomicUsize, Ordering};
    static HALTS: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn halt() {
        let n = HALTS.fetch_add(1, Ordering::SeqCst);
        if n == 0 {
            assert!(request_open());
        } else {
            assert!(!rect(wm::g()).is_empty());
            wm::g().quit = true;
        }
    }
    mocks::init();
    REGISTRY.pending.set(None);
    session::clear();
    unsafe {
        (*os32api::api_ptr()).sys_halt = halt;
    }
    *wm::g() = wm::GuiState::NEW;
    let before = mocks::FREES.load(Ordering::SeqCst);
    assert!(crate::standalone_loop(wm::g()));
    assert_eq!(HALTS.load(Ordering::SeqCst), 2);
    assert_eq!(mocks::FREES.load(Ordering::SeqCst), before + 1);
    assert!(rect(wm::g()).is_empty());
}
#[cfg(not(t5b_core_test))]
#[test]
fn small_screen_refuses_before_allocating() {
    use crate::{mocks, modal, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    st.screen_w = 100;
    let before = mocks::ALLOCS.load(Ordering::SeqCst);
    assert!(request_open());
    run(&mut st, |st| {
        assert!(rect(st).is_empty());
        assert!(modal::is_open());
        false
    });
    assert_eq!(mocks::ALLOCS.load(Ordering::SeqCst), before);
    modal::on_key(&mut st, 0, 0x1b, 0);
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_existing_exec_callback_retains_parent_model_until_return() {
    use crate::{mocks, session, wm};
    use std::sync::atomic::{AtomicUsize, Ordering};
    static BASE: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn child(_: *const u8) -> i32 {
        assert_eq!(
            mocks::FREES.load(Ordering::SeqCst),
            BASE.load(Ordering::SeqCst)
        );
        let before = read(|p| p.streams[0].term.model().cells().to_vec()).unwrap();
        assert!(!session::owner_active(wm::g())); // slotless child
        assert!(request(Request::Close));
        draw(wm::g(), rect(wm::g())); // existing non-X4 read-only redraw
        assert_eq!(
            read(|p| p.streams[0].term.model().cells().to_vec()),
            Some(before)
        );
        assert_eq!(
            mocks::FREES.load(Ordering::SeqCst),
            BASE.load(Ordering::SeqCst)
        );
        -1 // opaque existing exec return; no new classification
    }
    mocks::init();
    REGISTRY.pending.set(None);
    session::clear();
    *wm::g() = wm::GuiState::NEW;
    BASE.store(mocks::FREES.load(Ordering::SeqCst), Ordering::SeqCst);
    unsafe {
        (*os32api::api_ptr()).exec_run = child;
    }
    assert!(request_open());
    let mut n = 0;
    run(wm::g(), |st| {
        n += 1;
        if n == 1 {
            assert_eq!(crate::run_program(st, &[0; 256]), -1);
            assert!(!rect(st).is_empty());
            assert_eq!(REGISTRY.pending.get(), Some(Request::Close));
            true
        } else {
            assert!(rect(st).is_empty());
            false
        }
    });
    assert_eq!(
        mocks::FREES.load(Ordering::SeqCst),
        BASE.load(Ordering::SeqCst) + 1
    );
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_existing_cui_failure_keeps_model_success_ends_borrow_before_free() {
    use crate::{mocks, modal, session, wm};
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    static FAIL: AtomicBool = AtomicBool::new(true);
    static SWITCHES: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn open(_: *const u8, _: i32) -> i32 {
        4
    }
    unsafe extern "C" fn read_cfg(_: i32, _: *mut u8, _: u32) -> i32 {
        0
    }
    unsafe extern "C" fn write_cfg(_: i32, _: *const u8, n: u32) -> i32 {
        if FAIL.load(Ordering::SeqCst) {
            -1
        } else {
            n as i32
        }
    }
    unsafe extern "C" fn close(_: i32) {}
    unsafe extern "C" fn switch(_: *const u8) -> i32 {
        SWITCHES.fetch_add(1, Ordering::SeqCst);
        assert!(read(|_| 1).is_some());
        0
    }
    mocks::init();
    REGISTRY.pending.set(None);
    session::clear();
    unsafe {
        let a = &mut *os32api::api_ptr();
        a.sys_open = open;
        a.sys_read = read_cfg;
        a.sys_write = write_cfg;
        a.sys_close = close;
        a.sys_switch_shell = switch;
    }
    let mut st = wm::GuiState::NEW;
    assert!(request_open());
    let before = mocks::FREES.load(Ordering::SeqCst);
    let mut n = 0;
    run(&mut st, |st| {
        n += 1;
        let saved = read(|p| p.streams[0].term.model().cells().to_vec());
        if n == 1 {
            assert!(crate::switch_cui(st));
            assert!(modal::is_open());
            assert_eq!(read(|p| p.streams[0].term.model().cells().to_vec()), saved);
            assert_eq!(mocks::FREES.load(Ordering::SeqCst), before);
            modal::on_key(st, 0, 0x1b, 0);
            FAIL.store(false, Ordering::SeqCst);
            true
        } else {
            assert!(!crate::switch_cui(st));
            false
        }
    });
    assert_eq!(SWITCHES.load(Ordering::SeqCst), 1);
    assert_eq!(n, 2);
    assert_eq!(mocks::FREES.load(Ordering::SeqCst), before + 1);
    assert_eq!(read(|_| 1), None);
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_upper_overlays_and_cursor_survive_readonly_panel_redraw() {
    use crate::{cursor, fep, mocks, modal, startmenu, wm};
    unsafe extern "C" fn active() -> i32 {
        1
    }
    unsafe extern "C" fn inactive() -> i32 {
        0
    }
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.x = 0;
    w.y = 0;
    w.w = 600;
    w.h = 370;
    w.tc_visible = true;
    w.tc_x = 60;
    w.tc_y = 180;
    st.windows[0] = w;
    st.z_count = 1;
    st.zorder[0] = 0;
    assert!(request_open());
    run(&mut st, |st| {
        modal::open_wm_message(
            st,
            os32api::gui::proto::GUI_MODAL_OK,
            b"Modal\0",
            modal::WM_PURPOSE_NOTIFY,
        );
        startmenu::open_context(st, 40, 130);
        unsafe {
            (*os32api::api_ptr()).ime_is_active = active;
        }
        fep::install();
        fep::pre_cycle(st);
        fep::post_cycle(st);
        cursor::hide(st);
        wm::composite_rect(st, wm::Rect::new(0, 0, 640, 400));
        st.cursor.x = 200;
        st.cursor.y = 100;
        cursor::show(st);
        let before = mocks::pixels();
        let holes = [
            modal::rect(),
            startmenu::rect(),
            fep::rect(),
            cursor::rect(st),
            crate::taskbar::rect(st),
        ];
        assert!(holes.iter().all(|r| !r.is_empty()));
        draw(st, rect(st));
        let after = mocks::pixels();
        for r in holes {
            for y in r.y.max(0)..r.bottom().min(400) {
                for x in r.x.max(0)..r.right().min(640) {
                    let i = y as usize * mocks::W + x as usize;
                    assert_eq!(after[i], before[i]);
                }
            }
        }
        cursor::hide(st);
        modal::on_key(st, 0, 0x1b, 0);
        startmenu::close(st);
        unsafe {
            (*os32api::api_ptr()).ime_is_active = inactive;
        }
        fep::install();
        fep::pre_cycle(st);
        fep::post_cycle(st);
        false
    });
}
#[cfg(not(t5b_core_test))]
#[test]
fn regression_app_visible_capacity_overflow_never_exposes_panel() {
    use crate::{mocks, visible, wm};
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    for i in 0..16 {
        let mut w = wm::Win::EMPTY;
        w.used = true;
        w.visible = true;
        if i == 0 {
            w.x = 0;
            w.y = 0;
            w.w = 600;
            w.h = 370;
        } else {
            w.x = 20 + i as i32 * 30;
            w.y = 140;
            w.w = 5;
            w.h = 40;
        }
        st.windows[i] = w;
        st.zorder[i] = i;
    }
    st.z_count = 16;
    assert!(request_open());
    run(&mut st, |st| {
        visible::recompute_and_expose(st);
        assert!(st.windows[0].vis_capped);
        let panel = rect(st);
        for w in &st.windows {
            let (x, y) = w.client_origin();
            for r in w.vis.as_slice() {
                assert!(!r.translate(x, y).intersects(&panel));
            }
        }
        false
    });
}
#[test]
fn recursive_driver_cannot_allocate_or_consume_outer_pending() {
    REGISTRY.pending.set(None);
    struct Nested;
    impl Driver for Nested {
        fn rows(&self) -> usize {
            10
        }
        fn changed(&mut self) {}
        fn refused(&mut self) {}
        fn step(&mut self) -> bool {
            panic!("reentered top-level driver")
        }
    }
    struct Outer;
    impl Driver for Outer {
        fn rows(&self) -> usize {
            10
        }
        fn changed(&mut self) {}
        fn refused(&mut self) {}
        fn step(&mut self) -> bool {
            assert!(request(Request::Open));
            let mut h = TestHeap {
                fail: true,
                allocs: 0,
                frees: 0,
            };
            drive(&mut h, &mut Nested);
            assert_eq!(h.allocs, 0);
            assert_eq!(REGISTRY.pending.take(), Some(Request::Open));
            false
        }
    }
    drive(
        &mut TestHeap {
            fail: false,
            allocs: 0,
            frees: 0,
        },
        &mut Outer,
    );
}
#[cfg(not(t5b_core_test))]
#[test]
fn direct_x4_redraw_is_rejected_before_glyph_lookup() {
    use crate::{mocks, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    REGISTRY.pending.set(None);
    let mut st = wm::GuiState::NEW;
    assert!(request_open());
    run(&mut st, |st| {
        st.in_pump = true;
        let before = mocks::READS.load(Ordering::SeqCst);
        let pixels = mocks::pixels();
        draw(st, rect(st));
        assert_eq!(mocks::READS.load(Ordering::SeqCst), before);
        assert_eq!(mocks::pixels(), pixels);
        st.in_pump = false;
        false
    });
}
struct TestHeap {
    fail: bool,
    allocs: usize,
    frees: usize,
}
unsafe impl Heap for TestHeap {
    fn alloc(&mut self, bytes: usize) -> *mut Cell {
        self.allocs += 1;
        if self.fail {
            return core::ptr::null_mut();
        }
        unsafe {
            std::alloc::alloc(
                std::alloc::Layout::from_size_align(bytes, core::mem::align_of::<Cell>()).unwrap(),
            )
            .cast()
        }
    }
    unsafe fn free(&mut self, p: *mut Cell) {
        self.frees += 1;
        std::alloc::dealloc(
            p.cast(),
            std::alloc::Layout::from_size_align(BYTES, core::mem::align_of::<Cell>()).unwrap(),
        );
    }
}
#[test]
fn heap_scope_initializes_valid_cells_and_repeated_close_frees_once() {
    let mut h = TestHeap {
        fail: false,
        allocs: 0,
        frees: 0,
    };
    for n in 1..=20 {
        let result = with_cells(&mut h, |cells| {
            assert_eq!(cells.len(), CELLS);
            assert!(cells.iter().all(|c| *c == BLANK));
            cells[CELLS - 1] = Cell::Wide('日');
            42
        });
        assert_eq!(result, Some(42));
        assert_eq!((h.allocs, h.frees), (n, n));
    }
    h.fail = true;
    assert_eq!(
        with_cells(&mut h, |_| panic!("null allocation lent")),
        None::<()>
    );
    assert_eq!(h.frees, 20);
}

/// Enter 連打で、FEP が確定した文字が Input dialog の結果から抜けないこと。
///
/// 確定 Enter と決定 Enter が**同じ吸い出し周期**に入ると、`fep::flush_text`
/// が周期末尾のままでは、確定文字が field に入る前にダイアログが閉じる。
/// 結果は空になり、残った確定文字は `Text` として背後のアプリへ流れる。
#[cfg(not(t5b_core_test))]
#[test]
fn double_enter_keeps_fep_commit_in_the_input_dialog_result() {
    use crate::{fep, input, mocks, modal, slot, wm};
    use os32api::gui::proto::{GuiEvent, GUI_EV_TEXT, GUI_RING_CAPACITY};
    const SC_RETURN: i32 = 0x1C;
    const DOWN: i32 = 1 << 8;

    /* リングを直接読んで、アプリへ渡った `Text` の中身を集める。 */
    fn text_events(st: &wm::GuiState, slot_i: usize) -> Vec<u8> {
        let h = slot::read_header(st, slot_i);
        let base = slot::ring_ptr(st, slot_i);
        let mut out = Vec::new();
        let mut i = h.ring_head;
        while i != h.ring_tail {
            let ev: GuiEvent = unsafe {
                core::ptr::read_unaligned(
                    base.add((i as usize % GUI_RING_CAPACITY) * 16) as *const GuiEvent,
                )
            };
            if ev.kind == GUI_EV_TEXT {
                let n = (ev.sub & 0x7F) as usize;
                let n = if n > 8 { 8 } else { n };
                out.extend_from_slice(&ev.payload[..n]);
            }
            i = i.wrapping_add(1);
        }
        out
    }

    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = wm::GuiState::NEW;
    st.shm_base = shm.base();
    st.slots[0].used = true;
    st.slots[0].owner = 2;
    slot::init_header(&st, 0);
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.owner = 2;
    w.gen = 1;
    w.w = 600;
    w.h = 370;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    *mocks::MOUSE.lock().unwrap() = (0, 0, 0);

    /* 1 回目の Enter で "ab" を確定し、2 回目は FEP が素通しする。 */
    mocks::fep_script(&[0x61, 0x62, 0x00, 0x100]);
    fep::install();
    modal::open_wm_input(&mut st, b"Run\0", modal::WM_PURPOSE_FILE_LAUNCH);
    assert!(modal::is_open() && modal::is_input());

    /* 両方の Enter を**同じ周期**に積む。 */
    mocks::push_rawkeys(&[SC_RETURN | DOWN, SC_RETURN | DOWN]);
    input::capture(&mut st, input::Ctx::Wait);

    assert!(!modal::is_open(), "2 回目の Enter でダイアログが閉じていない");
    assert!(
        st.launch_pending,
        "確定文字が field に入る前に閉じた (結果が空)"
    );
    assert_eq!(
        &st.launch_path[..st.launch_path_len],
        b"ab",
        "結果から確定文字が抜けた"
    );
    assert!(
        text_events(&st, 0).is_empty(),
        "確定文字がダイアログを素通りしてアプリへ流れた: {:?}",
        text_events(&st, 0)
    );
}
