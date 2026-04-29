/*
 * hello_gfx — GFX "Hello World!" テストプログラム (Rust)
 *
 * OS32外部プログラムとしてRustで記述した最初のGFXデモ。
 * libos32gfx (C) とリンクし、400ラインモードでテキストを描画する。
 */
#![no_std]
#![no_main]

use core::panic::PanicInfo;

/* ================================================================ */
/*  KernelAPI 構造体 — os32_kapi_generated.h と完全一致             */
/*                                                                  */
/*  全152フィールド + sbrk_heap_limit を正確に定義する。             */
/*  i386では sizeof(関数ポインタ) == sizeof(u32) == 4。              */
/*  フィールドのインデックスはCヘッダの宣言順と同一 (0-indexed)。    */
/*                                                                  */
/*  使用するフィールドのみ型付きで宣言し、                          */
/*  残りは usize でパディングする。                                 */
/* ================================================================ */
#[repr(C)]
pub struct KernelAPI {
    /* idx  0 */ pub magic: u32,
    /* idx  1 */ pub version: u32,
    /* idx  2 */ pub gfx_init: unsafe extern "C" fn(),
    /* idx  3 */ _gfx_init_200: usize,
    /* idx  4 */ pub gfx_shutdown: unsafe extern "C" fn(),
    /* idx  5 */ pub gfx_present: unsafe extern "C" fn(),
    /* idx  6 */ _kbd_trygetchar: usize,
    /* idx  7 */ _mem_alloc: usize,
    /* idx  8 */ _mem_free: usize,
    /* idx  9 */ _get_tick: usize,
    /* idx 10 */ pub kprintf: unsafe extern "C" fn(attr: u8, fmt: *const u8, ...),
    /* idx 11..19: sys_unlink 〜 rtc_read (9個) */
    _skip_11_19: [usize; 9],
    /* idx 20 */ pub tvram_clear: unsafe extern "C" fn(),
    /* idx 21..23: tvram_putchar_at, tvram_putkanji_at, tvram_scroll (3個) */
    _skip_21_23: [usize; 3],
    /* idx 24 */ pub kbd_getchar: unsafe extern "C" fn() -> i32,
    /* idx 25..89: kbd_getkey 〜 sys_fstat (65個) */
    _skip_25_89: [usize; 65],
    /* idx 90 */ pub gfx_set_palette: unsafe extern "C" fn(idx: i32, r: u8, g: u8, b: u8),
    /* idx 91 */ _gfx_get_palette: usize,
    /* idx 92 */ pub gfx_get_framebuffer: unsafe extern "C" fn(fb: *mut u8),
    /* idx 93 */ pub gfx_add_dirty_rect: unsafe extern "C" fn(x: i32, y: i32, w: i32, h: i32),
    /* idx 94 */ pub gfx_present_dirty: unsafe extern "C" fn(),
    /* idx 95..151: gfx_present_nosync 〜 db_mem_used (57個) */
    _skip_95_151: [usize; 57],
    /* idx 152 */ pub sbrk_heap_limit: u32,
}

/* ================================================================ */
/*  libos32gfx (C) の外部関数宣言                                    */
/* ================================================================ */
extern "C" {
    fn libos32gfx_init(api: *mut KernelAPI);
    fn gfx_clear(color: u8);
    fn gfx_fill_rect(x: i32, y: i32, w: i32, h: i32, color: u8);
    fn gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u8);
    fn kcg_set_scale(scale: i32);
    fn kcg_draw_utf8(x: i32, y: i32, s: *const u8, fg: u8, bg: u8) -> i32;
}

/* ================================================================ */
/*  メイン関数                                                       */
/*                                                                  */
/*  crt0_c.c から main(argc, argv, api) として呼ばれる。            */
/*  GFXモードで "Hello World!" を描画し、キー待ちで終了。           */
/* ================================================================ */
#[no_mangle]
pub extern "C" fn main(
    _argc: i32,
    _argv: *const *const u8,
    api: *mut KernelAPI,
) -> i32 {
    unsafe {
        let a = &*api;

        /* GFXモード初期化 (400ラインモード) */
        libos32gfx_init(api);

        /* 背景をクリア (色0 = 黒) */
        gfx_clear(0);

        /* 背景装飾: カラーバー */
        gfx_fill_rect(0, 0,   640, 60, 8);   /* 赤 */
        gfx_fill_rect(0, 60,  640, 60, 10);  /* 黄 */
        gfx_fill_rect(0, 120, 640, 60, 11);  /* 緑 */
        gfx_fill_rect(0, 180, 640, 60, 12);  /* 水色 */
        gfx_fill_rect(0, 240, 640, 60, 13);  /* 青 */
        gfx_fill_rect(0, 300, 640, 100, 1);  /* 暗い青 */

        /* 中央に黒背景の矩形パネル */
        gfx_fill_rect(120, 130, 400, 140, 0);
        gfx_rect(118, 128, 404, 144, 7);
        gfx_rect(116, 126, 408, 148, 6);

        /* テキスト描画: "Hello World!" (拡大) */
        kcg_set_scale(2);
        kcg_draw_utf8(172, 160, b"Hello World!\0".as_ptr(), 7, 0);

        /* テキスト描画: "Rust on OS32 / PC-9801" (通常サイズ) */
        kcg_set_scale(1);
        kcg_draw_utf8(208, 220, b"Rust on OS32 / PC-9801\0".as_ptr(), 6, 0);

        /* VRAM転送 */
        (a.gfx_add_dirty_rect)(0, 0, 640, 400);
        (a.gfx_present_dirty)();

        /* キー入力待ち */
        (a.kbd_getchar)();

        /* GFXモード終了 → テキスト画面復帰 */
        (a.gfx_shutdown)();
        (a.tvram_clear)();
    }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
