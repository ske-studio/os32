//! shlib.rs — 共有ライブラリ `libos32gui.shlib` の先頭ページ (票 C3)。
//!
//! ```text
//!   0x400000  ┌──────────────────────────┐  MEM_SHLIB_BASE / .shlib_hdr
//!             │ OS32ShlibHeader (32B)    │  magic / version / nfunc /
//!             │                          │  data_vaddr / data_pages / text_pages
//!             │ entry[0..nfunc] (u32)    │  関数の絶対アドレス (**末尾追記のみ**)
//!   +0x1000   ├──────────────────────────┤  ← .text / .rodata (read-only, 全 PD 共有)
//!             │ .text .rodata            │
//!    (page)   ├──────────────────────────┤  ← __shlib_data_start (data_vaddr)
//!             │ .data .bss               │  アプリごとの物理ページ (K3)
//!             └──────────────────────────┘
//! ```
//!
//! `data_vaddr` / `data_pages` / `text_pages` は `sdk/link/shlib.ld` が定義する
//! 絶対シンボルから埋まる (`.long __shlib_*`)。`tools/mkshlib.py` が ELF を読んで
//! 値を検算し、0 のままなら書き込む。
//!
//! 鉄則:
//! - **末尾追記のみ**。既存の番号を動かしたら、古いアプリが別の関数へ飛ぶ。
//! - `static mut` はすべて `.data`/`.bss` (アプリごと)。`.text` へ書く経路は作らない。
//!
//! # 版を上げるとき (3 か所を必ず一緒に)
//!
//! 1. `sdk/rust/os32api/src/gui/proto.rs` の `GUI_PROTO_VERSION`
//!    (+ C 側 `sdk/include/os32/os32_gui_shared.h`)
//! 2. 下の `global_asm!` の `.long <version>` (ヘッダ 0x04)
//! 3. `make clean` → `make all` で**ライブラリとアプリを両方**焼き直す
//!
//! 1 と 2 がずれると `python3 tools/mkshlib.py --check` がビルドを止める。
//! 版を上げてアプリだけ古いままなら、アプリは `bind()` で `magic`/`version` を
//! 見て `dbg_print` + `sys_exit` する — **黙って別の関数へ飛ぶ stale の罠は作らない**。
//!
//! # ローダ (K3) への要件
//!
//! - `.data` は初期値を持つ (Rust の静的表がここに載る)。アプリごとの物理ページは
//!   **ゼロ埋めではなく、ライブラリイメージの `.data` を複製**すること。その後ろの
//!   `.bss` 部 (OS32X ヘッダの `bss_size`) だけをゼロにする。
//! - 複製する範囲は `data_vaddr` から `data_pages` ページ。`text_pages` (先頭の
//!   ヘッダページを含む) は read-only + USER で全 PD に共有する。
//!
//! # ジャンプ表 (**正典**。`os32api::gui::stub` の `E_*` はこの写し)
//!
//! ```text
//!  #  シンボル                          役割
//!  0  os32gui_shlib_init                KAPI をライブラリへ渡し、libos32gfx を掴む
//!  ---- client (契約 T) ----
//!  1  os32gui_client_init               OP_INIT。slot 番号 or 負のエラー
//!  2  os32gui_dbg_print
//!  3  os32gui_dbg_print_num
//!  4  os32gui_is_inited
//!  5  os32gui_slot
//!  6  os32gui_slot_base
//!  7  os32gui_raw_call                  gui_call 1 回
//!  8  os32gui_args_ptr
//!  9  os32gui_read_header
//! 10  os32gui_poll
//! 11  os32gui_wait
//! 12  os32gui_enter_handler
//! 13  os32gui_leave_handler
//! 14  os32gui_commit
//! 15  os32gui_invalidate
//! 16  os32gui_client_stats
//! 17  os32gui_trace_tick
//! 18  os32gui_lease_palette
//! 19  os32gui_utf8_seq_len
//! 20  os32gui_utf8_truncate
//!  ---- 生のウィンドウ op / タイマ (契約 U1 / U5) ----
//! 21  os32gui_win_create
//! 22  os32gui_win_destroy
//! 23  os32gui_win_raise
//! 24  os32gui_win_set_focus
//! 25  os32gui_win_client_rect
//! 26  os32gui_win_move
//! 27  os32gui_win_resize
//! 28  os32gui_win_show
//! 29  os32gui_win_set_title
//! 30  os32gui_win_set_text_cursor
//! 31  os32gui_timer_set
//! 32  os32gui_timer_kill
//!  ---- クリップ (契約 G2) ----
//! 33  os32gui_set_base_clip
//! 34  os32gui_clear_base_clip
//! 35  os32gui_push_clip
//! 36  os32gui_pop_clip
//! 37  os32gui_current_clip
//!  ---- 描画 (契約 G2 / G5 / G7) ----
//! 38  os32gui_fill_rect
//! 39  os32gui_draw_rect
//! 40  os32gui_hline
//! 41  os32gui_vline
//! 42  os32gui_line
//! 43  os32gui_blit
//! 44  os32gui_text
//! 45  os32gui_measure_text
//! 46  os32gui_screen_info
//! 47  os32gui_gfx_stats
//! 48  os32gui_base_violation_count
//!  ---- サーフェス (契約 G3) ----
//! 49  os32gui_create_surface
//! 50  os32gui_destroy_surface
//! 51  os32gui_create_window_surface
//! 52  os32gui_screen_surface
//! 53  os32gui_surface_size
//!  ---- ウィジェット (契約 U6) ----
//! 54  os32gui_w_row
//! 55  os32gui_w_column
//! 56  os32gui_w_label
//! 57  os32gui_w_button
//! 58  os32gui_w_checkbox
//! 59  os32gui_w_textbox
//! 60  os32gui_w_listbox
//! 61  os32gui_w_add
//! 62  os32gui_w_set_cross
//! 63  os32gui_w_set_min
//! 64  os32gui_w_set_text
//! 65  os32gui_w_get_text
//! 66  os32gui_w_set_checked
//! 67  os32gui_w_is_checked
//! 68  os32gui_w_set_enabled
//! 69  os32gui_w_set_hidden
//! 70  os32gui_w_rect
//! 71  os32gui_w_list_clear
//! 72  os32gui_w_list_add
//! 73  os32gui_w_list_selection
//! 74  os32gui_w_list_set_selection
//! 75  os32gui_w_list_item_text
//! 76  os32gui_w_set_focus
//! 77  os32gui_w_focused
//! 78  os32gui_w_resolve
//! 79  os32gui_w_id_of
//!  ---- ウィンドウ所有型の下請け (契約 U1 / T4) ----
//! 80  os32gui_window_create
//! 81  os32gui_window_surface
//! 82  os32gui_window_client_size
//! 83  os32gui_window_set_root
//! 84  os32gui_window_relayout
//! 85  os32gui_window_invalidate
//! 86  os32gui_window_is_focused
//! 87  os32gui_window_drop
//! 88  os32gui_window_count
//!  ---- U3 ループと Ui ----
//! 89  os32gui_run
//! 90  os32gui_flush_damage
//! 91  os32gui_ui_quit
//! 92  os32gui_ui_is_quitting
//! 93  os32gui_ui_input_unknown
//! 94  os32gui_ui_key_is_pressed
//!  ---- v1.2 デスクトップ client API (票 C4、契約 V12-C / V12-I) ----
//! 95  os32gui_modal_open                MODAL_OPEN の薄い包み → DialogId
//! 96  os32gui_modal_result              MODAL_RESULT 1 回。値を caller buffer へ
//! 97  os32gui_file_open                 GUI_MODAL_FILE_OPEN を開くだけ
//! 98  os32gui_input_open                GUI_MODAL_INPUT を開くだけ
//! 99  os32gui_session_request           SESSION_REQUEST (LAUNCH/SWITCH_CUI/SHUTDOWN)
//! 100 os32gui_draw_icon16               16x16 アイコン (mask 付き) を描く
//! ```
#![allow(clippy::missing_safety_doc)]

use core::ffi::c_void;
use core::ptr;

use crate::client;
use crate::uistate::s;
use crate::widget;
use crate::window;
use os32api::gui::proto::{GuiEvent, GuiRgb, GuiSlotHeader, GuiWinSpec};
use os32api::gui::stub::{
    style_of, AppVTable, GuiIcon16, Ui, WidgetId, SIZE_ABSOLUTE, SIZE_FIXED, SIZE_FLEX,
};
use os32api::gui::types::{Rect, ScreenInfo, Stats, SurfaceId};
use os32api::KernelAPI;

/* ================================================================ */
/*  先頭ページ — ヘッダ 32B + entry[]                                 */
/*                                                                  */
/*  const で関数アドレスを整数化できない (Rust の const eval が       */
/*  ポインタ→整数を許さない) ので、表はアセンブラで置く。`.long sym`   */
/*  は非 PIE リンクで R_386_32 として解決され、絶対アドレスが入る。    */
/*  data_vaddr / data_pages / text_pages はリンカスクリプトが定義する  */
/*  絶対シンボルを参照する (shlib.ld)。                                */
/* ================================================================ */

core::arch::global_asm!(
    r#"
    .section .shlib_hdr,"a",@progbits
    .globl  __os32_shlib_header
    .align  4
__os32_shlib_header:
    .long   0x42494C53                  /* 0x00 magic  'SLIB'            */
    .long   1                           /* 0x04 version = GUI_PROTO_VERSION */
    .long   101                         /* 0x08 nfunc                    */
    .long   __shlib_data_start          /* 0x0C data_vaddr               */
    .long   __shlib_data_pages          /* 0x10 data_pages               */
    .long   __shlib_text_pages          /* 0x14 text_pages               */
    .long   0                           /* 0x18 _rsvd[0]                 */
    .long   0                           /* 0x1C _rsvd[1]                 */
    /* 0x20: entry[] — 末尾追記のみ */
    .long   os32gui_shlib_init                  /*  0 */
    .long   os32gui_client_init                 /*  1 */
    .long   os32gui_dbg_print                   /*  2 */
    .long   os32gui_dbg_print_num               /*  3 */
    .long   os32gui_is_inited                   /*  4 */
    .long   os32gui_slot                        /*  5 */
    .long   os32gui_slot_base                   /*  6 */
    .long   os32gui_raw_call                    /*  7 */
    .long   os32gui_args_ptr                    /*  8 */
    .long   os32gui_read_header                 /*  9 */
    .long   os32gui_poll                        /* 10 */
    .long   os32gui_wait                        /* 11 */
    .long   os32gui_enter_handler               /* 12 */
    .long   os32gui_leave_handler               /* 13 */
    .long   os32gui_commit                      /* 14 */
    .long   os32gui_invalidate                  /* 15 */
    .long   os32gui_client_stats                /* 16 */
    .long   os32gui_trace_tick                  /* 17 */
    .long   os32gui_lease_palette               /* 18 */
    .long   os32gui_utf8_seq_len                /* 19 */
    .long   os32gui_utf8_truncate               /* 20 */
    .long   os32gui_win_create                  /* 21 */
    .long   os32gui_win_destroy                 /* 22 */
    .long   os32gui_win_raise                   /* 23 */
    .long   os32gui_win_set_focus               /* 24 */
    .long   os32gui_win_client_rect             /* 25 */
    .long   os32gui_win_move                    /* 26 */
    .long   os32gui_win_resize                  /* 27 */
    .long   os32gui_win_show                    /* 28 */
    .long   os32gui_win_set_title               /* 29 */
    .long   os32gui_win_set_text_cursor         /* 30 */
    .long   os32gui_timer_set                   /* 31 */
    .long   os32gui_timer_kill                  /* 32 */
    .long   os32gui_set_base_clip               /* 33 */
    .long   os32gui_clear_base_clip             /* 34 */
    .long   os32gui_push_clip                   /* 35 */
    .long   os32gui_pop_clip                    /* 36 */
    .long   os32gui_current_clip                /* 37 */
    .long   os32gui_fill_rect                   /* 38 */
    .long   os32gui_draw_rect                   /* 39 */
    .long   os32gui_hline                       /* 40 */
    .long   os32gui_vline                       /* 41 */
    .long   os32gui_line                        /* 42 */
    .long   os32gui_blit                        /* 43 */
    .long   os32gui_text                        /* 44 */
    .long   os32gui_measure_text                /* 45 */
    .long   os32gui_screen_info                 /* 46 */
    .long   os32gui_gfx_stats                   /* 47 */
    .long   os32gui_base_violation_count        /* 48 */
    .long   os32gui_create_surface              /* 49 */
    .long   os32gui_destroy_surface             /* 50 */
    .long   os32gui_create_window_surface       /* 51 */
    .long   os32gui_screen_surface              /* 52 */
    .long   os32gui_surface_size                /* 53 */
    .long   os32gui_w_row                       /* 54 */
    .long   os32gui_w_column                    /* 55 */
    .long   os32gui_w_label                     /* 56 */
    .long   os32gui_w_button                    /* 57 */
    .long   os32gui_w_checkbox                  /* 58 */
    .long   os32gui_w_textbox                   /* 59 */
    .long   os32gui_w_listbox                   /* 60 */
    .long   os32gui_w_add                       /* 61 */
    .long   os32gui_w_set_cross                 /* 62 */
    .long   os32gui_w_set_min                   /* 63 */
    .long   os32gui_w_set_text                  /* 64 */
    .long   os32gui_w_get_text                  /* 65 */
    .long   os32gui_w_set_checked               /* 66 */
    .long   os32gui_w_is_checked                /* 67 */
    .long   os32gui_w_set_enabled               /* 68 */
    .long   os32gui_w_set_hidden                /* 69 */
    .long   os32gui_w_rect                      /* 70 */
    .long   os32gui_w_list_clear                /* 71 */
    .long   os32gui_w_list_add                  /* 72 */
    .long   os32gui_w_list_selection            /* 73 */
    .long   os32gui_w_list_set_selection        /* 74 */
    .long   os32gui_w_list_item_text            /* 75 */
    .long   os32gui_w_set_focus                 /* 76 */
    .long   os32gui_w_focused                   /* 77 */
    .long   os32gui_w_resolve                   /* 78 */
    .long   os32gui_w_id_of                     /* 79 */
    .long   os32gui_window_create                /* 80 */
    .long   os32gui_window_surface              /* 81 */
    .long   os32gui_window_client_size          /* 82 */
    .long   os32gui_window_set_root             /* 83 */
    .long   os32gui_window_relayout             /* 84 */
    .long   os32gui_window_invalidate           /* 85 */
    .long   os32gui_window_is_focused           /* 86 */
    .long   os32gui_window_drop                 /* 87 */
    .long   os32gui_window_count                /* 88 */
    .long   os32gui_run                         /* 89 */
    .long   os32gui_flush_damage                /* 90 */
    .long   os32gui_ui_quit                     /* 91 */
    .long   os32gui_ui_is_quitting              /* 92 */
    .long   os32gui_ui_input_unknown            /* 93 */
    .long   os32gui_ui_key_is_pressed           /* 94 */
    .long   os32gui_modal_open                  /* 95 */
    .long   os32gui_modal_result                /* 96 */
    .long   os32gui_file_open                   /* 97 */
    .long   os32gui_input_open                  /* 98 */
    .long   os32gui_session_request             /* 99 */
    .long   os32gui_draw_icon16                 /* 100 */
    .text
"#
);

/* ================================================================ */
/*  小道具                                                           */
/* ================================================================ */

/// 生ポインタ + 長さを `&[u8]` に戻す (NULL / 0 は空スライス)。
#[inline]
unsafe fn slice<'a>(p: *const u8, len: u32) -> &'a [u8] {
    if p.is_null() || len == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, len as usize)
    }
}

#[inline]
unsafe fn slice_mut<'a>(p: *mut u8, len: u32) -> &'a mut [u8] {
    if p.is_null() || len == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, len as usize)
    }
}

/// `GuiResult<()>` を 0 / 負のエラーへ。
#[inline]
fn r0(r: client::GuiResult<()>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(e) => e.code(),
    }
}

/* ================================================================ */
/*  0: 初期化                                                        */
/* ================================================================ */

/// アプリの KAPI をライブラリへ渡し、libos32gfx を掴む。
///
/// `bind()` (アプリ側) が版照合の直後に 1 回呼ぶ。gshell 配下でないアプリ
/// (`gdi_test`) は自前で GFX モードに入ってからここに来るので、`gfx_init` は
/// しない — framebuffer 記述子とサーフェス/スプライトのプールだけ取り直す。
#[no_mangle]
pub extern "C" fn os32gui_shlib_init(api: *mut KernelAPI) -> i32 {
    if api.is_null() {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    os32api::os32_init(api);
    client::attach_gfx();
    0
}

/* ================================================================ */
/*  1..=20: client (契約 T)                                          */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_client_init() -> i32 {
    match client::init() {
        Ok(slot) => slot as i32,
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_dbg_print(p: *const u8, len: u32) {
    client::dbg_print(unsafe { slice(p, len) })
}

#[no_mangle]
pub extern "C" fn os32gui_dbg_print_num(p: *const u8, len: u32, v: i32) {
    client::dbg_print_num(unsafe { slice(p, len) }, v)
}

#[no_mangle]
pub extern "C" fn os32gui_is_inited() -> u32 {
    client::is_inited() as u32
}

#[no_mangle]
pub extern "C" fn os32gui_slot() -> u32 {
    client::slot()
}

#[no_mangle]
pub extern "C" fn os32gui_slot_base() -> *mut u8 {
    client::slot_base()
}

#[no_mangle]
pub extern "C" fn os32gui_raw_call(op: u32, arg: u32) -> i32 {
    match client::call(op, arg) {
        Ok(v) => v,
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_args_ptr() -> *mut u8 {
    client::args_ptr()
}

#[no_mangle]
pub extern "C" fn os32gui_read_header(out: *mut GuiSlotHeader) {
    if out.is_null() {
        return;
    }
    unsafe { ptr::write_unaligned(out, client::read_header()) }
}

/// `out[0..cap]` へ取り出す。戻り値: 件数 (負はエラー)。
/// `dropped` / `overflow` は NULL 可。
#[no_mangle]
pub extern "C" fn os32gui_poll(
    out: *mut GuiEvent,
    cap: u32,
    dropped: *mut u16,
    overflow: *mut u8,
) -> i32 {
    if out.is_null() || cap == 0 {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    let buf = unsafe { core::slice::from_raw_parts_mut(out, cap as usize) };
    match client::poll(buf) {
        Ok(p) => {
            if !dropped.is_null() {
                unsafe { ptr::write_unaligned(dropped, p.dropped) }
            }
            if !overflow.is_null() {
                unsafe { ptr::write_unaligned(overflow, p.overflow as u8) }
            }
            p.count as i32
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_wait(timeout_ticks: u32) -> i32 {
    match client::wait(timeout_ticks) {
        Ok(v) => v,
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_enter_handler() {
    client::enter_handler()
}

#[no_mangle]
pub extern "C" fn os32gui_leave_handler() {
    client::leave_handler()
}

#[no_mangle]
pub extern "C" fn os32gui_commit(window: u32) -> i32 {
    r0(client::commit(window))
}

#[no_mangle]
pub extern "C" fn os32gui_invalidate(window: u32, rect: Rect) -> i32 {
    r0(client::invalidate(window, rect))
}

#[no_mangle]
pub extern "C" fn os32gui_client_stats(out: *mut Stats) -> i32 {
    match client::stats() {
        Ok(v) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, v) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

/// 記録があれば `*out` に tick 下位 16bit を入れて 1、無ければ 0。
#[no_mangle]
pub extern "C" fn os32gui_trace_tick(serial: u16, out: *mut u16) -> i32 {
    match client::trace_tick(serial) {
        Some(t) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, t) }
            }
            1
        }
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn os32gui_lease_palette(first: u16, entries: *const GuiRgb, n: u32) -> i32 {
    if entries.is_null() || n == 0 {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    let e = unsafe { core::slice::from_raw_parts(entries, n as usize) };
    match client::lease_palette(first, e) {
        Ok(v) => v,
        Err(err) => err.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_utf8_seq_len(b: u8) -> u32 {
    client::utf8_seq_len(b) as u32
}

#[no_mangle]
pub extern "C" fn os32gui_utf8_truncate(p: *const u8, len: u32, max: u32) -> u32 {
    client::utf8_truncate(unsafe { slice(p, len) }, max as usize) as u32
}

/* ================================================================ */
/*  21..=32: 生のウィンドウ op / タイマ                               */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_win_create(spec: *const GuiWinSpec, out: *mut u32) -> i32 {
    if spec.is_null() {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    let sp = unsafe { ptr::read_unaligned(spec) };
    match client::win_create(&sp) {
        Ok(id) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, id) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_win_destroy(window: u32) -> i32 {
    r0(client::win_destroy(window))
}

#[no_mangle]
pub extern "C" fn os32gui_win_raise(window: u32) -> i32 {
    r0(client::win_raise(window))
}

#[no_mangle]
pub extern "C" fn os32gui_win_set_focus(window: u32) -> i32 {
    r0(client::win_set_focus(window))
}

#[no_mangle]
pub extern "C" fn os32gui_win_client_rect(window: u32, out: *mut Rect) -> i32 {
    match client::win_client_rect(window) {
        Ok(r) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, r) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_win_move(window: u32, x: i32, y: i32) -> i32 {
    r0(client::win_move(window, x as i16, y as i16))
}

#[no_mangle]
pub extern "C" fn os32gui_win_resize(window: u32, w: i32, h: i32) -> i32 {
    r0(client::win_resize(window, w as i16, h as i16))
}

#[no_mangle]
pub extern "C" fn os32gui_win_show(window: u32, show: u32) -> i32 {
    r0(client::win_show(window, show != 0))
}

#[no_mangle]
pub extern "C" fn os32gui_win_set_title(window: u32, p: *const u8, len: u32) -> i32 {
    r0(client::win_set_title(window, unsafe { slice(p, len) }))
}

#[no_mangle]
pub extern "C" fn os32gui_win_set_text_cursor(window: u32, x: i32, y: i32, visible: u32) -> i32 {
    r0(client::win_set_text_cursor(
        window,
        x as i16,
        y as i16,
        visible != 0,
    ))
}

#[no_mangle]
pub extern "C" fn os32gui_timer_set(window: u32, id: u32, interval_ticks: u32, repeat: u32) -> i32 {
    r0(client::timer_set(
        window,
        id as u8,
        interval_ticks as u16,
        repeat != 0,
    ))
}

#[no_mangle]
pub extern "C" fn os32gui_timer_kill(window: u32, id: u32) -> i32 {
    r0(client::timer_kill(window, id as u8))
}

/* ================================================================ */
/*  33..=37: クリップ (契約 G2)                                      */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_set_base_clip(surface: u32, rect: Rect) -> i32 {
    crate::clip::set_base_clip(SurfaceId(surface), rect)
}

#[no_mangle]
pub extern "C" fn os32gui_clear_base_clip() {
    crate::clip::clear_base_clip()
}

#[no_mangle]
pub extern "C" fn os32gui_push_clip(rect: Rect) -> i32 {
    crate::clip::push_clip(rect)
}

#[no_mangle]
pub extern "C" fn os32gui_pop_clip() {
    crate::clip::pop_clip()
}

#[no_mangle]
pub extern "C" fn os32gui_current_clip(out: *mut Rect) {
    if out.is_null() {
        return;
    }
    unsafe { ptr::write_unaligned(out, crate::clip::current_clip()) }
}

/* ================================================================ */
/*  38..=48: 描画 (契約 G2 / G5 / G7)                                */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_fill_rect(surface: u32, rect: Rect, style: u32) {
    crate::draw::fill_rect(SurfaceId(surface), rect, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_draw_rect(surface: u32, rect: Rect, style: u32) {
    crate::draw::draw_rect(SurfaceId(surface), rect, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_hline(surface: u32, x: i32, y: i32, w: i32, style: u32) {
    crate::draw::hline(SurfaceId(surface), x, y, w, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_vline(surface: u32, x: i32, y: i32, h: i32, style: u32) {
    crate::draw::vline(SurfaceId(surface), x, y, h, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_line(
    surface: u32,
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
    style: u32,
) {
    crate::draw::line(SurfaceId(surface), x0, y0, x1, y1, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_blit(surface: u32, dx: i32, dy: i32, bitmap: u32, src_rect: Rect) {
    crate::draw::blit(SurfaceId(surface), dx, dy, SurfaceId(bitmap), src_rect)
}

#[no_mangle]
pub extern "C" fn os32gui_text(
    surface: u32,
    x: i32,
    y: i32,
    p: *const u8,
    len: u32,
    style: u32,
) -> i32 {
    crate::draw::text(SurfaceId(surface), x, y, unsafe { slice(p, len) }, style_of(style))
}

#[no_mangle]
pub extern "C" fn os32gui_measure_text(p: *const u8, len: u32, ow: *mut i32, oh: *mut i32) {
    let (w, h) = crate::draw::measure_text(unsafe { slice(p, len) });
    if !ow.is_null() {
        unsafe { ptr::write_unaligned(ow, w) }
    }
    if !oh.is_null() {
        unsafe { ptr::write_unaligned(oh, h) }
    }
}

#[no_mangle]
pub extern "C" fn os32gui_screen_info(out: *mut ScreenInfo) {
    if out.is_null() {
        return;
    }
    unsafe { ptr::write_unaligned(out, crate::draw::screen_info()) }
}

#[no_mangle]
pub extern "C" fn os32gui_gfx_stats(out: *mut Stats) {
    if out.is_null() {
        return;
    }
    unsafe { ptr::write_unaligned(out, crate::draw::stats()) }
}

#[no_mangle]
pub extern "C" fn os32gui_base_violation_count() -> u32 {
    crate::draw::base_violation_count()
}

/* ================================================================ */
/*  49..=53: サーフェス (契約 G3)                                    */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_create_surface(w: i32, h: i32) -> u32 {
    crate::surface::create_surface(w, h).raw()
}

#[no_mangle]
pub extern "C" fn os32gui_destroy_surface(id: u32) -> i32 {
    crate::surface::destroy_surface(SurfaceId(id))
}

#[no_mangle]
pub extern "C" fn os32gui_create_window_surface(rect: Rect) -> u32 {
    crate::surface::create_window_surface(rect).raw()
}

#[no_mangle]
pub extern "C" fn os32gui_screen_surface() -> u32 {
    crate::surface::screen_surface().raw()
}

#[no_mangle]
pub extern "C" fn os32gui_surface_size(id: u32, ow: *mut i32, oh: *mut i32) {
    let (w, h) = crate::surface::surface_size(SurfaceId(id));
    if !ow.is_null() {
        unsafe { ptr::write_unaligned(ow, w) }
    }
    if !oh.is_null() {
        unsafe { ptr::write_unaligned(oh, h) }
    }
}

/* ================================================================ */
/*  54..=79: ウィジェット (契約 U6)                                  */
/* ================================================================ */

#[inline]
fn wid(r: client::GuiResult<WidgetId>, out: *mut u32) -> i32 {
    match r {
        Ok(id) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, id.raw()) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_w_row(pad: i32, gap: i32, out: *mut u32) -> i32 {
    wid(widget::row(pad as i16, gap as i16), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_column(pad: i32, gap: i32, out: *mut u32) -> i32 {
    wid(widget::column(pad as i16, gap as i16), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_label(p: *const u8, len: u32, out: *mut u32) -> i32 {
    wid(widget::label(unsafe { slice(p, len) }), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_button(p: *const u8, len: u32, out: *mut u32) -> i32 {
    wid(widget::button(unsafe { slice(p, len) }), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_checkbox(p: *const u8, len: u32, checked: u32, out: *mut u32) -> i32 {
    wid(widget::checkbox(unsafe { slice(p, len) }, checked != 0), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_textbox(p: *const u8, len: u32, out: *mut u32) -> i32 {
    wid(widget::textbox(unsafe { slice(p, len) }), out)
}

#[no_mangle]
pub extern "C" fn os32gui_w_listbox(out: *mut u32) -> i32 {
    wid(widget::listbox(), out)
}

/// `kind` は `SIZE_FIXED` / `SIZE_FLEX` / `SIZE_ABSOLUTE` (契約 U7)。
/// `v` は Fixed の px / Flex の重み、`rect` は Absolute のときだけ意味を持つ。
#[no_mangle]
pub extern "C" fn os32gui_w_add(parent: u32, child: u32, kind: u32, v: i32, rect: Rect) -> i32 {
    let spec = match kind {
        SIZE_FIXED => crate::layout::SizeSpec::Fixed(v as i16),
        SIZE_FLEX => crate::layout::SizeSpec::Flex(v as u16),
        SIZE_ABSOLUTE => crate::layout::SizeSpec::Absolute(rect),
        _ => return os32api::gui::proto::OS32_ERR_INVAL,
    };
    r0(widget::add(WidgetId(parent), WidgetId(child), spec))
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_cross(id: u32, v: i32) {
    widget::set_cross(WidgetId(id), v as i16)
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_min(id: u32, w: i32, h: i32) {
    widget::set_min(WidgetId(id), w as i16, h as i16)
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_text(id: u32, p: *const u8, len: u32) -> i32 {
    r0(widget::set_text(WidgetId(id), unsafe { slice(p, len) }))
}

#[no_mangle]
pub extern "C" fn os32gui_w_get_text(id: u32, out: *mut u8, cap: u32) -> u32 {
    widget::text(WidgetId(id), unsafe { slice_mut(out, cap) }) as u32
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_checked(id: u32, on: u32) -> i32 {
    r0(widget::set_checked(WidgetId(id), on != 0))
}

#[no_mangle]
pub extern "C" fn os32gui_w_is_checked(id: u32) -> u32 {
    widget::is_checked(WidgetId(id)) as u32
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_enabled(id: u32, on: u32) {
    widget::set_enabled(WidgetId(id), on != 0)
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_hidden(id: u32, hidden: u32) {
    widget::set_hidden(WidgetId(id), hidden != 0)
}

#[no_mangle]
pub extern "C" fn os32gui_w_rect(id: u32, out: *mut Rect) {
    if out.is_null() {
        return;
    }
    unsafe { ptr::write_unaligned(out, widget::rect(WidgetId(id))) }
}

#[no_mangle]
pub extern "C" fn os32gui_w_list_clear(id: u32) -> i32 {
    r0(widget::list_clear(WidgetId(id)))
}

#[no_mangle]
pub extern "C" fn os32gui_w_list_add(id: u32, p: *const u8, len: u32, out: *mut i32) -> i32 {
    match widget::list_add(WidgetId(id), unsafe { slice(p, len) }) {
        Ok(i) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, i) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_w_list_selection(id: u32) -> i32 {
    widget::list_selection(WidgetId(id))
}

#[no_mangle]
pub extern "C" fn os32gui_w_list_set_selection(id: u32, index: i32) -> i32 {
    r0(widget::list_set_selection(WidgetId(id), index))
}

#[no_mangle]
pub extern "C" fn os32gui_w_list_item_text(id: u32, index: i32, out: *mut u8, cap: u32) -> u32 {
    widget::list_item_text(WidgetId(id), index, unsafe { slice_mut(out, cap) }) as u32
}

#[no_mangle]
pub extern "C" fn os32gui_w_set_focus(id: u32) -> i32 {
    r0(widget::set_focus(WidgetId(id)))
}

/// 窓スロット `win` でフォーカス中のウィジェット。無効なら 0。
#[no_mangle]
pub extern "C" fn os32gui_w_focused(win: u32) -> u32 {
    let st = s();
    let i = win as usize;
    if i >= st.windows.len() || !st.windows[i].used {
        return 0;
    }
    widget::focused(i).raw()
}

/// `WidgetId` → スロット添字 (無効なら -1)。
#[no_mangle]
pub extern "C" fn os32gui_w_resolve(id: u32) -> i32 {
    match widget::resolve(WidgetId(id)) {
        Some(i) => i as i32,
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn os32gui_w_id_of(idx: u32) -> u32 {
    let i = idx as usize;
    if i >= os32api::gui::proto::GUI_MAX_WIDGETS {
        return 0;
    }
    widget::id_of(i).raw()
}

/* ================================================================ */
/*  80..=88: ウィンドウ所有型の下請け                                 */
/* ================================================================ */

#[no_mangle]
pub extern "C" fn os32gui_window_create(spec: *const GuiWinSpec, out: *mut u32) -> i32 {
    if spec.is_null() {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    let sp = unsafe { ptr::read_unaligned(spec) };
    match window::create_raw(&sp) {
        Ok(id) => {
            if !out.is_null() {
                unsafe { ptr::write_unaligned(out, id) }
            }
            0
        }
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_window_surface(id: u32) -> u32 {
    window::surface_of(id).raw()
}

#[no_mangle]
pub extern "C" fn os32gui_window_client_size(id: u32, ow: *mut i16, oh: *mut i16) {
    let (w, h) = window::client_size_of(id);
    if !ow.is_null() {
        unsafe { ptr::write_unaligned(ow, w) }
    }
    if !oh.is_null() {
        unsafe { ptr::write_unaligned(oh, h) }
    }
}

#[no_mangle]
pub extern "C" fn os32gui_window_set_root(id: u32, root: u32) -> i32 {
    r0(window::set_root_of(id, WidgetId(root)))
}

#[no_mangle]
pub extern "C" fn os32gui_window_relayout(id: u32) {
    window::relayout_of(id)
}

#[no_mangle]
pub extern "C" fn os32gui_window_invalidate(id: u32, rect: Rect) -> i32 {
    window::invalidate_of(id, rect);
    0
}

#[no_mangle]
pub extern "C" fn os32gui_window_is_focused(id: u32) -> u32 {
    window::is_focused_of(id) as u32
}

#[no_mangle]
pub extern "C" fn os32gui_window_drop(id: u32) {
    window::drop_window(id)
}

#[no_mangle]
pub extern "C" fn os32gui_window_count() -> u32 {
    window::count() as u32
}

/* ================================================================ */
/*  89..=94: U3 ループと Ui                                          */
/* ================================================================ */

/// U3 ループ本体。ハンドラはアプリ側 (`vt` / `this`)。
#[no_mangle]
pub extern "C" fn os32gui_run(vt: *const AppVTable, this: *mut c_void, ui: *mut Ui) -> i32 {
    if vt.is_null() || ui.is_null() {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    match crate::app::run_vt(vt, this, unsafe { &mut *ui }) {
        Ok(()) => 0,
        Err(e) => e.code(),
    }
}

#[no_mangle]
pub extern "C" fn os32gui_flush_damage() {
    crate::app::flush_damage()
}

#[no_mangle]
pub extern "C" fn os32gui_ui_quit() {
    s().quit = true;
}

#[no_mangle]
pub extern "C" fn os32gui_ui_is_quitting() -> u32 {
    s().quit as u32
}

#[no_mangle]
pub extern "C" fn os32gui_ui_input_unknown() -> u32 {
    s().input_unknown as u32
}

#[no_mangle]
pub extern "C" fn os32gui_ui_key_is_pressed(scan: u32) -> u32 {
    unsafe { ((os32api::api().kbd_is_pressed)(scan as i32) != 0) as u32 }
}

/* ================================================================ */
/*  95..=100: v1.2 デスクトップ client API (票 C4)                    */
/*                                                                  */
/*  戻り値の約束は他のエントリと同じ: 非負が成功、負は `OS32_ERR_*`。 */
/*  古い gshell では op 65 / 66 が `OS32_ERR_NOSYS` を返すので、       */
/*  ここはそれをそのまま呼び出し側へ通す (panic も loop もしない)。   */
/* ================================================================ */

/// `modal_open(parent, kind, message)` → DialogId (正) / 負のエラー。**待たない**。
#[no_mangle]
pub extern "C" fn os32gui_modal_open(parent: u32, kind: u32, msg: *const u8, len: u32) -> i32 {
    match crate::modal::modal_open(parent, kind as u16, unsafe { slice(msg, len) }) {
        Ok(id) => id as i32,
        Err(e) => e.code(),
    }
}

/// `modal_result(dialog, out, cap)` → `GUI_MODAL_RESULT_*` (非負) / 負のエラー。
///
/// `out` へ値を UTF-8 境界で `cap` バイトまで写す。`value_len` (NULL 可) には
/// WM が持っていた**本来の**長さ、`copied` (NULL 可) には実際に写した長さ。
/// **`on_modal` を受けた後にだけ呼ぶこと** (`OS32_ERR_AGAIN` は存在しない)。
#[no_mangle]
pub extern "C" fn os32gui_modal_result(
    dialog: u32,
    out: *mut u8,
    cap: u32,
    value_len: *mut u32,
    copied: *mut u32,
) -> i32 {
    let buf = unsafe { slice_mut(out, cap) };
    match crate::modal::modal_result(dialog as u16, buf) {
        Ok(r) => {
            if !value_len.is_null() {
                unsafe { ptr::write_unaligned(value_len, r.len as u32) }
            }
            if !copied.is_null() {
                unsafe { ptr::write_unaligned(copied, r.copied as u32) }
            }
            r.result as i32
        }
        Err(e) => e.code(),
    }
}

/// `file_open(parent, prompt)` → DialogId (正) / 負のエラー。path は同期返却しない。
#[no_mangle]
pub extern "C" fn os32gui_file_open(parent: u32, prompt: *const u8, len: u32) -> i32 {
    match crate::modal::file_open(parent, unsafe { slice(prompt, len) }) {
        Ok(id) => id as i32,
        Err(e) => e.code(),
    }
}

/// `input_open(parent, prompt)` → DialogId (正) / 負のエラー。text は同期返却しない。
#[no_mangle]
pub extern "C" fn os32gui_input_open(parent: u32, prompt: *const u8, len: u32) -> i32 {
    match crate::modal::input_open(parent, unsafe { slice(prompt, len) }) {
        Ok(id) => id as i32,
        Err(e) => e.code(),
    }
}

/// `session_request(action, value)` → 0 (**受理**。完了ではない) / 負のエラー。
///
/// `action` は `GUI_SESSION_LAUNCH` / `SWITCH_CUI` / `SHUTDOWN`。LAUNCH の
/// `value` は 1〜255B の絶対パスで、不正なら **WM を呼ばずに** `ERR_INVAL`。
#[no_mangle]
pub extern "C" fn os32gui_session_request(action: u32, value: *const u8, len: u32) -> i32 {
    if action > 0xFF {
        return os32api::gui::proto::OS32_ERR_INVAL;
    }
    r0(crate::session::session_request(action as u8, unsafe { slice(value, len) }))
}

/// `draw_icon16(surface, x, y, icon)` — 16x16 固定、mask=0 は描かない、拡大縮小なし。
#[no_mangle]
pub extern "C" fn os32gui_draw_icon16(surface: u32, x: i32, y: i32, icon: *const GuiIcon16) {
    if icon.is_null() {
        return;
    }
    /* アイコンは 160B なので値渡しせずアプリ側のバッファをそのまま読む
     * (align_of == 1 なので unaligned でも安全)。 */
    let ic = unsafe { ptr::read_unaligned(icon) };
    crate::icon::draw_icon16(SurfaceId(surface), x, y, &ic)
}
