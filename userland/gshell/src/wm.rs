//! wm.rs — ウィンドウマネージャの中核状態 (票 W1 の構成「wm.rs」)。
//!
//! Window 表 (16)、WindowId (index:16 | generation:16)、Z 順、フォーカス、
//! 所有者、Configure、そして SHM スロット / タイマの表を持つ単一グローバル状態
//! `GuiState` を定義する。他のモジュール (ring / damage / visible / input /
//! chrome / timer / handler / pump) はこの `GuiState` を `&mut` で受けて操作する。
//!
//! 座標系の規約:
//!   - ウィンドウ矩形 (`x,y,w,h`) は枠・タイトルバーを含む **外形** (画面座標)。
//!   - クライアント領域 (アプリが描く面) は外形の内側。`client_origin()`。
//!   - 損傷 (dirty) / 可視領域 (vis) は **クライアントローカル座標** (0,0 =
//!     クライアント原点) で持つ。`Paint` の矩形もクライアントローカル (契約 G1/U2)。

#![allow(dead_code)]

use core::cell::UnsafeCell;

use os32api::gui::proto::{
    GuiWinSpec, GUI_MAX_DAMAGE, GUI_MAX_TIMERS, GUI_MAX_WINDOWS, GUI_SLOT_MAX, GUI_WF_BORDER,
    GUI_WF_HAS_CLOSE, GUI_WF_MOVABLE, GUI_WF_VISIBLE, OS32_ERR_FULL, OS32_ERR_INVAL, OS32_ERR_STALE,
};

use crate::cursor::Cursor;
use crate::{chrome, cursor, damage, desktop, input, visible};

/* ================================================================ */
/*  レイアウト定数 (ピクセル)                                        */
/* ================================================================ */
pub const TITLEBAR_H: i32 = 18;
pub const BORDER_W: i32 = 2;
pub const CLOSE_BTN: i32 = 14;
/// 可視領域 / 損傷を持てる矩形数の上限 (契約 U6: 可視 16 / 損傷 8)。
pub const MAX_VIS: usize = 16;
pub const MAX_DMG: usize = GUI_MAX_DAMAGE; /* 8 */
/// 32px 境界 (契約 G4、既存 gfx_add_dirty_rect と同じ規則)。
pub const DAMAGE_SNAP: i32 = 32;

/* WM が予約する SHM 内オフセット: MEM_SHM_GUI_BASE = shm_base + 0x30000 (memmap.h)。 */
pub const GUI_SHM_OFFSET: u32 = 0x30000;

/* ================================================================ */
/*  Rect — 内部演算用 i32 矩形                                       */
/* ================================================================ */
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

impl Rect {
    pub const EMPTY: Rect = Rect { x: 0, y: 0, w: 0, h: 0 };

    #[inline]
    pub fn new(x: i32, y: i32, w: i32, h: i32) -> Rect {
        Rect { x, y, w, h }
    }
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.w <= 0 || self.h <= 0
    }
    #[inline]
    pub fn right(&self) -> i32 {
        self.x + self.w
    }
    #[inline]
    pub fn bottom(&self) -> i32 {
        self.y + self.h
    }
    #[inline]
    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.right() && py >= self.y && py < self.bottom()
    }
    /// 交差矩形 (空なら w/h<=0)。
    pub fn intersect(&self, o: &Rect) -> Rect {
        let x0 = if self.x > o.x { self.x } else { o.x };
        let y0 = if self.y > o.y { self.y } else { o.y };
        let x1 = if self.right() < o.right() { self.right() } else { o.right() };
        let y1 = if self.bottom() < o.bottom() { self.bottom() } else { o.bottom() };
        Rect { x: x0, y: y0, w: x1 - x0, h: y1 - y0 }
    }
    #[inline]
    pub fn intersects(&self, o: &Rect) -> bool {
        !self.intersect(o).is_empty()
    }
    #[inline]
    pub fn translate(&self, dx: i32, dy: i32) -> Rect {
        Rect { x: self.x + dx, y: self.y + dy, w: self.w, h: self.h }
    }
    /// 2 矩形を含む最小の外接矩形。
    pub fn union(&self, o: &Rect) -> Rect {
        if self.is_empty() {
            return *o;
        }
        if o.is_empty() {
            return *self;
        }
        let x0 = if self.x < o.x { self.x } else { o.x };
        let y0 = if self.y < o.y { self.y } else { o.y };
        let x1 = if self.right() > o.right() { self.right() } else { o.right() };
        let y1 = if self.bottom() > o.bottom() { self.bottom() } else { o.bottom() };
        Rect { x: x0, y: y0, w: x1 - x0, h: y1 - y0 }
    }
}

/* ================================================================ */
/*  RectSet — 固定容量の互いに素な矩形集合                           */
/* ================================================================ */
#[derive(Clone, Copy)]
pub struct RectSet {
    pub rects: [Rect; MAX_VIS],
    pub len: usize,
}

impl RectSet {
    pub const EMPTY: RectSet = RectSet { rects: [Rect::EMPTY; MAX_VIS], len: 0 };

    #[inline]
    pub fn clear(&mut self) {
        self.len = 0;
    }
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// 追加 (空・容量超過は無視)。**溢れは呼び出し側が全面フォールバックで扱う。**
    #[inline]
    pub fn push(&mut self, r: Rect) -> bool {
        if r.is_empty() {
            return true;
        }
        if self.len >= MAX_VIS {
            return false;
        }
        self.rects[self.len] = r;
        self.len += 1;
        true
    }
    #[inline]
    pub fn as_slice(&self) -> &[Rect] {
        &self.rects[..self.len]
    }
    /// 合計面積 (present バイト見積り用)。
    pub fn area(&self) -> i64 {
        let mut a: i64 = 0;
        let mut i = 0;
        while i < self.len {
            a += (self.rects[i].w as i64) * (self.rects[i].h as i64);
            i += 1;
        }
        a
    }
}

/* ================================================================ */
/*  WindowId 符号化: index:16 | generation:16 (契約 T4 / U2)          */
/*  generation は 1 から。id==0 は無効。                             */
/* ================================================================ */
#[inline]
pub fn make_id(index: usize, gen: u16) -> u32 {
    ((gen as u32) << 16) | ((index as u32) & 0xFFFF)
}
#[inline]
pub fn id_index(id: u32) -> usize {
    (id & 0xFFFF) as usize
}
#[inline]
pub fn id_gen(id: u32) -> u16 {
    (id >> 16) as u16
}

/* ================================================================ */
/*  Win — ウィンドウ 1 枚                                            */
/* ================================================================ */
#[derive(Clone, Copy)]
pub struct Win {
    pub used: bool,
    pub gen: u16,     /* 現在の generation (再利用で ++) */
    pub owner: i32,   /* exec ネスト段 (res_owner_get) */
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    pub flags: u16,
    pub min_w: i32,
    pub min_h: i32,
    pub title: [u8; 40],
    pub visible: bool,
    /* 損傷の 3 状態 (契約 G4)。クライアントローカル座標。 */
    pub dirty: RectSet,  /* invalidate 済み・Paint 未発行 */
    pub issued: RectSet, /* Paint 発行済み・COMMIT 待ち */
    /* 可視領域 (クライアントローカル、互いに素、≤16)。 */
    pub vis: RectSet,
    /* Configure 通知が必要 (座標確定時に立てる)。 */
    pub configure_pending: bool,
    /* テキストカーソル (SET_TEXT_CURSOR。FEP/キャレット位置は W2)。 */
    pub tc_x: i32,
    pub tc_y: i32,
    pub tc_visible: bool,
}

impl Win {
    pub const EMPTY: Win = Win {
        used: false,
        gen: 1,
        owner: 0,
        x: 0,
        y: 0,
        w: 0,
        h: 0,
        flags: 0,
        min_w: 0,
        min_h: 0,
        title: [0; 40],
        visible: false,
        dirty: RectSet::EMPTY,
        issued: RectSet::EMPTY,
        vis: RectSet::EMPTY,
        configure_pending: false,
        tc_x: 0,
        tc_y: 0,
        tc_visible: false,
    };

    /// 外形矩形 (画面座標)。
    #[inline]
    pub fn outer(&self) -> Rect {
        Rect::new(self.x, self.y, self.w, self.h)
    }
    #[inline]
    pub fn has_border(&self) -> bool {
        (self.flags & GUI_WF_BORDER) != 0
    }
    #[inline]
    pub fn has_close(&self) -> bool {
        (self.flags & GUI_WF_HAS_CLOSE) != 0
    }
    #[inline]
    pub fn movable(&self) -> bool {
        (self.flags & GUI_WF_MOVABLE) != 0
    }
    /// クライアント原点 (画面座標)。枠 + タイトルバーの内側。
    #[inline]
    pub fn client_origin(&self) -> (i32, i32) {
        (self.x + BORDER_W, self.y + BORDER_W + TITLEBAR_H)
    }
    /// クライアントの大きさ。
    #[inline]
    pub fn client_size(&self) -> (i32, i32) {
        let cw = self.w - BORDER_W * 2;
        let ch = self.h - BORDER_W * 2 - TITLEBAR_H;
        (if cw < 0 { 0 } else { cw }, if ch < 0 { 0 } else { ch })
    }
    /// クライアント矩形 (画面座標)。
    #[inline]
    pub fn client_rect_screen(&self) -> Rect {
        let (ox, oy) = self.client_origin();
        let (cw, ch) = self.client_size();
        Rect::new(ox, oy, cw, ch)
    }
    /// タイトルバー矩形 (画面座標)。
    #[inline]
    pub fn titlebar_rect(&self) -> Rect {
        Rect::new(self.x + BORDER_W, self.y + BORDER_W, self.w - BORDER_W * 2, TITLEBAR_H)
    }
    /// 閉じるボタン矩形 (画面座標)。
    #[inline]
    pub fn close_rect(&self) -> Rect {
        let pad = (TITLEBAR_H - CLOSE_BTN) / 2;
        Rect::new(
            self.x + self.w - BORDER_W - CLOSE_BTN - pad,
            self.y + BORDER_W + pad,
            CLOSE_BTN,
            CLOSE_BTN,
        )
    }
    #[inline]
    pub fn id(&self, index: usize) -> u32 {
        make_id(index, self.gen)
    }
}

/* ================================================================ */
/*  Slot — SHM スロット (アプリ 1 本 = 16KB)                         */
/* ================================================================ */
#[derive(Clone, Copy)]
pub struct Slot {
    pub used: bool,
    pub owner: i32,
    pub serial: u16,               /* 入力イベントの serial (契約 T3) */
    pub last_reported_dropped: u16, /* dropped の「前回申告分」(消し込み用) */
}

impl Slot {
    pub const EMPTY: Slot = Slot {
        used: false,
        owner: 0,
        serial: 0,
        last_reported_dropped: 0,
    };
}

/* ================================================================ */
/*  Timer — 8 本 / アプリ (契約 U5)                                  */
/* ================================================================ */
#[derive(Clone, Copy)]
pub struct Timer {
    pub used: bool,
    pub owner: i32,
    pub window: u32,        /* 完全な WindowId */
    pub timer_id: u16,
    pub interval: u32,      /* ティック単位 (最小 1) */
    pub next_deadline: u32, /* 発火予定 tick */
}

impl Timer {
    pub const EMPTY: Timer = Timer {
        used: false,
        owner: 0,
        window: 0,
        timer_id: 0,
        interval: 0,
        next_deadline: 0,
    };
}

/* ================================================================ */
/*  GuiState — 単一グローバル状態                                    */
/* ================================================================ */
pub struct GuiState {
    pub inited: bool,
    pub screen_w: i32,
    pub screen_h: i32,

    pub windows: [Win; GUI_MAX_WINDOWS],
    /* Z 順: 背面→前面。zorder[z_count-1] が最前面 = フォーカス。値は index。 */
    pub zorder: [usize; GUI_MAX_WINDOWS],
    pub z_count: usize,

    pub slots: [Slot; GUI_SLOT_MAX],
    pub timers: [Timer; GUI_MAX_TIMERS * GUI_SLOT_MAX],

    pub shm_base: u32,

    /* WM 自身の UI 状態 (契約 X3 でのみ進める)。 */
    pub drag_index: i32, /* ドラッグ中ウィンドウの index。-1=なし */
    pub drag_dx: i32,
    pub drag_dy: i32,
    pub drag_frame: Rect,   /* 現在描いている XOR 枠 (空=未描画) */
    pub mouse_x: i32,
    pub mouse_y: i32,
    pub prev_buttons: u8,

    /* WM が所有する画面損傷 (デスクトップ + クローム)。画面座標。 */
    pub screen_dirty: RectSet,

    /* 入力取りこぼしの累計 (kbd_dropped_count の前回値)。 */
    pub last_kbd_dropped: u32,

    /* マウスカーソル (損傷とは別経路。契約 W1 作業 7)。 */
    pub cursor: Cursor,

    /* 直近に読んだ tick (取り込み時刻の記録 P2 と期限判定で共有)。 */
    pub now: u32,

    /* ポンプ (X4) の再入防止。 */
    pub in_pump: bool,

    /* 標準単独ループ用フラグ。 */
    pub quit: bool,
    pub launch_pending: bool,
}

impl GuiState {
    pub const NEW: GuiState = GuiState {
        inited: false,
        screen_w: 640,
        screen_h: 400,
        windows: [Win::EMPTY; GUI_MAX_WINDOWS],
        zorder: [0; GUI_MAX_WINDOWS],
        z_count: 0,
        slots: [Slot::EMPTY; GUI_SLOT_MAX],
        timers: [Timer::EMPTY; GUI_MAX_TIMERS * GUI_SLOT_MAX],
        shm_base: 0,
        drag_index: -1,
        drag_dx: 0,
        drag_dy: 0,
        drag_frame: Rect::EMPTY,
        mouse_x: 320,
        mouse_y: 200,
        prev_buttons: 0,
        screen_dirty: RectSet::EMPTY,
        last_kbd_dropped: 0,
        cursor: Cursor::EMPTY,
        now: 0,
        in_pump: false,
        quit: false,
        launch_pending: false,
    };
}

/// generation を 1〜0x7FFF の範囲で進める。**0x8000 以上にしない**のは、
/// WindowId (`gen << 16 | index`) を `gui_call` の戻り値 (i32) として非負で
/// 返すため (負値は `OS32_ERR_*` の領域)。
#[inline]
pub fn next_gen(g: u16) -> u16 {
    let n = g.wrapping_add(1) & 0x7FFF;
    if n == 0 {
        1
    } else {
        n
    }
}

struct GuiCell(UnsafeCell<GuiState>);
unsafe impl Sync for GuiCell {}
static GUI: GuiCell = GuiCell(UnsafeCell::new(GuiState::NEW));

/// グローバル状態への可変参照 (単一スレッド前提)。
#[inline]
pub fn g() -> &'static mut GuiState {
    unsafe { &mut *GUI.0.get() }
}

/* ================================================================ */
/*  GuiState — ウィンドウ / スロット / Z 順ヘルパ                    */
/* ================================================================ */
impl GuiState {
    /// 完全な id → 有効なウィンドウ index。generation 不一致・破棄済みは None。
    pub fn win_by_id(&self, id: u32) -> Option<usize> {
        if id == 0 {
            return None;
        }
        let idx = id_index(id);
        if idx >= GUI_MAX_WINDOWS {
            return None;
        }
        let w = &self.windows[idx];
        if w.used && w.gen == id_gen(id) {
            Some(idx)
        } else {
            None
        }
    }

    /// 最前面ウィンドウの index (無ければ None)。
    pub fn front_index(&self) -> Option<usize> {
        if self.z_count == 0 {
            None
        } else {
            Some(self.zorder[self.z_count - 1])
        }
    }
    /// 最前面ウィンドウの owner (無ければ 0)。
    pub fn front_owner(&self) -> i32 {
        match self.front_index() {
            Some(i) => self.windows[i].owner,
            None => 0,
        }
    }
    /// 最前面ウィンドウの完全な id (無ければ 0)。
    pub fn front_id(&self) -> u32 {
        match self.front_index() {
            Some(i) => self.windows[i].id(i),
            None => 0,
        }
    }

    fn z_add_top(&mut self, index: usize) {
        if self.z_count < GUI_MAX_WINDOWS {
            self.zorder[self.z_count] = index;
            self.z_count += 1;
        }
    }
    fn z_remove(&mut self, index: usize) {
        let mut i = 0;
        while i < self.z_count {
            if self.zorder[i] == index {
                let mut j = i;
                while j + 1 < self.z_count {
                    self.zorder[j] = self.zorder[j + 1];
                    j += 1;
                }
                self.z_count -= 1;
                return;
            }
            i += 1;
        }
    }
    /// index を最前面へ (= フォーカス)。Z 順の変化で露出計算が要る。
    pub fn bring_to_front(&mut self, index: usize) {
        if self.front_index() == Some(index) {
            return;
        }
        self.z_remove(index);
        self.z_add_top(index);
    }

    /// z (0=背面) 番目のウィンドウ index。
    #[inline]
    pub fn z_at(&self, z: usize) -> usize {
        self.zorder[z]
    }
    /// index の Z 位置 (背面 0)。無ければ None。
    pub fn z_of(&self, index: usize) -> Option<usize> {
        let mut i = 0;
        while i < self.z_count {
            if self.zorder[i] == index {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    /// 空きウィンドウスロットの index。
    pub fn alloc_win(&mut self) -> Option<usize> {
        let mut i = 0;
        while i < GUI_MAX_WINDOWS {
            if !self.windows[i].used {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    /// owner → スロット番号。
    pub fn slot_of_owner(&self, owner: i32) -> Option<usize> {
        let mut i = 0;
        while i < GUI_SLOT_MAX {
            if self.slots[i].used && self.slots[i].owner == owner {
                return Some(i);
            }
            i += 1;
        }
        None
    }
    /// owner にスロットを割り当てる (既にあればそれ)。v1 は常に 0 を優先。
    pub fn alloc_slot(&mut self, owner: i32) -> Option<usize> {
        if let Some(s) = self.slot_of_owner(owner) {
            return Some(s);
        }
        let mut i = 0;
        while i < GUI_SLOT_MAX {
            if !self.slots[i].used {
                self.slots[i] = Slot::EMPTY;
                self.slots[i].used = true;
                self.slots[i].owner = owner;
                return Some(i);
            }
            i += 1;
        }
        None
    }

    /// WM 所有の画面損傷 (デスクトップ + クローム) に 1 矩形を足す。
    ///
    /// `RectSet::push` は容量 (16) を超えると**黙って捨てる**ので、そのまま
    /// 使うと塗り残しが残る。溢れた分は先頭と union して「過剰申告 = 安全側」に
    /// 倒す (転送量が増えるだけで、画面が壊れることは無い)。
    pub fn dirty_screen(&mut self, r: Rect) {
        let clip = r.intersect(&Rect::new(0, 0, self.screen_w, self.screen_h));
        if clip.is_empty() {
            return;
        }
        if !self.screen_dirty.push(clip) {
            self.screen_dirty.rects[0] = self.screen_dirty.rects[0].union(&clip);
        }
    }

    /// 画面座標の点 (px,py) を含む最前面の可視ウィンドウ index。
    pub fn hit_window(&self, px: i32, py: i32) -> Option<usize> {
        let mut z = self.z_count;
        while z > 0 {
            z -= 1;
            let idx = self.zorder[z];
            let w = &self.windows[idx];
            if w.used && w.visible && w.outer().contains(px, py) {
                return Some(idx);
            }
        }
        None
    }

    /* ---- 所有者回収 (契約 T4 / U8。GUI_OP_OWNER_EXIT) ---- */
    pub fn reclaim_owner(&mut self, owner: i32) {
        /* ウィンドウ */
        let mut i = 0;
        while i < GUI_MAX_WINDOWS {
            if self.windows[i].used && self.windows[i].owner == owner {
                let vac = self.windows[i].outer();
                self.z_remove(i);
                let gen = next_gen(self.windows[i].gen);
                self.windows[i] = Win::EMPTY;
                self.windows[i].gen = gen;
                self.dirty_screen(vac);
                if self.drag_index == i as i32 {
                    self.drag_index = -1;
                }
            }
            i += 1;
        }
        /* タイマ */
        let mut t = 0;
        while t < self.timers.len() {
            if self.timers[t].used && self.timers[t].owner == owner {
                self.timers[t] = Timer::EMPTY;
            }
            t += 1;
        }
        /* スロット */
        let mut s = 0;
        while s < GUI_SLOT_MAX {
            if self.slots[s].used && self.slots[s].owner == owner {
                self.slots[s] = Slot::EMPTY;
            }
            s += 1;
        }
    }
}

/// GUI_WF_VISIBLE ビットを visible フラグへ同期する補助。
#[inline]
pub fn wf_visible(flags: u16) -> bool {
    (flags & GUI_WF_VISIBLE) != 0
}


/* ================================================================ */
/*  present / compositor (WM が所有する画面 = デスクトップ + クローム) */
/*                                                                  */
/*  クライアント面の内側には触れない (アプリの COMMIT が present する)。 */
/* ================================================================ */

/// 画面座標の矩形を **転送キューへ積む** (まだ VRAM へは出さない)。
///
/// `gfx_present_rect` は 1 回ごとに `gfx_present_dirty` を呼ぶので
/// `gfx_stats().commits` が矩形の数だけ増える。契約 P の「commit はループ 1 周
/// 1 回」を守るため、WM は矩形を `gfx_add_dirty_rect` で積み、最後に
/// [`flush_present`] を 1 回だけ呼ぶ。
pub fn queue_present(st: &GuiState, r: Rect) {
    let clip = r.intersect(&Rect::new(0, 0, st.screen_w, st.screen_h));
    if clip.is_empty() {
        return;
    }
    unsafe {
        (os32api::api().gfx_add_dirty_rect)(clip.x, clip.y, clip.w, clip.h);
    }
}

/// 積んだ矩形をまとめて VRAM へ転送する (`commits` += 1)。積んだものが
/// 無ければカーネル側で早期 return する (commits は増えない)。
pub fn flush_present() {
    unsafe {
        (os32api::api().gfx_present_dirty)();
    }
}

/// 1 矩形だけを即転送する近道 (積む + 流す)。
pub fn present_rect(st: &GuiState, r: Rect) {
    queue_present(st, r);
    flush_present();
}

/// 画面座標の矩形にデスクトップ + クロームを再合成する (バックバッファのみ)。
/// クライアント面は「窓の外形 − 内側」ではなく **窓の外形を chrome が描く枠のみ**
/// を触り、内側 (アプリ描画) は残す。デスクトップは「矩形 − 全窓外形」に塗る。
pub fn composite_rect(st: &GuiState, r: Rect) {
    let clip = r.intersect(&Rect::new(0, 0, st.screen_w, st.screen_h));
    if clip.is_empty() {
        return;
    }
    /* デスクトップ: clip から全ての可視窓の外形を引いた残りを塗る。 */
    let mut region = RectSet::EMPTY;
    region.push(clip);
    let mut z = 0;
    while z < st.z_count {
        let idx = st.zorder[z];
        let w = &st.windows[idx];
        if w.used && w.visible {
            let (next, _ok) = visible::region_subtract_rect(&region, w.outer());
            region = next;
        }
        z += 1;
    }
    let mut i = 0;
    let mut hit_hint = false;
    while i < region.len {
        let d = region.rects[i];
        desktop::fill(st, d);
        if d.intersects(&desktop::hint_rect()) {
            hit_hint = true;
        }
        i += 1;
    }
    if hit_hint {
        desktop::draw_hint(st);
    }
    /* クローム: clip に掛かる窓を背面→前面で描き直す。 */
    let mut z2 = 0;
    while z2 < st.z_count {
        let idx = st.zorder[z2];
        let w = &st.windows[idx];
        if w.used && w.visible && w.outer().intersects(&clip) {
            let active = z2 + 1 == st.z_count;
            chrome::draw_window_chrome(w, active);
        }
        z2 += 1;
    }
}

/// 画面全体を合成して present する (起動時・フルスクリーン GFX からの復帰)。
/// **全画面 present は WM だけ** (契約 G4)。カーソルは描き直す。
pub fn composite_full(st: &mut GuiState) {
    let whole = Rect::new(0, 0, st.screen_w, st.screen_h);
    cursor::discard(st); /* 下地は全部塗り替わる */
    composite_rect(st, whole);
    st.screen_dirty.clear();
    cursor::show(st);
    queue_present(st, whole);
    flush_present();
}

/// WM が溜めた画面損傷 (デスクトップ + クローム) を合成して present し、クリアする。
/// アプリのクライアント面 (COMMIT) とは独立。commit は 1 回にまとめる。
pub fn flush_screen_dirty(st: &mut GuiState) {
    let dragging = st.drag_index >= 0;
    if st.screen_dirty.is_empty() && !dragging {
        return;
    }
    /* 下地を描き替えるので、まずカーソルを外して下地を戻す。 */
    cursor::hide(st);

    let n = st.screen_dirty.len;
    let mut i = 0;
    while i < n {
        let r = st.screen_dirty.rects[i];
        composite_rect(st, r);
        queue_present(st, r);
        i += 1;
    }
    st.screen_dirty.clear();

    /* ドラッグ中なら枠を再描画 (合成で消えているため)。 */
    if dragging {
        let f = st.drag_frame;
        chrome::draw_drag_outline(f.x, f.y, f.w, f.h);
        queue_present(st, f);
    }

    cursor::show(st);
    let cr = cursor::rect(st);
    queue_present(st, cr);
    flush_present();
}

/* ================================================================ */
/*  WM の周期 (契約 X3 / 単独ループ)                                 */
/* ================================================================ */

/// WM の 1 周: 入力取り込み → WM 自身の UI → クローム/デスクトップの present。
/// 文脈は [`input::Ctx`] (X3 = Wait / Standalone、X4 = Pump)。
pub fn wm_cycle(st: &mut GuiState, ctx: input::Ctx) {
    input::capture(st, ctx);
    if ctx != input::Ctx::Pump {
        flush_screen_dirty(st);
    }
}

/* ================================================================ */
/*  フォーカス (契約 U1 の set_focus)                                */
/* ================================================================ */

/// ウィンドウを最前面へ出してフォーカスを移す。`Focus` イベントを両者へ流す。
pub fn set_focus(st: &mut GuiState, owner: i32, id: u32) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    if st.front_index() == Some(index) {
        return 0;
    }
    let old_id = st.front_id();
    st.bring_to_front(index);
    visible::recompute_and_expose(st);
    let outer = st.windows[index].outer();
    st.dirty_screen(outer);
    let new_id = st.windows[index].id(index);
    input::emit_focus_change(st, old_id, new_id);
    0
}

/* ================================================================ */
/*  ウィンドウ操作 (契約 U1)。owner 検証つき。                        */
/*  返り値: 成功 = id (>=0) もしくは 0、失敗 = OS32_ERR_*。            */
/* ================================================================ */

/// id を owner が操作してよいか検証して index を返す。
fn resolve_owned(st: &GuiState, owner: i32, id: u32) -> Result<usize, i32> {
    match st.win_by_id(id) {
        None => Err(OS32_ERR_STALE),
        Some(i) => {
            if st.windows[i].owner != owner {
                Err(OS32_ERR_INVAL)
            } else {
                Ok(i)
            }
        }
    }
}

/// create_window。返り値: 完全な WindowId (i32 として非負) / OS32_ERR_FULL。
pub fn create_window(st: &mut GuiState, owner: i32, spec: &GuiWinSpec) -> i32 {
    let index = match st.alloc_win() {
        Some(i) => i,
        None => return OS32_ERR_FULL,
    };
    let gen = {
        let g = st.windows[index].gen;
        if g == 0 {
            1
        } else {
            g
        }
    };
    let mut w = Win::EMPTY;
    w.used = true;
    w.gen = gen;
    w.owner = owner;
    w.x = spec.rect.x as i32;
    w.y = spec.rect.y as i32;
    let mut ww = spec.rect.w as i32;
    let mut wh = spec.rect.h as i32;
    if ww < 60 {
        ww = 60;
    }
    if wh < TITLEBAR_H + 8 {
        wh = TITLEBAR_H + 8;
    }
    w.w = ww;
    w.h = wh;
    w.flags = if spec.flags == 0 {
        os32api::gui::proto::GUI_WF_DEFAULT
    } else {
        spec.flags
    };
    w.min_w = spec.min_w as i32;
    w.min_h = spec.min_h as i32;
    /* title (40B 固定、NUL 終端を保証)。 */
    let mut t = 0;
    while t < 39 {
        w.title[t] = spec.title[t];
        if spec.title[t] == 0 {
            break;
        }
        t += 1;
    }
    w.title[39] = 0;
    w.visible = wf_visible(w.flags);
    w.configure_pending = true;
    st.windows[index] = w;
    st.z_add_top(index);
    /* 初回は全面 dirty + 露出計算。 */
    damage::set_dirty_full(&mut st.windows[index]);
    visible::recompute_and_expose(st);
    st.dirty_screen(st.windows[index].outer());
    /* generation は 1〜0x7FFF に制限してあるので i32 として必ず非負。 */
    st.windows[index].id(index) as i32
}

pub fn destroy_window(st: &mut GuiState, owner: i32, id: u32) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    let vac = st.windows[index].outer();
    st.z_remove(index);
    let gen = next_gen(st.windows[index].gen);
    st.windows[index] = Win::EMPTY;
    st.windows[index].gen = gen;
    if st.drag_index == index as i32 {
        st.drag_index = -1;
    }
    st.dirty_screen(vac);
    visible::recompute_and_expose(st);
    0
}

pub fn move_window(st: &mut GuiState, owner: i32, id: u32, x: i32, y: i32) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    let old = st.windows[index].outer();
    st.windows[index].x = x;
    st.windows[index].y = y;
    st.dirty_screen(old);
    st.dirty_screen(st.windows[index].outer());
    damage::set_dirty_full(&mut st.windows[index]);
    st.windows[index].configure_pending = true;
    visible::recompute_and_expose(st);
    0
}

pub fn resize_window(st: &mut GuiState, owner: i32, id: u32, w: i32, h: i32) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    let old = st.windows[index].outer();
    let mut nw = w;
    let mut nh = h;
    if nw < st.windows[index].min_w {
        nw = st.windows[index].min_w;
    }
    if nh < st.windows[index].min_h {
        nh = st.windows[index].min_h;
    }
    if nw < 60 {
        nw = 60;
    }
    if nh < TITLEBAR_H + 8 {
        nh = TITLEBAR_H + 8;
    }
    st.windows[index].w = nw;
    st.windows[index].h = nh;
    st.dirty_screen(old);
    st.dirty_screen(st.windows[index].outer());
    damage::set_dirty_full(&mut st.windows[index]);
    st.windows[index].configure_pending = true;
    visible::recompute_and_expose(st);
    0
}

pub fn show_window(st: &mut GuiState, owner: i32, id: u32, show: bool) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    let outer = st.windows[index].outer();
    st.windows[index].visible = show;
    if show {
        st.windows[index].flags |= GUI_WF_VISIBLE;
        damage::set_dirty_full(&mut st.windows[index]);
    } else {
        st.windows[index].flags &= !GUI_WF_VISIBLE;
    }
    st.dirty_screen(outer);
    visible::recompute_and_expose(st);
    0
}

pub fn set_title(st: &mut GuiState, owner: i32, id: u32, title: &[u8], len: usize) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    let n = if len > 39 { 39 } else { len };
    let mut i = 0;
    while i < 40 {
        st.windows[index].title[i] = 0;
        i += 1;
    }
    let mut k = 0;
    while k < n {
        st.windows[index].title[k] = title[k];
        k += 1;
    }
    st.windows[index].title[39] = 0;
    /* タイトルバーだけ再描画。 */
    st.dirty_screen(st.windows[index].titlebar_rect());
    0
}

pub fn raise(st: &mut GuiState, owner: i32, id: u32) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    if st.front_index() != Some(index) {
        st.bring_to_front(index);
        st.dirty_screen(st.windows[index].outer());
        visible::recompute_and_expose(st);
    }
    0
}

pub fn client_rect(st: &GuiState, owner: i32, id: u32) -> Result<Rect, i32> {
    let index = resolve_owned(st, owner, id)?;
    let (cox, coy) = st.windows[index].client_origin();
    let (cw, ch) = st.windows[index].client_size();
    Ok(Rect::new(cox, coy, cw, ch))
}

pub fn set_text_cursor(
    st: &mut GuiState,
    owner: i32,
    id: u32,
    x: i32,
    y: i32,
    visible_flag: bool,
) -> i32 {
    let index = match resolve_owned(st, owner, id) {
        Ok(i) => i,
        Err(e) => return e,
    };
    st.windows[index].tc_x = x;
    st.windows[index].tc_y = y;
    st.windows[index].tc_visible = visible_flag;
    /* FEP / キャレット描画位置の記録のみ (実描画は W2)。 */
    0
}

/* ================================================================ */
/*  パレット / 画面情報 (起動時)                                     */
/* ================================================================ */

/// G6 のシステムパレットを gfx_set_palette で入れる (契約 G6)。
pub fn install_system_palette() {
    let a = unsafe { os32api::api() };
    let mut i = 0;
    while i < 16 {
        let c = os32api::gui::proto::GUI_SYSTEM_PALETTE[i];
        unsafe {
            (a.gfx_set_palette)(i as i32, c.r, c.g, c.b);
        }
        i += 1;
    }
}

/// gfx_screen_info を 1 回読んで画面寸法を控える (契約 G5、決め打ち禁止)。
pub fn read_screen_info(st: &mut GuiState) {
    #[repr(C)]
    #[derive(Clone, Copy)]
    struct ScreenInfo {
        width: u16,
        height: u16,
        bpp: u8,
        format: u8,
        flags: u32,
        lease_mask: u16,
        lease_first: u16,
        lease_count: u16,
        reserved: [u16; 5],
    }
    let mut si = ScreenInfo {
        width: 640,
        height: 400,
        bpp: 4,
        format: 0,
        flags: 0,
        lease_mask: 0,
        lease_first: 0,
        lease_count: 0,
        reserved: [0; 5],
    };
    unsafe {
        (os32api::api().gfx_screen_info)(&mut si as *mut ScreenInfo as *mut u8);
    }
    if si.width > 0 && si.height > 0 {
        st.screen_w = si.width as i32;
        st.screen_h = si.height as i32;
    }
}
