/*
 * os32api — OS32 Rust外部プログラム共通クレート
 *
 * 全Rustプログラムが依存する共通インフラ:
 * - KernelAPI バインディング (kapi_generated.rs, 自動生成)
 * - グローバルアロケータ (mem_alloc/mem_free ベース)
 * - パニックハンドラ
 * - 安全ラッパーマクロ
 */
#![no_std]

/* kapi_rust_gen.py で自動生成されたKernelAPIバインディング */
pub mod kapi_generated;
pub use kapi_generated::*;

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::panic::PanicInfo;

/* ================================================================ */
/*  グローバル KernelAPI ポインタ                                     */
/*                                                                  */
/*  crt0_c.c で設定されるグローバル変数 `kapi` (C側) とは別に、      */
/*  Rust側でも api ポインタを保持する。                              */
/*  os32_init() で初期化する。                                       */
/* ================================================================ */
struct ApiHolder(UnsafeCell<*mut KernelAPI>);
unsafe impl Sync for ApiHolder {}

static API: ApiHolder = ApiHolder(UnsafeCell::new(core::ptr::null_mut()));

/// KernelAPIポインタを初期化する (main関数の冒頭で呼ぶこと)
pub fn os32_init(api: *mut KernelAPI) {
    unsafe {
        *API.0.get() = api;
    }
}

/// グローバルKernelAPIへの参照を取得する
///
/// # Safety
/// os32_init() が呼ばれた後にのみ使用可能。
#[inline]
pub unsafe fn api() -> &'static KernelAPI {
    &**API.0.get()
}

/// グローバルKernelAPIへの可変ポインタを取得する
#[inline]
pub unsafe fn api_ptr() -> *mut KernelAPI {
    *API.0.get()
}

/* ================================================================ */
/*  グローバルアロケータ                                             */
/*                                                                  */
/*  KernelAPI の mem_alloc / mem_free を使用して、                   */
/*  Rust の alloc クレート (Vec, String, Box 等) を使えるようにする。*/
/* ================================================================ */
struct Os32Alloc;

unsafe impl GlobalAlloc for Os32Alloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let api = &**API.0.get();
        /* mem_alloc はアライメント保証が 4 バイトのみ。
           大きなアライメントが必要な場合は余分に確保して調整が必要だが、
           i386では通常4バイトアライメントで十分。 */
        (api.mem_alloc)(layout.size() as u32)
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        let api = &**API.0.get();
        (api.mem_free)(ptr);
    }
}

#[global_allocator]
static ALLOCATOR: Os32Alloc = Os32Alloc;

/* ================================================================ */
/*  パニックハンドラ                                                 */
/* ================================================================ */
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    /* カーネルコンソールにエラーメッセージを出力 */
    unsafe {
        let p = API.0.get();
        if !(*p).is_null() {
            let a = &**p;
            (a.kprintf)(
                ATTR_RED,
                b"[PANIC] Rust program panicked!\r\n\0".as_ptr(),
            );
        }
    }
    loop {}
}

/* ================================================================ */
/*  libos32gfx (C) 外部関数宣言                                     */
/*  Rustプログラムから利用できるようにre-export                      */
/* ================================================================ */
pub mod gfx {
    use super::KernelAPI;

    extern "C" {
        pub fn libos32gfx_init(api: *mut KernelAPI);
        pub fn libos32gfx_shutdown();
        pub fn gfx_clear(color: u8);
        pub fn gfx_pixel(x: i32, y: i32, color: u8);
        pub fn gfx_hline(x: i32, y: i32, w: i32, color: u8);
        pub fn gfx_vline(x: i32, y: i32, h: i32, color: u8);
        pub fn gfx_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u8);
        pub fn gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u8);
        pub fn gfx_fill_rect(x: i32, y: i32, w: i32, h: i32, color: u8);
        pub fn gfx_circle(cx: i32, cy: i32, r: i32, color: u8);
        pub fn gfx_fill_circle(cx: i32, cy: i32, r: i32, color: u8);
        pub fn kcg_set_scale(scale: i32);
        pub fn kcg_draw_utf8(x: i32, y: i32, s: *const u8, fg: u8, bg: u8) -> i32;
    }
}
