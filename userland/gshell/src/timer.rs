//! timer.rs — アプリ用タイマ (契約 U5、8 本 / アプリ、PIT 10ms 粒度)。
//!
//! `TIMER_SET` / `TIMER_KILL` を処理し、期限を過ぎた ID を導出型 `Timer` として
//! リングへ流す。`OP_WAIT` の期限計算 (次のタイマまで) にも使う。
//! 共有 ABI (GuiReqTimerSet) に repeat フィールドが無いため、v1 のタイマは
//! **反復 (interval ごと)** として扱う (判断: PM へ報告)。

use crate::ring;
use crate::wm::{GuiState, Timer};
use os32api::gui::proto::{GUI_MAX_TIMERS, GUI_SLOT_MAX, OS32_ERR_FULL, OS32_ERR_INVAL};

fn ms_to_ticks(ms: u16) -> u32 {
    let t = (ms as u32) / 10;
    if t == 0 {
        1
    } else {
        t
    }
}

/// タイマを設定する。同じ (owner,window,id) があれば更新。
pub fn set(st: &mut GuiState, owner: i32, window: u32, id: u16, interval_ms: u16, now: u32) -> i32 {
    let interval = ms_to_ticks(interval_ms);
    /* 既存を探す。 */
    let mut i = 0;
    while i < st.timers.len() {
        let t = &mut st.timers[i];
        if t.used && t.owner == owner && t.window == window && t.timer_id == id {
            t.interval = interval;
            t.next_deadline = now.wrapping_add(interval);
            return 0;
        }
        i += 1;
    }
    /* この owner が使っているタイマ数を数える (上限 8/アプリ)。 */
    let mut used = 0;
    let mut j = 0;
    while j < st.timers.len() {
        if st.timers[j].used && st.timers[j].owner == owner {
            used += 1;
        }
        j += 1;
    }
    if used >= GUI_MAX_TIMERS {
        return OS32_ERR_FULL;
    }
    /* 空きへ。 */
    let mut k = 0;
    while k < st.timers.len() {
        if !st.timers[k].used {
            st.timers[k] = Timer {
                used: true,
                owner,
                window,
                timer_id: id,
                interval,
                next_deadline: now.wrapping_add(interval),
            };
            return 0;
        }
        k += 1;
    }
    OS32_ERR_FULL
}

/// タイマを消す。
pub fn kill(st: &mut GuiState, owner: i32, window: u32, id: u16) -> i32 {
    let mut i = 0;
    while i < st.timers.len() {
        let t = &st.timers[i];
        if t.used && t.owner == owner && t.window == window && t.timer_id == id {
            st.timers[i] = Timer::EMPTY;
            return 0;
        }
        i += 1;
    }
    OS32_ERR_INVAL
}

/// この owner の次のタイマ期限 (tick)。無ければ None。`OP_WAIT` の期限計算用。
pub fn next_deadline_owner(st: &GuiState, owner: i32) -> Option<u32> {
    let mut best: Option<u32> = None;
    let mut i = 0;
    while i < st.timers.len() {
        let t = &st.timers[i];
        if t.used && t.owner == owner {
            best = Some(match best {
                Some(b) if b <= t.next_deadline => b,
                _ => t.next_deadline,
            });
        }
        i += 1;
    }
    best
}

/// この owner に期限切れタイマがあるか (OP_WAIT 起床条件)。
pub fn has_expired(st: &GuiState, owner: i32, now: u32) -> bool {
    let mut i = 0;
    while i < st.timers.len() {
        let t = &st.timers[i];
        if t.used && t.owner == owner && now.wrapping_sub(t.next_deadline) < 0x8000_0000 {
            return true;
        }
        i += 1;
    }
    false
}

/// 期限切れタイマを `Timer` イベントとしてリングへ流し、次回期限を進める。
/// リングが満杯なら期限を進めず (次の OP_POLL で再試行)。
pub fn fire_expired(st: &mut GuiState, slot: usize, owner: i32, now: u32) {
    let mut i = 0;
    while i < st.timers.len() {
        let (used, towner, window, id, interval, deadline) = {
            let t = &st.timers[i];
            (t.used, t.owner, t.window, t.timer_id, t.interval, t.next_deadline)
        };
        if used && towner == owner {
            /* now >= deadline (ラップ安全) */
            if now.wrapping_sub(deadline) < 0x8000_0000 {
                let ev = ring::ev_timer(window, id as u8);
                if ring::append(st, slot, &ev) {
                    /* 次の期限へ (反復)。取りこぼしを避けるため now 基準。 */
                    st.timers[i].next_deadline = now.wrapping_add(interval);
                } else {
                    /* 満杯: 進めずに次周へ持ち越す。 */
                    return;
                }
            }
        }
        i += 1;
    }
    let _ = GUI_SLOT_MAX;
}
