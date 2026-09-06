//! modal.rs — モーダルダイアログ (契約 U4、票 W2 の C)。
//!
//! **入れ子ループを作らない**のが要点。`MODAL_OPEN` は「入力の宛先をダイアログに
//! 限定する」状態を立てるだけで、WM は X3 (`OP_WAIT`) の周期の中でダイアログを
//! 描き、キーとクリックを解釈する。親アプリは自分のループを回したまま `Paint` を
//! 受け続け、完了は `Modal{dialog, result}` イベント 1 件で受け取る。
//!
//! ダイアログの中身は **WM 自身の窓** なので、`Window` 表には載せず (アプリの
//! 所有物ではない)、契約 U8 の「直接呼び出し」で描く。可視領域の計算
//! ([`crate::visible`]) はダイアログの矩形を**上に載っている窓**として扱うので、
//! 下のアプリはダイアログの下を描かず、閉じたときに露出分の `Paint` を受ける。
//!
//! 標準ダイアログ:
//!
//! | `buttons` | 種類 | ボタン |
//! |---|---|---|
//! | [`GUI_MODAL_OK`] | メッセージ | OK |
//! | [`GUI_MODAL_OK_CANCEL`] | メッセージ | OK / Cancel |
//! | [`GUI_MODAL_YES_NO`] | メッセージ | Yes / No |
//! | [`GUI_MODAL_FILE_OPEN`] | ファイル選択 | Open / Cancel |
//!
//! `result` は 1 = OK / Yes / Open、0 = Cancel / No / ESC。
//!
//! **PM への申し送り (契約の不足)**: ファイル選択で選ばれた**パスをアプリへ渡す
//! 経路が契約に無い** (`Modal` のペイロードは `dialog` + `result` の 4B)。
//! `GUI_OP_MODAL_RESULT` (応答ブロックへ `GuiString` を書く) のような op を
//! 末尾追記してもらうまで、ファイル選択は WM 自身の用途 (デスクトップから
//! プログラムを起動する) にだけ使い、アプリには 1/0 しか返さない。
//! `buttons` の値と `result` の意味も共有ヘッダに無いので、ここの定数を
//! `os32_gui_shared.h` へ写してほしい。

#![allow(dead_code)]

use crate::wm::{self, GuiState, Rect};
use crate::{chrome, damage, lease, ring, visible};
use os32api::gfx;
use os32api::gui::proto::{
    GUI_COLOR_FACE, GUI_COLOR_HIGHLIGHT, GUI_COLOR_LIGHT, GUI_COLOR_SEL_BG, GUI_COLOR_SEL_TEXT,
    GUI_COLOR_SHADOW, GUI_COLOR_TEXT, GUI_COLOR_TITLE_ACTIVE, GUI_COLOR_TITLE_TEXT,
    GUI_COLOR_WINDOW, GUI_MAX_WINDOWS, OS32_ERR_FULL, OS32_ERR_INVAL,
};

/* ================================================================ */
/*  ダイアログ種別 (共有ヘッダに無いので暫定。上の申し送り参照)      */
/* ================================================================ */
pub const GUI_MODAL_OK: u16 = 0;
pub const GUI_MODAL_OK_CANCEL: u16 = 1;
pub const GUI_MODAL_YES_NO: u16 = 2;
pub const GUI_MODAL_FILE_OPEN: u16 = 3;

pub const MODAL_RESULT_CANCEL: i16 = 0;
pub const MODAL_RESULT_OK: i16 = 1;

/* スキャンコード (drivers/kbd.h)。 */
const SC_ESC: u8 = 0x00;
const SC_TAB: u8 = 0x0F;
const SC_RETURN: u8 = 0x1C;
const SC_SPACE: u8 = 0x34;
const SC_ROLLUP: u8 = 0x36;
const SC_ROLLDOWN: u8 = 0x37;
const SC_UP: u8 = 0x3A;
const SC_LEFT: u8 = 0x3B;
const SC_RIGHT: u8 = 0x3C;
const SC_DOWN: u8 = 0x3D;
const SC_HOME: u8 = 0x3E;

/* 見た目 (libos32filer の版面に合わせた寸法)。 */
const BTN_W: i32 = 72;
const BTN_H: i32 = 20;
const BTN_GAP: i32 = 12;
const PAD: i32 = 10;
const LINE_H: i32 = 18;
const TITLE_H: i32 = 18;

pub const MAX_ENTRIES: usize = 96;
pub const NAME_LEN: usize = 48;
pub const PATH_LEN: usize = 256;
const MSG_LEN: usize = 200;
/// ファイル選択の一覧に出す行数。
const LIST_ROWS: usize = 12;

/* ================================================================ */
/*  DirEntry_Ext (os32_kapi_shared.h) の写し                         */
/* ================================================================ */
#[repr(C)]
struct DirEntryExt {
    name: [u8; 256],
    size: u32,
    ftype: u8,
}
const FILE_TYPE_DIR: u8 = 2;

/* ================================================================ */
/*  状態                                                             */
/* ================================================================ */
pub struct Modal {
    pub used: bool,
    /// DialogId (1 から。破棄で進む = 古い `Modal` を捨てられる)。
    pub id: u16,
    pub owner: i32,
    pub parent_win: u32,
    pub buttons: u16,
    pub rect: Rect,
    msg: [u8; MSG_LEN + 1],
    msg_len: usize,
    nbtn: usize,
    focus_btn: usize,
    /* ---- ファイル選択 ---- */
    names: [[u8; NAME_LEN]; MAX_ENTRIES],
    is_dir: [bool; MAX_ENTRIES],
    nentries: usize,
    cursor: usize,
    scroll: usize,
    cwd: [u8; PATH_LEN],
    cwd_len: usize,
    /// 選ばれたフルパス (NUL 終端)。WM 自身が使う。
    pub sel: [u8; PATH_LEN],
    pub sel_len: usize,
    /// WM 自身が開いたダイアログ (アプリへ `Modal` を送らない)。
    wm_owned: bool,
}

impl Modal {
    pub const NEW: Modal = Modal {
        used: false,
        id: 0,
        owner: 0,
        parent_win: 0,
        buttons: 0,
        rect: Rect::EMPTY,
        msg: [0; MSG_LEN + 1],
        msg_len: 0,
        /* 0 初期化のまま (= 全体が .bss)。開くときに 1 か 2 を入れる。 */
        nbtn: 0,
        focus_btn: 0,
        names: [[0; NAME_LEN]; MAX_ENTRIES],
        is_dir: [false; MAX_ENTRIES],
        nentries: 0,
        cursor: 0,
        scroll: 0,
        cwd: [0; PATH_LEN],
        cwd_len: 0,
        sel: [0; PATH_LEN],
        sel_len: 0,
        wm_owned: false,
    };
}

struct ModalCell(core::cell::UnsafeCell<Modal>);
unsafe impl Sync for ModalCell {}
static MODAL: ModalCell = ModalCell(core::cell::UnsafeCell::new(Modal::NEW));

#[inline]
pub fn state() -> &'static mut Modal {
    unsafe { &mut *MODAL.0.get() }
}

#[inline]
pub fn is_open() -> bool {
    state().used
}

/// ダイアログの外形 (画面座標)。閉じていれば空。
#[inline]
pub fn rect() -> Rect {
    let m = state();
    if m.used {
        m.rect
    } else {
        Rect::EMPTY
    }
}

/* ================================================================ */
/*  開く / 閉じる                                                    */
/* ================================================================ */

/// `MODAL_OPEN` (X1)。**描かない** — 矩形を決めて損傷に積むだけ。
pub fn open(
    st: &mut GuiState,
    owner: i32,
    parent_win: u32,
    buttons: u16,
    msg: &[u8],
    msg_len: usize,
) -> i32 {
    if state().used {
        return OS32_ERR_FULL; /* v1 は 1 枚だけ */
    }
    if buttons > GUI_MODAL_FILE_OPEN {
        return OS32_ERR_INVAL;
    }
    let next_id = {
        let m = state();
        let n = m.id.wrapping_add(1) & 0x7FFF;
        if n == 0 {
            1
        } else {
            n
        }
    };
    {
        let m = state();
        *m = Modal::NEW;
        m.used = true;
        m.id = next_id;
        m.owner = owner;
        m.parent_win = parent_win;
        m.buttons = buttons;
        m.wm_owned = false;
        let n = if msg_len > MSG_LEN { MSG_LEN } else { msg_len };
        let mut i = 0;
        while i < n {
            m.msg[i] = msg[i];
            i += 1;
        }
        m.msg[n] = 0;
        m.msg_len = n;
        m.nbtn = if buttons == GUI_MODAL_OK { 1 } else { 2 };
        m.focus_btn = 0;
    }
    if buttons == GUI_MODAL_FILE_OPEN {
        set_cwd(b"/");
        reload_dir();
    }
    layout(st);
    let r = state().rect;
    st.dirty_screen(r);
    visible::recompute_and_expose(st);
    state().id as i32
}

/// WM 自身がファイル選択を開く (デスクトップからプログラムを起動する)。
pub fn open_wm_file(st: &mut GuiState, start_dir: &[u8]) {
    if state().used {
        return;
    }
    let next_id = {
        let m = state();
        let n = m.id.wrapping_add(1) & 0x7FFF;
        if n == 0 {
            1
        } else {
            n
        }
    };
    {
        let m = state();
        *m = Modal::NEW;
        m.used = true;
        m.id = next_id;
        m.owner = 0;
        m.parent_win = 0;
        m.buttons = GUI_MODAL_FILE_OPEN;
        m.wm_owned = true;
        m.nbtn = 2;
    }
    set_cwd(start_dir);
    reload_dir();
    layout(st);
    let r = state().rect;
    st.dirty_screen(r);
    visible::recompute_and_expose(st);
}

/// 完了。親のリングへ `Modal{dialog, result}` を 1 件流し、下地を損傷にする。
fn finish(st: &mut GuiState, result: i16) {
    let (id, owner, parent, wm_owned, r) = {
        let m = state();
        (m.id, m.owner, m.parent_win, m.wm_owned, m.rect)
    };
    /* WM 自身のファイル選択: 選ばれた .bin を起動予約にする。 */
    if wm_owned {
        if result == MODAL_RESULT_OK {
            let m = state();
            if m.sel_len > 0 {
                st.set_launch_path(&m.sel, m.sel_len);
                st.launch_pending = true;
            }
        }
    } else if let Some(slot) = st.slot_of_owner(owner) {
        let ev = ring::ev_modal(parent, id, result);
        ring::append(st, slot, &ev);
    }
    state().used = false;
    /* 下に隠れていたものを描き直す (デスクトップ + クローム + アプリの Paint)。 */
    st.dirty_screen(r);
    let mut i = 0;
    while i < GUI_MAX_WINDOWS {
        if st.windows[i].used && st.windows[i].visible {
            let (ox, oy) = st.windows[i].client_origin();
            damage::add_dirty(&mut st.windows[i], r.translate(-ox, -oy));
        }
        i += 1;
    }
    visible::recompute_and_expose(st);
}

/// アプリが落ちたらそのダイアログも畳む (契約 U8)。
pub fn reclaim_owner(st: &mut GuiState, owner: i32) {
    let m = state();
    if !m.used || m.wm_owned || m.owner != owner {
        return;
    }
    let r = m.rect;
    m.used = false;
    st.dirty_screen(r);
    visible::recompute_and_expose(st);
}

/* ================================================================ */
/*  レイアウト                                                       */
/* ================================================================ */

fn layout(st: &GuiState) {
    let m = state();
    let (w, h) = if m.buttons == GUI_MODAL_FILE_OPEN {
        /* タイトル帯 + パス行 + 一覧 LIST_ROWS 行 + ボタン。 */
        (
            8 * 46 + PAD * 2,
            TITLE_H + PAD + LINE_H * (LIST_ROWS as i32 + 1) + PAD + BTN_H + PAD,
        )
    } else {
        let text_w = (m.msg_len as i32) * 8;
        let mut w = text_w + PAD * 2 + 4;
        let min_w = (m.nbtn as i32) * BTN_W + ((m.nbtn as i32) - 1) * BTN_GAP + PAD * 2;
        if w < min_w {
            w = min_w;
        }
        if w > st.screen_w - 16 {
            w = st.screen_w - 16;
        }
        (w, TITLE_H + PAD * 2 + LINE_H + BTN_H + PAD)
    };
    let x = (st.screen_w - w) / 2;
    let y = (st.screen_h - h) / 3;
    m.rect = Rect::new(if x < 0 { 0 } else { x }, if y < 0 { 0 } else { y }, w, h);
}

/// ボタン i の矩形 (画面座標)。右詰め。
fn button_rect(m: &Modal, i: usize) -> Rect {
    let n = m.nbtn as i32;
    let total = n * BTN_W + (n - 1) * BTN_GAP;
    let x0 = m.rect.x + m.rect.w - PAD - total;
    let y = m.rect.y + m.rect.h - PAD - BTN_H;
    Rect::new(x0 + (i as i32) * (BTN_W + BTN_GAP), y, BTN_W, BTN_H)
}

/// 一覧の 1 行の矩形 (ファイル選択)。
fn row_rect(m: &Modal, row: usize) -> Rect {
    Rect::new(
        m.rect.x + PAD,
        m.rect.y + TITLE_H + PAD + (row as i32) * LINE_H,
        m.rect.w - PAD * 2,
        LINE_H,
    )
}

fn button_label(buttons: u16, i: usize) -> &'static [u8] {
    match buttons {
        GUI_MODAL_OK => b"OK\0",
        GUI_MODAL_OK_CANCEL => {
            if i == 0 {
                b"OK\0"
            } else {
                b"Cancel\0"
            }
        }
        GUI_MODAL_YES_NO => {
            if i == 0 {
                b"Yes\0"
            } else {
                b"No\0"
            }
        }
        _ => {
            if i == 0 {
                b"Open\0"
            } else {
                b"Cancel\0"
            }
        }
    }
}

fn title_label(buttons: u16) -> &'static [u8] {
    if buttons == GUI_MODAL_FILE_OPEN {
        b"Open file\0"
    } else {
        b"Message\0"
    }
}

/* ================================================================ */
/*  入力 (X3 だけ。契約 T8)                                          */
/* ================================================================ */

/// キー 1 件。ダイアログが閉じたら true。
pub fn on_key(st: &mut GuiState, scan: u8, _ch: u8) -> bool {
    if !state().used {
        return false;
    }
    let file = state().buttons == GUI_MODAL_FILE_OPEN;
    match scan {
        SC_ESC => {
            finish(st, MODAL_RESULT_CANCEL);
            return true;
        }
        SC_TAB | SC_RIGHT => {
            let m = state();
            m.focus_btn = (m.focus_btn + 1) % m.nbtn;
            invalidate(st);
        }
        SC_LEFT => {
            let m = state();
            m.focus_btn = (m.focus_btn + m.nbtn - 1) % m.nbtn;
            invalidate(st);
        }
        SC_UP if file => {
            move_cursor(st, -1);
        }
        SC_DOWN if file => {
            move_cursor(st, 1);
        }
        SC_ROLLUP if file => {
            move_cursor(st, -(LIST_ROWS as i32));
        }
        SC_ROLLDOWN if file => {
            move_cursor(st, LIST_ROWS as i32);
        }
        SC_HOME if file => {
            let m = state();
            m.cursor = 0;
            m.scroll = 0;
            invalidate(st);
        }
        SC_RETURN | SC_SPACE => {
            return activate(st);
        }
        _ => {}
    }
    false
}

/// クリック 1 件 (押下)。ダイアログが閉じたら true。
pub fn on_button(st: &mut GuiState, mx: i32, my: i32) -> bool {
    if !state().used {
        return false;
    }
    /* ボタン */
    let (nbtn, buttons) = {
        let m = state();
        (m.nbtn, m.buttons)
    };
    let mut i = 0;
    while i < nbtn {
        let br = button_rect(state(), i);
        if br.contains(mx, my) {
            state().focus_btn = i;
            return activate(st);
        }
        i += 1;
    }
    /* 一覧の行 */
    if buttons == GUI_MODAL_FILE_OPEN {
        let mut row = 0;
        while row < LIST_ROWS {
            let rr = row_rect(state(), row);
            if rr.contains(mx, my) {
                let m = state();
                let idx = m.scroll + row;
                if idx < m.nentries {
                    m.cursor = idx;
                    invalidate(st);
                }
                return false;
            }
            row += 1;
        }
    }
    false
}

/// 「決定」= フォーカス中のボタンを押す。閉じたら true。
fn activate(st: &mut GuiState) -> bool {
    let (buttons, focus) = {
        let m = state();
        (m.buttons, m.focus_btn)
    };
    if buttons == GUI_MODAL_FILE_OPEN && focus == 0 {
        /* Open: ディレクトリなら降りる、ファイルなら確定。 */
        let (isdir, has) = {
            let m = state();
            if m.cursor < m.nentries {
                (m.is_dir[m.cursor], true)
            } else {
                (false, false)
            }
        };
        if !has {
            return false;
        }
        if isdir {
            enter_dir(st);
            return false;
        }
        build_selection();
        finish(st, MODAL_RESULT_OK);
        return true;
    }
    let result = if focus == 0 { MODAL_RESULT_OK } else { MODAL_RESULT_CANCEL };
    finish(st, result);
    true
}

fn move_cursor(st: &mut GuiState, delta: i32) {
    {
        let m = state();
        if m.nentries == 0 {
            return;
        }
        let mut c = m.cursor as i32 + delta;
        if c < 0 {
            c = 0;
        }
        if c >= m.nentries as i32 {
            c = m.nentries as i32 - 1;
        }
        m.cursor = c as usize;
        if m.cursor < m.scroll {
            m.scroll = m.cursor;
        } else if m.cursor >= m.scroll + LIST_ROWS {
            m.scroll = m.cursor + 1 - LIST_ROWS;
        }
    }
    invalidate(st);
}

/// 中身が変わったので描き直す (X3 の合成に任せる)。
fn invalidate(st: &mut GuiState) {
    let r = state().rect;
    st.dirty_screen(r);
}

/* ================================================================ */
/*  ファイル選択のディレクトリ走査                                   */
/* ================================================================ */

fn set_cwd(path: &[u8]) {
    let m = state();
    let mut n = 0;
    while n < path.len() && n < PATH_LEN - 2 && path[n] != 0 {
        m.cwd[n] = path[n];
        n += 1;
    }
    if n == 0 {
        m.cwd[0] = b'/';
        n = 1;
    }
    m.cwd[n] = 0;
    m.cwd_len = n;
}

extern "C" fn ls_cb(entry: *const DirEntryExt, _ctx: *mut u8) {
    let m = state();
    if m.nentries >= MAX_ENTRIES || entry.is_null() {
        return;
    }
    let e = unsafe { &*entry };
    /* "." は出さない (".." は上へ戻るのに使う)。 */
    if e.name[0] == b'.' && e.name[1] == 0 {
        return;
    }
    let mut i = 0;
    while i < NAME_LEN - 1 && e.name[i] != 0 {
        m.names[m.nentries][i] = e.name[i];
        i += 1;
    }
    m.names[m.nentries][i] = 0;
    m.is_dir[m.nentries] = e.ftype == FILE_TYPE_DIR;
    m.nentries += 1;
}

fn reload_dir() {
    {
        let m = state();
        m.nentries = 0;
        m.cursor = 0;
        m.scroll = 0;
    }
    let path = {
        let m = state();
        let mut p = [0u8; PATH_LEN];
        let mut i = 0;
        while i < m.cwd_len {
            p[i] = m.cwd[i];
            i += 1;
        }
        p[i] = 0;
        p
    };
    unsafe {
        (os32api::api().sys_ls)(
            path.as_ptr(),
            ls_cb as *const () as *mut u8,
            core::ptr::null_mut(),
        );
    }
}

/// カーソル位置のディレクトリへ降りる / ".." で上がる。
fn enter_dir(st: &mut GuiState) {
    {
        let m = state();
        if m.cursor >= m.nentries {
            return;
        }
        if m.names[m.cursor][0] == b'.' && m.names[m.cursor][1] == b'.' && m.names[m.cursor][2] == 0
        {
            /* 親へ */
            let mut n = m.cwd_len;
            while n > 1 && m.cwd[n - 1] != b'/' {
                n -= 1;
            }
            if n > 1 {
                n -= 1;
            }
            if n == 0 {
                n = 1;
            }
            m.cwd[n] = 0;
            m.cwd_len = n;
        } else {
            let mut n = m.cwd_len;
            if n > 0 && m.cwd[n - 1] != b'/' && n < PATH_LEN - 1 {
                m.cwd[n] = b'/';
                n += 1;
            }
            let mut i = 0;
            while i < NAME_LEN && m.names[m.cursor][i] != 0 && n < PATH_LEN - 1 {
                m.cwd[n] = m.names[m.cursor][i];
                n += 1;
                i += 1;
            }
            m.cwd[n] = 0;
            m.cwd_len = n;
        }
    }
    reload_dir();
    invalidate(st);
}

/// カーソル位置のファイルのフルパスを `sel` に組む。
fn build_selection() {
    let m = state();
    let mut n = 0;
    while n < m.cwd_len && n < PATH_LEN - 1 {
        m.sel[n] = m.cwd[n];
        n += 1;
    }
    if n > 0 && m.sel[n - 1] != b'/' && n < PATH_LEN - 1 {
        m.sel[n] = b'/';
        n += 1;
    }
    let mut i = 0;
    while i < NAME_LEN && m.names[m.cursor][i] != 0 && n < PATH_LEN - 1 {
        m.sel[n] = m.names[m.cursor][i];
        n += 1;
        i += 1;
    }
    m.sel[n] = 0;
    m.sel_len = n;
}

/* ================================================================ */
/*  描画 (X3。wm::composite_rect の最後に呼ばれる)                   */
/* ================================================================ */

/// `clip` に掛かるならダイアログ全体を描き直す (kcg にクリップが無いため)。
pub fn draw(st: &GuiState, clip: Rect) {
    let m = state();
    if !m.used || !m.rect.intersects(&clip) {
        return;
    }
    let mono = lease::mono(st);
    let r = m.rect;
    let face = if mono { GUI_COLOR_WINDOW } else { GUI_COLOR_FACE };

    unsafe {
        /* 面 + 立体枠 */
        gfx::gfx_fill_rect(r.x, r.y, r.w, r.h, face);
        gfx::gfx_rect(r.x, r.y, r.w, r.h, GUI_COLOR_TEXT);
        if mono {
            chrome::dither50(r.x + 1, r.y + r.h - 2, r.w - 2, 1, GUI_COLOR_TEXT);
        } else {
            gfx::gfx_hline(r.x + 1, r.y + 1, r.w - 2, GUI_COLOR_LIGHT);
            gfx::gfx_vline(r.x + 1, r.y + 1, r.h - 2, GUI_COLOR_LIGHT);
            gfx::gfx_hline(r.x + 1, r.y + r.h - 2, r.w - 2, GUI_COLOR_SHADOW);
            gfx::gfx_vline(r.x + r.w - 2, r.y + 1, r.h - 2, GUI_COLOR_SHADOW);
        }

        /* タイトル帯 (常にアクティブ扱い) */
        let tb = Rect::new(r.x + 1, r.y + 1, r.w - 2, TITLE_H);
        let tcol = if mono { GUI_COLOR_TEXT } else { GUI_COLOR_TITLE_ACTIVE };
        let ttxt = if mono { GUI_COLOR_WINDOW } else { GUI_COLOR_TITLE_TEXT };
        gfx::gfx_fill_rect(tb.x, tb.y, tb.w, tb.h, tcol);
        gfx::kcg_set_scale(1);
        gfx::kcg_draw_utf8(tb.x + 4, tb.y + 1, title_label(m.buttons).as_ptr(), ttxt, tcol);
    }

    if m.buttons == GUI_MODAL_FILE_OPEN {
        draw_list(m, mono, face);
    } else {
        unsafe {
            gfx::kcg_draw_utf8(
                r.x + PAD,
                r.y + TITLE_H + PAD,
                m.msg.as_ptr(),
                GUI_COLOR_TEXT,
                face,
            );
        }
    }

    /* ボタン */
    let mut i = 0;
    while i < m.nbtn {
        draw_button(m, i, mono, i == m.focus_btn);
        i += 1;
    }
}

fn draw_list(m: &Modal, mono: bool, face: u8) {
    unsafe {
        gfx::kcg_set_scale(1);
    }
    /* パス表示 */
    let r = m.rect;
    unsafe {
        gfx::kcg_draw_utf8(r.x + PAD, r.y + TITLE_H + 2, m.cwd.as_ptr(), GUI_COLOR_TEXT, face);
    }
    let mut row = 0;
    while row < LIST_ROWS {
        let rr = row_rect(m, row).translate(0, LINE_H);
        let idx = m.scroll + row;
        let sel = idx == m.cursor && idx < m.nentries;
        let bg = if !sel {
            face
        } else if mono {
            GUI_COLOR_TEXT
        } else {
            GUI_COLOR_SEL_BG
        };
        let fg = if !sel {
            GUI_COLOR_TEXT
        } else if mono {
            GUI_COLOR_WINDOW
        } else {
            GUI_COLOR_SEL_TEXT
        };
        unsafe {
            gfx::gfx_fill_rect(rr.x, rr.y, rr.w, rr.h, bg);
        }
        if idx < m.nentries {
            let mark: &[u8] = if m.is_dir[idx] { b"[D] \0" } else { b"    \0" };
            unsafe {
                gfx::kcg_draw_utf8(rr.x + 2, rr.y + 1, mark.as_ptr(), fg, bg);
                gfx::kcg_draw_utf8(rr.x + 2 + 32, rr.y + 1, m.names[idx].as_ptr(), fg, bg);
            }
        }
        row += 1;
    }
}

fn draw_button(m: &Modal, i: usize, mono: bool, focused: bool) {
    let br = button_rect(m, i);
    let face = if mono { GUI_COLOR_WINDOW } else { GUI_COLOR_FACE };
    unsafe {
        gfx::gfx_fill_rect(br.x, br.y, br.w, br.h, face);
        gfx::gfx_rect(br.x, br.y, br.w, br.h, GUI_COLOR_TEXT);
        if !mono {
            gfx::gfx_hline(br.x + 1, br.y + 1, br.w - 2, GUI_COLOR_LIGHT);
            gfx::gfx_hline(br.x + 1, br.y + br.h - 2, br.w - 2, GUI_COLOR_SHADOW);
        }
        let label = button_label(m.buttons, i);
        let lw = label_width(label);
        gfx::kcg_draw_utf8(
            br.x + (br.w - lw) / 2,
            br.y + 2,
            label.as_ptr(),
            GUI_COLOR_TEXT,
            face,
        );
    }
    if focused {
        /* フォーカスは点線矩形 (契約 G6 の DOTTED)。 */
        let fr = Rect::new(br.x + 3, br.y + 3, br.w - 6, br.h - 6);
        let c = if mono { GUI_COLOR_TEXT } else { GUI_COLOR_HIGHLIGHT };
        chrome::dotted_rect(fr.x, fr.y, fr.w, fr.h, c);
    }
}

fn label_width(label: &[u8]) -> i32 {
    let mut n = 0;
    while n < label.len() && label[n] != 0 {
        n += 1;
    }
    (n as i32) * 8
}

/* ================================================================ */
/*  周期の補助                                                       */
/* ================================================================ */

/// アプリの COMMIT (X2) がダイアログに掛かったら描き直す。
pub fn refresh_if_hit(st: &GuiState, r: Rect) -> bool {
    let m = state();
    if !m.used || !m.rect.intersects(&r) {
        return false;
    }
    draw(st, m.rect);
    true
}

/// ダイアログを閉じた後に全面が要るときの目安 (デバッグ用)。
pub fn present_rect(st: &GuiState) {
    let m = state();
    if m.used {
        wm::queue_present(st, m.rect);
    }
}
