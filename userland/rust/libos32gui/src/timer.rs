//! timer.rs — タイマの所有型 (契約 U5)。票 C2 作業 7。
//!
//! `set_timer(id, interval, repeat)` / `kill_timer(id)`、`Timer` イベントで届く。
//! PIT 10ms 粒度、8 本 / アプリ。
//!
//! **W1 との食い違い (申し送り)**: 共有 ABI の `GuiReqTimerSet` に `repeat` が
//! 無く、WM 側のタイマは**反復固定**。単発 (`repeat = false`) はクライアントで
//! 面倒を見る — 最初の `Timer` を受けた時点で `kill` する ([`fired`])。

use core::cell::UnsafeCell;

use crate::client::{self, GuiResult};
use crate::window::Window;
use os32api::gui::proto::GUI_MAX_TIMERS;

/* 単発タイマの台帳 (timer_id ごとの 1bit)。8 本しかないので配列で足りる。 */
struct OneShot(UnsafeCell<[Option<(u32, u16)>; GUI_MAX_TIMERS]>);
unsafe impl Sync for OneShot {}
static ONESHOT: OneShot = OneShot(UnsafeCell::new([None; GUI_MAX_TIMERS]));

#[inline]
fn oneshot() -> &'static mut [Option<(u32, u16)>; GUI_MAX_TIMERS] {
    unsafe { &mut *ONESHOT.0.get() }
}

/// アプリのタイマ 1 本。`Drop` で `kill_timer`。
pub struct Timer {
    window: u32,
    id: u16,
}

impl Timer {
    /// 反復タイマ (契約 U5 の `repeat = true`)。`interval_ms` は 10ms 粒度に丸まる。
    pub fn repeating(window: &Window, id: u16, interval_ms: u16) -> GuiResult<Timer> {
        client::timer_set(window.id(), id, interval_ms)?;
        clear_oneshot(window.id(), id);
        Ok(Timer { window: window.id(), id })
    }

    /// 単発タイマ (`repeat = false`)。最初の `Timer` イベントで自動的に止まる。
    pub fn once(window: &Window, id: u16, delay_ms: u16) -> GuiResult<Timer> {
        client::timer_set(window.id(), id, delay_ms)?;
        set_oneshot(window.id(), id);
        Ok(Timer { window: window.id(), id })
    }

    #[inline]
    pub fn id(&self) -> u16 {
        self.id
    }

    #[inline]
    pub fn window(&self) -> u32 {
        self.window
    }

    /// 間隔を張り替える。
    pub fn restart(&self, interval_ms: u16) -> GuiResult<()> {
        client::timer_set(self.window, self.id, interval_ms)
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        clear_oneshot(self.window, self.id);
        let _ = client::timer_kill(self.window, self.id);
    }
}

/* ================================================================ */
/*  単発タイマの台帳                                                 */
/* ================================================================ */

fn set_oneshot(window: u32, id: u16) {
    let t = oneshot();
    let mut i = 0;
    while i < GUI_MAX_TIMERS {
        if t[i].is_none() {
            t[i] = Some((window, id));
            return;
        }
        i += 1;
    }
}

fn clear_oneshot(window: u32, id: u16) {
    let t = oneshot();
    let mut i = 0;
    while i < GUI_MAX_TIMERS {
        if t[i] == Some((window, id)) {
            t[i] = None;
        }
        i += 1;
    }
}

/// `Timer` イベントを受けたとき `app::run` が呼ぶ。単発なら WM 側を止める。
pub(crate) fn fired(window: u32, id: u16) {
    let t = oneshot();
    let mut i = 0;
    while i < GUI_MAX_TIMERS {
        if t[i] == Some((window, id)) {
            t[i] = None;
            let _ = client::timer_kill(window, id);
            return;
        }
        i += 1;
    }
}
