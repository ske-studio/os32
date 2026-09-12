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
unsafe extern "C" fn dirty(_: i32, _: i32, _: i32, _: i32) {
    DIRTY_RECTS.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn present_dirty() {
    PRESENTS.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn mouse(p: *mut u8) {
    let (x, y, b) = *lk(&MOUSE);
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
/// いま入っている 16 色 (`gfx_set_palette` / `gfx_get_palette` の実体)。
pub static PALETTE: Mutex<[u8; 48]> = Mutex::new([0; 48]);
/// `gfx_set_palette(i, r, g, b)` の呼び出し列 (退避→復元の順序の観測点)。
pub static PALETTE_SETS: Mutex<Vec<(i32, u8, u8, u8)>> = Mutex::new(Vec::new());
unsafe extern "C" fn palette(i: i32, r: u8, g: u8, b: u8) {
    lk(&PALETTE_SETS).push((i, r, g, b));
    if (0..16).contains(&i) {
        let mut p = lk(&PALETTE);
        p[i as usize * 3] = r;
        p[i as usize * 3 + 1] = g;
        p[i as usize * 3 + 2] = b;
    }
}
unsafe extern "C" fn get_palette(i: i32, r: *mut u8, g: *mut u8, b: *mut u8) {
    let p = *lk(&PALETTE);
    let k = if (0..16).contains(&i) { i as usize } else { 0 };
    r.write(p[k * 3]);
    g.write(p[k * 3 + 1]);
    b.write(p[k * 3 + 2]);
}
/// `gfx_set_palette` の呼び出し列 (複製)。
pub fn palette_sets() -> Vec<(i32, u8, u8, u8)> {
    lk(&PALETTE_SETS).clone()
}
unsafe extern "C" fn render(_: *mut u8) {}
/// テストが積む生キー (`kbd_trygetrawkey` が 1 件ずつ返す)。空なら -1。
pub static RAWKEYS: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// テストが積むカーネル FEP (`ime_feed_key`) の返り値。空なら 0x100 (Pass)。
pub static IME_SCRIPT: Mutex<Vec<i32>> = Mutex::new(Vec::new());
unsafe extern "C" fn raw_key() -> i32 {
    let mut q = lk(&RAWKEYS);
    if q.is_empty() {
        -1
    } else {
        q.remove(0)
    }
}
unsafe extern "C" fn ime_feed(_keydata: i32) -> i32 {
    let mut q = lk(&IME_SCRIPT);
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
    *lk(&IME_SCRIPT) = script.to_vec();
}
pub fn push_rawkeys(keys: &[i32]) {
    lk(&RAWKEYS).extend_from_slice(keys);
}

/* ================================================================ */
/*  K5b-W: アプリ 4 本の同時実行 (KAPI v44) の差し替え                */
/*                                                                  */
/*  `exec_park` はゲストでは longjmp して**戻らない**。ホストでは戻る */
/*  しかないので、呼ばれた回数だけを数えて `PARK_RET` を返す。        */
/*  制御の流れ (park → top-level → resume) はカーネルの領分で、       */
/*  `tools/tests/multiapp_impl_host.c` が実物の AppSlot で検査済み。  */
/*  ここで押さえるのは **WM の判断** (誰を起こすか / いつ譲るか)。    */
/* ================================================================ */

/// `exec_park` が呼ばれた回数。
pub static PARKS: AtomicUsize = AtomicUsize::new(0);
/// `exec_park` の戻り値 (既定 `OS32_ERR_INVAL` = 譲れなかった)。
pub static PARK_RET: Mutex<i32> = Mutex::new(-1);
/// `exec_resume(app_id, wait_ret)` の呼び出し列。
pub static RESUMES: Mutex<Vec<(i32, i32)>> = Mutex::new(Vec::new());
/// `exec_resume` の戻り値の台本 (先頭から 1 件ずつ。空なら app_id = また park)。
pub static RESUME_SCRIPT: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `exec_start(cmdline)` の呼び出し列 (パス文字列)。
pub static STARTS: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());
/// `exec_start` の戻り値の台本 (空なら 2 = app_id 2 が park した)。
pub static START_SCRIPT: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `exec_kill(app_id)` の呼び出し列。
pub static KILLS: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `snd_focus(app_id)` の呼び出し列。
pub static SND_FOCUS: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `exec_abort_clear()` が呼ばれた回数 (KAPI v45、決裁 A1)。
pub static ABORT_CLEARS: AtomicUsize = AtomicUsize::new(0);
/// `kbd_inject_pending()` が返す未読バイト数 (KAPI v47、票 K7 指摘 C)。
pub static KBD_PENDING: AtomicUsize = AtomicUsize::new(0);
/// `exec_app_state(app_id)` が返す状態。添字 = app_id、既定は 2 (`PARKED`)。
pub static APP_STATE: Mutex<Vec<i32>> = Mutex::new(Vec::new());

/* ---- 起動要求表 (KAPI v49、票 T9 D3) ---------------------------------- */
/// `launch_pending()` が返す本数。
pub static LAUNCH_PENDING: AtomicUsize = AtomicUsize::new(0);
/// `launch_take` が順に返す要求 (token, kind, arg, cmdline)。空なら 0 (無し)。
pub static TAKE_SCRIPT: Mutex<Vec<(i32, i32, i32, Vec<u8>)>> = Mutex::new(Vec::new());
/// `launch_take` が呼ばれた回数 (「STALE を再試行しない」の観測点)。
pub static TAKES: AtomicUsize = AtomicUsize::new(0);
/// `launch_report(token, rc)` の呼び出し列。
pub static REPORTS: Mutex<Vec<(i32, i32)>> = Mutex::new(Vec::new());
/// `launch_report` の戻り値 (既定 0。`OS32_ERR_STALE` = -11 を返させる試験がある)。
pub static REPORT_RET: Mutex<i32> = Mutex::new(0);
/// `launch_child(id)` の答え。添字 = id、既定 0 (連鎖の末尾)。
pub static LAUNCH_CHILD: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `exec_kill(id)` で `FREE` (0) に落とす ID の列 (子孫ごと畳む K の振る舞い)。
pub static KILL_FREES: Mutex<Vec<i32>> = Mutex::new(Vec::new());
/// `get_tick()` が返す tick (D5 の「同じ tick に 2 回起こさない」の観測点)。
pub static TICK: AtomicUsize = AtomicUsize::new(0);

/// 積まれている起動要求の本数を置く。
pub fn set_launch_pending(n: usize) {
    LAUNCH_PENDING.store(n, Ordering::SeqCst);
}
/// `launch_take` が返す要求を 1 本積む (取り出すと `launch_pending` が 1 減る)。
pub fn push_take(token: i32, kind: i32, arg: i32, cmdline: &[u8]) {
    lk(&TAKE_SCRIPT).push((token, kind, arg, cmdline.to_vec()));
    LAUNCH_PENDING.fetch_add(1, Ordering::SeqCst);
}
/// `launch_take` が呼ばれた回数。
pub fn take_calls() -> usize {
    TAKES.load(Ordering::SeqCst)
}
/// `launch_report(token, rc)` の呼び出し列 (複製)。
pub fn report_calls() -> Vec<(i32, i32)> {
    lk(&REPORTS).clone()
}
/// `launch_report` の戻り値を置く (-11 = `OS32_ERR_STALE`)。
pub fn set_report_ret(rc: i32) {
    *lk(&REPORT_RET) = rc;
}
/// `launch_child(id)` の答えを置く (0 = 連鎖の末尾)。
pub fn set_launch_child(id: i32, child: i32) {
    let mut v = lk(&LAUNCH_CHILD);
    if v.len() <= id as usize {
        v.resize(id as usize + 1, 0);
    }
    v[id as usize] = child;
}
/// `exec_kill` が `FREE` に落とす ID を置く (既定は空 = 状態を動かさない)。
pub fn set_kill_frees(ids: &[i32]) {
    *lk(&KILL_FREES) = ids.to_vec();
}
/// `get_tick()` が返す値を置く。
pub fn set_tick(t: u32) {
    TICK.store(t as usize, Ordering::SeqCst);
}

unsafe extern "C" fn get_tick() -> u32 {
    TICK.load(Ordering::SeqCst) as u32
}
unsafe extern "C" fn launch_pending() -> i32 {
    LAUNCH_PENDING.load(Ordering::SeqCst) as i32
}
unsafe extern "C" fn launch_take(
    buf: *mut u8,
    cap: u32,
    requester: *mut i32,
    kind: *mut i32,
    arg: *mut i32,
) -> i32 {
    TAKES.fetch_add(1, Ordering::SeqCst);
    /* 実物と同じ門: cap < LAUNCH_CMDLINE_MAX は断る (§1a)。 */
    if buf.is_null() || cap < 256 {
        return -2; /* OS32_ERR_INVAL */
    }
    let mut q = lk(&TAKE_SCRIPT);
    if q.is_empty() {
        return 0;
    }
    let (token, k, a, cmd) = q.remove(0);
    LAUNCH_PENDING.fetch_sub(1, Ordering::SeqCst);
    let n = core::cmp::min(cmd.len(), cap as usize - 1);
    std::ptr::copy_nonoverlapping(cmd.as_ptr(), buf, n);
    *buf.add(n) = 0;
    if !requester.is_null() {
        /* KILL は孤児回収 (-1)、LAUNCH は要求者 2 を既定にする。 */
        *requester = if k == 2 { -1 } else { 2 };
    }
    if !kind.is_null() {
        *kind = k;
    }
    if !arg.is_null() {
        *arg = a;
    }
    token
}
unsafe extern "C" fn launch_report(token: i32, rc: i32) -> i32 {
    lk(&REPORTS).push((token, rc));
    *lk(&REPORT_RET)
}
unsafe extern "C" fn launch_child(id: i32) -> i32 {
    lk(&LAUNCH_CHILD).get(id as usize).copied().unwrap_or(0)
}

/// 中毒 (panic 中に掴んでいた) した Mutex でも読めるようにする。検査が
/// `assert!(*X.lock().unwrap() == ..)` で落ちると次の試験まで巻き添えになる。
fn lk<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// `snd_focus` へ渡した owner の列 (複製。ロックを持ったまま assert しない)。
pub fn snd_focus_calls() -> Vec<i32> {
    lk(&SND_FOCUS).clone()
}
/// `exec_resume(app_id, wait_ret)` の呼び出し列 (複製)。
pub fn resume_calls() -> Vec<(i32, i32)> {
    lk(&RESUMES).clone()
}
/// `exec_kill(app_id)` の呼び出し列 (複製)。
pub fn kill_calls() -> Vec<i32> {
    lk(&KILLS).clone()
}
/// `exec_start(cmdline)` の呼び出し列 (複製)。
pub fn start_calls() -> Vec<Vec<u8>> {
    lk(&STARTS).clone()
}
/// `exec_abort_clear()` が呼ばれた回数。
pub fn abort_clear_calls() -> usize {
    ABORT_CLEARS.load(Ordering::SeqCst)
}

/// `gfx_init` が呼ばれた回数。
pub static GFX_INITS: AtomicUsize = AtomicUsize::new(0);
/// `gfx_add_dirty_rect` に積まれた矩形の数 (票 T8 D4a の「描かない」の観測点)。
pub static DIRTY_RECTS: AtomicUsize = AtomicUsize::new(0);
/// `gfx_present_dirty` が呼ばれた回数。
pub static PRESENTS: AtomicUsize = AtomicUsize::new(0);
/// `gfx_screen_owner()` が返す画面の所有者 (KAPI v48。1 = WM、2〜5 = アプリ)。
pub static SCREEN_OWNER: AtomicUsize = AtomicUsize::new(1);
/// `sys_open` が開ける「ファイル」の中身 (空 = 開けない)。OS32X ヘッダ用。
pub static FILE_BYTES: Mutex<Vec<u8>> = Mutex::new(Vec::new());
/// `sys_open` に渡ったパスの列 (入口がヘッダを読んだことの観測点)。
pub static OPENS: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());
/// 開いている `fd` の読み出し位置 (単一 fd を前提にした最小の模型)。
static FILE_POS: AtomicUsize = AtomicUsize::new(0);

/// 画面の所有者を置く (1 = WM、2〜5 = アプリ)。
pub fn set_screen_owner(id: i32) {
    SCREEN_OWNER.store(id as usize, Ordering::SeqCst);
}
/// `gfx_add_dirty_rect` / `gfx_present_dirty` の回数 (積んだ矩形数, present 数)。
pub fn present_counts() -> (usize, usize) {
    (
        DIRTY_RECTS.load(Ordering::SeqCst),
        PRESENTS.load(Ordering::SeqCst),
    )
}
/// 次に `sys_open` で開けるファイルの中身を置く (空 = 開けない)。
pub fn set_file(bytes: &[u8]) {
    *lk(&FILE_BYTES) = bytes.to_vec();
}
/// OS32X ヘッダ 40B を組み立てる (`sdk/include/os32/os32_kapi_shared.h`)。
pub fn os32x_header(flags: u32) -> Vec<u8> {
    let mut h = vec![0u8; 40];
    h[0..4].copy_from_slice(&0x4F53_3332u32.to_le_bytes());
    h[4..8].copy_from_slice(&40u32.to_le_bytes());
    h[8..12].copy_from_slice(&2u32.to_le_bytes());
    h[12..16].copy_from_slice(&flags.to_le_bytes());
    h
}
/// `sys_open` に渡ったパスの列 (複製)。
pub fn open_calls() -> Vec<Vec<u8>> {
    lk(&OPENS).clone()
}

unsafe extern "C" fn screen_owner() -> i32 {
    SCREEN_OWNER.load(Ordering::SeqCst) as i32
}
unsafe extern "C" fn sys_open(path: *const u8, _mode: i32) -> i32 {
    let mut n = 0;
    while *path.add(n) != 0 && n < 256 {
        n += 1;
    }
    lk(&OPENS).push(std::slice::from_raw_parts(path, n).to_vec());
    if lk(&FILE_BYTES).is_empty() {
        return -1;
    }
    FILE_POS.store(0, Ordering::SeqCst);
    7
}
unsafe extern "C" fn sys_read(_fd: i32, buf: *mut u8, size: u32) -> i32 {
    let f = lk(&FILE_BYTES);
    let pos = FILE_POS.load(Ordering::SeqCst);
    let n = core::cmp::min(size as usize, f.len().saturating_sub(pos));
    std::ptr::copy_nonoverlapping(f.as_ptr().add(pos), buf, n);
    FILE_POS.store(pos + n, Ordering::SeqCst);
    n as i32
}
unsafe extern "C" fn sys_close(_fd: i32) {}

/// ゲストの `gfx_init` (`gfx/gfx_core.c`) は **VRAM の両ページをゼロクリア
/// する**。「消えたのに描き直させない」不具合 (W-1) をホストで再現するには
/// そこまで模す必要がある — 何もしない代用だと、画が消えたことを試験が
/// 観測できず検査が空振りする。
unsafe extern "C" fn gfx_init() {
    GFX_INITS.fetch_add(1, Ordering::SeqCst);
    clear(0);
}

unsafe extern "C" fn exec_park() -> i32 {
    PARKS.fetch_add(1, Ordering::SeqCst);
    *lk(&PARK_RET)
}
unsafe extern "C" fn exec_resume(app_id: i32, wait_ret: i32) -> i32 {
    lk(&RESUMES).push((app_id, wait_ret));
    let mut q = lk(&RESUME_SCRIPT);
    if q.is_empty() {
        app_id
    } else {
        q.remove(0)
    }
}
unsafe extern "C" fn exec_start(cmdline: *const u8) -> i32 {
    let mut n = 0;
    while *cmdline.add(n) != 0 && n < 256 {
        n += 1;
    }
    lk(&STARTS).push(std::slice::from_raw_parts(cmdline, n).to_vec());
    let mut q = lk(&START_SCRIPT);
    if q.is_empty() {
        2
    } else {
        q.remove(0)
    }
}
unsafe extern "C" fn exec_kill(app_id: i32) -> i32 {
    lk(&KILLS).push(app_id);
    /* 実物の `exec_kill` は連鎖の子孫ごと畳む (票 T9 D8)。畳んだ結果
     * `exec_app_state` が `FREE` になる ID を試験が置く (既定は空)。 */
    let freed = lk(&KILL_FREES).clone();
    for id in freed {
        set_app_state(id, 0);
    }
    0
}
unsafe extern "C" fn snd_focus(app_id: i32) -> i32 {
    lk(&SND_FOCUS).push(app_id);
    0
}
/* KAPI v45 (K5c、決裁 A1)。owner 1 (WM top-level) からしか呼べないので、
 * ゲストでは `op_wait` の中から呼ぶと `OS32_ERR_INVAL`。ここは回数を数える
 * だけ — 「どこから呼んだか」はカーネル側の領分
 * (`tools/tests/multiapp_impl_host.c` ケース 20 が実物の AppSlot で検査済み)。 */
unsafe extern "C" fn exec_abort_clear() -> i32 {
    ABORT_CLEARS.fetch_add(1, Ordering::SeqCst);
    0
}
/* KAPI v47 (K7)。注入リングはカーネルの持ち物なので、ここは試験が置いた
 * 「未読バイト数」と「その ID の状態」をそのまま返すだけ — 注入と取り出しの
 * 実体は `tools/tests/kbd_inject_host.c` が実物で検査済み。 */
unsafe extern "C" fn kbd_inject_pending() -> u32 {
    KBD_PENDING.load(Ordering::SeqCst) as u32
}
unsafe extern "C" fn exec_app_state(app_id: i32) -> i32 {
    lk(&APP_STATE).get(app_id as usize).copied().unwrap_or(2)
}
/// 注入リングの未読バイト数を置く (0 = 空)。
pub fn set_kbd_pending(n: u32) {
    KBD_PENDING.store(n as usize, Ordering::SeqCst);
}
/// `exec_app_state(app_id)` の答えを置く (3 = `APP_STATE_WAIT_KEY`)。
pub fn set_app_state(app_id: i32, state: i32) {
    let mut v = lk(&APP_STATE);
    if v.len() <= app_id as usize {
        v.resize(app_id as usize + 1, 2);
    }
    v[app_id as usize] = state;
}

pub fn init() {
    let mut a = os32api::mock_api();
    a.get_tick = get_tick;
    a.sys_time = zero;
    a.kbd_dropped_count = zero;
    a.kbd_trygetrawkey = raw_key;
    a.mouse_poll = mouse;
    a.gfx_init = gfx_init;
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
    a.gfx_present_dirty = present_dirty;
    /* 票 T8: 画面の所有者 (KAPI v48) と OS32X ヘッダの読み取り。 */
    a.gfx_screen_owner = screen_owner;
    a.sys_open = sys_open;
    a.sys_read = sys_read;
    a.sys_close = sys_close;
    /* `op_wait` / 単独ループの待ち。ホストでは何もしない (時計は get_tick 側)。 */
    a.sys_halt = nothing;
    a.exec_park = exec_park;
    a.exec_resume = exec_resume;
    a.exec_start = exec_start;
    a.exec_kill = exec_kill;
    a.exec_abort_clear = exec_abort_clear;
    a.snd_focus = snd_focus;
    a.kbd_inject_pending = kbd_inject_pending;
    a.exec_app_state = exec_app_state;
    /* 票 T9 (KAPI v49): 起動要求表。表そのものの遷移は実物で検査済み
     * (`tools/tests/launch_host.c`)。ここは WM が「いつ・何を渡したか」だけ見る。 */
    a.launch_pending = launch_pending;
    a.launch_take = launch_take;
    a.launch_report = launch_report;
    a.launch_child = launch_child;
    lk(&RAWKEYS).clear();
    lk(&IME_SCRIPT).clear();
    GFX_INITS.store(0, Ordering::SeqCst);
    DIRTY_RECTS.store(0, Ordering::SeqCst);
    PRESENTS.store(0, Ordering::SeqCst);
    SCREEN_OWNER.store(1, Ordering::SeqCst);
    FILE_POS.store(0, Ordering::SeqCst);
    lk(&FILE_BYTES).clear();
    lk(&OPENS).clear();
    *lk(&PALETTE) = [0; 48];
    lk(&PALETTE_SETS).clear();
    crate::fullscreen::reset();
    PARKS.store(0, Ordering::SeqCst);
    *lk(&PARK_RET) = -1;
    lk(&RESUMES).clear();
    lk(&RESUME_SCRIPT).clear();
    lk(&STARTS).clear();
    lk(&START_SCRIPT).clear();
    lk(&KILLS).clear();
    lk(&SND_FOCUS).clear();
    ABORT_CLEARS.store(0, Ordering::SeqCst);
    KBD_PENDING.store(0, Ordering::SeqCst);
    lk(&APP_STATE).clear();
    LAUNCH_PENDING.store(0, Ordering::SeqCst);
    TAKES.store(0, Ordering::SeqCst);
    TICK.store(0, Ordering::SeqCst);
    lk(&TAKE_SCRIPT).clear();
    lk(&REPORTS).clear();
    *lk(&REPORT_RET) = 0;
    lk(&LAUNCH_CHILD).clear();
    lk(&KILL_FREES).clear();
    crate::multiapp::reset();
    os32api::os32_init(Box::into_raw(Box::new(a)));
    clear(9);
}
