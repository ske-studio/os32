//! G 描画プリミティブ (契約 G2) — libos32gfx の上に載せる。
//!
//! **鉄則 (票 C1 / v2 C8)**:
//! - 描画の実体は libos32gfx を FFI で呼ぶ。プレーンへ直接ピクセルを書かない
//!   (PLANAR4)。線・枠はクライアント側の CPU 実装 (幾何) だが、点は `gfx_pixel`
//!   (libos32gfx) を通す。8bpp (`GFX_FMT_PACKED8`) だけは契約どおり C1 の CPU 実装で
//!   線形バッファへ直接書く分岐を用意する (v1.1 前半は PLANAR4 のみ動けばよい)。
//! - **クリップは自分で守る**: サーフェス外へは 1 ピクセルも出さない。矩形は交差で、
//!   線は 1 ピクセルごとにクリップ判定で、文字はグリフセル単位 (ANK は画素単位) で守る。
//! - ステートレス: 呼び出しごとに `Style` を受ける。DC 相当の選択状態は持たない。
use crate::clip::{self, Target};
use crate::ffi;
use crate::gstate::{screen_info_cached, st, SurfaceKind};
use crate::surface::offscreen_ptr;
use os32api::gui::proto::{
    GUI_STYLE_DITHER50, GUI_STYLE_DOTTED, GUI_STYLE_TRANSPARENT_BG, GUI_STYLE_XOR,
};
use os32api::gui::types::{Rect, ScreenInfo, Stats, Style, SurfaceId, GFX_FMT_PACKED8};

const CELL_H: i32 = 16; /* KCG セル高 (scale1) */
const ANK_W: i32 = 8; /* 半角セル幅 */
const KANJI_W: i32 = 16; /* 全角セル幅 */

/* ================================================================ */
/*  低レベル画素バックエンド (Target に対して局所座標で書く)         */
/* ================================================================ */

struct Painter {
    ox: i32,
    oy: i32,
    offscreen: *mut ffi::GfxSurface, /* null = 画面 */
    packed8: bool,
    fb_base: *mut u8,
    fb_pitch: i32,
    clip: Rect, /* 局所座標、サーフェス境界内 */
}

impl Painter {
    fn from_target(t: &Target) -> Painter {
        let info = screen_info_cached();
        let packed8 = !t.offscreen && info.format == GFX_FMT_PACKED8;
        let (fb_base, fb_pitch) = if packed8 {
            let fb = unsafe { ffi::gfx_fb };
            (fb.planes[0], fb.pitch)
        } else {
            (core::ptr::null_mut(), 0)
        };
        Painter {
            ox: t.ox,
            oy: t.oy,
            offscreen: if t.offscreen { offscreen_ptr(t.idx) } else { core::ptr::null_mut() },
            packed8,
            fb_base,
            fb_pitch,
            clip: t.clip,
        }
    }

    #[inline]
    fn in_clip(&self, lx: i32, ly: i32) -> bool {
        self.clip.contains(lx, ly)
    }

    /// 1 画素 (局所座標、クリップ判定つき)。
    #[inline]
    fn put(&self, lx: i32, ly: i32, color: u8) {
        if !self.in_clip(lx, ly) {
            return;
        }
        if !self.offscreen.is_null() {
            unsafe { ffi::gfx_surface_pixel(self.offscreen, lx, ly, color) };
        } else if self.packed8 {
            if !self.fb_base.is_null() {
                let off = (self.oy + ly) as isize * self.fb_pitch as isize + (self.ox + lx) as isize;
                unsafe { *self.fb_base.offset(off) = color };
            }
        } else {
            unsafe { ffi::gfx_pixel(self.ox + lx, self.oy + ly, color) };
        }
    }

    /// 1 画素の読み戻し (バックバッファ。VRAM は読まない)。画面のみ。
    #[inline]
    fn get(&self, lx: i32, ly: i32) -> u8 {
        if !self.offscreen.is_null() {
            0 /* オフスクリーンの読み戻しは libos32gfx に口が無い (XOR は画面用) */
        } else if self.packed8 {
            if self.fb_base.is_null() {
                0
            } else {
                let off = (self.oy + ly) as isize * self.fb_pitch as isize + (self.ox + lx) as isize;
                unsafe { *self.fb_base.offset(off) }
            }
        } else {
            unsafe { ffi::gfx_get_pixel(self.ox + lx, self.oy + ly) }
        }
    }

    /// クリップ済みの塗り矩形 (rc は既に self.clip 内)。まとめて塗る (速い経路)。
    fn fill_solid(&self, rc: Rect, color: u8) {
        if rc.is_empty() {
            return;
        }
        if !self.offscreen.is_null() {
            unsafe {
                ffi::gfx_surface_fill_rect(
                    self.offscreen,
                    rc.x as i32,
                    rc.y as i32,
                    rc.w as i32,
                    rc.h as i32,
                    color,
                )
            };
        } else if self.packed8 {
            let mut ly = rc.y as i32;
            let y1 = rc.bottom();
            while ly < y1 {
                let mut lx = rc.x as i32;
                let x1 = rc.right();
                while lx < x1 {
                    self.put(lx, ly, color);
                    lx += 1;
                }
                ly += 1;
            }
        } else {
            unsafe {
                ffi::gfx_fill_rect(
                    self.ox + rc.x as i32,
                    self.oy + rc.y as i32,
                    rc.w as i32,
                    rc.h as i32,
                    color,
                )
            };
        }
    }
}

/* ================================================================ */
/*  クリップ済み矩形ヘルパ                                           */
/* ================================================================ */

#[inline]
fn clip_rect(t: &Target, r: Rect) -> Rect {
    r.intersect(&t.clip)
}

/* ================================================================ */
/*  塗り / 枠                                                        */
/* ================================================================ */

/// 矩形塗り (契約 G2)。`style.bg` を使う。
/// フラグ: `TRANSPARENT_BG` = 何もしない / `DITHER50` = 市松 (fg,bg) / `XOR` = 画素反転。
pub fn fill_rect(surface: SurfaceId, rect: Rect, style: Style) {
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return,
    };
    let rc = clip_rect(&t, rect);
    if rc.is_empty() {
        return;
    }
    let p = Painter::from_target(&t);

    if style.has(GUI_STYLE_TRANSPARENT_BG) {
        return;
    }
    if style.has(GUI_STYLE_XOR) {
        let mut ly = rc.y as i32;
        let y1 = rc.bottom();
        while ly < y1 {
            let mut lx = rc.x as i32;
            let x1 = rc.right();
            while lx < x1 {
                let old = p.get(lx, ly);
                p.put(lx, ly, old ^ style.fg);
                lx += 1;
            }
            ly += 1;
        }
        return;
    }
    if style.has(GUI_STYLE_DITHER50) {
        let mut ly = rc.y as i32;
        let y1 = rc.bottom();
        while ly < y1 {
            let mut lx = rc.x as i32;
            let x1 = rc.right();
            while lx < x1 {
                let c = if (lx + ly) & 1 == 0 { style.fg } else { style.bg };
                p.put(lx, ly, c);
                lx += 1;
            }
            ly += 1;
        }
        return;
    }
    p.fill_solid(rc, style.bg);
}

/// 1px 枠 (契約 G2)。`style.fg`。`DOTTED` = 点線、`XOR` = 反転。
pub fn draw_rect(surface: SurfaceId, rect: Rect, style: Style) {
    if rect.is_empty() {
        return;
    }
    let y0 = rect.y as i32;
    let x1 = rect.right() - 1;
    let y1 = rect.bottom() - 1;
    hline(surface, rect.x, rect.y, rect.w, style);
    if rect.h > 1 {
        hline(surface, rect.x, y1 as i16, rect.w, style);
    }
    if rect.h > 2 {
        let inner_h = rect.h - 2;
        vline(surface, rect.x, (y0 + 1) as i16, inner_h, style);
        if rect.w > 1 {
            vline(surface, x1 as i16, (y0 + 1) as i16, inner_h, style);
        }
    }
}

/* ================================================================ */
/*  線                                                              */
/* ================================================================ */

/// 水平線 (契約 G2)。`style.fg`。長さ `w`。`DOTTED` / `XOR` 対応。
pub fn hline(surface: SurfaceId, x: i16, y: i16, w: i16, style: Style) {
    if w <= 0 {
        return;
    }
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return,
    };
    let rc = clip_rect(&t, Rect::new(x, y, w, 1));
    if rc.is_empty() {
        return;
    }
    let p = Painter::from_target(&t);
    let plain = !style.has(GUI_STYLE_DOTTED) && !style.has(GUI_STYLE_XOR);
    if plain {
        p.fill_solid(rc, style.fg);
        return;
    }
    let mut lx = rc.x as i32;
    let x1 = rc.right();
    let ly = rc.y as i32;
    while lx < x1 {
        span_pixel(&p, lx, ly, lx, &style);
        lx += 1;
    }
}

/// 垂直線 (契約 G2)。`style.fg`。長さ `h`。`DOTTED` / `XOR` 対応。
pub fn vline(surface: SurfaceId, x: i16, y: i16, h: i16, style: Style) {
    if h <= 0 {
        return;
    }
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return,
    };
    let rc = clip_rect(&t, Rect::new(x, y, 1, h));
    if rc.is_empty() {
        return;
    }
    let p = Painter::from_target(&t);
    let plain = !style.has(GUI_STYLE_DOTTED) && !style.has(GUI_STYLE_XOR);
    if plain {
        p.fill_solid(rc, style.fg);
        return;
    }
    let lx = rc.x as i32;
    let mut ly = rc.y as i32;
    let y1 = rc.bottom();
    while ly < y1 {
        span_pixel(&p, lx, ly, ly, &style);
        ly += 1;
    }
}

/// 点線/XOR を考慮した 1 画素 (parity は座標和で決める)。
#[inline]
fn span_pixel(p: &Painter, lx: i32, ly: i32, parity_src: i32, style: &Style) {
    if style.has(GUI_STYLE_DOTTED) && (parity_src & 1) != 0 {
        return; /* 点線: 1 個おき */
    }
    if style.has(GUI_STYLE_XOR) {
        let old = p.get(lx, ly);
        p.put(lx, ly, old ^ style.fg);
    } else {
        p.put(lx, ly, style.fg);
    }
}

/// 任意直線 (契約 G2)。`style.fg`。Bresenham を 1 画素ごとにクリップ判定して描く
/// (クリップ外へ 1 画素も出さない)。`DOTTED` / `XOR` 対応。
pub fn line(surface: SurfaceId, x0: i16, y0: i16, x1: i16, y1: i16, style: Style) {
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return,
    };
    if t.clip.is_empty() {
        return;
    }
    let p = Painter::from_target(&t);
    let mut x = x0 as i32;
    let mut y = y0 as i32;
    let xe = x1 as i32;
    let ye = y1 as i32;
    let dx = (xe - x).abs();
    let dy = -(ye - y).abs();
    let sx = if x < xe { 1 } else { -1 };
    let sy = if y < ye { 1 } else { -1 };
    let mut err = dx + dy;
    let mut step: i32 = 0;
    loop {
        span_pixel(&p, x, y, step, &style);
        if x == xe && y == ye {
            break;
        }
        let e2 = 2 * err;
        if e2 >= dy {
            err += dy;
            x += sx;
        }
        if e2 <= dx {
            err += dx;
            y += sy;
        }
        step += 1;
    }
}

/* ================================================================ */
/*  ビットマップ転送 (契約 G2、カラーキー = 色 255)                  */
/* ================================================================ */

/// `bitmap` (オフスクリーンサーフェス) の `src_rect` を `(dx,dy)` へ転送する。
/// カラーキー 255 は転送しない。dst は画面サーフェスのみ (v1)。
pub fn blit(surface: SurfaceId, dx: i16, dy: i16, bitmap: SurfaceId, src_rect: Rect) {
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return,
    };
    if t.offscreen {
        debug_assert!(false, "blit destination must be a screen surface in v1");
        return;
    }
    let s = st();
    let bidx = match s.resolve(bitmap) {
        Some(i) => i,
        None => return,
    };
    let src = match s.surfaces[bidx].kind {
        SurfaceKind::Offscreen { surf } => surf,
        SurfaceKind::Screen { .. } => return,
    };
    if src.is_null() {
        return;
    }

    /* dst 矩形をクリップ。src 原点をクリップのぶんだけずらす。 */
    let dst_full = Rect::new(dx, dy, src_rect.w, src_rect.h);
    let dst = clip_rect(&t, dst_full);
    if dst.is_empty() {
        return;
    }
    let off_x = dst.x as i32 - dx as i32;
    let off_y = dst.y as i32 - dy as i32;
    let sr = ffi::GfxRect {
        x: src_rect.x as i32 + off_x,
        y: src_rect.y as i32 + off_y,
        w: dst.w as i32,
        h: dst.h as i32,
    };
    unsafe {
        ffi::gfx_blit_colorkey(t.ox + dst.x as i32, t.oy + dst.y as i32, src, &sr, 255);
    }
}

/* ================================================================ */
/*  文字 (UTF-8 → KCG)                                              */
/* ================================================================ */

/// 1 コードポイントを取り出す。戻り: (バイト長, ASCII か, セル幅px)。末尾で不完全なら len=0。
fn decode_glyph(bytes: &[u8]) -> (usize, bool, i32) {
    if bytes.is_empty() {
        return (0, true, 0);
    }
    let b0 = bytes[0];
    if b0 < 0x80 {
        (1, true, ANK_W)
    } else if b0 & 0xE0 == 0xC0 {
        if bytes.len() >= 2 { (2, false, KANJI_W) } else { (0, false, 0) }
    } else if b0 & 0xF0 == 0xE0 {
        if bytes.len() >= 3 { (3, false, KANJI_W) } else { (0, false, 0) }
    } else if b0 & 0xF8 == 0xF0 {
        if bytes.len() >= 4 { (4, false, KANJI_W) } else { (0, false, 0) }
    } else {
        (1, true, ANK_W) /* 継続/不正バイト: 1 個スキップ、半角扱い */
    }
}

/// UTF-8 文字列を `(x,y)` から KCG で描く (契約 G2)。半角 8px / 全角 16px。
/// クリップはグリフセル単位 (ANK かつ `TRANSPARENT_BG` は画素単位)。戻り値: 送り幅 px。
pub fn text(surface: SurfaceId, x: i16, y: i16, utf8: &[u8], style: Style) -> i32 {
    let t = match clip::resolve_target(surface) {
        Some(t) => t,
        None => return 0,
    };
    let p = Painter::from_target(&t);
    unsafe { ffi::kcg_set_scale(1) };

    let transparent = style.has(GUI_STYLE_TRANSPARENT_BG);
    let ly = y as i32;
    let mut penx = x as i32;
    let mut i = 0usize;
    while i < utf8.len() {
        let (len, is_ank, cw) = decode_glyph(&utf8[i..]);
        if len == 0 {
            break; /* 末尾の不完全シーケンス: UTF-8 境界で切る */
        }
        if utf8[i] == 0 {
            break; /* NUL 終端も尊重 */
        }
        let cell = Rect::new(penx as i16, ly as i16, cw as i16, CELL_H as i16);

        if is_ank && transparent {
            /* ANK 透過: グリフビットマップを読み、fg 画素だけを画素単位クリップで置く */
            draw_ank_bitmap(&p, penx, ly, utf8[i], style.fg, None);
        } else if is_ank {
            /* ANK 不透過: ビットマップで画素単位クリップ (端でも欠けない) */
            draw_ank_bitmap(&p, penx, ly, utf8[i], style.fg, Some(style.bg));
        } else {
            /* 全角: セルがクリップ内に完全に収まるときだけ 1 グリフ描く
             * (kcg_draw_utf8 が UTF-8→JIS 変換と描画を担う)。透過は v1 では不透過に退避。 */
            if !p.offscreen.is_null() {
                /* オフスクリーンへの全角は kcg 経路が無い (v1 制限) */
            } else if t.clip.contains_rect(&cell) {
                let mut buf = [0u8; 5];
                let n = len.min(4);
                buf[..n].copy_from_slice(&utf8[i..i + n]);
                buf[n] = 0;
                unsafe {
                    ffi::kcg_draw_utf8(p.ox + penx, p.oy + ly, buf.as_ptr(), style.fg, style.bg);
                }
            }
        }

        penx += cw;
        i += len;
    }
    penx - x as i32
}

/// ANK (8x16) グリフをビットマップから画素単位で描く。`bg=None` は透過 (fg 画素だけ)。
/// ビットは MSB が左端と仮定 (kcg_read_ank のパック規約)。
fn draw_ank_bitmap(p: &Painter, penx: i32, ly: i32, ch: u8, fg: u8, bg: Option<u8>) {
    if !p.offscreen.is_null() && bg.is_some() {
        /* オフスクリーンの不透過 ANK は塗ってから... だが read_ank のビット順に依存するため
         * v1 では画面サーフェスに限定。オフスクリーン文字は未対応 (票 C1 の割り切り)。 */
    }
    /* ANK は 8x16 = 16B。念のため広めに確保して先頭 16B だけ使う。 */
    let mut pat = [0u8; 32];
    unsafe {
        let a = os32api::api();
        (a.kcg_read_ank)(ch, pat.as_mut_ptr());
    }
    let mut row = 0i32;
    while row < CELL_H {
        let bits = pat[row as usize];
        let mut col = 0i32;
        while col < ANK_W {
            let on = (bits >> (7 - col)) & 1 != 0;
            if on {
                p.put(penx + col, ly + row, fg);
            } else if let Some(b) = bg {
                p.put(penx + col, ly + row, b);
            }
            col += 1;
        }
        row += 1;
    }
}

/// レイアウト用の寸法 (契約 G2)。半角 8px / 全角 16px、高さ 16px (scale1)。
pub fn measure_text(utf8: &[u8]) -> (i32, i32) {
    let mut w = 0i32;
    let mut i = 0usize;
    while i < utf8.len() {
        if utf8[i] == 0 {
            break;
        }
        let (len, _ank, cw) = decode_glyph(&utf8[i..]);
        if len == 0 {
            break;
        }
        w += cw;
        i += len;
    }
    (w, CELL_H)
}

/* ================================================================ */
/*  能力とカウンタ (契約 G5 / G7)                                    */
/* ================================================================ */

/// 画面能力 (KAPI v40 `gfx_screen_info`)。GUI とアプリはこれを信じ、決め打ちしない。
pub fn screen_info() -> ScreenInfo {
    crate::gstate::refresh_screen_info()
}

/// GUI カウンタ (KAPI v41 `gfx_stats`)。累積。NP21/W では転送量で性能を見積もる。
pub fn stats() -> Stats {
    let mut out = Stats::ZERO;
    unsafe {
        let a = os32api::api();
        (a.gfx_stats)(&mut out as *mut Stats as *mut u8);
    }
    out
}

/// このライブラリが基底未設定の窓面描画を拒んだ累計 (テスト観測用)。
pub fn base_violation_count() -> u32 {
    st().base_violations
}
