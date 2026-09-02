//! gui_demo — libos32gui の目視確認デモ
//!
//! ウィンドウを 2 枚出し、以下を確認できる:
//!   - Z オーダー: 重なったウィンドウをクリックすると前面に来る
//!   - 移動:       タイトルバーをドラッグして動かす
//!   - 前面化:     クリックしたウィンドウがフォーカス色 (濃紺) になる
//!   - 閉じる:     右上の × でそのウィンドウが消える
//!   - ボタン:     Window 1 の [Count++] を押すとラベルの数字が増える
//!   - キー:       フォーカスウィンドウにキーが届く (ESC で終了)
//!
//! 起動: シェルから `gui_demo` (NHD/HostDrv に配備後)。
//! 終了: 2 枚とも閉じる or ESC キー。
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

    /* ウィンドウ 1: ボタン + カウントラベル */
    let win1 = libos32gui::gui_create_window(
        60,
        50,
        260,
        150,
        b"Window 1\0".as_ptr(),
        libos32gui::GUI_WF_DEFAULT,
    );
    let lbl = libos32gui::gui_add_label(win1, 16, 12, b"count: 0\0".as_ptr());
    let btn = libos32gui::gui_add_button(win1, 16, 44, 110, 26, b"Count++\0".as_ptr());

    /* ウィンドウ 2: 重ねて Z オーダーを見せる */
    let _win2 = libos32gui::gui_create_window(
        200,
        120,
        260,
        150,
        b"Window 2\0".as_ptr(),
        libos32gui::GUI_WF_DEFAULT,
    );
    libos32gui::gui_add_label(_win2, 16, 12, b"drag my titlebar\0".as_ptr());

    let mut count: i32 = 0;
    /* count 表示バッファ (ASCII 十進) */
    let mut running = true;

    while running {
        libos32gui::gui_pump();

        /* イベント処理 */
        let mut ev = libos32gui::GuiEvent::NONE;
        while unsafe { libos32gui::gui_poll_event(&mut ev as *mut _) } != 0 {
            match ev.kind {
                libos32gui::GUI_EV_WIN_CLOSE => {
                    libos32gui::gui_destroy_window(ev.win_id);
                }
                libos32gui::GUI_EV_BUTTON_CLICK => {
                    if ev.widget_id == btn {
                        count += 1;
                        let mut buf = [0u8; 24];
                        let n = fmt_count(&mut buf, count);
                        buf[n] = 0;
                        libos32gui::gui_widget_set_text(lbl, buf.as_ptr());
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

        /* 全ウィンドウ閉じたら終了 */
        if libos32gui::gui_window_count() == 0 {
            running = false;
        }

        libos32gui::gui_draw();

        /* ~16ms 相当のウェイト (get_tick は 10ms 単位) */
        let t0 = os32api::get_tick();
        while os32api::get_tick() == t0 {}
    }

    libos32gui::gui_shutdown();
    0
}

/* "count: N" を buf に書いて長さを返す (no_alloc, 自前十進変換) */
fn fmt_count(buf: &mut [u8; 24], mut v: i32) -> usize {
    let prefix = b"count: ";
    let mut n = 0usize;
    while n < prefix.len() {
        buf[n] = prefix[n];
        n += 1;
    }
    if v < 0 {
        v = 0;
    }
    /* 桁を逆順で作る */
    let mut tmp = [0u8; 12];
    let mut t = 0usize;
    if v == 0 {
        tmp[t] = b'0';
        t += 1;
    } else {
        while v > 0 {
            tmp[t] = b'0' + (v % 10) as u8;
            v /= 10;
            t += 1;
        }
    }
    while t > 0 {
        t -= 1;
        buf[n] = tmp[t];
        n += 1;
    }
    n
}
