//! libos32gui 共有型と定数
//!
//! すべて no_std / no_alloc。文字列は NUL 終端の固定長バイト配列で保持する。

/* ---- 容量 (固定長。no_alloc なのでコンパイル時に決める) ---- */
pub const MAX_WINDOWS: usize = 16;
pub const MAX_WIDGETS: usize = 64; /* 全ウィンドウ合計 */
pub const MAX_TITLE: usize = 40; /* NUL 込み。UTF-8 日本語も入る */
pub const EVENT_QUEUE_LEN: usize = 32;

/* ---- ウィンドウフラグ (extern C から見えるビットマスク) ---- */
pub const GUI_WF_VISIBLE: u32 = 0x01;
pub const GUI_WF_HAS_CLOSE: u32 = 0x02;
pub const GUI_WF_MOVABLE: u32 = 0x04;
pub const GUI_WF_BORDER: u32 = 0x08;
/* 既定のウィンドウ (枠 + タイトルバー + 閉じる + 移動可 + 表示) */
pub const GUI_WF_DEFAULT: u32 =
    GUI_WF_VISIBLE | GUI_WF_HAS_CLOSE | GUI_WF_MOVABLE | GUI_WF_BORDER;

/* ---- ウィジェット種別 ---- */
pub const GUI_WT_BUTTON: u8 = 1;
pub const GUI_WT_LABEL: u8 = 2;
pub const GUI_WT_TEXTBOX: u8 = 3;
pub const GUI_WT_LISTBOX: u8 = 4;

/* ---- イベント種別 (Win32 の WM_* 相当) ---- */
pub const GUI_EV_NONE: u32 = 0;
pub const GUI_EV_MOUSE_MOVE: u32 = 1;
pub const GUI_EV_MOUSE_DOWN: u32 = 2;
pub const GUI_EV_MOUSE_UP: u32 = 3;
pub const GUI_EV_KEY: u32 = 4;
pub const GUI_EV_WIN_ACTIVATE: u32 = 5;
pub const GUI_EV_WIN_MOVE: u32 = 6;
pub const GUI_EV_WIN_CLOSE: u32 = 7; /* 閉じるボタン押下。破棄はアプリが決める */
pub const GUI_EV_BUTTON_CLICK: u32 = 8;

/* ---- レイアウト定数 (ピクセル) ---- */
pub const TITLEBAR_H: i32 = 18;
pub const BORDER_W: i32 = 2;
pub const CLOSE_BTN: i32 = 14; /* 閉じるボタン一辺 */

/* ---- パレット色 (libos32gfx の 16 色パレットインデックス) ---- */
pub const COL_DESKTOP: u8 = 12; /* 水色デスクトップ */
pub const COL_BORDER: u8 = 0; /* 黒枠 */
pub const COL_TITLE_ACT: u8 = 1; /* アクティブ: 濃紺 */
pub const COL_TITLE_INACT: u8 = 6; /* 非アクティブ: グレー */
pub const COL_TITLE_TEXT: u8 = 7; /* 白 */
pub const COL_CLIENT: u8 = 7; /* クライアント面: 白 */
pub const COL_CLOSE_FACE: u8 = 8; /* 閉じるボタン面: 赤 */
pub const COL_CLOSE_X: u8 = 7; /* 閉じる印: 白 */
pub const COL_BTN_FACE: u8 = 6; /* ボタン面: グレー */
pub const COL_BTN_FACE_DN: u8 = 13; /* 押下時: 青 */
pub const COL_BTN_TEXT: u8 = 0; /* ボタン文字: 黒 */
pub const COL_LABEL_TEXT: u8 = 0; /* ラベル: 黒 */

/// ウィンドウ 1 枚分の状態。`used=false` は空きスロット。
#[derive(Clone, Copy)]
pub struct Window {
    pub used: bool,
    pub id: u32, /* ハンドル (1 以上。0 は無効) */
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    pub flags: u32,
    pub title: [u8; MAX_TITLE], /* NUL 終端 */
}

impl Window {
    pub const EMPTY: Window = Window {
        used: false,
        id: 0,
        x: 0,
        y: 0,
        w: 0,
        h: 0,
        flags: 0,
        title: [0; MAX_TITLE],
    };

    /// タイトルバー矩形 (画面座標)。枠内側の上端。
    #[inline]
    pub fn titlebar_rect(&self) -> (i32, i32, i32, i32) {
        (
            self.x + BORDER_W,
            self.y + BORDER_W,
            self.w - BORDER_W * 2,
            TITLEBAR_H,
        )
    }

    /// 閉じるボタン矩形 (画面座標)。タイトルバー右端。
    #[inline]
    pub fn close_rect(&self) -> (i32, i32, i32, i32) {
        let pad = (TITLEBAR_H - CLOSE_BTN) / 2;
        (
            self.x + self.w - BORDER_W - CLOSE_BTN - pad,
            self.y + BORDER_W + pad,
            CLOSE_BTN,
            CLOSE_BTN,
        )
    }

    /// クライアント領域原点 (画面座標)。ウィジェット座標はここが (0,0)。
    #[inline]
    pub fn client_origin(&self) -> (i32, i32) {
        (self.x + BORDER_W, self.y + BORDER_W + TITLEBAR_H)
    }

    #[inline]
    pub fn client_size(&self) -> (i32, i32) {
        (
            self.w - BORDER_W * 2,
            self.h - BORDER_W * 2 - TITLEBAR_H,
        )
    }
}

/// ウィジェット 1 個分の状態。座標は所属ウィンドウのクライアント相対。
#[derive(Clone, Copy)]
pub struct Widget {
    pub used: bool,
    pub id: u32,
    pub win_id: u32,
    pub kind: u8,
    pub pressed: bool, /* ボタン: 押下中 (armed) 表示 */
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    pub text: [u8; MAX_TITLE],
}

impl Widget {
    pub const EMPTY: Widget = Widget {
        used: false,
        id: 0,
        win_id: 0,
        kind: 0,
        pressed: false,
        x: 0,
        y: 0,
        w: 0,
        h: 0,
        text: [0; MAX_TITLE],
    };
}

/// 統合イベント。extern "C" 越しに 1 個ずつ取り出す (Win32 の MSG 相当)。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GuiEvent {
    pub kind: u32,
    pub win_id: u32,
    pub widget_id: u32,
    pub x: i32, /* マウス: 画面座標 / キー: 未使用 */
    pub y: i32,
    pub key: i32,     /* キーコード (GUI_EV_KEY) */
    pub button: u32,  /* マウスボタンビット (MOUSE_BTN_*) */
}

impl GuiEvent {
    pub const NONE: GuiEvent = GuiEvent {
        kind: GUI_EV_NONE,
        win_id: 0,
        widget_id: 0,
        x: 0,
        y: 0,
        key: 0,
        button: 0,
    };
}

/// mouse_poll(*mut u8) が書き込む C 構造体 (os32_kapi_shared.h の MouseInfo と同一レイアウト)。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MouseInfo {
    pub x: i16,
    pub y: i16,
    pub dx: i16,
    pub dy: i16,
    pub buttons: u8,
    pub mode: u8,
}

impl MouseInfo {
    pub const ZERO: MouseInfo = MouseInfo {
        x: 0,
        y: 0,
        dx: 0,
        dy: 0,
        buttons: 0,
        mode: 0,
    };
}

pub const MOUSE_BTN_LEFT: u8 = 0x01;

/// 矩形内包判定 (画面座標)。
#[inline]
pub fn point_in(px: i32, py: i32, rx: i32, ry: i32, rw: i32, rh: i32) -> bool {
    px >= rx && px < rx + rw && py >= ry && py < ry + rh
}

/// NUL 終端バイト列を固定長バッファへコピー。
/// バッファ末尾で切れる場合、UTF-8 の多バイト文字を途中で割らない。
pub fn copy_cstr(dst: &mut [u8; MAX_TITLE], src: *const u8) {
    *dst = [0; MAX_TITLE];
    if src.is_null() {
        return;
    }
    let mut i = 0usize;
    while i < MAX_TITLE - 1 {
        let b = unsafe { *src.add(i) };
        if b == 0 {
            break;
        }
        dst[i] = b;
        i += 1;
    }
    /* i = 書き込んだバイト数。末尾に不完全な UTF-8 シーケンスがあれば削る。
     * 直近のリードバイト位置 lead を探し、そこから必要バイト数がそろって
     * いなければ lead 以降を NUL で潰す。 */
    if i == 0 {
        return;
    }
    let mut lead = i;
    while lead > 0 && (dst[lead - 1] & 0xC0) == 0x80 {
        lead -= 1; /* 継続バイトを飛ばす */
    }
    if lead == 0 {
        return;
    }
    lead -= 1; /* dst[lead] がリードバイト */
    let b0 = dst[lead];
    let need: usize = if b0 & 0x80 == 0 {
        1
    } else if b0 & 0xE0 == 0xC0 {
        2
    } else if b0 & 0xF0 == 0xE0 {
        3
    } else if b0 & 0xF8 == 0xF0 {
        4
    } else {
        1
    };
    if lead + need > i {
        /* 不完全 — リードバイト以降を捨てる */
        let mut k = lead;
        while k < i {
            dst[k] = 0;
            k += 1;
        }
    }
}
