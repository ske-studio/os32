//! gui_demo — libos32gui の目視確認デモ (2nd スコープ)
//!
//! Window 1「Widgets」: テキストボックス / チェックボックス / リストボックス /
//!   OK ボタン と、それぞれの状態を映すラベル。
//! Window 2「Help」: 操作説明。
//!
//! 確認できること:
//!   - Zオーダー/移動/前面化/閉じる (1st スコープ)
//!   - テキストボックス編集: フォーカス中にタイプ、← → で移動、BS/DEL で削除
//!   - チェックボックス: クリック or SPACE でトグル
//!   - リストボックス: クリック or ↑↓ で選択移動
//!   - Tab / Shift+Tab: フォーカスウィジェット移動 (青い枠)
//!   - キーは最前面ウィンドウのフォーカスウィジェットへ届く
//!
//! 起動: シェルから `gui_demo`。終了: 全ウィンドウを閉じる or ESC。
#![no_std]
#![no_main]

extern crate os32api;

use os32api::KernelAPI;

const ESC: i32 = 27;

#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8, api: *mut KernelAPI) -> i32 {
    os32api::os32_init(api);
    if libos32gui::gui_init(api) != 0 {
        return -1;
    }

    /* ---- Window 1: ウィジェット ---- */
    let w1 = libos32gui::gui_create_window(
        40,
        40,
        320,
        236,
        b"Widgets\0".as_ptr(),
        libos32gui::GUI_WF_DEFAULT,
    );
    libos32gui::gui_add_label(w1, 12, 10, b"name:\0".as_ptr());
    let tb = libos32gui::gui_add_textbox(w1, 60, 6, 236, 20);
    let cb = libos32gui::gui_add_checkbox(w1, 12, 36, b"Enable sound\0".as_ptr());
    let lbl_cb = libos32gui::gui_add_label(w1, 12, 60, b"sound: OFF\0".as_ptr());
    let lb = libos32gui::gui_add_listbox(w1, 12, 84, 170, 80);
    libos32gui::gui_listbox_add_item(lb, b"Apple\0".as_ptr());
    libos32gui::gui_listbox_add_item(lb, b"Banana\0".as_ptr());
    libos32gui::gui_listbox_add_item(lb, b"Cherry\0".as_ptr());
    libos32gui::gui_listbox_add_item(lb, b"Durian\0".as_ptr());
    libos32gui::gui_listbox_add_item(lb, b"Elderberry\0".as_ptr());
    libos32gui::gui_listbox_add_item(lb, b"Fig\0".as_ptr());
    let lbl_sel = libos32gui::gui_add_label(w1, 196, 88, b"sel: -\0".as_ptr());
    let btn = libos32gui::gui_add_button(w1, 196, 116, 100, 26, b"OK\0".as_ptr());
    let lbl_tb = libos32gui::gui_add_label(w1, 12, 176, b"text:\0".as_ptr());
    let lbl_ok = libos32gui::gui_add_label(w1, 12, 200, b"\0".as_ptr());

    /* ---- Window 2: ヘルプ ---- */
    let w2 = libos32gui::gui_create_window(
        378,
        66,
        236,
        176,
        b"Help\0".as_ptr(),
        libos32gui::GUI_WF_DEFAULT,
    );
    libos32gui::gui_add_label(w2, 12, 10, b"Tab: next widget\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 30, b"S-Tab: prev widget\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 50, b"type: edit textbox\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 70, b"SPACE: toggle check\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 90, b"UP/DOWN: list select\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 110, b"drag title: move win\0".as_ptr());
    libos32gui::gui_add_label(w2, 12, 130, b"x: close   ESC: quit\0".as_ptr());

    /* Window 1 を最前面・フォーカスに */
    libos32gui::gui_set_focus(w1);

    let mut running = true;
    while running {
        libos32gui::gui_pump();

        let mut ev = libos32gui::GuiEvent::NONE;
        while unsafe { libos32gui::gui_poll_event(&mut ev as *mut _) } != 0 {
            match ev.kind {
                libos32gui::GUI_EV_WIN_CLOSE => {
                    libos32gui::gui_destroy_window(ev.win_id);
                }
                libos32gui::GUI_EV_TEXT_CHANGED => {
                    if ev.widget_id == tb {
                        let mut content = [0u8; 40];
                        unsafe {
                            libos32gui::gui_textbox_get_text(tb, content.as_mut_ptr(), 40);
                        }
                        let mut buf = [0u8; 64];
                        let n = concat(&mut buf, b"text: ", &content);
                        buf[n] = 0;
                        libos32gui::gui_widget_set_text(lbl_tb, buf.as_ptr());
                    }
                }
                libos32gui::GUI_EV_CHECKBOX_TOGGLED => {
                    if ev.widget_id == cb {
                        let msg: &[u8] = if ev.button != 0 {
                            b"sound: ON\0"
                        } else {
                            b"sound: OFF\0"
                        };
                        libos32gui::gui_widget_set_text(lbl_cb, msg.as_ptr());
                    }
                }
                libos32gui::GUI_EV_LIST_SELECT => {
                    if ev.widget_id == lb {
                        let mut item = [0u8; 40];
                        unsafe {
                            libos32gui::gui_listbox_get_item_text(lb, ev.x, item.as_mut_ptr(), 40);
                        }
                        let mut buf = [0u8; 64];
                        let n = concat(&mut buf, b"sel: ", &item);
                        buf[n] = 0;
                        libos32gui::gui_widget_set_text(lbl_sel, buf.as_ptr());
                    }
                }
                libos32gui::GUI_EV_BUTTON_CLICK => {
                    if ev.widget_id == btn {
                        libos32gui::gui_widget_set_text(lbl_ok, b"OK pressed!\0".as_ptr());
                    }
                }
                libos32gui::GUI_EV_KEY => {
                    if ev.key == ESC {
                        running = false;
                    }
                }
                _ => {}
            }
        }

        if libos32gui::gui_window_count() == 0 {
            running = false;
        }

        libos32gui::gui_draw();

        let t0 = os32api::get_tick();
        while os32api::get_tick() == t0 {}
    }

    libos32gui::gui_shutdown();
    0
}

/* prefix + NUL 終端 src を dst に連結。返り値: 書いたバイト数 (NUL 除く) */
fn concat(dst: &mut [u8], prefix: &[u8], src: &[u8]) -> usize {
    let mut n = 0usize;
    let mut i = 0;
    while i < prefix.len() && n + 1 < dst.len() {
        dst[n] = prefix[i];
        n += 1;
        i += 1;
    }
    let mut j = 0;
    while j < src.len() && src[j] != 0 && n + 1 < dst.len() {
        dst[n] = src[j];
        n += 1;
        j += 1;
    }
    n
}
