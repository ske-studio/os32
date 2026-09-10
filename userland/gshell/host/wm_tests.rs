//! wm_tests.rs — WM / 入力 / モーダルのホスト回帰試験。
//!
//! `src/input.rs` の末尾から `#[cfg(test)] #[path]` で取り込み、
//! `host/integration.py` が gshell の実モジュールをホスト ABI の代用
//! (`host/mocks.rs`) と一緒にビルドして走らせる (`make check-gshell-host`)。
//! 中身は 2026-09-10 の独立レビュー第 2・3 回で足した挙動試験で、
//! T5b (常駐表示パネル) 撤去のときに `host/terminal_tests.rs` から移した。

/// Enter 連打で、FEP が確定した文字が Input dialog の結果から抜けないこと。
///
/// 確定 Enter と決定 Enter が**同じ吸い出し周期**に入ると、`fep::flush_text`
/// が周期末尾のままでは、確定文字が field に入る前にダイアログが閉じる。
/// 結果は空になり、残った確定文字は `Text` として背後のアプリへ流れる。
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

/* ================================================================ */
/*  2026-09-10 レビュー第 3 回 (4 件のうち gshell 側 3 件)          */
/* ================================================================ */

/// 検査用に窓 1 枚とスロット 1 本を張った `GuiState` を作る。
fn one_window_state(shm: &crate::mocks::Shm) -> crate::wm::GuiState {
    use crate::{slot, wm};
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
    w.x = 40;
    w.y = 40;
    w.w = 400;
    w.h = 300;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    st
}

/// リング内の `Button` を (down, button, x, y) で集める。
fn button_events(st: &crate::wm::GuiState, slot_i: usize) -> Vec<(bool, u8, i16, i16)> {
    use crate::slot;
    use os32api::gui::proto::{GuiEvent, GUI_EV_BUTTON, GUI_RING_CAPACITY};
    let h = slot::read_header(st, slot_i);
    let base = slot::ring_ptr(st, slot_i);
    let mut out = Vec::new();
    let mut i = h.ring_head;
    while i != h.ring_tail {
        let ev: GuiEvent = unsafe {
            core::ptr::read_unaligned(
                base.add((i as usize % GUI_RING_CAPACITY) * 16) as *const GuiEvent
            )
        };
        if ev.kind == GUI_EV_BUTTON {
            let x = i16::from_le_bytes([ev.payload[0], ev.payload[1]]);
            let y = i16::from_le_bytes([ev.payload[2], ev.payload[3]]);
            out.push((ev.sub != 0, ev.payload[4], x, y));
        }
        i = i.wrapping_add(1);
    }
    out
}

/// アプリのクライアント内で押して**タスクバーの上で離す**と、Button-up が
/// 失われていた。ウィジェットの armed / アプリのドラッグ状態が解けず、
/// 押されたままの表示が残る。押下の配送先を捕捉して、離しは同じ相手へ返す。
#[test]
fn button_up_on_taskbar_still_reaches_the_app_that_got_the_press() {
    use crate::{input, mocks, taskbar};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_window_state(&shm);

    let (cx, cy) = (200i32, 200i32);
    assert!(
        st.windows[0].client_rect_screen().contains(cx, cy),
        "検査点がクライアント内でない"
    );
    /* 押下: クライアント内。 */
    *mocks::MOUSE.lock().unwrap() = (cx as i16, cy as i16, 1);
    input::capture(&mut st, input::Ctx::Wait);

    /* 離し: タスクバーの上。 */
    let ty = st.screen_h - 1;
    assert!(taskbar::hit(&st, 500, ty), "検査点がタスクバー上でない");
    *mocks::MOUSE.lock().unwrap() = (500, ty as i16, 0);
    input::capture(&mut st, input::Ctx::Wait);

    let evs = button_events(&st, 0);
    assert_eq!(evs.len(), 2, "Button が 2 件でない: {evs:?}");
    assert!(evs[0].0 && evs[0].1 == 1, "1 件目が左の押下でない: {evs:?}");
    assert!(
        !evs[1].0 && evs[1].1 == 1,
        "タスクバー上の離しが失われた: {evs:?}"
    );
}

/// タイトルバー / 枠の右クリックはアプリへ配らない (契約 D4 は
/// 「前面窓の**クライアント上**」)。負のクライアント座標が届くと、
/// アプリの context menu が誤作動する。
#[test]
fn right_click_on_titlebar_is_not_forwarded_but_client_still_is() {
    use crate::{input, mocks};

    /* (mx, my) で右押下 → 離し。アプリに届いた Button を返す。 */
    fn right_click_at(mx: i32, my: i32) -> Vec<(bool, u8, i16, i16)> {
        use crate::{input, mocks};
        mocks::init();
        let shm = mocks::Shm::new();
        let mut st = one_window_state(&shm);
        *mocks::MOUSE.lock().unwrap() = (mx as i16, my as i16, 2);
        input::capture(&mut st, input::Ctx::Wait);
        *mocks::MOUSE.lock().unwrap() = (mx as i16, my as i16, 0);
        input::capture(&mut st, input::Ctx::Wait);
        button_events(&st, 0)
    }

    /* タイトルバーの座標を実物から取る。 */
    let (tx, ty, cx, cy) = {
        mocks::init();
        let shm = mocks::Shm::new();
        let st = one_window_state(&shm);
        let tb = st.windows[0].titlebar_rect();
        let cr = st.windows[0].client_rect_screen();
        assert!(tb.w > 0 && tb.h > 0, "タイトルバーが無い");
        (
            tb.x + tb.w / 2,
            tb.y + tb.h / 2,
            cr.x + cr.w / 2,
            cr.y + cr.h / 2,
        )
    };
    let _ = (input::Ctx::Wait, mocks::W);

    let title = right_click_at(tx, ty);
    assert!(
        title.is_empty(),
        "タイトルバーの右クリックがアプリへ届いた: {title:?}"
    );

    /* 対照群: クライアント上の右クリックは今までどおり届く。 */
    let client = right_click_at(cx, cy);
    assert_eq!(client.len(), 2, "クライアントの右クリックが届かない: {client:?}");
    assert!(client[0].0 && client[0].1 == 2 && client[0].2 >= 0 && client[0].3 >= 0);
    assert!(!client[1].0 && client[1].1 == 2);
}

/// 48B 以上のエントリ名を、切り詰めた**別名**として返さない (契約 M1
/// 「path/text を切り詰めて別値として返してはならない」)。切り詰めた名前は
/// 実在しない path か、同じ 47B の接頭辞を持つ別ファイルを指す。
#[test]
fn file_dialog_refuses_names_that_do_not_fit_instead_of_truncating() {
    use crate::{input, mocks, modal, wm};

    /* `sys_ls` を差し替えて、この名前 1 件だけを返す。 */
    static LS_NAME: std::sync::Mutex<Vec<u8>> = std::sync::Mutex::new(Vec::new());
    #[repr(C)]
    struct DirEntryExt {
        name: [u8; 256],
        size: u32,
        ftype: u8,
    }
    unsafe extern "C" fn ls(_path: *const u8, cb: *mut u8, ctx: *mut u8) -> i32 {
        let mut e = DirEntryExt {
            name: [0u8; 256],
            size: 1,
            ftype: 1, /* FILE_TYPE_FILE */
        };
        let n = LS_NAME.lock().unwrap();
        e.name[..n.len()].copy_from_slice(&n);
        let f: extern "C" fn(*const DirEntryExt, *mut u8) = core::mem::transmute(cb);
        f(&e as *const DirEntryExt, ctx);
        0
    }

    /// `name` 1 件のディレクトリで RETURN を押し、(選ばれたか, 選ばれた path)。
    fn pick(name: &[u8]) -> (bool, Vec<u8>) {
        use crate::{mocks, modal, wm};
        mocks::init();
        unsafe {
            (*os32api::api_ptr()).sys_ls = ls;
        }
        *LS_NAME.lock().unwrap() = name.to_vec();
        let shm = mocks::Shm::new();
        let mut st = one_window_state(&shm);
        modal::open_wm_file(&mut st, b"/\0");
        assert!(modal::is_open());
        modal::x3_cycle(&mut st); /* sys_ls は X3 でだけ走る */
        modal::on_key(&mut st, 0x1C, 0x0D, 0); /* RETURN */
        /* 拒否された名前ではダイアログが開いたまま残る。modal は静的なので
         * 閉じずに戻ると次の試験の入力が全部ダイアログ宛になる (T5b の試験が
         * 間に挟まっていた頃は、そちらの ESC がたまたま閉じていた)。 */
        if modal::is_open() {
            modal::on_key(&mut st, 0, 0x1b, 0);
        }
        let _ = wm::GuiState::NEW.z_count;
        (
            st.launch_pending,
            st.launch_path[..st.launch_path_len].to_vec(),
        )
    }

    /* 47B は収まるので今までどおり選べる。 */
    let short = vec![b'a'; 47];
    let (ok, path) = pick(&short);
    assert!(ok, "47B の名前が選べない");
    assert_eq!(&path[1..], &short[..], "47B の名前が変わった: {path:?}");

    /* 48B は収まらない。切り詰めた別名を返さず、選べないこと。 */
    let long = vec![b'b'; 48];
    let (ok, path) = pick(&long);
    assert!(
        !ok,
        "48B の名前を切り詰めた別名として返した: {:?}",
        core::str::from_utf8(&path)
    );

    /* 80B でも同じ。 */
    let (ok, _) = pick(&vec![b'c'; 80]);
    assert!(!ok, "80B の名前を切り詰めた別名として返した");

    let _ = (input::Ctx::Wait, modal::is_open());
}
