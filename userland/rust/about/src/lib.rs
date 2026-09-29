//! about — 「About OS32」ダイアログ (/usr/bin/about.bin、ROADMAP v1.4 の About dialog)。
//!
//! Start → Programs の about.bin (/usr/bin/*.bin の一覧) から起動する普通の
//! libos32gui アプリ。窓 1 枚にラベルを並べ、OK ボタンで閉じる。Start の root
//! メニューに専用の項目は足していない (gshell と台本 `gui_gate.py` の `ROW_*` が動くため)。
//!
//! 出すもの (すべて既存の KAPI と CPUID から。KAPI は足していない):
//!
//! | 行 | 出どころ |
//! |---|---|
//! | OS32 v2.0 | include/config.h の `SYS_VERSION` (コンパイル時に切り出す、`ver` と同じ定義) |
//! | Build / Commit / Image CRC | `sys_get_build_info` / `boot_image_info` (`ver` と同じ) |
//! | KernelAPI | `KernelAPI.version` |
//! | CPU | CPUID (無ければ EFLAGS の AC / ID ビットで 386 / 486)。CPU を返す KAPI は無い |
//! | Memory | `sys_ram_kb` (`mem` の RAM 行と同じ) |
//! | Kernel heap | `kmalloc_free` / `kmalloc_total` (物理ページの空きを返す KAPI は無い) |
//! | Display | `gfx_screen_info` (バックエンド・解像度・色数) |
//! | Boot | `boot_image_info` の source (FD / HDD) |
//!
//! 閉じ方: RETURN / ESC / OK ボタン / 閉じるボタン / GRPH+f･4 (WM 側)。
//! 文字列の組み立ては [`info`] (ホスト試験 `tools/tests/test_about_info.py`)。
#![no_std]
#![no_main]

extern crate libos32gui;
extern crate os32api;

pub mod info;

use info::Line;
use libos32gui::widget::{self, WidgetId, SCAN_ESC, SCAN_RETURN};
use libos32gui::{App, SizeSpec, Ui, Window, WindowSpec};
use os32api::gui::types::Rect;
use os32api::KernelAPI;

/// 版の唯一の定義 (include/config.h) から `SYS_VERSION` をコンパイル時に切り出す。
const SYS_VERSION: &[u8] = info::sys_version(include_bytes!("../../../../include/config.h"));

/// `BootImageInfo` (os32_kapi_shared.h、40 バイト、並びは固定) の写し。
#[repr(C)]
struct BootImageInfo {
    image_crc: u32,
    image_size: u32,
    crc_valid: u8,
    source: u8,
    reserved: u16,
    commit: [u8; 24],
    reserved2: u32,
}
const _: () = assert!(core::mem::size_of::<BootImageInfo>() == 40);

/// `boot_image_info` が入った KAPI の版 (v65、`ver` と同じ判定)。
const KAPI_BOOT_IMAGE_INFO: u32 = 65;

/// 表示する行の数 (見出しの 1 行を含む)。
const NLINES: usize = 10;

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8, api: *mut KernelAPI) -> i32 {
    if libos32gui::init(api).is_err() {
        return -1;
    }
    let mut about = match About::build() {
        Ok(a) => a,
        Err(e) => return e.code(),
    };
    let _ = libos32gui::run(&mut about);
    0
}

struct About {
    win: Option<Window>,
    ok: WidgetId,
}

/// 各行を組み立てる。
fn collect(lines: &mut [Line; NLINES]) {
    let a = unsafe { os32api::api() };
    let kver = a.version;

    let mut build = [0u8; 32];
    unsafe { (a.sys_get_build_info)(build.as_mut_ptr(), build.len() as i32) };
    build[build.len() - 1] = 0;

    let mut bi = BootImageInfo {
        image_crc: 0,
        image_size: 0,
        crc_valid: 0,
        source: 0,
        reserved: 0,
        commit: [0; 24],
        reserved2: 0,
    };
    let bi_ok = kver >= KAPI_BOOT_IMAGE_INFO
        && unsafe { (a.boot_image_info)(&mut bi as *mut BootImageInfo as *mut u8) } == 0;
    bi.commit[bi.commit.len() - 1] = 0;

    let ram_kb = unsafe { (a.sys_ram_kb)() };
    let heap_free = unsafe { (a.kmalloc_free)() };
    let heap_total = unsafe { (a.kmalloc_total)() };
    let si = libos32gui::gapi::screen_info();
    let c = cpu::probe();

    info::title_line(&mut lines[0], SYS_VERSION);
    info::build_line(&mut lines[1], &build);
    info::commit_line(&mut lines[2], bi_ok, &bi.commit);
    info::crc_line(&mut lines[3], bi_ok, bi.crc_valid != 0, bi.image_crc, bi.image_size);
    info::kapi_line(&mut lines[4], kver);
    info::cpu_line(&mut lines[5], &c);
    info::mem_line(&mut lines[6], ram_kb);
    info::heap_line(&mut lines[7], heap_free, heap_total);
    info::display_line(&mut lines[8], si.width, si.height, si.bpp, si.format, si.flags);
    info::boot_line(&mut lines[9], bi_ok, bi.source);
}

impl About {
    fn build() -> libos32gui::GuiResult<About> {
        let mut lines = [
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
            Line::new(),
        ];
        collect(&mut lines);

        /* 画面能力を信じる (契約 G5: 640×400 を決め打ちしない)。
         * 幅は最長の行 (CPU のブランド文字列 48 桁 + "CPU: ") が入る 440px を上限に。 */
        let si = libos32gui::gapi::screen_info();
        let sw = si.width as i32;
        let sh = si.height as i32;
        let ww = clamp(sw - 16, 240, 440);
        let wh = clamp(sh - 40, 200, 272);
        let wx = (sw - ww) / 2;
        let wy = clamp((sh - 24 - wh) / 2, 0, sh);

        let win = Window::create(&WindowSpec::new(
            b"About OS32",
            Rect::new(wx as i16, wy as i16, ww as i16, wh as i16),
        ))?;

        let root = widget::column(10, 2)?;
        let mut i = 0;
        while i < NLINES {
            let l = widget::label(lines[i].as_bytes())?;
            widget::add(root, l, SizeSpec::Fixed(if i == 0 { 20 } else { 16 }))?;
            i += 1;
        }
        let spacer = widget::label(b"")?;
        widget::add(root, spacer, SizeSpec::Flex(1))?;

        let bar = widget::row(0, 0)?;
        let pad = widget::label(b"")?;
        let ok = widget::button(b"OK")?;
        widget::add(bar, pad, SizeSpec::Flex(1))?;
        widget::add(bar, ok, SizeSpec::Fixed(72))?;
        widget::add(root, bar, SizeSpec::Fixed(24))?;

        win.set_root(root)?;
        win.set_focus()?;
        widget::set_focus(ok)?;

        Ok(About { win: Some(win), ok })
    }
}

fn clamp(v: i32, lo: i32, hi: i32) -> i32 {
    if v < lo {
        lo
    } else if v > hi {
        hi
    } else {
        v
    }
}

impl App for About {
    fn on_click(&mut self, ui: &mut Ui, w: WidgetId) {
        if w == self.ok {
            ui.quit();
        }
    }

    /// 閉じるボタン (と WM の GRPH+f･4) — 窓を落として終わる。
    fn on_close(&mut self, ui: &mut Ui, _window: u32) {
        self.win = None;
        ui.quit();
    }

    /// RETURN / ESC で閉じる (OK にフォーカスが無くても効く)。
    fn on_key(&mut self, ui: &mut Ui, _window: u32, scan: u8, _ch: u8, _mods: u8, down: bool) {
        if down && (scan == SCAN_ESC || scan == SCAN_RETURN) {
            ui.quit();
        }
    }
}

/* ================================================================ */
/*  CPU の識別 (CPL=3 で使える命令だけ。KAPI は使わない)              */
/* ================================================================ */
mod cpu {
    use crate::info::{family_model, Cpu};
    use core::arch::asm;
    use core::arch::x86::__cpuid;

    const EFLAGS_AC: u32 = 1 << 18;
    const EFLAGS_ID: u32 = 1 << 21;

    /// EFLAGS の `mask` のビットを反転して書けるか (書いた値はすぐ戻す)。
    /// AC (bit 18) は 386 では固定、ID (bit 21) は CPUID のある CPU だけが書ける。
    /// どちらも CPL=3 の popfd で書けるビット (IF / IOPL と違い黙って捨てられない)。
    fn flag_toggles(mask: u32) -> bool {
        let changed: u32;
        unsafe {
            asm!(
                "pushfd",
                "pop {a}",
                "mov {b}, {a}",
                "xor {a}, {m}",
                "push {a}",
                "popfd",
                "pushfd",
                "pop {a}",
                "push {b}",
                "popfd",
                "xor {a}, {b}",
                a = out(reg) changed,
                b = out(reg) _,
                m = in(reg) mask,
            );
        }
        (changed & mask) != 0
    }

    #[allow(unused_unsafe)]
    pub fn probe() -> Cpu {
        let mut c = Cpu::empty();
        if !flag_toggles(EFLAGS_ID) {
            c.class = if flag_toggles(EFLAGS_AC) { 4 } else { 3 };
            return c;
        }
        let r0 = unsafe { __cpuid(0) };
        put(&mut c.vendor[0..4], r0.ebx);
        put(&mut c.vendor[4..8], r0.edx);
        put(&mut c.vendor[8..12], r0.ecx);
        if r0.eax >= 1 {
            let r1 = unsafe { __cpuid(1) };
            let (f, m) = family_model(r1.eax);
            c.family = f;
            c.model = m;
        }
        let ext = unsafe { __cpuid(0x8000_0000) };
        if ext.eax >= 0x8000_0004 && ext.eax < 0x8000_1000 {
            let mut leaf = 0;
            while leaf < 3 {
                let r = unsafe { __cpuid(0x8000_0002 + leaf as u32) };
                let o = leaf * 16;
                put(&mut c.brand[o..o + 4], r.eax);
                put(&mut c.brand[o + 4..o + 8], r.ebx);
                put(&mut c.brand[o + 8..o + 12], r.ecx);
                put(&mut c.brand[o + 12..o + 16], r.edx);
                leaf += 1;
            }
            c.brand[47] = 0;
        }
        c
    }

    fn put(dst: &mut [u8], v: u32) {
        dst[0] = v as u8;
        dst[1] = (v >> 8) as u8;
        dst[2] = (v >> 16) as u8;
        dst[3] = (v >> 24) as u8;
    }
}
