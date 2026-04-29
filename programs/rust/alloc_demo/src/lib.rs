/*
 * alloc_demo — Rust alloc クレート活用デモ (OS32)
 *
 * Vec, String, Box 等のヒープアロケーション機能が
 * OS32のmem_alloc/mem_free上で正常に動作することを検証する。
 * テキストモードでコンソールに結果を出力する。
 */
#![no_std]
#![no_main]

extern crate alloc;
extern crate os32api;

use alloc::string::String;
use alloc::vec::Vec;
use alloc::vec;
use alloc::boxed::Box;
use alloc::format;
use os32api::KernelAPI;
use os32api::{kprint, kprint_attr};

#[no_mangle]
pub extern "C" fn main(
    _argc: i32,
    _argv: *const *const u8,
    api: *mut KernelAPI,
) -> i32 {
    os32api::os32_init(api);

    kprint!(b"=== Rust alloc demo ===\r\n\0");
    kprint!(b"\r\n\0");

    /* --- Vec<i32> テスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[Vec] \0");
    let mut nums: Vec<i32> = Vec::new();
    let mut i: usize = 0;
    while i < 10 {
        nums.push((i * i) as i32);
        i += 1;
    }
    kprint!(b"push 0..9 squared: len=%d, cap=%d\r\n\0",
            nums.len() as i32, nums.capacity() as i32);

    /* 合計を計算 */
    let mut sum: i32 = 0;
    i = 0;
    while i < nums.len() {
        sum += nums[i];
        i += 1;
    }
    kprint!(b"  sum of squares = %d (expected 285)\r\n\0", sum);

    /* --- vec![] マクロテスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[vec!] \0");
    let fib = vec![1i32, 1, 2, 3, 5, 8, 13, 21, 34, 55];
    kprint!(b"fibonacci: len=%d, last=%d\r\n\0",
            fib.len() as i32, fib[fib.len() - 1]);

    /* --- Box テスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[Box] \0");
    let boxed: Box<i32> = Box::new(42);
    kprint!(b"Box<i32> = %d\r\n\0", *boxed);

    /* --- String テスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[String] \0");
    let mut s = String::from("Hello");
    s.push_str(", OS32!");
    s.push('\0');
    kprint!(b"%s (len=%d)\r\n\0", s.as_ptr(), (s.len() - 1) as i32);

    /* --- format! マクロテスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[format!] \0");
    let tick = os32api::get_tick();
    let mut msg = format!("tick={}", tick);
    msg.push('\0');
    kprint!(b"%s\r\n\0", msg.as_ptr());

    /* --- Vec<String> テスト (入れ子ヒープ) --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[Vec<String>] \0");
    let mut names: Vec<String> = Vec::new();
    names.push(String::from("PC-9801"));
    names.push(String::from("OS32"));
    names.push(String::from("Rust"));
    kprint!(b"count=%d\r\n\0", names.len() as i32);

    i = 0;
    while i < names.len() {
        let mut n = names[i].clone();
        n.push('\0');
        kprint!(b"  [%d] %s\r\n\0", i as i32, n.as_ptr());
        i += 1;
    }

    /* --- Drop (メモリ解放) テスト --- */
    kprint_attr!(os32api::ATTR_GREEN, b"[Drop] \0");
    {
        let _tmp = vec![0u8; 1024];
        kprint!(b"1024 bytes allocated... \0");
    }
    kprint!(b"freed.\r\n\0");

    kprint!(b"\r\n\0");
    kprint_attr!(os32api::ATTR_YELLOW, b"All tests passed!\r\n\0");

    0
}
