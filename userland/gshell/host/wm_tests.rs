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

/* ================================================================ */
/*  2026-09-10 レビュー第 4 回                                       */
/*                                                                  */
/*  前半は T5b 撤去 (1c98613) で消えた「上位 UI 上の離し」4 本の      */
/*  書き直し、後半はモーダル中に捕捉した離しが失われる [P2] の回帰。 */
/* ================================================================ */

/// アプリのクライアント内の代表点 (窓 1 枚の `one_window_state` 用)。
const APP_PT: (i32, i32) = (200, 200);

/// マウスの位置とボタンをモックへ差し込む。
fn set_mouse(x: i32, y: i32, buttons: u8) {
    *crate::mocks::MOUSE.lock().unwrap() = (x as i16, y as i16, buttons);
}

unsafe extern "C" fn ime_on() -> i32 {
    1
}
unsafe extern "C" fn ime_off() -> i32 {
    0
}

/// 上位 UI (タスクバー / メニュー / モーダル / FEP) の上で押して離しても、
/// **押していないボタンの離し**がアプリへ飛ばず、その直後のアプリ内の押下は
/// 届く。T5b 撤去で消えた 4 本の、パネルに依存しない書き直し。
///
/// 元の 4 本は「常駐パネルが最初の押下を食って `prev_buttons` を立てる」
/// 仕掛けだった。ここでは押下を**上位 UI 自身**に取らせる。ただし FEP は
/// マウスの領分を持たない (`input.rs` は FEP へマウスを一切渡さない) ので、
/// FEP の回だけは元の試験と同じく「押下はタスクバーが取り、離しを FEP 矩形の
/// 上で行う」形にした — 元の被覆 (FEP 矩形の上の離しが漏れない) はそちら。
fn upper_ui_press_release_then_app_press(kind: &str) {
    use crate::{fep, input, mocks, modal, startmenu, taskbar};
    use os32api::gui::proto::GUI_MODAL_OK;

    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_window_state(&shm);
    let (ax, ay) = APP_PT;
    assert!(
        st.windows[0].client_rect_screen().contains(ax, ay),
        "検査点がクライアント内でない"
    );

    /* ---- 上位 UI を出し、押下と離しの位置を決める ---- */
    /* Start ボタンでも窓ボタンでもない帯の上 (押しても何も起きない場所)。
     * Start の上だと押下でメニューが開いてしまい、直後のアプリ押下が
     * 「メニューを閉じる」に化けて試験の主題がぼける。 */
    let taskbar_pt = (500, st.screen_h - 1);
    assert!(
        taskbar::hit(&st, taskbar_pt.0, taskbar_pt.1),
        "検査点がタスクバー上でない"
    );
    assert!(
        !startmenu::is_open(),
        "前の試験がメニューを開いたまま残した"
    );
    let (press_pt, release_pt) = match kind {
        "taskbar" => (taskbar_pt, taskbar_pt),
        "menu" => {
            /* 開いたメニューの外側 = アプリのクライアント上を押すと、
             * メニューが閉じて対の離しは捨てられる (swallow_up)。 */
            startmenu::open_context(&mut st, 60, 60);
            assert!(startmenu::is_open());
            ((ax, ay), (ax, ay))
        }
        "modal" => {
            modal::open_wm_message(&mut st, GUI_MODAL_OK, b"Modal\0", modal::WM_PURPOSE_NOTIFY);
            assert!(modal::is_open());
            let r = modal::rect();
            ((r.x + 2, r.y + 2), (r.x + 2, r.y + 2))
        }
        "fep" => {
            st.windows[0].tc_visible = true;
            st.windows[0].tc_x = 60;
            st.windows[0].tc_y = 180;
            unsafe {
                (*os32api::api_ptr()).ime_is_active = ime_on;
            }
            fep::install();
            fep::pre_cycle(&mut st);
            fep::post_cycle(&mut st);
            let r = fep::rect();
            assert!(!r.is_empty(), "FEP の矩形が出ていない");
            (taskbar_pt, (r.x, r.y))
        }
        _ => unreachable!(),
    };

    /* ---- 上位 UI の上で押して離す: アプリへは 1 件も出ない ---- */
    set_mouse(press_pt.0, press_pt.1, 1);
    input::capture(&mut st, input::Ctx::Wait);
    assert!(
        button_events(&st, 0).is_empty(),
        "上位 UI ({kind}) の押下がアプリへ漏れた: {:?}",
        button_events(&st, 0)
    );
    set_mouse(release_pt.0, release_pt.1, 0);
    input::capture(&mut st, input::Ctx::Wait);
    assert!(
        button_events(&st, 0).is_empty(),
        "上位 UI ({kind}) の離しがアプリへ漏れた: {:?}",
        button_events(&st, 0)
    );

    /* ---- 上位 UI を片付ける (静的な状態を次の試験へ持ち越さない) ---- */
    match kind {
        "menu" => {
            assert!(!startmenu::is_open(), "外側の押下でメニューが閉じていない");
        }
        "modal" => {
            assert!(modal::is_open(), "ボタン以外の押下でダイアログが閉じた");
            modal::on_key(&mut st, 0, 0x1b, 0);
            assert!(!modal::is_open());
        }
        "fep" => {
            unsafe {
                (*os32api::api_ptr()).ime_is_active = ime_off;
            }
            fep::install();
            fep::pre_cycle(&mut st);
            fep::post_cycle(&mut st);
        }
        _ => {}
    }

    /* ---- 直後のアプリ内の押下は届く (間に無操作の周を挟まない) ---- */
    set_mouse(ax, ay, 1);
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(
        evs.len(),
        1,
        "上位 UI ({kind}) の離しの直後、アプリの押下が飲まれた: {evs:?}"
    );
    assert!(evs[0].0 && evs[0].1 == 1, "押下でない: {evs:?}");

    /* 捕捉を残さない (CAPTURE は静的)。 */
    set_mouse(ax, ay, 0);
    input::capture(&mut st, input::Ctx::Wait);
}

#[test]
fn taskbar_press_release_then_app_press_is_delivered() {
    upper_ui_press_release_then_app_press("taskbar");
}

#[test]
fn menu_press_release_then_app_press_is_delivered() {
    upper_ui_press_release_then_app_press("menu");
}

#[test]
fn modal_press_release_then_app_press_is_delivered() {
    upper_ui_press_release_then_app_press("modal");
}

#[test]
fn fep_press_release_then_app_press_is_delivered() {
    upper_ui_press_release_then_app_press("fep");
}

/// アプリ内で押したまま、アプリ自身がダイアログを開き (OP_MODAL_OPEN)、
/// それから離す。契約 U4「モーダル中は宛先をダイアログに限定」は**新しい
/// 入力**の規則で、モーダル前の押下と対になる離しはその押下を受けたアプリの
/// もの。捕捉経由で返さないと、アプリのウィジェットは armed のまま残り、
/// `prev_buttons` だけ進むので up_edge は二度と立たない。
fn captured_release_while_modal(button: u8) {
    use crate::{input, mocks, modal};
    use os32api::gui::proto::GUI_MODAL_OK;

    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_window_state(&shm);
    let (ax, ay) = APP_PT;

    /* 押下はアプリのクライアント内 = アプリの領分。 */
    set_mouse(ax, ay, button);
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(evs.len(), 1, "押下が届いていない: {evs:?}");
    assert!(evs[0].0 && evs[0].1 == button, "押下が違う: {evs:?}");

    /* 押したままアプリがダイアログを開く。 */
    modal::open_wm_message(&mut st, GUI_MODAL_OK, b"Modal\0", modal::WM_PURPOSE_NOTIFY);
    assert!(modal::is_open());

    /* ダイアログの上で離す。 */
    let r = modal::rect();
    set_mouse(r.x + 2, r.y + 2, 0);
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(
        evs.len(),
        2,
        "モーダル中に捕捉した離しが失われた (button={button}): {evs:?}"
    );
    assert!(
        !evs[1].0 && evs[1].1 == button,
        "2 件目が対の離しでない: {evs:?}"
    );

    /* 閉じたあと、余分な Button が後から湧かない。 */
    assert!(modal::is_open(), "離しでダイアログが閉じた");
    modal::on_key(&mut st, 0, 0x1b, 0);
    assert!(!modal::is_open());
    set_mouse(ax, ay, 0);
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(evs.len(), 2, "閉じたあとに Button が増えた: {evs:?}");
}

#[test]
fn captured_release_is_delivered_while_modal_is_open() {
    captured_release_while_modal(1);
}

#[test]
fn captured_right_release_is_delivered_while_modal_is_open() {
    captured_release_while_modal(2);
}

/// X4 (ポンプ) がモーダル中に離しを先に見ても、`prev_buttons` を進めないので
/// 次の X3 が同じエッジを拾い直し、離しは 1 件だけ届く (二重配送も欠落も無し)。
#[test]
fn captured_release_survives_the_pump_while_modal_is_open() {
    use crate::{input, mocks, modal};
    use os32api::gui::proto::GUI_MODAL_OK;

    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_window_state(&shm);
    let (ax, ay) = APP_PT;

    set_mouse(ax, ay, 1);
    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(button_events(&st, 0).len(), 1);

    modal::open_wm_message(&mut st, GUI_MODAL_OK, b"Modal\0", modal::WM_PURPOSE_NOTIFY);
    assert!(modal::is_open());

    /* ポンプが先に離しを見る: ここでは何も配らず prev_buttons も進めない。 */
    let r = modal::rect();
    set_mouse(r.x + 2, r.y + 2, 0);
    input::capture(&mut st, input::Ctx::Pump);
    assert_eq!(
        button_events(&st, 0).len(),
        1,
        "ポンプが離しを配った (X3 と二重になる)"
    );

    /* 次の X3 が同じエッジを拾い直して、捕捉した相手へ返す。 */
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(evs.len(), 2, "X4 の後で離しが失われた: {evs:?}");
    assert!(!evs[1].0 && evs[1].1 == 1, "2 件目が離しでない: {evs:?}");

    modal::on_key(&mut st, 0, 0x1b, 0);
    assert!(!modal::is_open());
}

/// 負例: モーダル中に**新しく**始まった押下と離しは、契約 U4 のとおり
/// ダイアログだけのもの。アプリへは 1 件も配らない。
#[test]
fn new_press_during_modal_is_not_forwarded_to_the_app() {
    use crate::{input, mocks, modal};
    use os32api::gui::proto::GUI_MODAL_OK;

    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_window_state(&shm);
    let (ax, ay) = APP_PT;

    /* 先に押下と離しを 1 組済ませ、捕捉が空であることを確かめてから開く。 */
    set_mouse(ax, ay, 1);
    input::capture(&mut st, input::Ctx::Wait);
    set_mouse(ax, ay, 0);
    input::capture(&mut st, input::Ctx::Wait);
    let base = button_events(&st, 0).len();
    assert_eq!(base, 2, "前提の押下・離しが 2 件でない");

    modal::open_wm_message(&mut st, GUI_MODAL_OK, b"Modal\0", modal::WM_PURPOSE_NOTIFY);
    assert!(modal::is_open());

    let r = modal::rect();
    set_mouse(r.x + 2, r.y + 2, 1);
    input::capture(&mut st, input::Ctx::Wait);
    set_mouse(r.x + 2, r.y + 2, 0);
    input::capture(&mut st, input::Ctx::Wait);
    let evs = button_events(&st, 0);
    assert_eq!(
        evs.len(),
        base,
        "モーダル中の新しい押下・離しがアプリへ漏れた: {evs:?}"
    );

    modal::on_key(&mut st, 0, 0x1b, 0);
    assert!(!modal::is_open());
}

/* ================================================================ */
/*  K5b-W — アプリ 4 本の同時実行 (票 TASK_K5B_gshell.md)            */
/*                                                                  */
/*  規則の正典は K5a 設計 D11-3 / D11-3a、模型は                     */
/*  `tools/tests/multiapp_model_host.c` のケース 12〜16 (84 検査)。   */
/*  ここは**同じ性質**を、模型の `MaState` ではなく実物の             */
/*  `GuiState` + `multiapp` の状態に対して検査する。                  */
/*                                                                  */
/*  `input_ready` / `derived_ready` は模型では試験が直接立てていたが、 */
/*  実物は WM 状態から算出するので、試験は「リングにイベントを積む」  */
/*  「`configure_pending` を立てる」という**実物の材料**で作る。      */
/* ================================================================ */

/// アプリ 4 本 (owner 2〜5) がスロット 0〜3 と窓を 1 枚ずつ持つ状態。
/// Z 順は 0,1,2,3 なので最前面 = 窓 3 = owner 5 (`front_owner()`)。
fn four_app_state(shm: &crate::mocks::Shm) -> crate::wm::GuiState {
    use crate::{slot, wm};
    let mut st = wm::GuiState::NEW;
    st.shm_base = shm.base();
    let mut k = 0usize;
    while k < 4 {
        let owner = 2 + k as i32;
        st.slots[k].used = true;
        st.slots[k].owner = owner;
        slot::init_header(&st, k);
        let mut w = wm::Win::EMPTY;
        w.used = true;
        w.visible = true;
        w.owner = owner;
        w.gen = 1;
        w.x = 10 + 140 * k as i32;
        w.y = 10;
        w.w = 100;
        w.h = 80;
        st.windows[k] = w;
        st.zorder[k] = k;
        k += 1;
    }
    st.z_count = 4;
    st
}

/// GUI アプリ 1 本 (owner 2、スロット 0 と窓 1 枚) だけの状態。票 K7 の
/// 「端末から起動した CUI アプリ」は `OP_INIT` を通らないのでスロットも窓も
/// 持たない — 表 (`multiapp`) にだけ載る本を作るための土台。
fn one_gui_app_state(shm: &crate::mocks::Shm) -> crate::wm::GuiState {
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
    w.x = 10;
    w.y = 10;
    w.w = 100;
    w.h = 80;
    st.windows[0] = w;
    st.zorder[0] = 0;
    st.z_count = 1;
    st
}

/// `multiapp` の表に 4 本を載せる (`exec_start` が 4 回成功した後と同じ形)。
fn seed_four_apps() {
    use crate::multiapp;
    let mut id = 2;
    while id <= 5 {
        multiapp::on_start(id);
        id += 1;
    }
}

/// 入力群 (未読の待ち行列型) を作る / 消す。実物のリングを使う。
fn set_input_ready(st: &mut crate::wm::GuiState, id: i32, on: bool) {
    use crate::{ring, slot};
    use os32api::gui::proto::GUI_EV_CLOSE;
    let s = st.slot_of_owner(id).expect("スロットが無い");
    if on {
        if crate::multiapp::input_ready(st, id) {
            return;
        }
        let ev = ring::ev_simple(GUI_EV_CLOSE, 0, 0);
        assert!(ring::append(st, s, &ev), "リングへ積めない");
    } else {
        let mut h = slot::read_header(st, s);
        h.ring_head = h.ring_tail;
        slot::write_header(st, s, &h);
    }
}

/// 導出群 (`Configure` 未通知) を作る / 消す。
fn set_derived_ready(st: &mut crate::wm::GuiState, id: i32, on: bool) {
    let i = (id - 2) as usize;
    st.windows[i].configure_pending = on;
}

/// フォーカス (= 最前面の可視窓の owner) をこの ID にする。
fn focus_app(st: &mut crate::wm::GuiState, id: i32) {
    st.bring_to_front((id - 2) as usize);
    assert_eq!(st.front_owner(), id, "フォーカスが動いていない");
}

/// 「park して次の 1 本を起こす」1 回ぶん。**判断は実物**
/// (`should_park` / `pick` / `mark_resumed`) で、ここが模すのは
/// カーネルの制御の流れ (longjmp と `ring3_resume`) だけ。
/// 戻り値は新しく走り出した ID (park しなかったら `cur` のまま、
/// 起こす相手が居なければ 0 = WM top-level)。
fn sched_step(st: &mut crate::wm::GuiState, cur: i32) -> i32 {
    use crate::multiapp;
    if !multiapp::should_park(st, cur) {
        return cur;
    }
    multiapp::note_parked(cur, None);
    let k = multiapp::pick(st);
    if k == 0 {
        return 0;
    }
    multiapp::mark_resumed(k);
    k
}

/* ---- ケース 12 相当: 同時 ready の選択規則 (D11-3 の (2)) ---- */
#[test]
fn pick_follows_the_frozen_rule_focus_then_input_then_derived() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();

    /* 12a フォーカス窓の owner が入力群に居れば、それを最優先。 */
    set_input_ready(&mut st, 3, true);
    set_input_ready(&mut st, 5, true);
    focus_app(&mut st, 5);
    multiapp::set_last_run(2);
    assert_eq!(multiapp::pick(&st), 5, "12a 入力群のフォーカスを選ばない");

    /* 12b フォーカスが入力群に居なければ last_run+1 から ID 昇順に巡回。 */
    focus_app(&mut st, 2); /* 2 は ready でない */
    multiapp::set_last_run(3); /* 巡回は 4 → 5 → 2 → 3 */
    assert_eq!(multiapp::pick(&st), 5, "12b 入力群のラウンドロビンが違う");

    /* 12c 入力群が空なら導出群を同じ巡回で。 */
    set_input_ready(&mut st, 3, false);
    set_input_ready(&mut st, 5, false);
    set_derived_ready(&mut st, 2, true);
    set_derived_ready(&mut st, 4, true);
    focus_app(&mut st, 2);
    multiapp::set_last_run(2); /* 巡回は 3 → 4 → 5 → 2 */
    assert_eq!(multiapp::pick(&st), 4, "12c 導出群のラウンドロビンが違う");

    /* 12d 導出群ではフォーカスを優先しない (優先していれば 4 になる)。 */
    focus_app(&mut st, 4);
    multiapp::set_last_run(4); /* 巡回は 5 → 2 → 3 → 4 */
    assert_eq!(multiapp::pick(&st), 2, "12d 導出群でフォーカスを優先した");

    /* 12e 入力は導出より必ず先。 */
    set_input_ready(&mut st, 5, true);
    focus_app(&mut st, 2);
    multiapp::set_last_run(4);
    assert_eq!(multiapp::pick(&st), 5, "12e 入力群が導出群より後になった");

    /* 12f 誰も ready でなければ起こさない。 */
    set_derived_ready(&mut st, 2, false);
    set_derived_ready(&mut st, 4, false);
    set_input_ready(&mut st, 5, false);
    assert_eq!(multiapp::pick(&st), 0, "12f ready ゼロで誰かを起こした");
}

/* ---- ケース 13 相当: 走っているアプリが譲るか (D11-3 の (1)) ---- */
#[test]
fn should_park_matches_the_five_frozen_branches() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();
    multiapp::mark_resumed(2); /* ID 2 が走っている */

    /* 13a 自分に入力があれば park しない (打鍵の連続を取りこぼさない)。 */
    set_input_ready(&mut st, 2, true);
    set_input_ready(&mut st, 4, true);
    assert!(!multiapp::should_park(&st, 2), "13a 自分の入力で譲った");
    assert_eq!(multiapp::input_streak(), 1, "13a 据え置きが数えられていない");

    /* 13b 他に ready が居なければ park しない (1 本のときの回帰ゼロ)。 */
    set_input_ready(&mut st, 2, false);
    set_derived_ready(&mut st, 2, true);
    set_input_ready(&mut st, 4, false);
    assert!(!multiapp::should_park(&st, 2), "13b 相手が居ないのに譲った");

    /* 13c 他に入力があれば、導出だけの自分は譲る。 */
    set_input_ready(&mut st, 4, true);
    assert!(multiapp::should_park(&st, 2), "13c 他の入力に譲らない");

    /* 13d 他も導出だけなら巡回のために譲る。 */
    set_input_ready(&mut st, 4, false);
    set_derived_ready(&mut st, 4, true);
    assert!(multiapp::should_park(&st, 2), "13d 導出どうしで譲らない");

    /* 13e 自分が ready でなければ譲る。 */
    set_derived_ready(&mut st, 2, false);
    assert!(multiapp::should_park(&st, 2), "13e ready でないのに譲らない");

    /* 13f (実物だけの分岐) WM が握っていない ID は park できない。 */
    assert!(!multiapp::should_park(&st, 1), "13f シェル帯を park しようとした");
    multiapp::on_owner_exit(3);
    assert!(!multiapp::should_park(&st, 3), "13f 未追跡の ID を park しようとした");
}

/* ---- ケース 14 相当: 導出群だけの 4 本が 1 周で全員走る ---- */
#[test]
fn derived_only_apps_each_get_exactly_one_turn_per_round() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();
    let mut id = 2;
    while id <= 5 {
        set_derived_ready(&mut st, id, true);
        id += 1;
    }
    focus_app(&mut st, 2); /* フォーカス固定。飢餓の原因にならないこと */
    multiapp::set_last_run(0);

    let mut order = [0i32; 4];
    let mut k = 0;
    while k < 4 {
        let picked = multiapp::pick(&st);
        assert!(picked >= 2 && picked <= 5, "14a {k} 周目に 1 本選べない");
        order[k] = picked;
        multiapp::mark_resumed(picked);
        multiapp::note_parked(picked, None);
        k += 1;
    }
    assert_eq!(order, [2, 3, 4, 5], "14b 巡回が ID 昇順でない: {order:?}");
    let mut seen = [0u8; 4];
    for o in order.iter() {
        seen[(*o - 2) as usize] += 1;
    }
    assert_eq!(seen, [1, 1, 1, 1], "14c 4 周で全員 1 回ずつにならない");
    assert_eq!(multiapp::pick(&st), 2, "14d 1 周したら先頭へ戻らない");
}

/* ---- ケース 15 相当: 自作入力で park を回避できない (反例 1) ----
 *  レビュアーの反例。アプリが自分の窓 2 枚へ交互に `set_focus()` すると
 *  `emit_focus_change` が旧窓と新窓の**両方の owner** へ `Focus` を流すので、
 *  そのアプリ自身に待ち行列型が湧き続ける (人間の入力は要らない)。
 *  「入力群は有限」では飢餓を止められず、止めるのは `INPUT_STREAK_MAX`。 */
#[test]
fn an_app_feeding_itself_focus_events_cannot_starve_another() {
    use crate::{mocks, multiapp, wm};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    /* N = 2 (ID 2 と 3 だけ)。窓とスロットも 2 本に絞る。 */
    st.windows[2] = wm::Win::EMPTY;
    st.windows[3] = wm::Win::EMPTY;
    st.z_count = 2;
    st.slots[2] = wm::Slot::EMPTY;
    st.slots[3] = wm::Slot::EMPTY;
    multiapp::on_start(2);
    multiapp::on_start(3);

    /* A (=2) は自分で湧かせた入力を持ち続ける。B (=3) は Paint 待ち。 */
    set_input_ready(&mut st, 2, true);
    set_derived_ready(&mut st, 3, true);
    focus_app(&mut st, 2); /* A がフォーカスを握ったまま */
    multiapp::mark_resumed(2);

    let mut cur = 2;
    let mut b_at = 0u32;
    let mut parks = 0u32;
    let mut i = 0u32;
    while i < 200 {
        let before = cur;
        cur = sched_step(&mut st, cur);
        assert_ne!(cur, 0, "15a 譲ったのに起こす相手が居ない");
        if cur != before {
            parks += 1;
        }
        if cur == 3 && b_at == 0 {
            b_at = i + 1;
        }
        i += 1;
    }
    assert!(parks > 0, "15a 自作入力を続けると park が 1 度も起きない");
    assert!(b_at > 0, "15b Paint 待ちの B が走らない (飢餓)");
    /* N=2 の上限 = (2N-2)x(STREAK+1) = 10 に**ちょうど**届く (式がタイト)。 */
    assert_eq!(b_at, 10, "15c N=2 の上限 (10) と実測がずれた");
    assert!(b_at <= multiapp::STARVE_BOUND, "15d 定数の上限を超えた");
}

/* ---- ケース 16 相当: ラウンドをまたぐ待ちの上限 (反例 2) ---- */
fn run_cross_round(focus_id: i32) -> u32 {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();
    let mut id = 2;
    while id <= 5 {
        set_input_ready(&mut st, id, true);
        id += 1;
    }
    focus_app(&mut st, 2);
    let k = multiapp::pick(&st);
    assert_eq!(k, 2, "16 前提: ラウンド先頭で A が選ばれる");
    multiapp::mark_resumed(2);
    /* A は入力を消費して Paint だけ残す = 以後ずっと導出群。
     * B/C/D は入力 ready を維持する (消費しない)。 */
    set_input_ready(&mut st, 2, false);
    set_derived_ready(&mut st, 2, true);
    focus_app(&mut st, focus_id);

    let mut cur = 2;
    let mut n = 0u32;
    while n < 200 {
        /* n = 0 が「A が park する OP_WAIT」= 起算点 (D11-3a の前提 1)。 */
        cur = sched_step(&mut st, cur);
        assert_ne!(cur, 0, "16 譲ったのに起こす相手が居ない");
        if cur == 2 {
            return n;
        }
        n += 1;
    }
    0
}

#[test]
fn a_derived_only_app_runs_within_the_re_derived_bound_across_rounds() {
    use crate::multiapp;
    let a2 = run_cross_round(2);
    let a3 = run_cross_round(3);
    let a5 = run_cross_round(5);
    assert!(a2 > 0, "16a 導出群の A が走らない (無限待ち)");
    assert!(a2 <= multiapp::STARVE_BOUND, "16b A が上限 (30) を超えた: {a2}");
    assert_eq!(
        a2,
        multiapp::STARVE_BOUND,
        "16c 上限が緩い (この構成はちょうど 30 に届くはず)"
    );
    assert!(a3 > 0 && a3 <= multiapp::STARVE_BOUND, "16d フォーカス B で超過: {a3}");
    assert!(a5 > 0 && a5 <= multiapp::STARVE_BOUND, "16e フォーカス D で超過: {a5}");
}

/* ---- 票の追加検査 1: 5 本目は ERR_FULL (契約 T2a、受入 G3) ---- */
#[test]
fn the_fifth_app_gets_err_full_from_op_init_and_the_four_survive() {
    use crate::{handler, mocks, wm};
    use os32api::gui::proto::{GUI_OP_INIT, GUI_SLOT_MAX, OS32_ERR_FULL};
    mocks::init();
    let shm = mocks::Shm::new();
    {
        let st = wm::g();
        *st = wm::GuiState::NEW;
        st.shm_base = shm.base();
        st.inited = true;
    }

    let mut k = 0;
    while k < GUI_SLOT_MAX {
        let owner = 2 + k as i32;
        assert_eq!(
            handler::gshell_gui_handler(GUI_OP_INIT, 0, owner),
            k as i32,
            "{owner} にスロット {k} が配られない"
        );
        k += 1;
    }
    assert_eq!(
        handler::gshell_gui_handler(GUI_OP_INIT, 0, 6),
        OS32_ERR_FULL,
        "5 本目が ERR_FULL でない"
    );
    /* 既存 4 本は無事 (T2a「5 本目は起動できない」= 4 本は動き続ける)。 */
    let st = wm::g();
    let mut k = 0;
    while k < GUI_SLOT_MAX {
        assert!(st.slots[k].used, "5 本目の拒否で既存スロットが壊れた");
        assert_eq!(st.slots[k].owner, 2 + k as i32);
        k += 1;
    }
    st.inited = false;
}

/* ---- 票の追加検査 2: 終了で 1 本分だけ回収 (契約 T4 / U8、受入 G2) ---- */
#[test]
fn owner_exit_reclaims_exactly_one_app_worth_of_state() {
    use crate::{handler, mocks, multiapp, timer, wm};
    use os32api::gui::proto::GUI_OP_OWNER_EXIT;
    mocks::init();
    let shm = mocks::Shm::new();
    {
        let g = wm::g();
        *g = four_app_state(&shm);
        g.inited = true;
    }
    seed_four_apps();
    /* 4 本ともタイマを 1 本ずつ持つ (U5)。 */
    let mut id = 2;
    while id <= 5 {
        let g = wm::g();
        let wi = (id - 2) as usize;
        let win = g.windows[wi].id(wi);
        assert_eq!(timer::set(g, id, win, 1, 5, true, 0), 0, "タイマが張れない");
        id += 1;
    }

    /* ID 3 だけ畳む。 */
    assert_eq!(handler::gshell_gui_handler(GUI_OP_OWNER_EXIT, 0, 3), 0);

    let g = wm::g();
    assert!(!multiapp::is_tracked(3), "畳んだ ID が表に残っている");
    assert_eq!(multiapp::live_count(), 3, "回収が 1 本分でない");
    assert!(g.slot_of_owner(3).is_none(), "ID 3 のスロットが残った");
    assert!(!g.windows[1].used, "ID 3 の窓が残った");
    assert!(!timer::has_expired(g, 3, 1000), "ID 3 のタイマが残った");
    /* 他の 3 本は 1 バイトも触られない。 */
    let mut id = 2;
    while id <= 5 {
        if id != 3 {
            assert!(multiapp::is_tracked(id), "ID {id} が巻き添えで消えた");
            assert!(g.slot_of_owner(id).is_some(), "ID {id} のスロットが消えた");
            assert!(g.windows[(id - 2) as usize].used, "ID {id} の窓が消えた");
            assert!(timer::has_expired(g, id, 1000), "ID {id} のタイマが消えた");
        }
        id += 1;
    }
    g.inited = false;
}

/* ---- 票の追加検査 3: 切替は `op_wait` の中でだけ ----
 *  `exec_park` を呼ぶ点も、そこへ入る `maybe_park` を呼ぶ点も 1 か所。
 *  「呼ばれない経路」は実行時に観測できないので、gshell の全ソースを
 *  走査して数える (後から別の場所へ足したら落ちる)。 */
#[test]
fn exec_park_has_exactly_one_call_site_and_it_is_the_op_wait_loop_head() {
    let src_dir = std::path::Path::new(file!())
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("src");
    let mut park_calls = 0;
    let mut maybe_park_calls = 0;
    let mut resume_calls = 0;
    for e in std::fs::read_dir(&src_dir).expect("src が読めない") {
        let p = e.unwrap().path();
        if p.extension().and_then(|s| s.to_str()) != Some("rs") {
            continue;
        }
        let text = std::fs::read_to_string(&p).unwrap();
        for line in text.lines() {
            /* コメント行は数えない。 */
            let t = line.trim_start();
            if t.starts_with("//") || t.starts_with('*') || t.starts_with("/*") {
                continue;
            }
            if line.contains(".exec_park)(") {
                park_calls += 1;
            }
            if line.contains(".exec_resume)(") {
                resume_calls += 1;
            }
            if line.contains("multiapp::maybe_park(") {
                maybe_park_calls += 1;
                assert_eq!(
                    p.file_name().unwrap(),
                    "handler.rs",
                    "maybe_park が handler.rs の外から呼ばれている: {p:?}"
                );
            }
        }
    }
    assert_eq!(park_calls, 1, "exec_park の呼び出し点が 1 つでない");
    assert_eq!(resume_calls, 1, "exec_resume の呼び出し点が 1 つでない");
    assert_eq!(maybe_park_calls, 1, "maybe_park の呼び出し点が 1 つでない");

    /* その 1 か所が `op_wait` の中で、`wm_cycle` より前 (= ループ先頭) にある。 */
    let handler = std::fs::read_to_string(src_dir.join("handler.rs")).unwrap();
    let body = handler
        .split("fn op_wait(")
        .nth(1)
        .expect("op_wait が見つからない");
    let body = body.split("\nfn ").next().unwrap();
    let park_at = body.find("multiapp::maybe_park(").expect("op_wait の中に無い");
    let cycle_at = body.find("wm::wm_cycle(").expect("op_wait に wm_cycle が無い");
    assert!(
        park_at < cycle_at,
        "maybe_park がループ先頭 (wm_cycle の前) に無い"
    );
}

/* ---- 票の追加検査 4: 1 本のときは `exec_park` が 0 回 (回帰ゼロ) ---- */
/// `get_tick` を 1 呼び出しごとに進める (`op_wait` の期限を切るため)。
static TICKS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
unsafe extern "C" fn ticking() -> u32 {
    TICKS.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
}

/// `op_wait` を timeout つきで 1 回回す (期限で必ず戻る)。
fn drive_op_wait(owner: i32, timeout: u32) -> i32 {
    use crate::handler;
    use os32api::gui::proto::GUI_OP_WAIT;
    TICKS.store(0, std::sync::atomic::Ordering::SeqCst);
    unsafe {
        (*os32api::api_ptr()).get_tick = ticking;
    }
    handler::gshell_gui_handler(GUI_OP_WAIT, timeout, owner)
}

#[test]
fn a_single_app_never_parks_and_keeps_the_old_wm_cycle_halt_loop() {
    use crate::{mocks, multiapp, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    let shm = mocks::Shm::new();
    {
        let g = wm::g();
        *g = four_app_state(&shm);
        /* アプリは 1 本だけ (ID 2)。他の窓とスロットは畳む。 */
        let mut k = 1;
        while k < 4 {
            g.windows[k] = wm::Win::EMPTY;
            g.slots[k] = wm::Slot::EMPTY;
            k += 1;
        }
        g.z_count = 1;
        g.inited = true;
    }
    multiapp::on_start(2);
    multiapp::mark_resumed(2);

    drive_op_wait(2, 3);
    assert_eq!(
        mocks::PARKS.load(Ordering::SeqCst),
        0,
        "アプリが 1 本なのに exec_park を呼んだ (回帰)"
    );
    /* 空回りで 0 回になったのではないこと: `wm_cycle` が 1 周でも回れば
     * 末尾の `sync_snd_focus` がフォーカス (owner 2) を音へ渡している。 */
    assert_eq!(
        mocks::snd_focus_calls(),
        vec![2],
        "op_wait のループが 1 周も回っていない (試験が空振り)"
    );
    wm::g().inited = false;
}

/* ---- 票の追加検査 5: 2 本目が ready なら `op_wait` の中で譲る ---- */
#[test]
fn op_wait_parks_when_another_app_is_ready() {
    use crate::{mocks, multiapp, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    let shm = mocks::Shm::new();
    {
        let g = wm::g();
        *g = four_app_state(&shm);
        let mut k = 2;
        while k < 4 {
            g.windows[k] = wm::Win::EMPTY;
            g.slots[k] = wm::Slot::EMPTY;
            k += 1;
        }
        g.z_count = 2;
        g.inited = true;
        g.windows[1].configure_pending = true; /* 相手 (ID 3) だけが ready */
    }
    multiapp::on_start(2);
    multiapp::on_start(3);
    multiapp::mark_resumed(2);

    drive_op_wait(2, 3);
    assert!(
        mocks::PARKS.load(Ordering::SeqCst) >= 1,
        "相手が ready なのに op_wait が譲らなかった"
    );
    wm::g().inited = false;
}

/* ---- 票の追加検査 6: フォーカス切替で `snd_focus` は 1 回だけ ----
 *  決裁 D9-4 / 受入 G10。同じフォーカスのまま何周回しても呼ばない。 */
#[test]
fn snd_focus_is_called_once_per_focus_change() {
    use crate::{mocks, multiapp, wm};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);

    multiapp::sync_snd_focus(&st); /* 最前面 = owner 5 */
    assert_eq!(mocks::snd_focus_calls(), vec![5], "初回が違う");
    multiapp::sync_snd_focus(&st);
    multiapp::sync_snd_focus(&st);
    assert_eq!(
        mocks::snd_focus_calls().len(),
        1,
        "フォーカスが動いていないのに snd_focus を呼んだ"
    );

    focus_app(&mut st, 2);
    multiapp::sync_snd_focus(&st);
    assert_eq!(
        mocks::snd_focus_calls(),
        vec![5, 2],
        "切替のたびに 1 回だけ呼ばれていない"
    );
    assert_eq!(multiapp::snd_owner(), 2);

    /* 窓が 1 枚も無くなったら音の所有権はシェル帯 (1) へ戻す。 */
    let mut k = 0;
    while k < 4 {
        st.windows[k] = wm::Win::EMPTY;
        k += 1;
    }
    st.z_count = 0;
    multiapp::sync_snd_focus(&st);
    assert_eq!(
        mocks::snd_focus_calls(),
        vec![5, 2, 1],
        "最後の窓が消えても音がアプリのままになっている"
    );
}

/* ---- 票の追加検査 7: `LAUNCH` は「1 本増やす」(契約 S2 の読み替え) ---- */
#[test]
fn launch_no_longer_quits_the_running_apps_but_switch_cui_still_does() {
    use crate::{mocks, ring, session, slot, wm};
    use os32api::gui::proto::{
        GuiEvent, GUI_EV_QUIT, GUI_RING_CAPACITY, GUI_SESSION_LAUNCH, GUI_SESSION_SWITCH_CUI,
    };

    fn quit_events(st: &wm::GuiState, slot_i: usize) -> usize {
        let h = slot::read_header(st, slot_i);
        let base = slot::ring_ptr(st, slot_i);
        let mut n = 0;
        let mut i = h.ring_head;
        while i != h.ring_tail {
            let ev: GuiEvent = unsafe {
                core::ptr::read_unaligned(
                    base.add((i as usize % GUI_RING_CAPACITY) * 16) as *const GuiEvent,
                )
            };
            if ev.kind == GUI_EV_QUIT {
                n += 1;
            }
            i = i.wrapping_add(1);
        }
        n
    }

    mocks::init();
    session::clear();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);

    /* LAUNCH: 誰にも Quit を送らない。top-level では即実行してよい。 */
    let path = b"/usr/bin/gui_demo.bin";
    assert_eq!(
        session::request(&mut st, 2, GUI_SESSION_LAUNCH, 0, path, path.len()),
        0
    );
    let mut k = 0;
    while k < 4 {
        assert_eq!(quit_events(&st, k), 0, "LAUNCH がスロット {k} を Quit させた");
        assert_eq!(ring::pending(&st, k), 0);
        k += 1;
    }
    assert!(session::ready_to_run(&st), "LAUNCH がアプリ生存で足止めされた");
    session::clear();

    /* SWITCH_CUI: GUI ごと畳むので全アプリへ Quit。回収まで実行しない。 */
    assert_eq!(
        session::request(&mut st, 2, GUI_SESSION_SWITCH_CUI, 0, b"", 0),
        0
    );
    let mut k = 0;
    while k < 4 {
        assert_eq!(quit_events(&st, k), 1, "SWITCH_CUI がスロット {k} へ届いていない");
        k += 1;
    }
    assert!(
        !session::ready_to_run(&st),
        "アプリが生きているのに SWITCH_CUI を実行しようとした"
    );
    session::clear();
}

/* ---- 票の追加検査 8: top-level は 1 周に 1 本だけ起こす ---- */
#[test]
fn resume_one_wakes_a_single_app_with_the_unread_count_as_the_wait_result() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();
    set_input_ready(&mut st, 4, true);
    focus_app(&mut st, 4);

    assert!(multiapp::resume_one(&mut st), "起こす相手が居るのに起こさない");
    let calls = mocks::resume_calls();
    assert_eq!(calls.len(), 1, "1 周で 2 本以上起こした: {calls:?}");
    /* 契約 T3: OP_WAIT の戻り値は未読件数。 */
    assert_eq!(calls[0], (4, 1), "exec_resume の引数が違う: {calls:?}");
    assert_eq!(multiapp::last_run(), 4);
    assert!(multiapp::turn_used(4), "resume で turn が使われていない");
    assert_eq!(multiapp::running(), 0, "resume から戻ったのに走ったまま");

    /* ready が 1 本も無ければ誰も起こさない (= top-level は sys_halt へ)。 */
    set_input_ready(&mut st, 4, false);
    assert!(!multiapp::resume_one(&mut st), "ready ゼロで誰かを起こした");
    assert_eq!(mocks::resume_calls().len(), 1);
}

/* ---- 票の追加検査 9: 止めてあるアプリの Quit は `exec_kill` ---- */
#[test]
fn a_parked_app_is_folded_with_exec_kill_from_the_top_level() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    seed_four_apps();

    multiapp::request_kill(4);
    assert!(multiapp::resume_one(&mut st), "kill 予約が実行されない");
    assert_eq!(mocks::kill_calls(), vec![4], "exec_kill の相手が違う");
    assert!(
        mocks::resume_calls().is_empty(),
        "kill する相手を起こしてしまった"
    );
    assert!(!multiapp::is_tracked(4), "kill した ID が表に残った");
    assert_eq!(multiapp::live_count(), 3, "kill が 1 本分でない");
}

/* ---- 票の追加検査 10: 起動したてのアプリは最初の `OP_WAIT` で譲れる ----
 *  `exec_start` は「アプリが最初に park する」まで戻らない (決裁 D9-5)。
 *  park の判断は WM の表を見るが、**その表に載る id は `exec_start` の
 *  戻り値**なので、起動中のアプリはまだ表に居ない。ここを「未追跡だから
 *  譲らない」で弾くと `exec_start` が永久に戻らず、
 *    - 2 本目以降が park できない = 1 本目が二度と起こされない
 *    - top-level へ戻れないので 3 本目の `LAUNCH` も実行できない
 *  という固まり方をする。起動が進行中の間だけ、走っている ID を表へ迎える。 */
#[test]
fn a_just_launched_app_can_park_on_its_first_op_wait() {
    use crate::{mocks, multiapp, wm};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = four_app_state(&shm);
    /* 生きているのは A (=2) だけ。B (=3) はこれから起動する。 */
    st.windows[2] = wm::Win::EMPTY;
    st.windows[3] = wm::Win::EMPTY;
    st.z_count = 2;
    st.slots[2] = wm::Slot::EMPTY;
    st.slots[3] = wm::Slot::EMPTY;
    multiapp::on_start(2);
    set_derived_ready(&mut st, 2, true); /* A は Paint 待ちで park 中 */

    /* 起動中 (`exec_start` を呼んでから戻るまで)。B の最初の OP_WAIT。 */
    multiapp::begin_start();
    assert!(
        multiapp::should_park(&st, 3),
        "起動したてのアプリが最初の OP_WAIT で譲れない (exec_start が戻らない)"
    );
    assert!(multiapp::is_tracked(3), "起動したてのアプリが表に載らない");
    multiapp::note_parked(3, None);
    multiapp::end_start(3);
    assert_eq!(multiapp::live_count(), 2, "起動で 1 本増えていない");
    assert_eq!(multiapp::running(), 0, "park したのに走ったままになっている");

    /* 対照群 1: 起動中でない未追跡 ID (CUI の入れ子の子) は譲らない。
     * カーネルの `appslot_park_check` も `!a->gui` で弾く側。 */
    multiapp::on_owner_exit(3);
    assert!(
        !multiapp::should_park(&st, 3),
        "起動中でない未追跡 ID を park しようとした"
    );

    /* 対照群 2: 1 本目の起動 (他に ready が居ない) では譲らない = 回帰ゼロ。
     * `exec_start` が従来の `exec_run` と同じく終了まで塞ぐのが正しい。 */
    multiapp::reset();
    set_derived_ready(&mut st, 2, false);
    multiapp::begin_start();
    assert!(
        !multiapp::should_park(&st, 2),
        "1 本目の起動で譲った (相手が居ないのに CR3 が動く = 回帰)"
    );
}

/* ================================================================ */
/*  不具合 W-1 (2026-09-11 の実機受入で発見)                          */
/*  park 中のアプリの露出領域が再描画されない                        */
/* ================================================================ */

/// 検査用: `set` のどれか 1 枚が `r` を丸ごと含むか。
fn covers(set: &crate::wm::RectSet, r: crate::wm::Rect) -> bool {
    let mut i = 0;
    while i < set.len {
        let s = set.rects[i];
        if s.x <= r.x && s.y <= r.y && s.right() >= r.right() && s.bottom() >= r.bottom() {
            return true;
        }
        i += 1;
    }
    false
}

/// 検査用: A (owner 2、窓 index 0) と B (owner 3、窓 index 1、A の上半分を覆う)
/// を張った状態を `wm::g()` に置く。
fn two_app_overlap_state(shm: &crate::mocks::Shm) {
    use crate::{slot, wm};
    let g = wm::g();
    *g = one_window_state(shm); /* A = owner 2、窓 index 0、slot 0 */
    g.inited = true;
    /* B の受け皿。窓は「起動中に作られた」ことにして先に張る
     * (`exec_start` のモックは窓を作れない)。A の上半分だけを覆う。 */
    g.slots[1].used = true;
    g.slots[1].owner = 3;
    slot::init_header(g, 1);
    let mut b = wm::Win::EMPTY;
    b.used = true;
    b.visible = true;
    b.owner = 3;
    b.gen = 1;
    b.x = 40;
    b.y = 40;
    b.w = 400;
    b.h = 120;
    g.windows[1] = b;
    g.zorder[1] = 1; /* B が前面 */
    g.z_count = 2;
}

/// A (park 中、1 窓) の上に B の窓が開くと、A の**露出部**が黒いまま残る
/// (実機 `gui_bench` + `gui_demo`、`ring3_switch_count` が動かない)。
///
/// 仕掛けは `run_program` の `gfx_init()`。`gfx/gfx_core.c` の `gfx_init` は
/// **VRAM の両ページをゼロクリアする**。`exec_run` の時代はアプリが終わって
/// からしか戻らなかったので消えるのは死んだアプリの画だけだったが、
/// `exec_start` は park した時点で戻る = **生きているアプリのクライアント面
/// まで消える**。WM はクライアント面を持たない (契約 G4) ので、消したら本人に
/// 描き直させるしか無い。ところが遮蔽は露出を生まないので
/// `recompute_and_expose` は dirty を 1 つも足さず、`derived_ready` が偽の
/// まま = `pick` が A を選ばない = 誰も `exec_resume` しない。
///
/// 検査は「画が残っている」か「全面 dirty で描き直させる」かの**どちらかは
/// 成り立つ**こと。どちらでもないのが W-1 の状態。
#[test]
fn a_parked_app_is_not_left_black_when_another_app_is_launched() {
    use crate::{mocks, multiapp, visible, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    let shm = mocks::Shm::new();
    two_app_overlap_state(&shm);
    multiapp::on_start(2);

    /* A は全面を描き終えて COMMIT 済み = dirty 無しで park している。 */
    visible::recompute_and_expose(wm::g());
    wm::g().windows[0].dirty.clear();
    wm::g().windows[0].configure_pending = false;
    multiapp::note_parked(2, None);
    assert!(
        !multiapp::derived_ready(wm::g(), 2),
        "前提が崩れている: A が最初から ready"
    );

    /* A のクライアント面に目印を塗る (= アプリが描いた画)。露出部の標本は
     * B に覆われない下側から取る。 */
    let cr = wm::g().windows[0].client_rect_screen();
    let (sx, sy) = (cr.x + 8, cr.bottom() - 8);
    assert!(
        !wm::g().windows[1].outer().contains(sx, sy),
        "標本点が B に隠れている (試験の geometry が違う)"
    );
    unsafe { os32api::gfx::gfx_fill_rect(cr.x, cr.y, cr.w, cr.h, 7) };
    assert_eq!(mocks::gfx_get_pixel(sx, sy), 7);

    /* B を起動する (`exec_start` は park 済みの app_id 3 を返す)。 */
    *mocks::START_SCRIPT.lock().unwrap() = vec![3];
    let mut path = [0u8; 256];
    let p = b"/usr/bin/gui_demo.bin\0";
    path[..p.len()].copy_from_slice(p);
    let rc = crate::run_program(wm::g(), &path);
    assert_eq!(rc, 3, "exec_start の戻り値を取り違えている");
    assert_eq!(multiapp::live_count(), 2, "起動で 1 本増えていない");

    /* (1) 画が残っているか、残っていないなら全面 dirty で描き直させるか。 */
    let kept = mocks::gfx_get_pixel(sx, sy) == 7;
    let (cw, ch) = wm::g().windows[0].client_size();
    let repaint = covers(&wm::g().windows[0].dirty, crate::wm::Rect::new(0, 0, cw, ch));
    assert!(
        kept || repaint,
        "W-1: 起動で A の画が消えた (gfx_init {} 回) のに描き直させない (dirty len={})",
        mocks::GFX_INITS.load(Ordering::SeqCst),
        wm::g().windows[0].dirty.len
    );

    /* (2) 描き直しが要るなら、A は導出群の ready = top-level が起こす相手。 */
    if !kept {
        assert!(
            multiapp::derived_ready(wm::g(), 2),
            "W-1: 描き直しが要るのに A が ready にならない (誰も起こさない)"
        );
        assert_eq!(multiapp::pick(wm::g()), 2, "W-1: top-level が A を選ばない");
    }
    wm::g().inited = false;
}

/// 走っている B は、park 中の A に配送できる `Paint` がある間は `OP_WAIT` で
/// 譲る。譲らないと A は永久に描き直せない (D11-3 の (1))。
/// 併せて「A が `Paint` を消費したら dirty が残らない」「B の窓を閉じたら
/// A の露出部が dirty になり、また A が選ばれる」も見る。
#[test]
fn a_running_app_yields_to_a_parked_app_that_has_a_deliverable_paint() {
    use crate::{handler, mocks, multiapp, visible, wm};
    use os32api::gui::proto::{GuiEvent, GUI_EV_PAINT, GUI_OP_POLL, GUI_RING_CAPACITY};
    mocks::init();
    let shm = mocks::Shm::new();
    two_app_overlap_state(&shm);
    multiapp::on_start(2);
    multiapp::on_start(3);
    visible::recompute_and_expose(wm::g());
    wm::g().windows[0].dirty.clear();
    wm::g().windows[0].configure_pending = false;
    wm::g().windows[1].dirty.clear();
    wm::g().windows[1].configure_pending = false;
    multiapp::note_parked(2, None);
    multiapp::mark_resumed(3); /* B が走っている */

    /* (1) A に配送できる Paint がある = B は譲る。 */
    crate::damage::set_dirty_full(&mut wm::g().windows[0]);
    assert!(
        multiapp::derived_ready(wm::g(), 2),
        "露出部に dirty があるのに A が ready でない"
    );
    assert!(
        multiapp::should_park(wm::g(), 3),
        "A に配送できる Paint があるのに B が譲らない"
    );

    /* (2) A を起こして `OP_POLL` させると Paint が配られ、dirty は残らない。 */
    assert_eq!(multiapp::pick(wm::g()), 2, "top-level が A を選ばない");
    multiapp::mark_resumed(2);
    let n = handler::gshell_gui_handler(GUI_OP_POLL, 0, 2);
    assert!(n > 0, "OP_POLL が Paint を 1 件も返さない");
    /* 残ってよいのは **B に隠れている分だけ** (契約 G4: 隠れた場所は露出する
     * まで dirty のまま)。配送できる分が残っていたら配り落としている。 */
    assert!(
        !crate::damage::has_deliverable_paint(&wm::g().windows[0]),
        "Paint を配ったのに配送できる dirty が残った (len={})",
        wm::g().windows[0].dirty.len
    );
    /* 配られたのが Paint であること。 */
    let mut seen = false;
    {
        let h = crate::slot::read_header(wm::g(), 0);
        let base = crate::slot::ring_ptr(wm::g(), 0);
        let mut i = h.ring_head;
        while i != h.ring_tail {
            let ev: GuiEvent = unsafe {
                core::ptr::read_unaligned(
                    base.add((i as usize % GUI_RING_CAPACITY) * 16) as *const GuiEvent,
                )
            };
            if ev.kind == GUI_EV_PAINT {
                seen = true;
            }
            i = i.wrapping_add(1);
        }
    }
    assert!(seen, "配られたイベントに Paint が無い");
    set_input_ready(wm::g(), 2, false); /* アプリが読み切った */
    assert!(
        !multiapp::derived_ready(wm::g(), 2),
        "Paint を消費したのに A がまだ ready"
    );

    /* (3) B の窓を閉じると A の露出部が dirty になり、また A が選ばれる。 */
    multiapp::note_parked(2, None);
    multiapp::mark_resumed(3);
    let bid = wm::g().windows[1].id(1);
    assert_eq!(wm::destroy_window(wm::g(), 3, bid), 0, "B の窓を閉じられない");
    assert!(
        multiapp::derived_ready(wm::g(), 2),
        "B の窓を閉じたのに A の露出部が dirty にならない"
    );
    assert!(
        multiapp::should_park(wm::g(), 3),
        "露出した A を描かせるために B が譲らない"
    );
    assert_eq!(multiapp::pick(wm::g()), 2, "露出した A が選ばれない");
    wm::g().inited = false;
}

/* ================================================================ */
/*  W-2 — K5c への追随 (ユーザー決裁 2026-09-11 A1 / A3)             */
/*                                                                  */
/*  A1: CTRL+STOP の宛先はフォーカス窓のアプリ (契約 T6)。カーネルは  */
/*      IRQ1 で「走っているアプリ」にしか要求を立てられないので、     */
/*      フォーカスが別のアプリなら WM が `exec_abort_clear` で本人の  */
/*      要求を降ろし、フォーカス窓の ID を `exec_kill` で畳む。       */
/*      どちらも owner 1 (top-level) からしか呼べない (K5c) ので、    */
/*      `op_wait` の中では**予約するだけ**で、park で top-level へ    */
/*      戻ったところで実行する。                                     */
/*  A3: `SWITCH_CUI` / `SHUTDOWN` で Quit に応答しないアプリは        */
/*      `QUIT_GRACE_CYCLES` 周待ってから畳む。                       */
/* ================================================================ */

/// アプリ 2 本 (owner 2, 3) をグローバルの `GuiState` に載せる。
/// Z 順は 0,1 なので最前面 = 窓 1 = owner 3。
fn two_app_global(shm: &crate::mocks::Shm) {
    use crate::{multiapp, wm};
    let g = wm::g();
    *g = four_app_state(shm);
    let mut k = 2;
    while k < 4 {
        g.windows[k] = wm::Win::EMPTY;
        g.slots[k] = wm::Slot::EMPTY;
        k += 1;
    }
    g.z_count = 2;
    g.inited = true;
    multiapp::on_start(2);
    multiapp::on_start(3);
    multiapp::mark_resumed(2); /* 走っているのは 2 */
}

/* ---- (a) フォーカスが本人なら abort は取り消さない ---- */
#[test]
fn ctrl_stop_with_focus_on_the_running_app_keeps_the_kernel_abort() {
    use crate::{mocks, multiapp, session, wm};
    mocks::init();
    session::clear(); /* 前の試験の SessionAction を持ち越さない */
    let shm = mocks::Shm::new();
    two_app_global(&shm);
    focus_app(wm::g(), 2); /* フォーカス = 走っている本人 */
    wm::g().abort_seen = true;

    drive_op_wait(2, 3);

    assert_eq!(
        mocks::abort_clear_calls(),
        0,
        "フォーカスが本人なのに exec_abort_clear を呼んだ (本人が畳まれない)"
    );
    assert!(
        mocks::kill_calls().is_empty(),
        "フォーカスが本人なのに exec_kill を予約した: {:?}",
        mocks::kill_calls()
    );
    assert!(
        !multiapp::pending_top_level_work(),
        "top-level の予約が残った (フォーカス = 本人は現行どおりのはず)"
    );
    wm::g().inited = false;
}

/* ---- (b) フォーカスが別アプリなら取り消し + フォーカス窓を kill ---- */
#[test]
fn ctrl_stop_with_focus_on_another_app_clears_the_abort_and_kills_the_focused_one() {
    use crate::{mocks, multiapp, session, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    session::clear(); /* 前の試験の SessionAction を持ち越さない */
    let shm = mocks::Shm::new();
    two_app_global(&shm);
    focus_app(wm::g(), 3); /* フォーカス = 別のアプリ */
    wm::g().abort_seen = true;

    drive_op_wait(2, 3);

    /* `op_wait` の中では owner がアプリ ID なので KAPI は呼べない (K5c)。 */
    assert_eq!(
        mocks::abort_clear_calls(),
        0,
        "op_wait の中から exec_abort_clear を呼んだ (owner 1 でないので ERR_INVAL)"
    );
    assert!(
        multiapp::pending_top_level_work(),
        "取り消しと kill が top-level へ持ち越されていない"
    );
    /* 持ち越すには top-level へ戻る = 譲るしかない。 */
    assert!(
        mocks::PARKS.load(Ordering::SeqCst) >= 1,
        "予約を積んだのに top-level へ戻ろうとしていない (永久に実行されない)"
    );

    /* top-level の 1 周 (単独ループの `resume_one`)。 */
    assert!(multiapp::resume_one(wm::g()), "top-level が予約を実行しない");
    assert_eq!(
        mocks::abort_clear_calls(),
        1,
        "exec_abort_clear がちょうど 1 回でない"
    );
    assert_eq!(
        mocks::kill_calls(),
        vec![3],
        "畳む相手がフォーカス窓の owner でない"
    );
    assert!(multiapp::is_tracked(2), "走っている本人まで畳んでしまった");
    assert!(!multiapp::is_tracked(3), "kill した ID が表に残った");
    wm::g().inited = false;
}

/* ---- (c) SWITCH_CUI: Quit に応答しないアプリは N 周後に畳む ---- */
#[test]
fn switch_cui_folds_an_app_that_ignores_quit_after_the_grace_cycles() {
    use crate::{mocks, multiapp, session, wm};
    use os32api::gui::proto::GUI_SESSION_SWITCH_CUI;
    mocks::init();
    session::clear(); /* 前の試験の SessionAction を持ち越さない */
    let shm = mocks::Shm::new();
    two_app_global(&shm);

    /* 全アプリへ Quit を配る (契約 S5)。 */
    assert_eq!(session::set_wm(wm::g(), GUI_SESSION_SWITCH_CUI, b"\0"), 0);
    assert_eq!(
        session::quit_grace_left(),
        session::QUIT_GRACE_CYCLES,
        "Quit を配ったのに猶予が始まっていない"
    );

    /* アプリ 3 は Quit に応じて終了した (回収は `gui_owner_exit` 経由)。 */
    wm::g().reclaim_owner(3);
    session::reclaim_owner(3);
    multiapp::on_owner_exit(3);

    /* アプリ 2 は Quit を無視して待ち続ける。N-1 周ではまだ畳まない。 */
    let mut n = 0;
    while n < session::QUIT_GRACE_CYCLES - 1 {
        session::x3_cycle(wm::g());
        n += 1;
    }
    assert!(
        !multiapp::pending_top_level_work(),
        "猶予が切れる前に畳もうとした ({n} 周)"
    );

    /* N 周目で打ち切り。 */
    session::x3_cycle(wm::g());
    assert!(
        multiapp::pending_top_level_work(),
        "N 周待っても応答しないアプリが畳まれない (SWITCH_CUI が永久に成立しない)"
    );
    assert!(multiapp::resume_one(wm::g()), "top-level が kill を実行しない");
    assert_eq!(
        mocks::kill_calls(),
        vec![2],
        "応答したアプリまで畳んだ / 相手が違う"
    );
    assert_eq!(multiapp::live_count(), 0, "全回収になっていない");
    wm::g().inited = false;
}

/* ---- (c') 全員が応答したら誰も畳まない ---- */
#[test]
fn switch_cui_kills_nobody_when_every_app_answers_the_quit() {
    use crate::{mocks, multiapp, session, wm};
    use os32api::gui::proto::GUI_SESSION_SWITCH_CUI;
    mocks::init();
    session::clear(); /* 前の試験の SessionAction を持ち越さない */
    let shm = mocks::Shm::new();
    two_app_global(&shm);
    assert_eq!(session::set_wm(wm::g(), GUI_SESSION_SWITCH_CUI, b"\0"), 0);

    let mut id = 2;
    while id <= 3 {
        wm::g().reclaim_owner(id);
        session::reclaim_owner(id);
        multiapp::on_owner_exit(id);
        id += 1;
    }
    let mut n = 0;
    while n < session::QUIT_GRACE_CYCLES + 2 {
        session::x3_cycle(wm::g());
        n += 1;
    }
    assert_eq!(
        session::quit_grace_left(),
        0,
        "全員が応答したのに猶予が走り続けている"
    );
    assert!(
        mocks::kill_calls().is_empty(),
        "応答したアプリを畳んだ: {:?}",
        mocks::kill_calls()
    );
    assert!(session::ready_to_run(wm::g()), "全回収なのに SWITCH_CUI が実行できない");
    wm::g().inited = false;
}

/* ---- (d) 1 本のときは現行と同じ経路 (kill 0 回) ---- */
#[test]
fn a_single_app_keeps_the_old_ctrl_stop_path() {
    use crate::{mocks, multiapp, session, wm};
    use std::sync::atomic::Ordering;
    mocks::init();
    session::clear(); /* 前の試験の SessionAction を持ち越さない */
    let shm = mocks::Shm::new();
    {
        let g = wm::g();
        *g = four_app_state(&shm);
        let mut k = 1;
        while k < 4 {
            g.windows[k] = wm::Win::EMPTY;
            g.slots[k] = wm::Slot::EMPTY;
            k += 1;
        }
        g.z_count = 1;
        g.inited = true;
        g.abort_seen = true;
    }
    multiapp::on_start(2);
    multiapp::mark_resumed(2);

    drive_op_wait(2, 3);

    /* 1 本しか居なければ「フォーカス = 本人」なので、抜けてカーネルに
     * 畳ませる現行の経路そのまま。KAPI は 1 つも増えない。 */
    assert_eq!(mocks::abort_clear_calls(), 0, "1 本なのに exec_abort_clear を呼んだ");
    assert!(
        mocks::kill_calls().is_empty(),
        "1 本なのに exec_kill を呼んだ: {:?}",
        mocks::kill_calls()
    );
    assert_eq!(
        mocks::PARKS.load(Ordering::SeqCst),
        0,
        "1 本なのに exec_park を呼んだ (回帰)"
    );
    assert!(!multiapp::pending_top_level_work(), "1 本なのに予約が積まれた");
    wm::g().inited = false;
}

/* ================================================================ */
/*  票 K7-W — 鍵待ち (WAIT_KEY) の CUI アプリ (指摘 A / C)           */
/*                                                                  */
/*  端末アプリ経由で走る CUI プログラムは `OP_INIT` を通らないので   */
/*  スロットも窓も持たない。止まる理由は `kbd_getchar` の待ち        */
/*  (`APP_STATE_WAIT_KEY`) だけで、起こしてよいかは注入リングの      */
/*  未読バイト数 (`kbd_inject_pending`) が決める。                   */
/* ================================================================ */

/// カーネル `exec/appslot.h` の `APP_STATE_*` (`exec_app_state` の答え)。
const ST_PARKED: i32 = 2;
const ST_WAIT_KEY: i32 = 3;

/* ---- K7-W 検査 1: pending 0 では起こさず、しかし忘れもしない ---- */
#[test]
fn a_slotless_app_waiting_for_a_key_is_never_forgotten() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_gui_app_state(&shm);
    multiapp::on_start(2); /* GUI アプリ (スロット 0) */
    multiapp::on_start(3); /* 端末から起動した CUI (スロット無し) */
    mocks::set_app_state(2, ST_PARKED);
    mocks::set_app_state(3, ST_WAIT_KEY);

    /* 注入リングが空 = 起床の理由が無い (指摘 C)。 */
    mocks::set_kbd_pending(0);
    assert!(!multiapp::input_ready(&st, 3), "pending 0 なのに入力群になった");
    assert_eq!(multiapp::pick(&st), 0, "pending 0 の WAIT_KEY を選んだ");
    assert!(!multiapp::resume_one(&mut st), "pending 0 で誰かを起こした");
    assert!(
        mocks::resume_calls().is_empty(),
        "pending 0 で exec_resume を呼んだ: {:?}",
        mocks::resume_calls()
    );
    /* 指摘 A: スロットが無いからといって表から落としてはならない。 */
    assert!(multiapp::is_tracked(3), "鍵待ちのアプリを forget してしまった");
    assert!(mocks::kill_calls().is_empty(), "鍵待ちのアプリを畳んでしまった");
}

/* ---- K7-W 検査 2: pending > 0 なら入力群として選ばれ resume される ---- */
#[test]
fn a_slotless_app_waiting_for_a_key_is_resumed_when_a_byte_is_injected() {
    use crate::{mocks, multiapp};
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_gui_app_state(&shm);
    multiapp::on_start(2);
    multiapp::on_start(3);
    mocks::set_app_state(2, ST_PARKED);
    mocks::set_app_state(3, ST_WAIT_KEY);
    mocks::set_kbd_pending(1);

    assert!(multiapp::input_ready(&st, 3), "注入があるのに入力群でない");
    assert_eq!(multiapp::pick(&st), 3, "注入があるのに WAIT_KEY を選ばない");
    assert!(multiapp::resume_one(&mut st), "起こす相手が居るのに起こさない");
    let calls = mocks::resume_calls();
    assert_eq!(calls.len(), 1, "1 周で 2 本以上起こした: {calls:?}");
    /* 指摘 B: 文字はカーネルが `wait_ret` を上書きして渡すので WM は 0。 */
    assert_eq!(calls[0], (3, 0), "exec_resume の引数が違う: {calls:?}");
    assert!(multiapp::is_tracked(3), "resume したのに表から落ちた");
    assert!(mocks::kill_calls().is_empty(), "resume できたのに畳んだ");
}

/* ---- K7-W 検査 3: `OS32_ERR_AGAIN` はその周を譲るだけ ---- */
#[test]
fn an_again_from_exec_resume_yields_the_round_without_folding_the_app() {
    use crate::{mocks, multiapp};
    use os32api::gui::proto::OS32_ERR_AGAIN;
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_gui_app_state(&shm);
    multiapp::on_start(2);
    multiapp::on_start(3);
    mocks::set_app_state(2, ST_PARKED);
    mocks::set_app_state(3, ST_WAIT_KEY);
    mocks::set_kbd_pending(1);
    *mocks::RESUME_SCRIPT.lock().unwrap() = vec![OS32_ERR_AGAIN];
    let last_before = multiapp::last_run();

    assert!(multiapp::resume_one(&mut st), "AGAIN の周が「何もしない」になった");
    assert_eq!(mocks::resume_calls(), vec![(3, 0)], "resume を呼んでいない");
    /* 負値だからといって「起こせない本」として畳んではならない。 */
    assert!(mocks::kill_calls().is_empty(), "AGAIN で exec_kill を呼んだ");
    assert!(multiapp::is_tracked(3), "AGAIN で表から落とした");
    /* 譲るだけ = turn も巡回の起点も動かさない (streak に数えない)。 */
    assert!(!multiapp::turn_used(3), "AGAIN が turn を使った");
    assert_eq!(multiapp::last_run(), last_before, "AGAIN が巡回の起点を動かした");
    assert_eq!(multiapp::running(), 0, "AGAIN から戻ったのに走ったまま");
}

/* ---- K7-W 検査 4: 鍵待ちでないスロット無しは従来どおり忘れる ---- */
#[test]
fn a_slotless_app_that_is_not_waiting_for_a_key_is_still_forgotten() {
    use crate::{mocks, multiapp};
    use crate::wm;
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = one_gui_app_state(&shm);
    /* owner 3 の窓だけがあってスロットは無い (`OP_INIT` 前 / 回収済み)。
     * `Configure` 未通知で導出群に入るので `pick` が選ぶ。 */
    let mut w = wm::Win::EMPTY;
    w.used = true;
    w.visible = true;
    w.owner = 3;
    w.gen = 1;
    w.x = 200;
    w.y = 10;
    w.w = 100;
    w.h = 80;
    w.configure_pending = true;
    st.windows[1] = w;
    st.zorder[1] = 1;
    st.z_count = 2;
    multiapp::on_start(2);
    multiapp::on_start(3);
    mocks::set_app_state(2, ST_PARKED);
    mocks::set_app_state(3, ST_PARKED); /* 鍵待ちではない */
    mocks::set_kbd_pending(0);

    assert!(multiapp::derived_ready(&st, 3), "導出群になっていない");
    assert_eq!(multiapp::pick(&st), 3, "導出群の 1 本を選ばない");
    assert!(multiapp::resume_one(&mut st), "1 周で何もしなかった");
    assert!(
        mocks::resume_calls().is_empty(),
        "スロットの無い非鍵待ちを起こした: {:?}",
        mocks::resume_calls()
    );
    assert!(!multiapp::is_tracked(3), "スロットの無い非鍵待ちが表に残った");
}
