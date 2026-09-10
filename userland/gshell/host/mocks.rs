//! Explicit host substitutes: pixel RAM, font patterns, input samples and heap.
//! These are not evidence of guest KAPI/heap/backend correctness.
use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Mutex,
};
pub const W: usize = 640;
pub const H: usize = 480;
static PIXELS: Mutex<Vec<u8>> = Mutex::new(Vec::new());
pub static READS: AtomicUsize = AtomicUsize::new(0);
pub static ALLOCS: AtomicUsize = AtomicUsize::new(0);
pub static FREES: AtomicUsize = AtomicUsize::new(0);
pub static MOUSE: Mutex<(i16, i16, u8)> = Mutex::new((0, 0, 0));
pub fn clear(c: u8) {
    *PIXELS.lock().unwrap() = vec![c; W * H];
}
pub fn pixels() -> Vec<u8> {
    PIXELS.lock().unwrap().clone()
}
#[no_mangle]
pub extern "C" fn gfx_pixel(x: i32, y: i32, c: u8) {
    if x >= 0 && y >= 0 && (x as usize) < W && (y as usize) < H {
        let mut p = PIXELS.lock().unwrap();
        if !p.is_empty() {
            p[y as usize * W + x as usize] = c;
        }
    }
}
#[no_mangle]
pub extern "C" fn gfx_get_pixel(x: i32, y: i32) -> u8 {
    if x >= 0 && y >= 0 && (x as usize) < W && (y as usize) < H {
        PIXELS
            .lock()
            .unwrap()
            .get(y as usize * W + x as usize)
            .copied()
            .unwrap_or(0)
    } else {
        0
    }
}
#[no_mangle]
pub extern "C" fn gfx_hline(x: i32, y: i32, w: i32, c: u8) {
    for xx in x..x + w {
        gfx_pixel(xx, y, c);
    }
}
#[no_mangle]
pub extern "C" fn gfx_vline(x: i32, y: i32, h: i32, c: u8) {
    for yy in y..y + h {
        gfx_pixel(x, yy, c);
    }
}
#[no_mangle]
pub extern "C" fn gfx_fill_rect(x: i32, y: i32, w: i32, h: i32, c: u8) {
    for yy in y..y + h {
        gfx_hline(x, yy, w, c);
    }
}
#[no_mangle]
pub extern "C" fn gfx_rect(x: i32, y: i32, w: i32, h: i32, c: u8) {
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}
#[no_mangle]
pub extern "C" fn gfx_line(mut x: i32, mut y: i32, x1: i32, y1: i32, c: u8) {
    let dx = (x1 - x).abs();
    let sx = if x < x1 { 1 } else { -1 };
    let dy = -(y1 - y).abs();
    let sy = if y < y1 { 1 } else { -1 };
    let mut e = dx + dy;
    loop {
        gfx_pixel(x, y, c);
        if x == x1 && y == y1 {
            break;
        }
        let e2 = 2 * e;
        if e2 >= dy {
            e += dy;
            x += sx;
        }
        if e2 <= dx {
            e += dx;
            y += sy;
        }
    }
}
#[no_mangle]
pub extern "C" fn kcg_set_scale(_: i32) {}
#[no_mangle]
pub unsafe extern "C" fn kcg_draw_utf8(x: i32, y: i32, s: *const u8, fg: u8, bg: u8) -> i32 {
    let mut n = 0;
    while *s.add(n) != 0 && n < 256 {
        n += 1;
    }
    let text = std::str::from_utf8(std::slice::from_raw_parts(s, n)).unwrap_or("?");
    let mut xx = x;
    for c in text.chars() {
        let w = if c.is_ascii() { 8 } else { 16 };
        gfx_fill_rect(xx, y, w, 16, bg);
        gfx_hline(xx, y, w, fg);
        xx += w;
    }
    xx - x
}
/* lib/utf8.c の unicode_to_ank と同じ対応。全角は 0 (= FEP のセル幅判定が
 * 2 桁扱いにする)。以前は libos32term_render::ank を借りていたが、gshell は
 * もう T4/T5R に依存しないのでここへ写した。 */
#[no_mangle]
pub extern "C" fn unicode_to_ank(cp: u32) -> u8 {
    match cp {
        n @ 0x20..=0x7e => n as u8,
        0xa5 => 0x5c,
        n @ 0xff61..=0xff9f => (n - 0xff61 + 0xa1) as u8,
        n @ 0xff01..=0xff5e => (n - 0xff01 + 0x21) as u8,
        _ => 0,
    }
}
unsafe extern "C" fn ank(_: u8, p: *mut u8) {
    READS.fetch_add(1, Ordering::SeqCst);
    std::ptr::write_bytes(p, 0xa5, 16);
}
unsafe extern "C" fn kanji(_: u16, p: *mut u8) {
    READS.fetch_add(1, Ordering::SeqCst);
    std::ptr::write_bytes(p, 0xa5, 32);
}
unsafe extern "C" fn alloc(n: u32) -> *mut u8 {
    ALLOCS.fetch_add(1, Ordering::SeqCst);
    std::alloc::alloc(std::alloc::Layout::from_size_align(n as usize, 4).unwrap())
}
/* 数えるだけ。gshell 側に mem_alloc を呼ぶ経路がもう無く、確保長を控えて
 * いないので、ここで dealloc すると layout 不一致になる。試験プロセスは
 * 短命なので意図的に leak させる。 */
unsafe extern "C" fn free(_p: *mut u8) {
    FREES.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn zero() -> u32 {
    0
}
unsafe extern "C" fn zero_i32() -> i32 {
    0
}
unsafe extern "C" fn no_key() -> i32 {
    -1
}
unsafe extern "C" fn nothing() {}
unsafe extern "C" fn dirty(_: i32, _: i32, _: i32, _: i32) {}
unsafe extern "C" fn mouse(p: *mut u8) {
    let (x, y, b) = *MOUSE.lock().unwrap();
    std::ptr::write_bytes(p, 0, 10);
    p.cast::<i16>().write(x);
    p.add(2).cast::<i16>().write(y);
    p.add(8).write(b);
}
pub struct Shm {
    ptr: *mut u8,
    len: usize,
}
impl Shm {
    pub fn new() -> Self {
        unsafe extern "C" {
            fn mmap(p: *mut u8, n: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
        }
        let len = 0x30000 + 16 * 16384;
        let ptr = unsafe { mmap(core::ptr::null_mut(), len, 3, 0x22 | 0x40, -1, 0) };
        assert!(
            (ptr as usize) < u32::MAX as usize && !ptr.is_null(),
            "MAP_32BIT required for exact u32 SHM code"
        );
        Self { ptr, len }
    }
    pub fn base(&self) -> u32 {
        self.ptr as u32
    }
}
impl Drop for Shm {
    fn drop(&mut self) {
        unsafe extern "C" {
            fn munmap(p: *mut u8, n: usize) -> i32;
        }
        unsafe {
            munmap(self.ptr, self.len);
        }
    }
}
unsafe extern "C" fn palette(_: i32, _: u8, _: u8, _: u8) {}
unsafe extern "C" fn get_palette(_: i32, r: *mut u8, g: *mut u8, b: *mut u8) {
    r.write(0);
    g.write(0);
    b.write(0);
}
unsafe extern "C" fn render(_: *mut u8) {}
/// テストが積む生キー (`kbd_trygetrawkey` が 1 件ずつ返す)。空なら -1。
pub static RAWKEYS: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// テストが積むカーネル FEP (`ime_feed_key`) の返り値。空なら 0x100 (Pass)。
pub static IME_SCRIPT: Mutex<Vec<i32>> = Mutex::new(Vec::new());
unsafe extern "C" fn raw_key() -> i32 {
    let mut q = RAWKEYS.lock().unwrap();
    if q.is_empty() {
        -1
    } else {
        q.remove(0)
    }
}
unsafe extern "C" fn ime_feed(_keydata: i32) -> i32 {
    let mut q = IME_SCRIPT.lock().unwrap();
    if q.is_empty() {
        0x100
    } else {
        q.remove(0)
    }
}
unsafe extern "C" fn ime_active() -> i32 {
    1
}
/// FEP を on にして、`ime_feed_key` に `script` を仕込む。
pub fn fep_script(script: &[i32]) {
    unsafe {
        (*os32api::api_ptr()).ime_is_active = ime_active;
        (*os32api::api_ptr()).ime_feed_key = ime_feed;
    }
    *IME_SCRIPT.lock().unwrap() = script.to_vec();
}
pub fn push_rawkeys(keys: &[i32]) {
    RAWKEYS.lock().unwrap().extend_from_slice(keys);
}

pub fn init() {
    let mut a = os32api::mock_api();
    a.get_tick = zero;
    a.sys_time = zero;
    a.kbd_dropped_count = zero;
    a.kbd_trygetrawkey = raw_key;
    a.mouse_poll = mouse;
    a.gfx_init = nothing;
    a.gfx_shutdown = nothing;
    a.tvram_clear = nothing;
    a.gfx_set_palette = palette;
    a.gfx_get_palette = get_palette;
    a.ime_set_render = render;
    a.ime_is_active = zero_i32;
    a.kcg_read_ank = ank;
    a.kcg_read_kanji = kanji;
    a.mem_alloc = alloc;
    a.mem_free = free;
    a.gfx_add_dirty_rect = dirty;
    a.gfx_present_dirty = nothing;
    RAWKEYS.lock().unwrap().clear();
    IME_SCRIPT.lock().unwrap().clear();
    os32api::os32_init(Box::into_raw(Box::new(a)));
    clear(9);
}
