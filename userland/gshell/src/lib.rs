//! gshell — OS32 GUI シェル / ウィンドウマネージャ本体 (票 W1)。
//!
//! シェル帯 (0x300000, CPL=0, owner 1) に常駐する WM。`shell.bin` と排他で、
//! 切替はカーネルのシェル起動ループ (`sys_switch_shell`、契約 T9) が行う。
//!
//! ```text
//!  起動  gfx_init → screen_info (G5) → G6 パレット → gui_register → デスクトップ
//!  単独  自分のループで X3 の周期を回す (入力 → WM の UI → present → sys_halt)
//!  アプリ実行中は WM はアプリの gui_call の中でだけ走る (契約 T8):
//!         X1 ハンドラ / X2 COMMIT / X3 WAIT / X4 ポンプ
//!  終了  Start → CUI mode / Shut Down → 確認 → SessionAction → top-level で実行
//!         (**これが唯一の経路**。ESC の即時切替と上部バーは G5 で製品から撤去、
//!          デバッグ時だけ DEBUG_SHORTCUTS で戻せる)
//! ```
//!
//! モジュールの役割:
//!
//! | ファイル | 役割 |
//! |---|---|
//! | `wm.rs`      | Window 表 (16)・Z 順・フォーカス・所有者・合成・present の集約 |
//! | `slot.rs`    | SHM スロット (T2) の番地解決とヘッダ / 要求 / 応答 / 取り込み tick |
//! | `ring.rs`    | イベントリング 128×16B (T3)。Pointer の畳み込み、OVERFLOW |
//! | `damage.rs`  | 損傷 dirty / issued (G4)。32px 境界・隣接結合・上限 8 |
//! | `visible.rs` | 可視領域 (互いに素な矩形 ≤16) と露出の再計算 |
//! | `chrome.rs`  | 枠・タイトルバー・閉じるボタン・ドラッグ枠 |
//! | `desktop.rs` | 背景と手引き |
//! | `taskbar.rs` | タスクバー (Start / 窓ボタン / 時計) と作業領域 (V12-D の D1/D3) |
//! | `startmenu.rs` | Start メニューと右クリックメニュー (D2 / D4) |
//! | `session.rs` | SessionAction・sticky Quit・トップレベル handoff (V12-S) |
//! | `cursor.rs`  | マウスカーソル (損傷とは別経路の退避・再描画) |
//! | `input.rs`   | 入力取り込み → Key / Text / Pointer / Button (T3 / U2a) |
//! | `fep.rs`     | 日本語入力 (U2a)。cooked 待ち行列 = FEP、未確定行と候補窓 |
//! | `lease.rs`   | 14 色パレットのリースとフォーカス追従 (G8) |
//! | `modal.rs`   | モーダルと標準ダイアログ (U4)。入れ子ループなし |
//! | `timer.rs`   | アプリタイマ 8 本 (U5) |
//! | `handler.rs` | `gui_call` ハンドラ (op → 関数表)。X1 / X2 / X3 |
//! | `pump.rs`    | syscall 境界ポンプ (X4) |
//! | `reqs.rs`    | 要求 / 応答構造体 (C `os32_gui_shared.h` の写し) |

#![no_std]

extern crate os32api;

mod chrome;
mod cursor;
mod damage;
mod desktop;
mod fep;
mod ffi;
mod handler;
mod input;
mod lease;
mod modal;
mod pump;
mod reqs;
mod ring;
mod session;
mod slot;
mod startmenu;
mod taskbar;
mod timer;
mod visible;
mod wm;

use os32api::gfx;
use os32api::gui::proto::{
    GUI_MODAL_OK, GUI_SESSION_LAUNCH, GUI_SESSION_SHUTDOWN, GUI_SESSION_SWITCH_CUI,
};
use os32api::KernelAPI;

/// 「CUI へ」で戻る先 (契約 T9)。
static CUI_SHELL: &[u8] = b"/sys/shell.bin\0";

/// 起動設定 (`GUI=0/1`)。CUI へ戻すときにここを書き換える (契約 S6 の 4)。
/// 正典は `include/config.h` の `SYS_SYSTEM_CFG`。
static SYSTEM_CFG: &[u8] = b"/etc/system.cfg\0";

/// **デバッグ専用スイッチ** (票 W3 §4.1、ユーザー決定 2026-09-06)。
///
/// v1.1 の「ESC で即 CUI」「上部の `OS32 GUI shell ESC:CUI F1..F5` バー」
/// 「F1〜F5 のランチャ」を出す。**G5 (v1.2 完成) で `false` に倒し、製品の
/// 出荷形はこの 3 つを持たない** (契約 S6、票 W3 §4.1〜4.2)。CUI へ戻る経路は
/// Start → "CUI mode" → 確認ダイアログ → SessionAction (`GUI=0` を永続化) だけ。
///
/// コード本体は**デバッグ設備として残してある** — 実機で GUI が起動直後に
/// 固まったときに `true` へ戻せば、Start メニューを操作できなくても ESC で
/// CUI へ抜けられる。`true` に倒すのは手元の調査中だけで、コミットはしない。
/// 参照先 ([`LAUNCH_APPS`] / [`input::standalone_key`] / [`desktop::draw_hint`] /
/// [`launch_app`]) はこの定数から到達可能なので、`false` でも警告は出ない。
pub(crate) const DEBUG_SHORTCUTS: bool = false;

/// F1〜F4 で起動する確認用アプリ (ゲート G2 / G3 の検証用、[`DEBUG_SHORTCUTS`])。
/// gshell 単独時 (窓が 1 枚も無いとき) に [`input::standalone_key`] が横取りする。
/// パスは `userland/deploy.yaml` の登録 (`/usr/bin/`) に合わせてある。
/// F5 は WM 自身のファイル選択ダイアログ (任意の .bin を選んで起動)。
pub(crate) static LAUNCH_APPS: [&[u8]; 4] = [
    b"/usr/bin/gui_demo.bin\0",   /* F1 — C2 のデモ (窓 2 枚・ウィジェット) */
    b"/usr/bin/gui_bench.bin\0",  /* F2 — 契約 P2 の入力→表示の測定器 */
    b"/usr/bin/gui_busy.bin\0",   /* F3 — K2 の計算中ポンプ / CTRL+STOP 脱出 */
    b"/usr/bin/lease_test.bin\0", /* F4 — 契約 G8 の 14 色リース */
];

/* ================================================================ */
/*  エントリ (crt0.asm が main(argc, argv, api) を呼ぶ)              */
/* ================================================================ */
#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8, api: *mut KernelAPI) -> i32 {
    if api.is_null() {
        return -1;
    }
    os32api::os32_init(api);

    let st = wm::g();
    *st = wm::GuiState::NEW;
    /* SHM の先頭 (KAPI のデータフィールド)。スロットは +192KB から 16KB ずつ。 */
    st.shm_base = unsafe { (*api).shm_base };

    /* GFX モードへ (libos32gfx_init → gfx_init + framebuffer + surface/sprite)。 */
    gfx::init();
    /* 640×400 / 16 色を決め打ちしない (契約 G5)。起動時に 1 回だけ読む。 */
    wm::read_screen_info(st);
    /* G6 のシステムパレットを入れる。 */
    wm::install_system_palette();
    /* FEP (契約 U2a)。GFX 版の IME_Render を用意する (K の KAPI 待ち)。 */
    fep::install();

    unsafe {
        let a = os32api::api();
        (a.mouse_set_bounds)(0, 0, (st.screen_w - 1) as i16, (st.screen_h - 1) as i16);
        /* ハード/テキストのカーソルは使わない (自前で描く)。 */
        (a.mouse_cursor_hide)();
    }
    st.mouse_x = st.screen_w / 2;
    st.mouse_y = st.screen_h / 2;
    st.cursor.x = st.mouse_x;
    st.cursor.y = st.mouse_y;
    st.inited = true;

    /* WM を登録する (契約 T1)。以後アプリの gui_call がここへ来る。
     * 失敗 (シェル帯以外から起動された等) でも単独 WM としては動く。 */
    unsafe {
        let a = os32api::api();
        (a.gui_register)(
            handler::gshell_gui_handler as *const () as *mut u8,
            pump::gshell_gui_pump as *const () as *mut u8,
        );
    }

    /* デスクトップ。全画面 present は WM だけ (契約 G4)。 */
    wm::composite_full(st);

    /* ---- 単独ループ (契約 T8: アプリが居ないときは gshell が X3 を回す) ---- */
    while !st.quit {
        wm::wm_cycle(st, input::Ctx::Standalone);
        if st.launch_pending {
            st.launch_pending = false;
            launch_app(st);
        }
        /* SessionAction のトップレベル handoff (契約 §7)。`launch_app` から
         * 戻った直後と、アプリが居ないときの周回でここを通る。**WM の文脈
         * (X1/X3/X4) からは絶対に来ない** = 入れ子 exec_run にならない。 */
        if session::pending_action() != 0 && !session::owner_active(st) && !session_handoff(st) {
            /* SWITCH_CUI が成立した (shell 切替済み)。ここで gshell を抜ける。 */
            return 0;
        }
        /* 待ちは sys_halt のみ (get_tick スピン禁止)。 */
        unsafe { (os32api::api().sys_halt)() };
    }

    /* ---- 「CUI へ」(デバッグ用の ESC = DEBUG_SHORTCUTS。契約 T9)。
     *      製品では `st.quit` が立たないのでここは通らない — 出荷形の CUI 復帰は
     *      下の `switch_cui` (SessionAction) 側だけ。 ---- */
    cursor::hide(st);
    st.inited = false;
    unsafe {
        let a = os32api::api();
        /* FEP の描画先を TVRAM 版へ戻す (レビュー #4 ①)。gshell が抜けると同じ
         * 0x300000 帯に shell.bin が載るので、GFX 版の関数表を残すとカーネル FEP が
         * 上書き済みコードへ間接 call する。gfx_shutdown より前に行う。 */
        (a.ime_set_render)(core::ptr::null_mut());
        (a.gfx_shutdown)();
        (a.tvram_clear)();
        (a.sys_switch_shell)(CUI_SHELL.as_ptr());
    }
    0
}

/* ================================================================ */
/*  アプリの起動 (単独ループから。G2 の目視確認用)                    */
/* ================================================================ */

/// デバッグ用の F1〜F5 経路 ([`DEBUG_SHORTCUTS`])。製品では `launch_pending` が
/// 立たないので呼ばれない (出荷形の起動は Start → Run... = `GUI_SESSION_LAUNCH`)。
fn launch_app(st: &mut wm::GuiState) {
    /* F1〜F4 / F5 のファイル選択が置いたパス。無ければ既定のデモ。 */
    let mut path = [0u8; 256];
    if st.launch_path_len > 0 {
        let mut i = 0;
        while i <= st.launch_path_len && i < 256 {
            path[i] = st.launch_path[i];
            i += 1;
        }
    } else {
        let p = LAUNCH_APPS[0];
        let mut i = 0;
        while i < p.len() && i < 256 {
            path[i] = p[i];
            i += 1;
        }
    }
    st.launch_path_len = 0;
    run_program(st, &path);
}

/// 外部プログラムを 1 本走らせる。戻ってくるまでこの関数は返らない
/// (その間 WM はアプリの `gui_call` の中でだけ走る = 契約 T8)。
/// 終了時にカーネルが `gui_owner_exit` を呼ぶので、窓は自動で回収される。
///
/// `path` は**この関数の呼び出し元が用意した NUL 終端の私有バッファ**
/// (契約 §7.1 の 3)。戻り値は `exec_run` の返り値。
fn run_program(st: &mut wm::GuiState, path: &[u8; 256]) -> i32 {
    cursor::hide(st);
    /* 前のアプリ宛の CTRL+STOP を持ち越さない (カーネル側 g_ring3_abort_req と同じ扱い)。 */
    st.abort_seen = false;
    /* フルスクリーン GFX プログラムに備えてパレット全体を退避する (契約 G6/G8)。 */
    let saved = wm::save_palette();
    let rc = unsafe {
        let a = os32api::api();
        let r = (a.exec_run)(path.as_ptr());
        /* アプリがフルスクリーン GFX を使って抜けた場合に備えて描画モードを戻す。 */
        (a.gfx_init)();
        r
    };
    /* 退避しておいた 16 色をそのまま戻し、念のためシステム色を入れ直してから、
     * まだ生きているリースがあれば再適用する。 */
    wm::restore_palette(&saved);
    wm::install_system_palette();
    lease::reapply(st);
    visible::recompute_and_expose(st);
    wm::composite_full(st);
    rc
}

/* ================================================================ */
/*  SessionAction のトップレベル handoff (契約 V12-S §7、票 W3 §7)   */
/* ================================================================ */

/// 保留中の SessionAction を 1 件実行する。
///
/// 戻り値 `true` = デスクトップを続ける、`false` = gshell の `main` を抜ける
/// (SWITCH_CUI が成立し `sys_switch_shell` 済み)。SHUTDOWN は返らない。
///
/// **呼べるのは gshell の単独ループ (top-level) だけ**。ここが「WM の文脈から
/// `exec_run()` を呼ばない」(契約 S2) の担保になっている。
fn session_handoff(st: &mut wm::GuiState) -> bool {
    /* Programs の cache は世代が変わるので捨てる (契約 D2)。 */
    startmenu::invalidate_cache();
    match session::pending_action() {
        GUI_SESSION_LAUNCH => {
            /* §7.1: path を NUL 終端の私有バッファへ写し、**先に consume してから**
             * 起動する (失敗した path を無限 retry しない)。 */
            let mut path = [0u8; session::PATH_MAX + 1];
            let n = session::copy_path(&mut path);
            session::clear();
            if n == 0 {
                return true;
            }
            let mut buf = [0u8; 256];
            let mut i = 0;
            while i <= n && i < 256 {
                buf[i] = path[i];
                i += 1;
            }
            if run_program(st, &buf) < 0 {
                modal::open_wm_message(
                    st,
                    GUI_MODAL_OK,
                    b"Launch failed (not found or not executable)\0",
                    modal::WM_PURPOSE_NOTIFY,
                );
            }
            true
        }
        GUI_SESSION_SWITCH_CUI => switch_cui(st),
        GUI_SESSION_SHUTDOWN => shutdown(st),
        _ => {
            session::clear();
            true
        }
    }
}

/// CUI へ戻す (契約 S6 の 1〜6)。cfg 更新に失敗したら**切替を実行せず**
/// デスクトップへ戻し、エラーを表示する (永続設定と実 shell の不一致を作らない)。
fn switch_cui(st: &mut wm::GuiState) -> bool {
    cursor::hide(st);
    st.inited = false;
    unsafe {
        let a = os32api::api();
        /* FEP の描画先を TVRAM 版へ戻す (gfx_shutdown より前)。 */
        (a.ime_set_render)(core::ptr::null_mut());
        (a.gfx_shutdown)();
    }
    if !cfg_set_gui(b"0") {
        /* 失敗: GFX を戻してデスクトップへ復帰し、action は捨てる。 */
        unsafe { (os32api::api().gfx_init)() };
        fep::install();
        st.inited = true;
        wm::install_system_palette();
        lease::reapply(st);
        visible::recompute_and_expose(st);
        wm::composite_full(st);
        session::clear();
        modal::open_wm_message(
            st,
            GUI_MODAL_OK,
            b"Cannot write /etc/system.cfg - staying in GUI\0",
            modal::WM_PURPOSE_NOTIFY,
        );
        return true;
    }
    session::clear();
    unsafe {
        let a = os32api::api();
        (a.tvram_clear)();
        (a.sys_switch_shell)(CUI_SHELL.as_ptr());
    }
    false
}

/// system halt (契約 S7)。`sys_halt()` は 1 回の `hlt` で IRQ 後に戻るので、
/// **必ず無限ループで呼ぶ**。通常コードへは二度と戻らない。
fn shutdown(st: &mut wm::GuiState) -> ! {
    cursor::hide(st);
    st.inited = false;
    unsafe {
        let a = os32api::api();
        (a.ime_set_render)(core::ptr::null_mut());
        (a.gfx_shutdown)();
        (a.tvram_clear)();
    }
    tvram_print(0, 0, b"System halted. Reset to restart.");
    loop {
        unsafe { (os32api::api().sys_halt)() };
    }
}

/// TVRAM へ ANK 文字列を 1 行置く (GFX を落とした後の最後の表示)。
fn tvram_print(x: i32, y: i32, s: &[u8]) {
    /* ATTR_WHITE (os32_kapi_shared.h)。 */
    const ATTR_WHITE: u8 = 0xE1;
    let a = unsafe { os32api::api() };
    let mut i = 0;
    while i < s.len() && s[i] != 0 {
        unsafe { (a.tvram_putchar_at)(x + i as i32, y, s[i], ATTR_WHITE) };
        i += 1;
    }
}

/* ================================================================ */
/*  /etc/system.cfg の GUI= 書き換え (契約 S6 の 4)                  */
/*                                                                  */
/*  userland/shell/cmd_sys.c の `cfg_set_key()` (os32gui on/off) と  */
/*  同じ規則: 既存の他キー行はそのまま残し、GUI= 行だけ差し替える。   */
/* ================================================================ */

/// `KEY=VALUE` 1 行分として確保しておく余白 (cmd_sys.c の CFG_LINE_RESERVE)。
const CFG_LINE_RESERVE: usize = 64;
const CFG_BUF: usize = 1024;
const CFG_OUT: usize = 1152;

fn cfg_set_gui(val: &[u8]) -> bool {
    let a = unsafe { os32api::api() };
    let mut buf = [0u8; CFG_BUF];
    let mut out = [0u8; CFG_OUT];

    /* 既存内容を読む (無ければ空から作る)。 */
    let mut n = 0usize;
    unsafe {
        let fd = (a.sys_open)(SYSTEM_CFG.as_ptr(), 0 /* KAPI_O_RDONLY */);
        if fd >= 0 {
            let r = (a.sys_read)(fd, buf.as_mut_ptr(), (CFG_BUF - 1) as u32);
            (a.sys_close)(fd);
            if r > 0 {
                n = r as usize;
            }
        }
    }

    /* 行ごとにコピー。旧 GUI= 行だけ捨てる。 */
    let mut o = 0usize;
    let mut i = 0usize;
    while i < n {
        let ls = i;
        while i < n && buf[i] != b'\n' {
            i += 1;
        }
        let len = i - ls;
        if i < n {
            i += 1; /* 改行を飛ばす */
        }
        if cfg_line_is_gui(&buf[ls..ls + len]) {
            continue;
        }
        let mut k = 0;
        while k < len && o < CFG_OUT - CFG_LINE_RESERVE {
            out[o] = buf[ls + k];
            o += 1;
            k += 1;
        }
        if o < CFG_OUT - 2 {
            out[o] = b'\n';
            o += 1;
        }
    }

    /* 新しい GUI=VALUE 行を追記。 */
    let key = b"GUI";
    let mut k = 0;
    while k < key.len() && o < CFG_OUT - 2 {
        out[o] = key[k];
        o += 1;
        k += 1;
    }
    if o < CFG_OUT - 2 {
        out[o] = b'=';
        o += 1;
    }
    let mut v = 0;
    while v < val.len() && val[v] != 0 && o < CFG_OUT - 2 {
        out[o] = val[v];
        o += 1;
        v += 1;
    }
    if o < CFG_OUT - 1 {
        out[o] = b'\n';
        o += 1;
    }

    unsafe {
        /* KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC (os32_kapi_shared.h)。 */
        let fd = (a.sys_open)(SYSTEM_CFG.as_ptr(), 0x01 | 0x0100 | 0x0200);
        if fd < 0 {
            return false;
        }
        let w = (a.sys_write)(fd, out.as_ptr(), o as u32);
        (a.sys_close)(fd);
        w == o as i32
    }
}

/// 行が `GUI=` の代入行か (前後空白許容。cmd_sys.c の `cfg_line_is_key`)。
fn cfg_line_is_gui(line: &[u8]) -> bool {
    let key = b"GUI";
    let mut i = 0usize;
    while i < line.len() && (line[i] == b' ' || line[i] == b'\t') {
        i += 1;
    }
    let mut k = 0usize;
    while k < key.len() {
        if i >= line.len() || line[i] != key[k] {
            return false;
        }
        i += 1;
        k += 1;
    }
    while i < line.len() && (line[i] == b' ' || line[i] == b'\t') {
        i += 1;
    }
    i < line.len() && line[i] == b'='
}
