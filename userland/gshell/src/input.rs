//! input.rs — 入力の取り込み (契約 T3 / U2a)。
//!
//! `kbd_trygetrawkey` の生 make/break を `Key` (down 0/1) に、印字可能キーは加えて
//! `Text` にして、フォーカス窓の所有者スロットのリングへ積む。`mouse_poll` から
//! `Pointer` (最新 1 件へ畳む) と `Button` を作る。修飾は raw エントリに載る (イベント時点)。
//! `kbd_dropped_count` の差分を `dropped` に足して `OVERFLOW` (契約 T3)。
//!
//! WM 自身の UI (ドラッグ / 閉じる / フォーカス切替) は [`Ctx::Wait`] /
//! [`Ctx::Standalone`] (= `OP_WAIT` の中 / gshell 単独ループ、契約 X3) でだけ
//! 進める。ポンプ [`Ctx::Pump`] (X4) は入力のリング追記とカーソル移動だけで、
//! 状態機械を進めない。
//!
//! **W2 で足したもの** (契約 U2a / U4): SHIFT+SPACE を WM が消費して FEP を
//! 切り替え、押下は先に FEP ([`crate::fep`]) へ通して**消費されたキーは `Key` と
//! して配送しない**。確定文字列は取り込みの最後に `Text` でまとめて流す。
//! モーダル中は宛先をダイアログに限定する。GUI 中は cooked 待ち行列
//! (`kbd_buf`) にカーネルが積まないので (K2)、打鍵の入口は raw 1 本だけ。

use crate::wm::{GuiState, Rect};
use crate::{cursor, fep, modal, ring, slot, visible, wm};
use os32api::gui::proto::{GuiRect16, GUI_EV_CONFIGURE, GUI_EV_FOCUS};

/// 入力取り込みの実行文脈 (契約 T8)。
///
/// - [`Ctx::Pump`]       … X4。int 0x80 の入口 (K2)。入力のリング追記とカーソル移動だけ。
/// - [`Ctx::Wait`]       … X3。アプリの `OP_WAIT` の中。WM の状態機械を全部進める。
/// - [`Ctx::Standalone`] … X3。gshell 単独ループ。Wait に加えて ESC / F1 を横取りする。
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Ctx {
    Pump,
    Wait,
    Standalone,
}

impl Ctx {
    /// WM 自身の UI 状態機械 (ドラッグ / 閉じる / フォーカス切替) を進めてよいか。
    #[inline]
    pub fn wm_ui(self) -> bool {
        self != Ctx::Pump
    }
}

/* 修飾ビット (drivers/kbd.h の SHIFT_* と一致)。 */
const MOD_SHIFT: u32 = 0x01;
const MOD_CAPS: u32 = 0x02;
const MOD_CTRL: u32 = 0x10;

/* 修飾キーのスキャンコード (KEY_SHIFT..KEY_CTRL)。Key として配送しない。 */
const SC_SHIFT: u8 = 0x70;
const SC_CTRL: u8 = 0x74;
/* WM が単独時に横取りするキー (KEY_ESC / KEY_F1〜F5)。
 * F1〜F4 = `crate::LAUNCH_APPS` の起動、F5 = ファイル選択ダイアログ。 */
const SC_ESC: u8 = 0x00;
const SC_F1: u8 = 0x62;
const SC_F4: u8 = 0x65;
const SC_F5: u8 = 0x66;
/* KEY_SPACE。SHIFT+SPACE で FEP を切り替える (契約 U2a)。 */
const SC_SPACE: u8 = 0x34;

const MOUSE_BTN_LEFT: u8 = 0x01;

/// mouse_poll(*mut u8) が書き込む構造体 (os32_kapi_shared.h MouseInfo と同一)。
#[repr(C)]
#[derive(Clone, Copy)]
struct MouseInfo {
    x: i16,
    y: i16,
    dx: i16,
    dy: i16,
    buttons: u8,
    mode: u8,
}

/* drivers/kbd.c の scancode_to_ascii[128] の写し (8 個 / 行、C 側と同じ並び)。 */
#[rustfmt::skip]
static SC2A: [u8; 128] = [
    /* 0x00 */ 0x1B, b'1',  b'2',  b'3',  b'4',  b'5',  b'6',  b'7',
    /* 0x08 */ b'8',  b'9',  b'0',  b'-',  b'^',  b'\\', 0x08,  0x09,
    /* 0x10 */ b'q',  b'w',  b'e',  b'r',  b't',  b'y',  b'u',  b'i',
    /* 0x18 */ b'o',  b'p',  b'@',  b'[',  0x0D,  b'a',  b's',  b'd',
    /* 0x20 */ b'f',  b'g',  b'h',  b'j',  b'k',  b'l',  b';',  b':',
    /* 0x28 */ b']',  b'z',  b'x',  b'c',  b'v',  b'b',  b'n',  b'm',
    /* 0x30 */ b',',  b'.',  b'/',  0,     b' ',  0,     0x12,  0x03,
    /* 0x38 */ 0x16,  0x7F,  0x1E,  0x1D,  0x1C,  0x1F,  0x01,  0x05,
    /* 0x40 */ b'-',  b'/',  b'7',  b'8',  b'9',  b'*',  b'4',  b'5',
    /* 0x48 */ b'6',  b'+',  b'1',  b'2',  b'3',  b'=',  b'0',  b',',
    /* 0x50 */ b'.',  0, 0, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
];

/* drivers/kbd.c の scancode_to_ascii_shift[128] の写し。 */
#[rustfmt::skip]
static SC2A_SHIFT: [u8; 128] = [
    /* 0x00 */ 0x1B, b'!',  b'"',  b'#',  b'$',  b'%',  b'&',  b'\'',
    /* 0x08 */ b'(',  b')',  0,     b'=',  b'`',  b'|',  0x08,  0x09,
    /* 0x10 */ b'Q',  b'W',  b'E',  b'R',  b'T',  b'Y',  b'U',  b'I',
    /* 0x18 */ b'O',  b'P',  b'~',  b'{',  0x0D,  b'A',  b'S',  b'D',
    /* 0x20 */ b'F',  b'G',  b'H',  b'J',  b'K',  b'L',  b'+',  b'*',
    /* 0x28 */ b'}',  b'Z',  b'X',  b'C',  b'V',  b'B',  b'N',  b'M',
    /* 0x30 */ b'<',  b'>',  b'?',  b'_',  b' ',  0,     0x12,  0x03,
    /* 0x38 */ 0x16,  0x7F,  0x1E,  0x1D,  0x1C,  0x1F,  0x01,  0x05,
    /* 0x40 */ b'-',  b'/',  b'7',  b'8',  b'9',  b'*',  b'4',  b'5',
    /* 0x48 */ b'6',  b'+',  b'1',  b'2',  b'3',  b'=',  b'0',  b',',
    /* 0x50 */ b'.',  0, 0, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
];

/// スキャンコード + 修飾 → ASCII (drivers/kbd.c と同じ規則。無ければ 0)。
fn translate(scan: u8, mods: u32) -> u8 {
    let i = (scan & 0x7F) as usize;
    let mut a = if (mods & MOD_SHIFT) != 0 { SC2A_SHIFT[i] } else { SC2A[i] };
    if (mods & MOD_CAPS) != 0 {
        if a >= b'a' && a <= b'z' {
            a -= 32;
        } else if a >= b'A' && a <= b'Z' {
            a += 32;
        }
    }
    if (mods & MOD_CTRL) != 0 && a >= b'a' && a <= b'z' {
        a = a - b'a' + 1;
    }
    a
}

/// フォーカス窓 (最前面) の配送先。無ければ None。
pub struct Target {
    pub slot: usize,
    pub win_id: u32,
    pub cox: i32,
    pub coy: i32,
}

pub fn focus_target(st: &GuiState) -> Option<Target> {
    let index = st.front_index()?;
    let owner = st.windows[index].owner;
    let slot = st.slot_of_owner(owner)?;
    let (cox, coy) = st.windows[index].client_origin();
    Some(Target { slot, win_id: st.windows[index].id(index), cox, coy })
}

/* ================================================================ */
/*  入力取り込み本体                                                 */
/* ================================================================ */

/// 入力を取り込みリングへ流す (契約 T3 / T8)。
pub fn capture(st: &mut GuiState, ctx: Ctx) {
    st.now = unsafe { (os32api::api().get_tick)() };
    capture_keyboard(st, ctx);
    capture_mouse(st, ctx);
}

/// gshell 単独 (窓が 1 枚も無い) ときに WM が横取りするキー (make のみ)。
pub fn standalone_key(st: &mut GuiState, scan: u8) {
    if scan == SC_ESC {
        st.quit = true;
    } else if scan >= SC_F1 && scan <= SC_F4 {
        /* F1〜F4 = 検証用アプリの起動 (G2 / G3)。実際に走らせるのは
         * 単独ループ (`lib.rs` の `launch_app`) — ここでは予約だけ。 */
        let p = crate::LAUNCH_APPS[(scan - SC_F1) as usize];
        st.set_launch_path(p, p.len() - 1); /* 末尾 NUL は set 側が付ける */
        st.launch_pending = true;
    } else if scan == SC_F5 {
        /* WM 自身のファイル選択ダイアログ (契約 U4 の標準ダイアログ)。 */
        modal::open_wm_file(st, b"/");
    }
}

/// raw リング (`kbd_trygetrawkey`) のエントリ = `keycode | down<<8 | mods<<9`。
/// `mods` は**そのイベント時点**の修飾状態 (kbd.c の SHIFT_*、MOD_* と同値)。
/// 取り込み時の最新状態で変換すると Shift↓ A↓ A↑ Shift↑ が溜まったときに
/// a/A を取り違えるため、イベントごとの値を使う (レビュー #3 ②)。
/// cooked キュー (`kbd_buf`) は GUI モード中カーネルが積まない (K2) ので触らない。
///
/// **W2 で足したもの** (契約 U2a / U4):
///   1. SHIFT+SPACE は常に WM が消費して FEP を on/off する (配送しない)。
///   2. モーダル中は宛先をダイアログに限定する。
///   3. 押下は先に FEP へ通し、FEP が消費したら `Key` を配送しない。
///      確定文字列は取り込みの最後に `Text` でまとめて流す。
fn capture_keyboard(st: &mut GuiState, ctx: Ctx) {
    /* X4 で見た SHIFT+SPACE をここで実行する (契約 T8: 辞書を開く重い処理は X3)。 */
    if ctx.wm_ui() {
        fep::apply_pending_toggle(st);
    }

    /* 取りこぼしの差分を dropped に加算 (契約 T3)。 */
    let cur_drop = unsafe { (os32api::api().kbd_dropped_count)() };
    let delta = cur_drop.wrapping_sub(st.last_kbd_dropped);
    st.last_kbd_dropped = cur_drop;
    if delta > 0 {
        if let Some(t) = focus_target(st) {
            let n = if delta > 0xFFFF { 0xFFFF } else { delta as u16 };
            ring::add_dropped(st, t.slot, n);
        }
    }

    loop {
        /* モーダル中の X4 は取り込まない — 宛先はダイアログで、その状態機械を
         * 進めてよいのは X3 だけ (契約 T8)。打鍵はカーネル待ち行列に残す。 */
        if ctx == Ctx::Pump && modal::is_open() {
            break;
        }
        /* 満杯に近ければ取り込まない (カーネル待ち行列に残す。契約 T3)。
         * FEP の確定文字列が続く可能性があるので 4 件分を見る。 */
        let space_ok = match focus_target(st) {
            Some(t) => ring::space(st, t.slot) >= 4,
            None => true, /* 宛先無し: 取り込んでも捨てるだけなので読む (キューを空ける) */
        };
        if !space_ok {
            break;
        }
        /* X4 (ポンプ) で FEP がオンなら変換をここでは行わない (契約 T8: 辞書検索
         * まで走る)。退避して次の X3 に回す。SHIFT+SPACE が保留中 (on/off が次の
         * X3 で確定する) のときと、既に退避した打鍵があるときも退避する — でないと
         * 「SHIFT+SPACE → n i h o n g o」が X4 に来た場合、toggle 前の打鍵が FEP を
         * 素通りして先にアプリへ届く (2026-09-06 実測: 変換されず "nihongo" が入った)。
         *
         * 退避先の空きは **カーネルから取り出す前に** 見る (レビュー #5 ①)。取り出した
         * 後に満杯を知ると、その 1 件はカーネル待ち行列にも退避列にも残らず、dropped
         * にも数えられずに消える。空きが無ければ読まずにカーネル側へ残す (契約 T3)。 */
        let defer = ctx == Ctx::Pump
            && (fep::is_on() || fep::toggle_pending() || st.pending_raw_n > 0);
        if defer && st.pending_raw_n >= st.pending_raw.len() {
            break;
        }
        /* X3 は前の X4 が退避した raw を先に消費する (順序を保つ)。 */
        let raw = if ctx.wm_ui() && st.pending_raw_n > 0 {
            let r = st.pending_raw[0];
            let mut q = 1;
            while q < st.pending_raw_n {
                st.pending_raw[q - 1] = st.pending_raw[q];
                q += 1;
            }
            st.pending_raw_n -= 1;
            r
        } else {
            unsafe { (os32api::api().kbd_trygetrawkey)() }
        };
        if raw < 0 {
            break;
        }
        if defer {
            /* 空きは取り出す前に確認済み。 */
            st.pending_raw[st.pending_raw_n] = raw;
            st.pending_raw_n += 1;
            continue;
        }
        let scan = (raw & 0x7F) as u8;
        let down = ((raw >> 8) & 1) != 0;
        let mods = ((raw >> 9) & 0x7F) as u32; /* イベント時点の修飾状態 */

        /* 修飾キー自体は Key として配送しない (状態は mods で見る)。 */
        if scan >= SC_SHIFT && scan <= SC_CTRL {
            continue;
        }

        /* SHIFT+SPACE = FEP の on/off。**常に WM が消費**し、押下も離しも
         * アプリへ配送しない (契約 U2a)。 */
        if scan == SC_SPACE && (mods & MOD_SHIFT) != 0 {
            if down {
                if ctx.wm_ui() {
                    fep::toggle(st);
                } else {
                    fep::request_toggle();
                }
            }
            continue;
        }

        /* モーダル中は宛先をダイアログに限定する (契約 U4)。 */
        if modal::is_open() {
            if down && ctx.wm_ui() {
                let ch = translate(scan, mods);
                modal::on_key(st, scan, ch);
            }
            continue;
        }

        /* WM 単独時 (フォーカス窓なし) の横取り: ESC=終了 / F1=デモ / F2=ファイル選択。
         * アプリの OP_WAIT (Ctx::Wait) では横取りしない — ESC はアプリのもの。 */
        if ctx == Ctx::Standalone && st.front_index().is_none() {
            if down {
                standalone_key(st, scan);
            }
            continue;
        }

        let ch = translate(scan, mods);

        /* 押下は**先に FEP へ通す** (契約 U2a)。FEP が消費したキー (かな入力、
         * 未確定の編集、候補操作、確定、取消) は `Key` として配送しない。 */
        if down && fep::feed(st, scan, ch, mods) == fep::Fed::Consumed {
            continue;
        }

        let t = match focus_target(st) {
            Some(t) => t,
            None => continue,
        };
        let serial = next_serial(st, t.slot);
        let ev = ring::ev_key(down, t.win_id, scan, ch, mods as u8, serial);
        ring::append(st, t.slot, &ev);

        /* 印字可能キーは Text も配送 (契約 U2a。FEP が消費していれば上で
         * continue しているので二重にはならない)。 */
        if down && ch >= 0x20 && ch <= 0x7E {
            let mut utf8 = [0u8; 8];
            utf8[0] = ch;
            let s2 = next_serial(st, t.slot);
            let evt = ring::ev_text(t.win_id, utf8, 1, s2);
            ring::append(st, t.slot, &evt);
        }
    }

    /* FEP の確定文字列を `Text` (8B ずつ、`more`) で流す (契約 U2)。 */
    fep::flush_text(st);
}

fn capture_mouse(st: &mut GuiState, ctx: Ctx) {
    let mut mi = MouseInfo { x: 0, y: 0, dx: 0, dy: 0, buttons: 0, mode: 0 };
    unsafe {
        (os32api::api().mouse_poll)(&mut mi as *mut MouseInfo as *mut u8);
    }
    let mx = mi.x as i32;
    let my = mi.y as i32;
    let moved = mx != st.mouse_x || my != st.mouse_y;
    st.mouse_x = mx;
    st.mouse_y = my;
    let btn = mi.buttons;
    let down_edge = (btn & MOUSE_BTN_LEFT) != 0 && (st.prev_buttons & MOUSE_BTN_LEFT) == 0;
    let up_edge = (btn & MOUSE_BTN_LEFT) == 0 && (st.prev_buttons & MOUSE_BTN_LEFT) != 0;

    /* ---- モーダル中は宛先をダイアログに限定する (契約 U4) ---- */
    if modal::is_open() {
        if moved {
            cursor::move_to(st, mx, my);
        }
        /* 状態機械を進めるのは X3 だけ (契約 T8)。X4 (ポンプ) では
         * prev_buttons を**進めない** — 進めると X4 が先に押下を見たとき
         * 次の X3 に down_edge が立たず、ダイアログのボタンが反応しない
         * (レビュー #4 ④)。X3 が現在値と prev を比べてエッジを拾う。 */
        if ctx.wm_ui() {
            if down_edge {
                let _ = modal::on_button(st, mx, my);
            }
            st.prev_buttons = btn;
        }
        return;
    }

    if ctx.wm_ui() {
        /* ---- ドラッグ追従 (枠だけ動かす。実体は drop で移す。R2) ---- */
        if st.drag_index >= 0 {
            update_drag(st, mx, my);
        } else if moved {
            /* カーソルの移動は損傷に含めない (別経路で退避・再描画)。 */
            cursor::move_to(st, mx, my);
        }
        if down_edge {
            wm_button_down(st, mx, my);
        } else if up_edge {
            wm_button_up(st, mx, my);
        } else {
            /* 移動: フォーカス窓へ Pointer (畳み込み) */
            forward_pointer(st, mx, my, btn);
        }
    } else {
        /* ポンプ (X4): 状態機械を進めず、入力の追記とカーソル移動だけ。
         *
         * ボタンのエッジは **WM が扱うものなら prev_buttons を進めない**
         * (モーダルと同じ扱い、レビュー #4 ④ の一般形)。進めてしまうと X4 が
         * 先に見たエッジは次の X3 に立たず、ドラッグ中の離しは永久に来ず
         * (枠が残ったまま次の押下まで動き続ける)、タイトルバー / 閉じる /
         * 背面窓への押下はアプリのクライアントへ誤配される (2026-09-06 に
         * /api/mouse で実測: 離しが失われ、× を押すと前面化だけ起きた)。
         * アプリの領分 (前面窓のクライアント / 枠) のエッジだけをここで配る。 */
        if moved {
            cursor::move_to(st, mx, my);
        }
        if (down_edge || up_edge) && wm_owns_edge(st, mx, my, down_edge) {
            return; /* prev_buttons は据え置き → 次の X3 がエッジを拾う */
        }
        if down_edge {
            forward_button(st, mx, my, btn, true);
        } else if up_edge {
            forward_button(st, mx, my, btn, false);
        } else {
            forward_pointer(st, mx, my, btn);
        }
    }
    st.prev_buttons = btn;
}

/// このボタンエッジは WM の状態機械 (X3) が処理すべきものか。
/// - ドラッグ中: 押下も離しも WM (離しで drop する)
/// - 押下: 窓の外 (デスクトップ) / 背面の窓 (前面化 + Focus) / 閉じるボタン /
///   タイトルバー (ドラッグ開始) は WM。前面窓のクライアント・枠だけがアプリ
/// wm_button_down と同じ判定順で見る (ずれると二重配送か取りこぼしになる)。
fn wm_owns_edge(st: &GuiState, mx: i32, my: i32, down_edge: bool) -> bool {
    if st.drag_index >= 0 {
        return true;
    }
    if !down_edge {
        return false; /* ドラッグ外の離しはアプリの領分 */
    }
    let idx = match st.hit_window(mx, my) {
        Some(i) => i,
        None => return true,
    };
    if st.front_index() != Some(idx) {
        return true;
    }
    let w = st.windows[idx];
    if w.has_close() && w.close_rect().contains(mx, my) {
        return true;
    }
    if w.movable() && w.titlebar_rect().contains(mx, my) {
        return true;
    }
    false
}

/* ---- WM 状態機械 (X3 のみ) ---- */

fn wm_button_down(st: &mut GuiState, mx: i32, my: i32) {
    let hit = st.hit_window(mx, my);
    let idx = match hit {
        Some(i) => i,
        None => return,
    };
    /* 前面化 + フォーカス切替 (Focus イベント)。 */
    let old_front = st.front_id();
    let changed = st.front_index() != Some(idx);
    if changed {
        st.bring_to_front(idx);
        visible::recompute_and_expose(st);
        let vac = st.windows[idx].outer();
        st.dirty_screen(vac);
        let new_front = st.windows[idx].id(idx);
        emit_focus_change(st, old_front, new_front);
    }

    /* 閉じるボタン? */
    let w = st.windows[idx];
    if w.has_close() && w.close_rect().contains(mx, my) {
        if let Some(slot) = st.slot_of_owner(w.owner) {
            let ev = ring::ev_simple(os32api::gui::proto::GUI_EV_CLOSE, 0, w.id(idx));
            ring::append(st, slot, &ev);
        }
        return;
    }
    /* タイトルバー? → ドラッグ開始 (実体は動かさない)。 */
    if w.movable() && w.titlebar_rect().contains(mx, my) {
        st.drag_index = idx as i32;
        st.drag_dx = mx - w.x;
        st.drag_dy = my - w.y;
        st.drag_frame = w.outer();
        cursor::hide(st);
        crate::chrome::draw_drag_outline(w.x, w.y, w.w, w.h, crate::lease::mono(st));
        queue_frame_edges(st, w.outer());
        cursor::show(st);
        let cr = cursor::rect(st);
        wm::queue_present(st, cr);
        wm::flush_present();
        return;
    }
    /* それ以外 (クライアント / 枠) → Button をアプリへ。 */
    forward_button(st, mx, my, MOUSE_BTN_LEFT, true);
}

fn wm_button_up(st: &mut GuiState, mx: i32, my: i32) {
    if st.drag_index >= 0 {
        let idx = st.drag_index as usize;
        let old_outer = st.windows[idx].outer();
        /* 枠の最終位置へ実体を移す。 */
        let nf = st.drag_frame;
        st.windows[idx].x = nf.x;
        st.windows[idx].y = nf.y;
        st.drag_index = -1;
        st.drag_frame = Rect::EMPTY;

        /* 露出計算 + 旧位置と新位置を画面損傷に。 */
        st.dirty_screen(old_outer);
        st.dirty_screen(st.windows[idx].outer());
        visible::recompute_and_expose(st);
        /* 移動した窓のクライアントは全面再描画が要る (中身は同じでも位置が変わる)。 */
        crate::damage::set_dirty_full(&mut st.windows[idx]);
        /* Configure を通知 (座標確定、R2)。 */
        st.windows[idx].configure_pending = true;
        emit_configure(st, idx);
        return;
    }
    forward_button(st, mx, my, 0, false);
}

fn update_drag(st: &mut GuiState, mx: i32, my: i32) {
    let idx = st.drag_index as usize;
    let w = st.windows[idx];
    let mut nx = mx - st.drag_dx;
    let mut ny = my - st.drag_dy;
    /* 画面内にクランプ (タイトルバーが掴める範囲を残す)。 */
    if nx < -(w.w - 40) {
        nx = -(w.w - 40);
    }
    if nx > st.screen_w - 40 {
        nx = st.screen_w - 40;
    }
    if ny < 0 {
        ny = 0;
    }
    if ny > st.screen_h - crate::wm::TITLEBAR_H {
        ny = st.screen_h - crate::wm::TITLEBAR_H;
    }
    let old_frame = st.drag_frame;
    let new_frame = Rect::new(nx, ny, w.w, w.h);
    let old_cursor = cursor::rect(st);
    let cursor_moved = st.mouse_x != st.cursor.x || st.mouse_y != st.cursor.y;
    if new_frame == old_frame && !cursor_moved {
        return;
    }
    st.drag_frame = new_frame;
    /* 旧枠を下地で消し、新枠を描いて、両者の縁とカーソルを 1 回で present。 */
    cursor::hide(st);
    erase_frame_edges(st, old_frame);
    crate::chrome::draw_drag_outline(
        new_frame.x,
        new_frame.y,
        new_frame.w,
        new_frame.h,
        crate::lease::mono(st),
    );
    st.cursor.x = st.mouse_x;
    st.cursor.y = st.mouse_y;
    cursor::show(st);
    queue_frame_edges(st, old_frame);
    queue_frame_edges(st, new_frame);
    wm::queue_present(st, old_cursor);
    let cr = cursor::rect(st);
    wm::queue_present(st, cr);
    wm::flush_present();
}

/* ---- アプリへの配送 ---- */

fn forward_pointer(st: &mut GuiState, mx: i32, my: i32, btn: u8) {
    let t = match focus_target(st) {
        Some(t) => t,
        None => return,
    };
    let cx = (mx - t.cox) as i16;
    let cy = (my - t.coy) as i16;
    let serial = next_serial(st, t.slot);
    let ev = ring::ev_pointer(t.win_id, cx, cy, btn, serial);
    ring::append(st, t.slot, &ev);
}

fn forward_button(st: &mut GuiState, mx: i32, my: i32, btn: u8, down: bool) {
    let t = match focus_target(st) {
        Some(t) => t,
        None => return,
    };
    let cx = (mx - t.cox) as i16;
    let cy = (my - t.coy) as i16;
    let serial = next_serial(st, t.slot);
    let ev = ring::ev_button(down, t.win_id, cx, cy, MOUSE_BTN_LEFT, serial);
    ring::append(st, t.slot, &ev);
    let _ = btn;
}

/* ---- Focus / Configure ---- */

/// フォーカスの移動を両者へ `Focus` で知らせる (契約 U2)。
pub fn emit_focus_change(st: &mut GuiState, old_id: u32, new_id: u32) {
    if old_id != 0 {
        if let Some(oi) = st.win_by_id(old_id) {
            if let Some(slot) = st.slot_of_owner(st.windows[oi].owner) {
                let ev = ring::ev_simple(GUI_EV_FOCUS, 0, old_id);
                ring::append(st, slot, &ev);
            }
        }
    }
    if new_id != 0 {
        if let Some(ni) = st.win_by_id(new_id) {
            if let Some(slot) = st.slot_of_owner(st.windows[ni].owner) {
                let ev = ring::ev_simple(GUI_EV_FOCUS, 1, new_id);
                ring::append(st, slot, &ev);
            }
        }
    }
}

/// 座標が確定したウィンドウの `Configure` (導出型) を 1 件流す。
/// 矩形は**クライアント矩形の画面絶対座標** (C2 が `create_window_surface` の
/// 原点に使う)。リングに入らなければ `configure_pending` を残して次周へ。
pub fn emit_configure(st: &mut GuiState, index: usize) {
    let w = &st.windows[index];
    if !w.configure_pending {
        return;
    }
    let owner = w.owner;
    let id = w.id(index);
    let (cw, ch) = w.client_size();
    let (cox, coy) = w.client_origin();
    let rect = GuiRect16 { x: cox as i16, y: coy as i16, w: cw as i16, h: ch as i16 };
    if let Some(slot) = st.slot_of_owner(owner) {
        let ev = ring::ev_rect(GUI_EV_CONFIGURE, id, rect);
        if ring::append(st, slot, &ev) {
            st.windows[index].configure_pending = false;
        }
    }
}

/// 入力イベントの `serial` を 1 つ払い出し、**取り込んだ tick を記録する**
/// (契約 P2: serial ごとに直近 64 件をスロットの予備領域へ)。
#[inline]
pub fn next_serial(st: &mut GuiState, slot_no: usize) -> u16 {
    st.slots[slot_no].serial = st.slots[slot_no].serial.wrapping_add(1);
    let s = st.slots[slot_no].serial;
    slot::record_trace(st, slot_no, s, st.now);
    s
}

/* ---- ドラッグ枠の縁の present / 消去 (wm compositor へ委譲) ---- */

fn frame_edges(f: Rect) -> [Rect; 4] {
    let t = 2; /* 枠の太さ */
    [
        Rect::new(f.x, f.y, f.w, t),                       /* 上 */
        Rect::new(f.x, f.y + f.h - t, f.w, t),             /* 下 */
        Rect::new(f.x, f.y, t, f.h),                       /* 左 */
        Rect::new(f.x + f.w - t, f.y, t, f.h),             /* 右 */
    ]
}

/// 枠の縁 4 本を転送キューへ積む (flush は呼ぶ側で 1 回)。
fn queue_frame_edges(st: &GuiState, f: Rect) {
    for e in frame_edges(f).iter() {
        wm::queue_present(st, *e);
    }
}

fn erase_frame_edges(st: &mut GuiState, f: Rect) {
    for e in frame_edges(f).iter() {
        wm::composite_rect(st, *e);
    }
}
