//! fep.rs — 日本語入力 (FEP) を WM が持つ (契約 U2a、票 W2 の A)。
//!
//! ## 誰がキーを読むか
//!
//! `drivers/kbd.c` は 1 打鍵につき **2 本**の待ち行列へ積む:
//!
//! | 行列 | 内容 | 読み手 |
//! |---|---|---|
//! | cooked `kbd_buf` | make のみ、`(scancode<<8) \| ascii` | **FEP** (`ime_trygetkey`) |
//! | raw `kbd_raw_buf` | make / break の全キー | WM ([`crate::input`]) — break だけ使う |
//!
//! カーネルの FEP (`kernel/ime.c`) は `ime_trygetkey()` の中で自分から
//! `kbd_trygetkey()` を引く。したがって **cooked 側の所有者は FEP** で、WM は
//! `ime_trygetkey()` を回すだけでよい (W1 の `drain_cooked_queue` は不要になった)。
//! make の `Key` はここから配送し、raw 側は break (`down=0`) 専用にする。
//! これで「FEP が消費したキーは `Key` として配送しない」(契約 U2a 改訂) が
//! **対応付けの推測なしに**成立する — 消費されたキーはそもそも出てこない。
//!
//! `ime_trygetkey()` の戻り値は 3 種類あり、次の規則で**一意に**分類できる:
//!
//! | 値 | 意味 |
//! |---|---|
//! | `< 0` | FEP が消費した / 行列が空 |
//! | `>= 0x100` | 素通りしたキー `(scancode<<8) \| ascii` |
//! | `== 0x1B` | 素通りした ESC (scancode 0、ascii 0x1B) |
//! | その他 `0x01..0xFF` | **確定文字列の 1 バイト** |
//!
//! scancode 0 は ESC だけで、その ascii は必ず 0x1B。確定文字列 (かな・漢字・
//! ローマ字出力の UTF-8) に 0x1B は現れないので、両者は衝突しない。
//!
//! ## 未確定文字列と候補窓の描画
//!
//! カーネルの FEP は `IME_Render` 関数表 (`kernel/ime_render.h`) 越しに
//! **80x25 のセル格子**へ描く。ここに GFX 版の関数表 [`GSHELL_IME_RENDER_GFX`] を
//! 用意した (実体は gshell 側 = CPL=0 なのでカーネルから直接呼べる)。関数表は
//! **画素を書かず、セル格子 (`Grid`) を更新するだけ**なので、X1 / X4 (ポンプ) から
//! 呼ばれても契約 T8 に反しない。実際に画素を置くのは X3 の [`post_cycle`]。
//!
//! **PM への申し送り (K レーンへの依頼)**: 関数表を差し込む KAPI が無い。
//! `ime_set_render(void *table)` (と、できれば `ime_get_render()`) を K が
//! 足してくれれば、gshell 起動時に [`install`] が呼んで未確定文字列・候補窓が
//! そのまま GFX へ出る。それまでは WM 側で「[あ]」のモード表示だけを出す
//! (格子・レイアウト・損傷・present の経路は完成しているので、KAPI 1 本で繋がる)。

#![allow(dead_code)]

use crate::wm::{GuiState, Rect};
use crate::{cursor, damage, input, modal, ring, wm};
use os32api::gfx;
use os32api::gui::proto::{GUI_COLOR_ACCENT, GUI_COLOR_CLOSE, GUI_COLOR_LINK, GUI_COLOR_OK,
                          GUI_COLOR_TEXT, GUI_COLOR_WARN, GUI_COLOR_WINDOW, GUI_MAX_WINDOWS};

/* ================================================================ */
/*  セル格子 (TVRAM 版と同じ 80x25 / 8x16 セル)                      */
/* ================================================================ */
pub const CELL_W: i32 = 8;
pub const CELL_H: i32 = 16;
pub const COLS: usize = 80;
pub const ROWS: usize = 25;
/// `kernel/ime.c` の `IME_PREEDIT_ROW`。候補リストはこの上の行に出る。
pub const PREEDIT_ROW: usize = 24;

const NCELL: usize = COLS * ROWS;

/* カーネル側の ATTR_* (os32_kapi_shared.h)。os32api の ATTR_* は
 * GREEN / CYAN の値が入れ替わっているので、こちらを正典として写す。 */
const K_ATTR_WHITE: u8 = 0xE1;
const K_ATTR_CYAN: u8 = 0xA1;
const K_ATTR_GREEN: u8 = 0x81;
const K_ATTR_YELLOW: u8 = 0xC1;
const K_ATTR_RED: u8 = 0x41;
const K_ATTR_MAGENTA: u8 = 0x61;

/// 枠の内側余白 (画素)。
const PAD: i32 = 2;

/// 確定文字列の受け皿 (UTF-8)。1 回の変換確定は高々 128B。
const COMMIT_MAX: usize = 128;

/* ================================================================ */
/*  IME_Render 関数表 (kernel/ime_render.h と同じ並び)               */
/* ================================================================ */
#[repr(C)]
pub struct ImeRender {
    pub putc: extern "C" fn(x: i32, y: i32, ank: u8, color: u8),
    pub putw: extern "C" fn(x: i32, y: i32, codepoint: u32, color: u8) -> i32,
    pub clear_row: extern "C" fn(y: i32, color: u8),
    pub begin: extern "C" fn(),
    pub end: extern "C" fn(),
}
unsafe impl Sync for ImeRender {}

/// GFX バックエンドの実体。K が `ime_set_render()` を足したらこの番地を渡す。
#[no_mangle]
pub static GSHELL_IME_RENDER_GFX: ImeRender = ImeRender {
    putc: gshell_ime_putc,
    putw: gshell_ime_putw,
    clear_row: gshell_ime_clear_row,
    begin: gshell_ime_begin,
    end: gshell_ime_end,
};

/* ================================================================ */
/*  状態                                                             */
/* ================================================================ */
#[derive(Clone, Copy)]
struct Grid {
    /// Unicode コードポイント。0 = 空白 (または全角の右セル)。
    cp: [u32; NCELL],
    /// カーネル ATTR_*。
    attr: [u8; NCELL],
    /// 1 = 半角、2 = 全角の左セル、0 = 空 / 全角の右セル。
    wide: [u8; NCELL],
}

impl Grid {
    const EMPTY: Grid = Grid { cp: [0; NCELL], attr: [0; NCELL], wide: [0; NCELL] };
}

pub struct Fep {
    /// `ime_is_active()` の写し (直近の同期時点)。
    pub on: bool,
    /// 関数表がカーネルから呼ばれたことがあるか (= K が繋いでくれた)。
    pub kernel_drives: bool,
    grid: Grid,
    /// 格子の内容が変わった。
    grid_dirty: bool,
    /// 下地を潰されたので描き直しが要る。
    redraw: bool,
    /// 画面に出ている描画の外接矩形 (空 = 非表示)。
    shown: Rect,
    /// 次に出す外接矩形 ([`pre_cycle`] が決め、[`post_cycle`] が使う)。
    next: Rect,
    /// 未確定行の箱 / 候補窓の箱 (画面座標)。
    pre_box: Rect,
    cand_box: Rect,
    /// 未確定行 / 候補行の内容範囲 (列・行)。
    pre_c0: usize,
    pre_c1: usize,
    cand_r0: usize,
    cand_r1: usize,
    cand_c0: usize,
    cand_c1: usize,
    pending_draw: bool,
    commit: [u8; COMMIT_MAX],
    commit_len: usize,
    /// [`GSHELL_IME_RENDER_GFX`] の番地。K が `ime_set_render` を足したら
    /// これをそのまま渡す。**参照を 1 つ作っておかないと `--gc-sections` で
    /// 関数表ごと落ちる**ので、ここで必ず番地を控える。
    pub render_table: u32,
}

impl Fep {
    pub const NEW: Fep = Fep {
        on: false,
        kernel_drives: false,
        grid: Grid::EMPTY,
        grid_dirty: false,
        redraw: false,
        shown: Rect::EMPTY,
        next: Rect::EMPTY,
        pre_box: Rect::EMPTY,
        cand_box: Rect::EMPTY,
        /* 全フィールドを 0 にしておくと `Fep` 全体が .bss に入り、12KB の
         * ゼロが .data として gshell.bin に載らない。境界値は箱が空でない
         * ときにしか読まれないので 0 で構わない。 */
        pre_c0: 0,
        pre_c1: 0,
        cand_r0: 0,
        cand_r1: 0,
        cand_c0: 0,
        cand_c1: 0,
        pending_draw: false,
        commit: [0; COMMIT_MAX],
        commit_len: 0,
        render_table: 0,
    };
}

struct FepCell(core::cell::UnsafeCell<Fep>);
unsafe impl Sync for FepCell {}
static FEP: FepCell = FepCell(core::cell::UnsafeCell::new(Fep::NEW));

/// FEP 状態への可変参照 (単一スレッド前提。`wm::g()` と同じ流儀)。
#[inline]
pub fn state() -> &'static mut Fep {
    unsafe { &mut *FEP.0.get() }
}

/* ================================================================ */
/*  関数表の実体 — 画素は書かず、格子を更新するだけ (契約 T8)        */
/* ================================================================ */

extern "C" fn gshell_ime_begin() {
    state().kernel_drives = true;
}

extern "C" fn gshell_ime_end() {}

extern "C" fn gshell_ime_putc(x: i32, y: i32, ank: u8, color: u8) {
    let f = state();
    f.kernel_drives = true;
    grid_put(f, x, y, ank as u32, color, 1);
}

extern "C" fn gshell_ime_putw(x: i32, y: i32, codepoint: u32, color: u8) -> i32 {
    let f = state();
    f.kernel_drives = true;
    /* ANK に落ちる文字は 1 セル、それ以外は全角 2 セル (TVRAM 版と同じ規則)。 */
    let ank = unsafe { crate::ffi::unicode_to_ank(codepoint) };
    if ank != 0 {
        grid_put(f, x, y, ank as u32, color, 1);
        return 1;
    }
    if x < 0 || x >= (COLS as i32) - 1 || y < 0 || y >= ROWS as i32 {
        return 0;
    }
    grid_put(f, x, y, codepoint, color, 2);
    2
}

extern "C" fn gshell_ime_clear_row(y: i32, color: u8) {
    let f = state();
    f.kernel_drives = true;
    grid_clear_row(f, y, color);
}

/* ---- 格子の操作 ---- */

fn grid_put(f: &mut Fep, x: i32, y: i32, cp: u32, attr: u8, w: u8) {
    if x < 0 || y < 0 || y >= ROWS as i32 {
        return;
    }
    let xi = x as usize;
    let yi = y as usize;
    if xi >= COLS || (w == 2 && xi + 1 >= COLS) {
        return;
    }
    let i = yi * COLS + xi;
    if f.grid.cp[i] != cp || f.grid.attr[i] != attr || f.grid.wide[i] != w {
        f.grid_dirty = true;
    }
    f.grid.cp[i] = cp;
    f.grid.attr[i] = attr;
    f.grid.wide[i] = w;
    if w == 2 {
        f.grid.cp[i + 1] = 0;
        f.grid.attr[i + 1] = attr;
        f.grid.wide[i + 1] = 0;
    }
}

fn grid_clear_row(f: &mut Fep, y: i32, _attr: u8) {
    if y < 0 || y >= ROWS as i32 {
        return;
    }
    let base = (y as usize) * COLS;
    let mut x = 0;
    while x < COLS {
        if f.grid.wide[base + x] != 0 || f.grid.cp[base + x] != 0 {
            f.grid_dirty = true;
        }
        f.grid.cp[base + x] = 0;
        f.grid.attr[base + x] = 0;
        f.grid.wide[base + x] = 0;
        x += 1;
    }
}

fn grid_clear_all(f: &mut Fep) {
    let mut y = 0;
    while y < ROWS as i32 {
        grid_clear_row(f, y, 0);
        y += 1;
    }
}

/* ================================================================ */
/*  起動時の差し込み                                                 */
/* ================================================================ */

/// GFX 版の関数表をカーネル FEP へ差し込む。
///
/// **現状は何もしない**: `ime_set_render()` に相当する KAPI がまだ無い
/// (モジュール先頭の申し送りを参照)。KAPI が生えたらここで
/// `(a.ime_set_render)(&GSHELL_IME_RENDER_GFX as *const _ as *mut u8)` を呼ぶ。
/// 差し込めていない間は [`kernel_drives`](Fep::kernel_drives) が false のままで、
/// WM 側がモード表示だけを格子へ書く。
pub fn install() {
    let f = state();
    f.render_table = &GSHELL_IME_RENDER_GFX as *const ImeRender as u32;
    grid_clear_all(f);
    f.on = unsafe { (os32api::api().ime_is_active)() } != 0;
    f.grid_dirty = true;
}

/* ================================================================ */
/*  キーの取り込み (契約 U2a / T3)                                   */
/* ================================================================ */

/// cooked 待ち行列を FEP 越しに読み切り、`Key` / `Text` をリングへ流す。
///
/// `ctx` が [`input::Ctx::Pump`] (X4) のときはモーダル中は何もしない
/// (モーダルの状態機械を進めるのは X3 だけ = 契約 T8)。
pub fn pump(st: &mut GuiState, ctx: input::Ctx, mods: u32) {
    if ctx == input::Ctx::Pump && modal::is_open() {
        return;
    }
    let mut guard = 0;
    loop {
        guard += 1;
        if guard > 128 {
            break;
        }
        /* リングの空きが無ければ取り込まない (契約 T3: カーネル待ち行列に残す)。 */
        if !space_ok(st) {
            break;
        }
        let v = unsafe { (os32api::api().ime_trygetkey)() };
        if v < 0 {
            break;
        }
        if v >= 0x100 || v == 0x1B {
            /* 素通りしたキー。直前までの確定文字列を先に出して順序を保つ。 */
            flush_commit(st);
            deliver_key(st, ctx, v, mods);
        } else {
            push_commit(v as u8);
        }
    }
    flush_commit(st);
    sync_mode(st);
}

/// フォーカス窓のリングに `Key` + `Text` 2 件分の空きがあるか。
fn space_ok(st: &GuiState) -> bool {
    match input::focus_target(st) {
        Some(t) => ring::space(st, t.slot) >= 4,
        /* 宛先が無いときは読む (行列を空けるだけ)。 */
        None => true,
    }
}

fn push_commit(b: u8) {
    let f = state();
    if f.commit_len < COMMIT_MAX {
        f.commit[f.commit_len] = b;
        f.commit_len += 1;
    }
}

/// UTF-8 先頭バイトから文字の長さを得る (壊れていれば 1)。
fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >= 0xF0 {
        4
    } else if b >= 0xE0 {
        3
    } else if b >= 0xC0 {
        2
    } else {
        1
    }
}

/// 溜まった確定文字列を `Text` (8B ずつ UTF-8 境界、`more`) で配送する (契約 U2)。
fn flush_commit(st: &mut GuiState) {
    let len = state().commit_len;
    if len == 0 {
        return;
    }
    state().commit_len = 0;
    let t = match input::focus_target(st) {
        Some(t) => t,
        /* 宛先無し (モーダル中・窓無し) は捨てる。 */
        None => return,
    };
    if modal::is_open() {
        return;
    }
    let mut i = 0;
    while i < len {
        let mut n = 0;
        while i + n < len && n < 8 {
            let cl = utf8_len(state().commit[i + n]);
            if n + cl > 8 {
                break;
            }
            n += cl;
        }
        if n == 0 {
            n = 1;
        }
        if i + n > len {
            n = len - i;
        }
        let mut buf = [0u8; 8];
        let mut k = 0;
        while k < n {
            buf[k] = state().commit[i + k];
            k += 1;
        }
        let more = if i + n < len { 0x80u8 } else { 0u8 };
        let serial = input::next_serial(st, t.slot);
        let ev = ring::ev_text(t.win_id, buf, (n as u8) | more, serial);
        if !ring::append(st, t.slot, &ev) {
            /* 長い確定文字列でリングが尽きた。**黙って落とさない** — 契約 T3 の
             * `dropped` + `OVERFLOW` で残りの件数を申告する。 */
            let rest = (len - i + 7) / 8;
            ring::add_dropped(st, t.slot, rest as u16);
            return;
        }
        i += n;
    }
}

/// FEP を素通りしたキー 1 件を届ける。
fn deliver_key(st: &mut GuiState, ctx: input::Ctx, keydata: i32, mods: u32) {
    let scan = ((keydata >> 8) & 0x7F) as u8;
    let ch = (keydata & 0xFF) as u8;

    /* モーダル中は宛先をダイアログに限定する (契約 U4)。 */
    if modal::is_open() {
        if ctx != input::Ctx::Pump {
            modal::on_key(st, scan, ch);
        }
        return;
    }
    /* gshell 単独 (窓が 1 枚も無い) ときの WM ショートカット。 */
    if ctx == input::Ctx::Standalone && st.front_index().is_none() {
        input::standalone_key(st, scan);
        return;
    }
    let t = match input::focus_target(st) {
        Some(t) => t,
        None => return,
    };
    let serial = input::next_serial(st, t.slot);
    let ev = ring::ev_key(true, t.win_id, scan, ch, mods as u8, serial);
    ring::append(st, t.slot, &ev);

    /* 印字可能キーは `Text` も配送 (契約 U2a)。FEP オン中に印字可能キーが
     * 素通りすることは無い (かな入力として消費される) ので二重にならない。 */
    if ch >= 0x20 && ch <= 0x7E {
        let mut utf8 = [0u8; 8];
        utf8[0] = ch;
        let s2 = input::next_serial(st, t.slot);
        let evt = ring::ev_text(t.win_id, utf8, 1, s2);
        ring::append(st, t.slot, &evt);
    }
}

/// `ime_is_active()` を写し取り、関数表が繋がっていない間のモード表示を作る。
fn sync_mode(st: &mut GuiState) {
    let on = unsafe { (os32api::api().ime_is_active)() } != 0;
    let f = state();
    if on == f.on {
        return;
    }
    f.on = on;
    let _ = st;
    if f.kernel_drives {
        /* 未確定行はカーネルが描く (preedit_draw / preedit_clear)。 */
        return;
    }
    grid_clear_row(f, PREEDIT_ROW as i32, 0);
    if on {
        /* TVRAM 版の preedit_draw と同じ「[あ]」。 */
        grid_put(f, 0, PREEDIT_ROW as i32, b'[' as u32, K_ATTR_CYAN, 1);
        grid_put(f, 1, PREEDIT_ROW as i32, 0x3042, K_ATTR_CYAN, 2);
        grid_put(f, 3, PREEDIT_ROW as i32, b']' as u32, K_ATTR_CYAN, 1);
    }
    f.grid_dirty = true;
}

/* ================================================================ */
/*  レイアウト                                                       */
/* ================================================================ */

/// 描く原点 = フォーカス窓が `SET_TEXT_CURSOR` で知らせた位置 (画面座標)。
/// 知らせが無ければ TVRAM 版と同じ「画面最下行の左端」。
fn anchor(st: &GuiState) -> (i32, i32) {
    if let Some(i) = st.front_index() {
        let w = &st.windows[i];
        if w.tc_visible {
            let (ox, oy) = w.client_origin();
            return (ox + w.tc_x, oy + w.tc_y);
        }
    }
    (0, st.screen_h - CELL_H)
}

/// 行 y の内容がある列範囲。空なら (1, 0)。
fn row_bounds(f: &Fep, y: usize) -> (usize, usize) {
    let base = y * COLS;
    let mut c0 = COLS;
    let mut c1 = 0usize;
    let mut x = 0;
    while x < COLS {
        if f.grid.wide[base + x] != 0 {
            if x < c0 {
                c0 = x;
            }
            let end = x + (f.grid.wide[base + x] as usize) - 1;
            if end > c1 {
                c1 = end;
            }
        }
        x += 1;
    }
    if c0 == COLS {
        (1, 0)
    } else {
        (c0, c1)
    }
}

/// 箱の外形 (画素) を内容セル数から求める。
fn box_rect(x: i32, y: i32, cells_w: usize, rows: usize) -> Rect {
    Rect::new(
        x,
        y,
        (cells_w as i32) * CELL_W + PAD * 2 + 2,
        (rows as i32) * CELL_H + PAD * 2 + 2,
    )
}

fn clamp_box(st: &GuiState, r: Rect) -> Rect {
    let mut x = r.x;
    let mut y = r.y;
    if x + r.w > st.screen_w {
        x = st.screen_w - r.w;
    }
    if x < 0 {
        x = 0;
    }
    if y + r.h > st.screen_h {
        y = st.screen_h - r.h;
    }
    if y < 0 {
        y = 0;
    }
    Rect::new(x, y, r.w, r.h)
}

/// 未確定行と候補窓の箱を決める。返り値は 2 つの外接矩形。
fn compute_layout(st: &GuiState) -> Rect {
    let f = state();
    let (ax, ay) = anchor(st);

    /* 未確定行 (格子の PREEDIT_ROW)。 */
    let (pc0, pc1) = row_bounds(f, PREEDIT_ROW);
    f.pre_c0 = pc0;
    f.pre_c1 = pc1;
    f.pre_box = if pc1 >= pc0 {
        clamp_box(st, box_rect(ax, ay, pc1 - pc0 + 1, 1))
    } else {
        Rect::EMPTY
    };

    /* 候補窓 (PREEDIT_ROW より上の行すべて)。 */
    let mut r0 = ROWS;
    let mut r1 = 0usize;
    let mut c0 = COLS;
    let mut c1 = 0usize;
    let mut y = 0;
    while y < PREEDIT_ROW {
        let (a, b) = row_bounds(f, y);
        if b >= a {
            if y < r0 {
                r0 = y;
            }
            if y > r1 {
                r1 = y;
            }
            if a < c0 {
                c0 = a;
            }
            if b > c1 {
                c1 = b;
            }
        }
        y += 1;
    }
    if r0 == ROWS {
        f.cand_r0 = 1;
        f.cand_r1 = 0;
        f.cand_box = Rect::EMPTY;
    } else {
        f.cand_r0 = r0;
        f.cand_r1 = r1;
        f.cand_c0 = c0;
        f.cand_c1 = c1;
        /* 候補窓は未確定行の 1 行下 (ウィンドウの上に浮く)。 */
        let by = if f.pre_box.is_empty() { ay + CELL_H } else { f.pre_box.bottom() };
        f.cand_box = clamp_box(st, box_rect(ax, by, c1 - c0 + 1, r1 - r0 + 1));
    }
    f.pre_box.union(&f.cand_box)
}

/* ================================================================ */
/*  X3 の周期 (契約 T8)                                              */
/* ================================================================ */

/// 周の頭で呼ぶ: 新しい配置を決め、**旧 ∪ 新**を下地の損傷として積む。
/// これで [`wm::flush_screen_dirty`] がデスクトップ / クロームを描き直し、
/// 下のウィンドウには `Paint` が出る (候補窓を閉じたときの再描画 = 票 A-4)。
pub fn pre_cycle(st: &mut GuiState) {
    {
        let f = state();
        if !f.grid_dirty && !f.redraw {
            return;
        }
    }
    let newr = compute_layout(st);
    let old = state().shown;
    let dmg = old.union(&newr);
    if !dmg.is_empty() {
        damage_under(st, dmg);
    }
    let f = state();
    f.next = newr;
    f.pending_draw = true;
}

/// 周の尻で呼ぶ: 実際に画素を置いて present する (X3 の描画)。
pub fn post_cycle(st: &mut GuiState) {
    {
        let f = state();
        if !f.pending_draw {
            return;
        }
        f.pending_draw = false;
        f.grid_dirty = false;
        f.redraw = false;
        f.shown = f.next;
        if f.shown.is_empty() {
            return;
        }
    }
    cursor::hide(st);
    draw(st);
    cursor::show(st);
    let r = state().shown;
    wm::queue_present(st, r);
    let cr = cursor::rect(st);
    wm::queue_present(st, cr);
    wm::flush_present();
}

/// COMMIT (X2) や WM の再合成で下地を潰されたときの描き直し。
/// present は呼ぶ側がまとめる。掛かったら true。
pub fn refresh_if_hit(st: &mut GuiState, r: Rect) -> bool {
    let shown = state().shown;
    if shown.is_empty() || !shown.intersects(&r) {
        return false;
    }
    draw(st);
    true
}

/// 現在の描画矩形 (空 = 出ていない)。
#[inline]
pub fn rect() -> Rect {
    state().shown
}

/// 次の周で配置し直す (テキストカーソルが動いた等)。
#[inline]
pub fn mark_redraw() {
    state().redraw = true;
}

/// 下地を塗り直した直後に、その場で描き直す (present は呼ぶ側)。
pub fn redraw_now(st: &GuiState) {
    if state().shown.is_empty() {
        return;
    }
    draw(st);
}

/// 下にあるもの (デスクトップ / クローム / ウィンドウのクライアント面) を損傷にする。
fn damage_under(st: &mut GuiState, r: Rect) {
    st.dirty_screen(r);
    let mut i = 0;
    while i < GUI_MAX_WINDOWS {
        if st.windows[i].used && st.windows[i].visible {
            let (ox, oy) = st.windows[i].client_origin();
            damage::add_dirty(&mut st.windows[i], r.translate(-ox, -oy));
        }
        i += 1;
    }
}

/* ================================================================ */
/*  描画 (バックバッファ)                                            */
/* ================================================================ */

/// カーネル ATTR_* → G6 の役割色。
fn attr_color(attr: u8) -> u8 {
    match attr {
        K_ATTR_CYAN => GUI_COLOR_LINK,
        K_ATTR_GREEN => GUI_COLOR_OK,
        K_ATTR_YELLOW => GUI_COLOR_WARN,
        K_ATTR_RED => GUI_COLOR_CLOSE,
        K_ATTR_MAGENTA => GUI_COLOR_ACCENT,
        K_ATTR_WHITE => GUI_COLOR_WINDOW,
        _ => GUI_COLOR_WINDOW,
    }
}

/// コードポイントを UTF-8 (NUL 終端) にする。返り値はバイト数。
fn cp_to_utf8(cp: u32, out: &mut [u8; 5]) -> usize {
    let n;
    if cp < 0x80 {
        out[0] = cp as u8;
        n = 1;
    } else if cp < 0x800 {
        out[0] = 0xC0 | ((cp >> 6) as u8);
        out[1] = 0x80 | ((cp & 0x3F) as u8);
        n = 2;
    } else if cp < 0x10000 {
        out[0] = 0xE0 | ((cp >> 12) as u8);
        out[1] = 0x80 | (((cp >> 6) & 0x3F) as u8);
        out[2] = 0x80 | ((cp & 0x3F) as u8);
        n = 3;
    } else {
        out[0] = 0xF0 | ((cp >> 18) as u8);
        out[1] = 0x80 | (((cp >> 12) & 0x3F) as u8);
        out[2] = 0x80 | (((cp >> 6) & 0x3F) as u8);
        out[3] = 0x80 | ((cp & 0x3F) as u8);
        n = 4;
    }
    out[n] = 0;
    n
}

fn draw(st: &GuiState) {
    let f = state();
    if !f.pre_box.is_empty() {
        draw_box(st, f.pre_box);
        draw_cells(f, PREEDIT_ROW, PREEDIT_ROW, f.pre_c0, f.pre_box);
    }
    if !f.cand_box.is_empty() {
        draw_box(st, f.cand_box);
        draw_cells(f, f.cand_r0, f.cand_r1, f.cand_c0, f.cand_box);
    }
}

/// 箱 (黒地 + 白枠)。TVRAM 版の「黒背景に白/水色/黄」に合わせる。
fn draw_box(_st: &GuiState, r: Rect) {
    unsafe {
        gfx::gfx_fill_rect(r.x, r.y, r.w, r.h, GUI_COLOR_TEXT);
        gfx::gfx_rect(r.x, r.y, r.w, r.h, GUI_COLOR_WINDOW);
    }
}

fn draw_cells(f: &Fep, r0: usize, r1: usize, c0: usize, boxr: Rect) {
    unsafe {
        gfx::kcg_set_scale(1);
    }
    let mut y = r0;
    while y <= r1 && y < ROWS {
        let base = y * COLS;
        let py = boxr.y + 1 + PAD + ((y - r0) as i32) * CELL_H;
        let mut x = c0;
        while x < COLS {
            let w = f.grid.wide[base + x];
            if w != 0 {
                let px = boxr.x + 1 + PAD + ((x - c0) as i32) * CELL_W;
                let mut buf = [0u8; 5];
                cp_to_utf8(f.grid.cp[base + x], &mut buf);
                unsafe {
                    gfx::kcg_draw_utf8(
                        px,
                        py,
                        buf.as_ptr(),
                        attr_color(f.grid.attr[base + x]),
                        GUI_COLOR_TEXT,
                    );
                }
            }
            x += 1;
        }
        y += 1;
    }
}
