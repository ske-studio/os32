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
