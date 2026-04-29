/*
 * os32api — OS32 Rust外部プログラム共通クレート
 *
 * 全Rustプログラムが依存する共通インフラ:
 * - KernelAPI バインディング (kapi_generated.rs, 自動生成)
 * - グローバルアロケータ (mem_alloc/mem_free ベース)
 * - パニックハンドラ
 * - 安全ラッパーマクロ / ヘルパー関数
 */
#![no_std]

extern crate alloc;

/* kapi_rust_gen.py で自動生成されたKernelAPIバインディング */
pub mod kapi_generated;
pub use kapi_generated::*;

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::panic::PanicInfo;

/* ================================================================ */
/*  グローバル KernelAPI ポインタ                                     */
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
/*  コンソール出力ヘルパー                                           */
/* ================================================================ */

/// コンソールに文字列を出力する (白色)
pub fn print(s: &[u8]) {
    unsafe {
        let a = api();
        (a.kprintf)(ATTR_WHITE, b"%s\0".as_ptr(), s.as_ptr());
    }
}

/// コンソールに文字列を出力する (色指定)
pub fn print_attr(attr: u8, s: &[u8]) {
    unsafe {
        let a = api();
        (a.kprintf)(attr, b"%s\0".as_ptr(), s.as_ptr());
    }
}

/// コンソールに整数を出力する
pub fn print_int(attr: u8, val: i32) {
    unsafe {
        let a = api();
        (a.kprintf)(attr, b"%d\0".as_ptr(), val);
    }
}

/// kprintf を直接呼ぶマクロ (フォーマット文字列 + 引数)
///
/// 使用例:
/// ```
/// kprint!(b"Hello %s, tick=%d\r\n\0", name_ptr, tick);
/// ```
#[macro_export]
macro_rules! kprint {
    ($fmt:expr $(, $arg:expr)*) => {
        unsafe {
            let a = $crate::api();
            (a.kprintf)($crate::ATTR_WHITE, $fmt.as_ptr() $(, $arg)*);
        }
    };
}

/// 色付き kprintf マクロ
#[macro_export]
macro_rules! kprint_attr {
    ($attr:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe {
            let a = $crate::api();
            (a.kprintf)($attr, $fmt.as_ptr() $(, $arg)*);
        }
    };
}

/* ================================================================ */
/*  キーボード入力ヘルパー                                           */
/* ================================================================ */

/// キー入力を待つ (ブロッキング)
pub fn wait_key() -> i32 {
    unsafe { (api().kbd_getchar)() }
}

/// キー入力を試みる (ノンブロッキング、入力なし時は -1)
pub fn try_key() -> Option<i32> {
    let k = unsafe { (api().kbd_trygetchar)() };
    if k > 0 { Some(k) } else { None }
}

/* ================================================================ */
/*  システムヘルパー                                                 */
/* ================================================================ */

/// 現在のティックカウントを取得する (1tick = 10ms)
pub fn get_tick() -> u32 {
    unsafe { (api().get_tick)() }
}

/* ================================================================ */
/*  グローバルアロケータ                                             */
/* ================================================================ */
struct Os32Alloc;

unsafe impl GlobalAlloc for Os32Alloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let api = &**API.0.get();
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

    /* GFX 安全ラッパー */

    /// GFXモードを初期化する (400ラインモード)
    pub fn init() {
        unsafe {
            libos32gfx_init(super::api_ptr());
        }
    }

    /// GFXモードを終了しテキスト画面に復帰する
    pub fn shutdown() {
        unsafe {
            let a = super::api();
            (a.gfx_shutdown)();
            (a.tvram_clear)();
        }
    }

    /// バックバッファをVRAMに転送する (全画面)
    pub fn present() {
        unsafe {
            let a = super::api();
            (a.gfx_add_dirty_rect)(0, 0, 640, 400);
            (a.gfx_present_dirty)();
        }
    }

    /// 画面をクリアしてVRAMに転送する
    pub fn clear_and_present(color: u8) {
        unsafe {
            gfx_clear(color);
        }
        present();
    }

    /// テキストを描画する (UTF-8, NUL終端)
    pub fn draw_text(x: i32, y: i32, text: &[u8], fg: u8, bg: u8) {
        unsafe {
            kcg_draw_utf8(x, y, text.as_ptr(), fg, bg);
        }
    }

    /// テキストスケールを設定する (1=通常, 2=倍角)
    pub fn set_text_scale(scale: i32) {
        unsafe {
            kcg_set_scale(scale);
        }
    }
}

