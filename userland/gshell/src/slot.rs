//! slot.rs — SHM スロットの物理アドレス解決とヘッダ / 要求 / 応答 / リングの
//! 低レベルアクセス (契約 T2 / T2a)。
//!
//! MEM_SHM_GUI_BASE = shm_base + 0x30000 (memmap.h)。スロット N の先頭 =
//! MEM_SHM_GUI_BASE + N * 16KB。gshell は CPL=0 なので SHM を直接読み書きできる。
//! ヘッダの `ring_tail` / `flags` / `dropped` / `seq` は WM (生産者) だけが書く。
//! `ring_head` はアプリ (消費者) だけが書く (契約 T3)。

use core::ptr;

use crate::wm::{GuiState, GUI_SHM_OFFSET};
use os32api::gui::proto::{
    GuiSlotHeader, GUI_PROTO_VERSION, GUI_SLOT_HDR_OFF, GUI_SLOT_REQ_OFF, GUI_SLOT_RESP_OFF,
    GUI_SLOT_RING_OFF, GUI_SLOT_SIZE,
};

/// スロット N の先頭バイトポインタ。
#[inline]
pub fn slot_base(st: &GuiState, slot: usize) -> *mut u8 {
    (st.shm_base + GUI_SHM_OFFSET + (slot as u32) * (GUI_SLOT_SIZE as u32)) as *mut u8
}

#[inline]
pub fn header_ptr(st: &GuiState, slot: usize) -> *mut GuiSlotHeader {
    unsafe { slot_base(st, slot).add(GUI_SLOT_HDR_OFF) as *mut GuiSlotHeader }
}

#[inline]
pub fn req_ptr(st: &GuiState, slot: usize) -> *const u8 {
    unsafe { slot_base(st, slot).add(GUI_SLOT_REQ_OFF) as *const u8 }
}

#[inline]
pub fn resp_ptr(st: &GuiState, slot: usize) -> *mut u8 {
    unsafe { slot_base(st, slot).add(GUI_SLOT_RESP_OFF) as *mut u8 }
}

/// リング先頭 (128 × 16B)。
#[inline]
pub fn ring_ptr(st: &GuiState, slot: usize) -> *mut u8 {
    unsafe { slot_base(st, slot).add(GUI_SLOT_RING_OFF) as *mut u8 }
}

/// ヘッダを読む (コピー)。
#[inline]
pub fn read_header(st: &GuiState, slot: usize) -> GuiSlotHeader {
    unsafe { ptr::read_unaligned(header_ptr(st, slot)) }
}

/// ヘッダを書く。
#[inline]
pub fn write_header(st: &GuiState, slot: usize, h: &GuiSlotHeader) {
    unsafe { ptr::write_unaligned(header_ptr(st, slot), *h) }
}

/// スロットヘッダを初期化する (INIT 時)。ring_head/tail を 0 に、proto を入れる。
pub fn init_header(st: &GuiState, slot: usize) {
    let h = GuiSlotHeader {
        proto_version: GUI_PROTO_VERSION,
        flags: 0,
        seq: 0,
        ring_head: 0,
        ring_tail: 0,
        dropped: 0,
        reserved: 0,
    };
    write_header(st, slot, &h);
}

/// 要求ブロックから型 T を読む (先頭から)。
///
/// # Safety
/// アプリが要求ブロックへ正しい構造体を書いていることを前提にする。
#[inline]
pub unsafe fn read_req<T: Copy>(st: &GuiState, slot: usize) -> T {
    ptr::read_unaligned(req_ptr(st, slot) as *const T)
}

/// 応答ブロックへ型 T を書く (先頭から)。
#[inline]
pub fn write_resp<T: Copy>(st: &GuiState, slot: usize, v: T) {
    unsafe { ptr::write_unaligned(resp_ptr(st, slot) as *mut T, v) }
}
