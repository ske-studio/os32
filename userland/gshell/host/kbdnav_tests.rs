//! kbdnav_tests.rs — 票 KBD_NAV の受け入れ K1 (ホスト試験)。
//!
//! `src/kbdnav.rs` の末尾から `#[cfg(test)] #[path]` で取り込み、
//! `host/integration.py` (`make check-gshell-host`) が走らせる。gshell の入力部を
//! **偽の raw 列** (`mocks::push_rawkeys`) で駆動し、アプリのリングに積まれた
//! イベントで判定する。項目番号は票 §4 の K1 ①〜⑦ (+ ⑧ CTRL の半分速度)。

use crate::wm::{GuiState, Win};
use crate::{fep, input, kbdnav, mocks, modal, slot, startmenu, wm};
use os32api::gui::proto::{
    GuiEvent, GUI_EV_BUTTON, GUI_EV_FOCUS, GUI_EV_KEY, GUI_EV_POINTER, GUI_EV_TEXT,
    GUI_MODAL_YES_NO, GUI_RING_CAPACITY,
};

/* ---- raw の組み立て (drivers/kbd.c: keycode | down<<8 | mods<<9) ---- */
const DOWN: i32 = 1 << 8;
const SHIFT: i32 = 0x01 << 9;
const KANA: i32 = 0x04 << 9;
const GRPH: i32 = 0x08 << 9;
const CTRL: i32 = 0x10 << 9;

const SC_ESC: i32 = 0x00;
const SC_TAB: i32 = 0x0F;
const SC_A: i32 = 0x1D;
const SC_B: i32 = 0x2D;
const SC_N: i32 = 0x2E;
const SC_I: i32 = 0x17;
const SC_H: i32 = 0x22;
const SC_SPACE: i32 = 0x34;
const SC_F4: i32 = 0x65;
const SC_F10: i32 = 0x6B;
const SC_KANA: i32 = 0x72;
const SC_GRPH: i32 = 0x73;
const NP_6: i32 = 0x48;
const NP_5: i32 = 0x47;
const NP_PLUS: i32 = 0x49;
const NP_0: i32 = 0x4E;
const NP_DOT: i32 = 0x50;

/// make + break (同じ修飾)。
fn tap(scan: i32, mods: i32) -> [i32; 2] {
    [scan | DOWN | mods, scan | mods]
}

/* ---- 状態の組み立て ---- */

/// 窓を `rects` の順に作る (添字 i = 所有者 2+i、スロット i)。Z 順も同じ順
/// (最後が最前面)。MRU も最前面が先頭になるように積む。
fn windows(shm: &mocks::Shm, rects: &[(i32, i32, i32, i32)]) -> GuiState {
    let mut st = GuiState::NEW;
    st.shm_base = shm.base();
    let mut i = 0;
    while i < rects.len() {
        st.slots[i].used = true;
        st.slots[i].owner = 2 + i as i32;
        slot::init_header(&st, i);
        let mut w = Win::EMPTY;
        w.used = true;
        w.visible = true;
        w.owner = 2 + i as i32;
        w.gen = 1;
        w.flags = os32api::gui::proto::GUI_WF_DEFAULT;
        w.x = rects[i].0;
        w.y = rects[i].1;
        w.w = rects[i].2;
        w.h = rects[i].3;
        st.windows[i] = w;
        st.zorder[i] = i;
        i += 1;
    }
    st.z_count = rects.len();
    let mut k = 0;
    while k < rects.len() {
        let id = st.windows[k].id(k);
        kbdnav::mru_touch(&mut st, id);
        k += 1;
    }
    st
}

/// FEP をオフに戻す (静的な FEP 状態を前後の試験へ持ち越さない)。
fn fep_off() {
    unsafe extern "C" fn off() -> i32 {
        0
    }
    unsafe {
        (*os32api::api_ptr()).ime_is_active = off;
    }
    fep::install();
}

/// ポインタを (x, y) に置く (実マウスをそこへ動かして 1 周)。リングは空にする。
fn park_pointer(st: &mut GuiState, x: i32, y: i32) {
    *mocks::MOUSE.lock().unwrap() = (x as i16, y as i16, 0);
    input::capture(st, input::Ctx::Wait);
    clear_rings(st);
}

fn clear_rings(st: &GuiState) {
    let mut i = 0;
    while i < 4 {
        if st.slots[i].used {
            slot::init_header(st, i);
        }
        i += 1;
    }
}

fn events(st: &GuiState, slot_i: usize) -> Vec<GuiEvent> {
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
        out.push(ev);
        i = i.wrapping_add(1);
    }
    out
}

/// (down, button) の列。
fn buttons(st: &GuiState, slot_i: usize) -> Vec<(bool, u8)> {
    events(st, slot_i)
        .iter()
        .filter(|e| e.kind == GUI_EV_BUTTON)
        .map(|e| (e.sub != 0, e.payload[4]))
        .collect()
}

/// (in?, window) の列。
fn focus(st: &GuiState, slot_i: usize) -> Vec<(bool, u32)> {
    events(st, slot_i)
        .iter()
        .filter(|e| e.kind == GUI_EV_FOCUS)
        .map(|e| (e.sub != 0, e.window))
        .collect()
}

/// (down, scan, ch, mods) の列。
fn keys(st: &GuiState, slot_i: usize) -> Vec<(bool, u8, u8, u8)> {
    events(st, slot_i)
        .iter()
        .filter(|e| e.kind == GUI_EV_KEY)
        .map(|e| (e.sub != 0, e.payload[0], e.payload[1], e.payload[2]))
        .collect()
}

fn text(st: &GuiState, slot_i: usize) -> Vec<u8> {
    let mut out = Vec::new();
    for e in events(st, slot_i) {
        if e.kind == GUI_EV_TEXT {
            let n = ((e.sub & 0x7F) as usize).min(8);
            out.extend_from_slice(&e.payload[..n]);
        }
    }
    out
}

fn run(st: &mut GuiState, raws: &[i32]) {
    mocks::push_rawkeys(raws);
    input::capture(st, input::Ctx::Wait);
}

fn id(st: &GuiState, i: usize) -> u32 {
    st.windows[i].id(i)
}

/* ================================================================ */
/*  ① GRPH+TAB — GRPH を離したときに 1 回だけ切り替える (MRU 順)     */
/* ================================================================ */

/// MRU が A(前)・B・C(最小化) のとき GRPH↓ TAB TAB GRPH↑ → GRPH↑ の時点で
/// 1 回だけ C が元に戻って前、Focus は A→C の 1 回だけ (B へは出ない)。
#[test]
fn k1_1_grph_tab_switches_once_on_release_in_mru_order() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    /* 添字 0 = C (最背面)、1 = B、2 = A (最前面)。 */
    let mut st = windows(&shm, &[(20, 20, 200, 120), (60, 60, 200, 120), (100, 100, 200, 120)]);
    let (c, b, a) = (0usize, 1usize, 2usize);
    wm::minimize(&mut st, c);
    assert!(st.windows[c].minimized && !st.windows[c].visible);
    assert_eq!(st.front_index(), Some(a));
    park_pointer(&mut st, 600, 10);

    let mut r = vec![SC_GRPH | DOWN | GRPH];
    r.extend_from_slice(&tap(SC_TAB, GRPH));
    /* 途中 (2 回目の TAB の後、GRPH↑ の前) で止めて「まだ何も動いていない」を見る。 */
    r.extend_from_slice(&tap(SC_TAB, GRPH));
    run(&mut st, &r);
    assert_eq!(st.front_index(), Some(a), "GRPH を離す前に切り替わった");
    assert_eq!(kbdnav::switch_selection(&st), Some(id(&st, c)), "選択が C でない");
    assert_eq!(crate::taskbar::pressed_id(&st), id(&st, c), "タスクバーの押し込みが選択を示さない");
    assert!(focus(&st, a).is_empty() && focus(&st, b).is_empty() && focus(&st, c).is_empty());

    run(&mut st, &[SC_GRPH]);
    assert_eq!(st.front_index(), Some(c), "GRPH↑ で C へ切り替わらない");
    assert!(!st.windows[c].minimized && st.windows[c].visible, "C が元に戻っていない");
    assert_eq!(focus(&st, a), vec![(false, id(&st, a))], "A の Focus out が 1 回でない");
    assert_eq!(focus(&st, c), vec![(true, id(&st, c))], "C の Focus in が 1 回でない");
    assert!(focus(&st, b).is_empty(), "B へ Focus が出た: {:?}", focus(&st, b));
    /* TAB の Key はどこへも配られない。 */
    for s in 0..3 {
        assert!(keys(&st, s).is_empty(), "WM のキーがアプリへ漏れた: slot {s}");
    }
}

/// 途中で ESC → 何も変わらない (ESC の make / break もアプリへ行かない)。
#[test]
fn k1_1_grph_tab_esc_cancels() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (60, 60, 200, 120), (100, 100, 200, 120)]);
    wm::minimize(&mut st, 0);
    park_pointer(&mut st, 600, 10);
    let mut r = vec![SC_GRPH | DOWN | GRPH];
    r.extend_from_slice(&tap(SC_TAB, GRPH));
    r.extend_from_slice(&tap(SC_TAB, GRPH));
    r.extend_from_slice(&tap(SC_ESC, GRPH));
    r.push(SC_GRPH);
    run(&mut st, &r);
    assert_eq!(st.front_index(), Some(2), "ESC で取り消したのに切り替わった");
    assert!(st.windows[0].minimized, "取り消したのに C が戻った");
    for s in 0..3 {
        assert!(focus(&st, s).is_empty(), "Focus が出た: slot {s}");
        assert!(keys(&st, s).is_empty(), "キーがアプリへ漏れた: slot {s}");
    }
    assert_eq!(kbdnav::switch_selection(&st), None);
}

/// 初期状態 A・B で GRPH↓ TAB GRPH↑ を 2 回 → B → A と戻る (MRU)。
#[test]
fn k1_1_grph_tab_twice_goes_back_and_forth() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    /* 0 = B、1 = A (最前面)。 */
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    park_pointer(&mut st, 600, 10);
    let one = [SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH, SC_GRPH];
    run(&mut st, &one);
    assert_eq!(st.front_index(), Some(0), "1 回目で B にならない");
    run(&mut st, &one);
    assert_eq!(st.front_index(), Some(1), "2 回目で A に戻らない (MRU でない)");
    /* SHIFT+TAB は逆回り: 2 枚なら同じく相手へ。 */
    run(
        &mut st,
        &[SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH | SHIFT, SC_TAB | GRPH | SHIFT, SC_GRPH],
    );
    assert_eq!(st.front_index(), Some(0), "SHIFT+TAB で切り替わらない");
}

/// 最小化した窓は Z 順の途中に居ても MRU の末尾 (Win98 と同じ)。
/// Z 順 (前から) で並べる実装だと 1 回目の TAB が最小化した窓に当たる。
#[test]
fn k1_1_minimized_window_is_last_in_mru_even_if_higher_in_z() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    /* Z: 0 = B (最背面)、1 = C、2 = A (最前面)。C を最小化 → MRU は A・B・C。 */
    let mut st = windows(&shm, &[(20, 20, 200, 120), (60, 60, 200, 120), (100, 100, 200, 120)]);
    wm::minimize(&mut st, 1);
    park_pointer(&mut st, 600, 10);
    run(&mut st, &[SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH, SC_GRPH]);
    assert_eq!(st.front_index(), Some(0), "1 回目の TAB が MRU の 2 番目 (B) でない");
    assert!(st.windows[1].minimized, "最小化した窓が戻った");
}

/* ================================================================ */
/*  ② モーダル中: §1-2 の表の全キーでアプリのリングに何も積まれない  */
/* ================================================================ */

#[test]
fn k1_2_modal_swallows_wm_keys_and_grph_f4_cancels_the_dialog() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    park_pointer(&mut st, 600, 10);
    modal::open_wm_message(
        &mut st,
        GUI_MODAL_YES_NO,
        b"halt?\0",
        modal::WM_PURPOSE_CONFIRM_HALT,
    );
    assert!(modal::is_open());
    let mut r = Vec::new();
    r.extend_from_slice(&tap(SC_ESC, CTRL));
    r.push(SC_GRPH | DOWN | GRPH);
    r.extend_from_slice(&tap(SC_TAB, GRPH));
    r.extend_from_slice(&tap(SC_ESC, GRPH));
    r.extend_from_slice(&tap(SC_SPACE, GRPH));
    r.push(SC_GRPH);
    r.extend_from_slice(&tap(SC_F10, SHIFT));
    run(&mut st, &r);
    assert!(modal::is_open(), "GRPH+f･4 の前にダイアログが閉じた");
    assert!(!startmenu::is_open(), "モーダル中に CTRL+ESC でメニューが開いた");
    assert_eq!(st.front_index(), Some(1), "モーダル中に窓が切り替わった");
    for s in 0..2 {
        assert!(events(&st, s).is_empty(), "アプリのリングに積まれた: slot {s}");
    }
    /* GRPH+f･4 = ダイアログへ ESC (取消)。停止は予約されない。 */
    run(&mut st, &tap(SC_F4, GRPH));
    assert!(!modal::is_open(), "GRPH+f･4 でダイアログが閉じない");
    assert_eq!(crate::session::pending_action(), 0, "取消のはずが停止が予約された");
    for s in 0..2 {
        assert!(events(&st, s).is_empty(), "アプリのリングに積まれた: slot {s}");
    }
}

/// 窓が無い状態で GRPH+f･4 → Shut Down の確認が出る (K2 の最後の段のホスト版)。
#[test]
fn k1_2_grph_f4_without_windows_asks_to_shut_down() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[]);
    run(&mut st, &tap(SC_F4, GRPH));
    assert!(modal::is_open(), "Shut Down の確認が出ない");
    run(&mut st, &tap(SC_ESC, 0));
    assert!(!modal::is_open());
    assert_eq!(crate::session::pending_action(), 0);
}

/* ================================================================ */
/*  ③ X4 (ポンプ) では何も配らず、次の X3 で GRPH↑ の時点で切り替え  */
/* ================================================================ */

#[test]
fn k1_3_pump_defers_wm_keys_and_the_rest_to_the_next_x3() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    let (b, a) = (0usize, 1usize);
    park_pointer(&mut st, 600, 10);
    let mut r = vec![SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH, SC_GRPH];
    r.extend_from_slice(&tap(SC_A, 0));
    r.extend_from_slice(&tap(SC_B, 0));
    mocks::push_rawkeys(&r);
    input::capture(&mut st, input::Ctx::Pump);
    assert_eq!(st.front_index(), Some(a), "X4 で切り替わった");
    assert!(events(&st, a).is_empty() && events(&st, b).is_empty(), "X4 で配った");
    assert!(st.pending_raw_n > 0, "退避していない");

    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(st.front_index(), Some(b), "次の X3 で切り替わらない");
    assert_eq!(text(&st, b), b"ab".to_vec(), "切り替え後の窓に ab が届かない");
    assert!(text(&st, a).is_empty() && keys(&st, a).is_empty(), "元の窓へ ab が漏れた");
}

/// GRPH を押したままの文字は、今までどおり GRPH 付きの Key として元の窓へ
/// (切り替えは GRPH↑ で起きる)。
#[test]
fn k1_3_chars_typed_while_grph_is_held_go_to_the_old_window() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    let (b, a) = (0usize, 1usize);
    park_pointer(&mut st, 600, 10);
    let mut r = vec![SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH];
    r.extend_from_slice(&tap(SC_A, GRPH));
    r.push(SC_GRPH);
    run(&mut st, &r);
    let ka = keys(&st, a);
    assert_eq!(ka.len(), 2, "GRPH+a が元の窓へ届かない: {ka:?}");
    assert!(ka[0].0 && ka[0].1 == SC_A as u8 && (ka[0].3 & 0x08) != 0, "GRPH 付きでない: {ka:?}");
    assert!(keys(&st, b).is_empty(), "GRPH+a が切り替え先へ届いた");
    assert_eq!(st.front_index(), Some(b), "GRPH↑ で切り替わらない");
}

/* ================================================================ */
/*  ④ マウスキー (カナ中のテンキー) のボタン                         */
/* ================================================================ */

fn one_window(shm: &mocks::Shm) -> GuiState {
    let mut st = windows(shm, &[(40, 40, 400, 300)]);
    park_pointer(&mut st, 200, 200);
    st
}

#[test]
fn k1_4_numpad5_is_a_click_with_two_edges_in_one_cycle() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_5, KANA));
    run(&mut st, &r);
    assert_eq!(buttons(&st, 0), vec![(true, 1), (false, 1)], "5 が押下→離しにならない");
    assert!(keys(&st, 0).is_empty(), "テンキーが Key として漏れた");
    assert!(st.kn.mk_on);
    clear_rings(&st);
    run(&mut st, &tap(NP_PLUS, KANA));
    assert_eq!(
        buttons(&st, 0),
        vec![(true, 1), (false, 1), (true, 1), (false, 1)],
        "+ がダブルクリックにならない"
    );
    assert!(keys(&st, 0).is_empty());
}

#[test]
fn k1_4_held_button_is_released_when_kana_turns_off() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_0, KANA));
    run(&mut st, &r);
    assert_eq!(buttons(&st, 0), vec![(true, 1)], "0 で押したままにならない");
    assert_eq!(st.synth_buttons, 1);
    run(&mut st, &[SC_KANA | DOWN]); /* カナ解除 (KANA ビットが落ちた raw) */
    assert_eq!(buttons(&st, 0), vec![(true, 1), (false, 1)], "カナ解除で離しが配られない");
    assert_eq!(st.synth_buttons, 0);
}

#[test]
fn k1_4_kana_release_read_by_the_pump_still_releases_on_the_next_x3() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_0, KANA));
    run(&mut st, &r);
    clear_rings(&st);
    mocks::push_rawkeys(&[SC_KANA | DOWN]);
    input::capture(&mut st, input::Ctx::Pump);
    assert!(buttons(&st, 0).is_empty(), "X4 で離しを配った");
    assert_eq!(st.synth_buttons, 1, "X4 が状態機械を進めた");
    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(buttons(&st, 0), vec![(false, 1)], "次の X3 で離しが配られない");
}

#[test]
fn k1_4_numpad5_during_modal_does_not_reach_the_window_behind() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    modal::open_wm_message(&mut st, GUI_MODAL_YES_NO, b"x\0", modal::WM_PURPOSE_NOTIFY);
    let mr = modal::rect();
    /* 背後の窓のクライアントで、ダイアログの外の点。 */
    let (px, py) = (50, 330);
    assert!(st.windows[0].client_rect_screen().contains(px, py) && !mr.contains(px, py));
    park_pointer(&mut st, px, py);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_5, KANA));
    run(&mut st, &r);
    assert!(events(&st, 0).is_empty(), "モーダル中の 5 が背後の窓へ届いた: {}", events(&st, 0).len());
    modal::on_key(&mut st, 0, 0x1B, 0);
    assert!(!modal::is_open());
}

/// カナ ON・5・カナ OFF・5 を 1 回で取り込む → 1 つ目はクリック、2 つ目は数字 "5"
/// (モードは raw の KANA ビットで決める。モードの外で押したテンキーの break はアプリへ)。
#[test]
fn k1_4_kana_bit_is_taken_from_each_raw() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    /* 取り込む時点の現在値は「オフ」(最後のカナ解除の後)。これを読む実装は
     * 1 つ目の 5 も数字にしてしまう。 */
    mocks::set_kbd_mods(0);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_5, KANA));
    r.push(SC_KANA | DOWN);
    r.extend_from_slice(&tap(NP_5, 0));
    run(&mut st, &r);
    assert_eq!(buttons(&st, 0), vec![(true, 1), (false, 1)], "1 つ目がクリックでない");
    assert_eq!(
        keys(&st, 0),
        vec![(true, NP_5 as u8, b'5', 0), (false, NP_5 as u8, b'5', 0)],
        "2 つ目が数字の 5 でない"
    );
    assert_eq!(text(&st, 0), b"5".to_vec());
    assert!(!st.kn.mk_on);
}

/* ================================================================ */
/*  ⑤ 加速: 6 を make 20 回 → 7×1 + 8×4 + 5×8 = 79 ドット            */
/* ================================================================ */

fn pointer_x(st: &GuiState, slot_i: usize) -> Option<i16> {
    events(st, slot_i)
        .iter()
        .filter(|e| e.kind == GUI_EV_POINTER)
        .map(|e| i16::from_le_bytes([e.payload[0], e.payload[1]]))
        .last()
}

#[test]
fn k1_5_numpad6_twenty_makes_move_79_dots() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let x0 = st.mouse_x;
    let mut r = vec![SC_KANA | DOWN | KANA];
    for _ in 0..20 {
        r.push(NP_6 | DOWN | KANA);
    }
    run(&mut st, &r);
    assert_eq!(st.mouse_x - x0, 79, "20 回の make で 79 ドットでない");
    let (cox, _) = st.windows[0].client_origin();
    assert_eq!(pointer_x(&st, 0), Some((x0 + 79 - cox) as i16), "Pointer が最後の位置でない");
    /* break で数え直し: 次の 1 回は 1 ドット。 */
    run(&mut st, &[NP_6 | KANA, NP_6 | DOWN | KANA]);
    assert_eq!(st.mouse_x - x0, 80, "break で数え直していない");
    /* 止まっている実マウスで位置が巻き戻らない。 */
    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(st.mouse_x - x0, 80, "止まった実マウスの値で巻き戻った");
}

/* ================================================================ */
/*  ⑧ CTRL = 半分の速度 (端数 1/2 ドットは持ち越す)                  */
/* ================================================================ */

#[test]
fn k1_8_ctrl_numpad6_twenty_makes_move_39_dots_and_keep_the_half() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let x0 = st.mouse_x;
    let mut r = vec![SC_KANA | DOWN | KANA];
    for _ in 0..20 {
        r.push(NP_6 | DOWN | KANA | CTRL);
    }
    run(&mut st, &r);
    assert_eq!(st.mouse_x - x0, 39, "CTRL で半分 (79/2 = 39) にならない");
    /* 端数 1/2 は持ち越す: 数え直した次の 1 回 (1/2 ドット) で整数になる。 */
    run(&mut st, &[NP_6 | KANA | CTRL, NP_6 | DOWN | KANA | CTRL]);
    assert_eq!(st.mouse_x - x0, 40, "端数 1/2 を捨てた");
    /* 1 回目 (1/2) だけでは動かない。 */
    run(&mut st, &[NP_6 | KANA | CTRL, NP_6 | DOWN | KANA | CTRL]);
    assert_eq!(st.mouse_x - x0, 40, "1/2 ドットで動いた");
    /* カナを切ると端数は捨てる。 */
    run(&mut st, &[NP_6 | KANA | CTRL, SC_KANA | DOWN, SC_KANA | DOWN | KANA]);
    run(&mut st, &[NP_6 | DOWN | KANA | CTRL]);
    assert_eq!(st.mouse_x - x0, 40, "モードが切れても端数が残った");
}

/* ================================================================ */
/*  ⑦ タイトルバーで 0 → 6×10 → . (同じ取り込み周期) → 窓が移る      */
/* ================================================================ */

fn titlebar_point(st: &GuiState) -> (i32, i32) {
    let tb = st.windows[0].titlebar_rect();
    (tb.x + 20, tb.y + tb.h / 2)
}

#[test]
fn k1_7_drag_by_numpad_moves_the_window_in_one_cycle() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(40, 40, 400, 300)]);
    let (tx, ty) = titlebar_point(&st);
    park_pointer(&mut st, tx, ty);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_0, KANA));
    for _ in 0..10 {
        r.push(NP_6 | DOWN | KANA);
    }
    r.push(NP_6 | KANA);
    r.extend_from_slice(&tap(NP_DOT, KANA));
    run(&mut st, &r);
    /* 7×1 + 3×4 = 19 ドット。 */
    assert_eq!((st.windows[0].x, st.windows[0].y), (59, 40), "離しで窓が移らない");
    assert_eq!(st.drag_index, -1, "ドラッグが終わっていない");
    assert!(buttons(&st, 0).is_empty(), "タイトルバーの押下がアプリへ漏れた");
}

/// 枠 (XOR 枠) がマウスキーの移動に追従する (離す前に見る)。
#[test]
fn k1_7_drag_frame_follows_numpad_moves() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(40, 40, 400, 300)]);
    let (tx, ty) = titlebar_point(&st);
    park_pointer(&mut st, tx, ty);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_0, KANA));
    for _ in 0..10 {
        r.push(NP_6 | DOWN | KANA);
    }
    run(&mut st, &r);
    assert_eq!(st.drag_index, 0, "ドラッグが始まらない");
    assert_eq!(st.drag_frame.x, 59, "枠が追従しない");
    assert_eq!(st.windows[0].x, 40, "離す前に窓が動いた");
    run(&mut st, &tap(NP_DOT, KANA));
    assert_eq!(st.windows[0].x, 59);
}

/* ================================================================ */
/*  ⑥ FEP の未確定文字はフォーカスが移る前に元の窓へ確定する        */
/* ================================================================ */

/// "にほん" の UTF-8 を 1 バイトずつ返す台本 (確定) + 終端。
fn nihon_commit() -> Vec<i32> {
    let mut v: Vec<i32> = "にほん".bytes().map(|b| b as i32).collect();
    v.push(0);
    v
}

fn fep_two_windows(shm: &mocks::Shm) -> GuiState {
    /* 0 = B、1 = A (最前面)。 */
    let mut st = windows(shm, &[(300, 20, 200, 150), (20, 20, 200, 150)]);
    park_pointer(&mut st, 600, 300);
    /* n i h の打鍵は FEP が消費 (-1)、確定は RETURN 1 回で "にほん"。 */
    let mut script = vec![-1, -1, -1];
    script.extend(nihon_commit());
    mocks::fep_script(&script);
    fep::install();
    assert!(fep::is_on());
    let mut r = Vec::new();
    r.extend_from_slice(&tap(SC_N, 0));
    r.extend_from_slice(&tap(SC_I, 0));
    r.extend_from_slice(&tap(SC_H, 0));
    run(&mut st, &r);
    /* FEP は make だけを取る (break は今までどおりアプリへ)。 */
    assert!(
        text(&st, 1).is_empty() && keys(&st, 1).iter().all(|k| !k.0),
        "未確定の打鍵がアプリへ届いた"
    );
    st
}

#[test]
fn k1_6_grph_tab_commits_the_preedit_to_the_old_window() {
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = fep_two_windows(&shm);
    run(&mut st, &[SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH, SC_GRPH]);
    assert_eq!(st.front_index(), Some(0), "切り替わらない");
    assert_eq!(text(&st, 1), "にほん".as_bytes().to_vec(), "元の窓 A に確定文字が届かない");
    assert!(text(&st, 0).is_empty(), "切り替え先 B に確定文字が届いた");
    fep_off();
}

#[test]
fn k1_6_mouse_click_on_another_window_commits_to_the_old_window() {
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = fep_two_windows(&shm);
    let cr = st.windows[0].client_rect_screen();
    *mocks::MOUSE.lock().unwrap() = ((cr.x + 10) as i16, (cr.y + 10) as i16, 1);
    input::capture(&mut st, input::Ctx::Wait);
    *mocks::MOUSE.lock().unwrap() = ((cr.x + 10) as i16, (cr.y + 10) as i16, 0);
    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(st.front_index(), Some(0), "クリックで切り替わらない");
    assert_eq!(text(&st, 1), "にほん".as_bytes().to_vec(), "元の窓 A に確定文字が届かない");
    assert!(text(&st, 0).is_empty(), "クリックした B に確定文字が届いた");
    fep_off();
}

/// アプリの set_focus (X1) は確定を予約だけし、次の X3 の頭で元の窓へ流す。
#[test]
fn k1_6_app_set_focus_commits_to_the_old_window_at_the_next_x3() {
    mocks::init();
    let shm = mocks::Shm::new();
    let mut st = fep_two_windows(&shm);
    let b = id(&st, 0);
    assert_eq!(wm::set_focus(&mut st, 2, b), 0);
    assert!(text(&st, 1).is_empty(), "X1 で変換を走らせた");
    input::capture(&mut st, input::Ctx::Wait);
    assert_eq!(text(&st, 1), "にほん".as_bytes().to_vec(), "元の窓 A に確定文字が届かない");
    assert!(text(&st, 0).is_empty());
    fep_off();
}

/* ================================================================ */
/*  窓メニュー (GRPH+SPACE) と GRPH+ESC / CTRL+ESC / SHIFT+f･10      */
/* ================================================================ */

#[test]
fn winmenu_move_by_arrows_and_return() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(40, 40, 300, 200)]);
    park_pointer(&mut st, 600, 10);
    run(&mut st, &tap(SC_SPACE, GRPH));
    assert_eq!(startmenu::winmenu_target(), Some(id(&st, 0)), "窓メニューが開かない");
    /* DOWN で "Move" → RETURN。 */
    let mut r = Vec::new();
    r.extend_from_slice(&tap(0x3D, 0));
    r.extend_from_slice(&tap(0x1C, 0));
    run(&mut st, &r);
    assert!(!startmenu::is_open());
    assert_eq!(st.drag_index, 0, "移動が始まらない");
    let mut r = Vec::new();
    r.extend_from_slice(&tap(0x3C, 0)); /* → 8 */
    r.extend_from_slice(&tap(0x3D, SHIFT)); /* SHIFT+↓ 1 */
    r.extend_from_slice(&tap(0x1C, 0));
    run(&mut st, &r);
    assert_eq!((st.windows[0].x, st.windows[0].y), (48, 41), "矢印で移らない");
    assert_eq!(st.drag_index, -1);
    for s in 0..1 {
        assert!(keys(&st, s).is_empty(), "移動中のキーがアプリへ漏れた");
    }
}

#[test]
fn winmenu_minimize_then_grph_tab_restores() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    park_pointer(&mut st, 600, 10);
    /* 窓メニュー → Minimize (3 行下)。 */
    let mut r = Vec::new();
    r.extend_from_slice(&tap(SC_SPACE, GRPH));
    for _ in 0..3 {
        r.extend_from_slice(&tap(0x3D, 0));
    }
    r.extend_from_slice(&tap(0x1C, 0));
    run(&mut st, &r);
    assert!(st.windows[1].minimized, "最小化されない");
    assert_eq!(st.front_index(), Some(0));
    /* GRPH+TAB で戻す (最小化は MRU の末尾 = 前面の次)。 */
    run(&mut st, &[SC_GRPH | DOWN | GRPH, SC_TAB | DOWN | GRPH, SC_TAB | GRPH, SC_GRPH]);
    assert!(!st.windows[1].minimized && st.front_index() == Some(1), "GRPH+TAB で戻らない");
}

#[test]
fn winmenu_maximize_and_restore() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(40, 40, 300, 200)]);
    park_pointer(&mut st, 600, 10);
    let w0 = id(&st, 0);
    kbdnav::winmenu_run(&mut st, w0, kbdnav::WM_MAXIMIZE);
    let wa = wm::work_area(&st);
    assert!(st.windows[0].outer() == wa, "最大化で作業領域いっぱいにならない");
    assert!(!kbdnav::winmenu_enabled(&st, id(&st, 0), kbdnav::WM_MOVE));
    kbdnav::winmenu_run(&mut st, w0, kbdnav::WM_RESTORE);
    assert_eq!(
        (st.windows[0].x, st.windows[0].y, st.windows[0].w, st.windows[0].h),
        (40, 40, 300, 200)
    );
}

#[test]
fn grph_esc_sends_front_to_back_and_ctrl_esc_toggles_start() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120), (300, 20, 200, 120)]);
    park_pointer(&mut st, 600, 10);
    run(&mut st, &tap(SC_ESC, GRPH));
    assert_eq!(st.front_index(), Some(0), "GRPH+ESC で次の窓へ行かない");
    assert_eq!(st.zorder[0], 1, "最背面へ回らない");
    run(&mut st, &tap(SC_ESC, CTRL));
    assert!(startmenu::is_open() && startmenu::start_pressed(), "CTRL+ESC で開かない");
    run(&mut st, &tap(SC_ESC, CTRL));
    assert!(!startmenu::is_open(), "CTRL+ESC で閉じない");
    for s in 0..2 {
        assert!(keys(&st, s).is_empty(), "WM のキーがアプリへ漏れた");
    }
}

#[test]
fn shift_f10_goes_to_the_window_or_opens_the_desktop_menu() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = windows(&shm, &[(20, 20, 200, 120)]);
    park_pointer(&mut st, 600, 10);
    run(&mut st, &tap(SC_F10, SHIFT));
    assert!(!startmenu::is_open(), "窓があるのに WM がメニューを出した");
    assert_eq!(keys(&st, 0).len(), 2, "SHIFT+f･10 が窓へ届かない");
    let mut st2 = windows(&shm, &[]);
    run(&mut st2, &tap(SC_F10, SHIFT));
    assert!(startmenu::is_open(), "窓が無いのにデスクトップのメニューが出ない");
    startmenu::close(&mut st2);
}

/* ================================================================ */
/*  ④' ボタンの直接割り当て (ユーザー決定 2026-09-26)                */
/*  5 = 左、- = 右、+ = 左ダブル、0 = 左を押したまま、. = 離す       */
/* ================================================================ */

const NP_MINUS: i32 = 0x40;
const NP_SLASH: i32 = 0x41;
const NP_STAR: i32 = 0x45;

#[test]
fn k1_4_minus_is_a_right_click_and_slash_star_do_nothing() {
    mocks::init();
    fep_off();
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_MINUS, KANA));
    run(&mut st, &r);
    assert_eq!(buttons(&st, 0), vec![(true, 2), (false, 2)], "- 1 回で右クリックにならない");
    clear_rings(&st);
    /* / と * はマウスモード中は消費して何もしない (ボタンの選択は持たない)。 */
    let mut r = Vec::new();
    r.extend_from_slice(&tap(NP_SLASH, KANA));
    r.extend_from_slice(&tap(NP_STAR, KANA));
    r.extend_from_slice(&tap(NP_5, KANA));
    run(&mut st, &r);
    assert!(keys(&st, 0).is_empty() && text(&st, 0).is_empty(), "/ * が文字として漏れた");
    assert_eq!(buttons(&st, 0), vec![(true, 1), (false, 1)], "/ * の後の 5 が左クリックでない");
}

/// + は同じ周期の down・up・down・up — WM のファイル選択のダブルクリック判定
/// (modal.rs、同じ行・DBLCLICK_TICKS 以内) に入って項目が開く。filer
/// (`userland/rust/filer` の 30 tick 以内・同じ行) も同じ判定の形。
#[test]
fn k1_4_plus_is_a_double_click_that_the_file_dialog_accepts() {
    static LS_DONE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
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
            ftype: 1,
        };
        e.name[..5].copy_from_slice(b"a.bin");
        let f: extern "C" fn(*const DirEntryExt, *mut u8) = core::mem::transmute(cb);
        f(&e as *const DirEntryExt, ctx);
        LS_DONE.store(true, std::sync::atomic::Ordering::SeqCst);
        0
    }
    mocks::init();
    fep_off();
    unsafe {
        (*os32api::api_ptr()).sys_ls = ls;
    }
    let shm = mocks::Shm::new();
    let mut st = one_window(&shm);
    modal::open_wm_file(&mut st, b"/\0");
    modal::x3_cycle(&mut st);
    assert!(LS_DONE.load(std::sync::atomic::Ordering::SeqCst));
    let rr = modal::file_row_rect(0);
    park_pointer(&mut st, rr.x + 8, rr.y + rr.h / 2);
    let mut r = vec![SC_KANA | DOWN | KANA];
    r.extend_from_slice(&tap(NP_PLUS, KANA));
    run(&mut st, &r);
    assert!(!modal::is_open(), "+ でダブルクリックにならない (ダイアログが開いたまま)");
    assert!(st.launch_pending, "ダブルクリックで項目が選ばれない");
    assert_eq!(&st.launch_path[..st.launch_path_len], b"/a.bin");
}
