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
//!  終了  ESC → gfx_shutdown → sys_switch_shell("/sys/shell.bin") → return
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
//! | `desktop.rs` | 背景と手引き (v1.2 でタスクバー) |
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
mod slot;
mod timer;
mod visible;
mod wm;

use os32api::gfx;
use os32api::KernelAPI;

/// 「CUI へ」で戻る先 (契約 T9)。
static CUI_SHELL: &[u8] = b"/sys/shell.bin\0";

/// F1〜F4 で起動する確認用アプリ (ゲート G2 / G3 の検証用)。gshell 単独時
/// (窓が 1 枚も無いとき) に [`input::standalone_key`] が横取りして起動する。
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
        /* 待ちは sys_halt のみ (get_tick スピン禁止)。 */
        unsafe { (os32api::api().sys_halt)() };
    }

    /* ---- 「CUI へ」(契約 T9) ---- */
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

/// 外部プログラムを 1 本走らせる。戻ってくるまでこの関数は返らない
/// (その間 WM はアプリの `gui_call` の中でだけ走る = 契約 T8)。
/// 終了時にカーネルが `gui_owner_exit` を呼ぶので、窓は自動で回収される。
fn launch_app(st: &mut wm::GuiState) {
    cursor::hide(st);
    /* 前のアプリ宛の CTRL+STOP を持ち越さない (カーネル側 g_ring3_abort_req と同じ扱い)。 */
    st.abort_seen = false;
    /* フルスクリーン GFX プログラムに備えてパレット全体を退避する (契約 G6/G8)。 */
    let saved = wm::save_palette();
    /* F1〜F4 / F5 のファイル選択が置いたパス。無ければ既定のデモ。 */
    let path: *const u8 = if st.launch_path_len > 0 {
        st.launch_path.as_ptr()
    } else {
        LAUNCH_APPS[0].as_ptr()
    };
    unsafe {
        let a = os32api::api();
        (a.exec_run)(path);
        /* アプリがフルスクリーン GFX を使って抜けた場合に備えて描画モードを戻す。 */
        (a.gfx_init)();
    }
    st.launch_path_len = 0;
    /* 退避しておいた 16 色をそのまま戻し、念のためシステム色を入れ直してから、
     * まだ生きているリースがあれば再適用する。 */
    wm::restore_palette(&saved);
    wm::install_system_palette();
    lease::reapply(st);
    visible::recompute_and_expose(st);
    wm::composite_full(st);
}
