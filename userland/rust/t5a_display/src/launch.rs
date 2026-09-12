//! launch.rs — 起動要求表 (KAPI v49) を端末側から読む純粋部分 (票 T9 D4 / D9)。
//!
//! `prompt.rs` / `sink.rs` と同じ流儀で、ここには `no_std` の純関数と小さな
//! 状態だけを置く。KAPI (`launch_req` / `launch_poll` / `launch_cancel`) を
//! 実際に呼ぶのは `guest.rs` で、こちらは「返ってきた値をどう読むか」と
//! 「次に何をするか」だけを決める。ホスト試験がそのまま取り込める。
//!
//! ## なぜ要求表なのか (票 T9 §0 / D4)
//!
//! GUI 中の CPL=3 アプリは入れ子 `exec_run` を使えない (子が park できず協調型
//! 全体が止まる)。端末は `session_launch` で「1 本増やす」ことはできたが、
//! **子 ID も終了も分からない**ので、con_sink の `EXIT` レコードをモード判定に
//! 使っていた。要求表は要求者ごとに 1 本あり、token で照合して子 ID と完了を
//! 問い合わせられる (票 §7 blocker: 制御情報をリングに載せない)。
//!
//! ## この端末が守ること
//!
//! - `EXIT` レコードは**表示だけ** (D4)。モードは `launch_poll` の答えで動かす。
//! - 接続モードの ESC は `launch_cancel` (D9)。子には注入しない。
//! - `OS32_ERR_AGAIN` (WM がまだ取っていない) は次のタイマで自動再試行。
//! - `exit` / 端末自身の終了では取消を出さない — 要求者の退場はカーネルが
//!   孤児回収する (`launch_owner_exit` が KILL を積む、票 §10 non-blocker 1)。

/* ================================================================ */
/*  ワイヤ定数 (sdk/include/os32/os32_kapi_shared.h が正典)          */
/* ================================================================ */

/// `LAUNCH_ST_PENDING` — 要求は積まれたが WM がまだ取っていない。
pub const ST_PENDING: i32 = 0x000;
/// `LAUNCH_ST_TAKEN` — WM が取った (`exec_start` 中)。
pub const ST_TAKEN: i32 = 0x001;
/// `LAUNCH_ST_RUNNING` — 下位に子 ID が乗る。
pub const ST_RUNNING: i32 = 0x100;
/// `LAUNCH_ST_DONE` — 子が終わった (短命な子は最初の poll でこれ)。
pub const ST_DONE: i32 = 0x200;
/// `LAUNCH_ST_FAILED` — 下位に `-rc` が乗る。
pub const ST_FAILED: i32 = 0x300;
/// status の下位 (子 ID / `-rc`) を取り出す遮蔽。
pub const ST_LOW: i32 = 0xFF;
/// status の上位 (種別)。
pub const ST_HIGH: i32 = 0xFF00;

/// `OS32_ERR_STALE` — 表はもう IDLE (完了を 1 度渡した後など)。
/// guest は負の戻り値を理由で分けない (どれもプロンプトへ戻る) ので、
/// 名前を持つのは試験と読み手のため。
#[allow(dead_code)]
pub const ERR_STALE: i32 = -11;
/// `OS32_ERR_FULL` — 自分の表がまだ空いていない (前の要求が残っている)。
pub const ERR_FULL: i32 = -13;
/// `OS32_ERR_AGAIN` — `PENDING` / `TAKEN` への取消。WM がまだ取っていない。
pub const ERR_AGAIN: i32 = -14;

/* ================================================================ */
/*  status の読み方                                                  */
/* ================================================================ */

/// `launch_poll` が書いた status の意味 (§1a の ABI 表)。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Phase {
    /// 積んだだけ。WM はまだ取っていない。
    Pending,
    /// WM が取った (`exec_start` 中)。
    Taken,
    /// 子が走っている。中身は子 ID。
    Running(i32),
    /// 子が終わった。表はこの poll で IDLE に戻っている。
    Done,
    /// 起動できなかった。中身は元の負の `rc`。
    Failed(i32),
    /// 知らない値。固まらないようプロンプトへ戻す材料にする。
    Unknown,
}

/// **純関数**: status → [`Phase`]。
///
/// `DONE` の下位は規約上 0 だが、来ても `Done` に倒す — 知らないビットで
/// 接続モードに居座る方が害が大きい。負の status は `Unknown`。
pub fn phase(status: i32) -> Phase {
    if status < 0 {
        return Phase::Unknown;
    }
    let low = status & ST_LOW;
    match status & ST_HIGH {
        0 => match low {
            ST_PENDING => Phase::Pending,
            ST_TAKEN => Phase::Taken,
            _ => Phase::Unknown,
        },
        ST_RUNNING => Phase::Running(low),
        ST_DONE => Phase::Done,
        /* FAILED は `0x300 + (-rc)` なので符号を戻す。0x300 ちょうど
         * (rc = 0) は FAILED にならないはずだが、来たら 0 のまま渡す。 */
        ST_FAILED => Phase::Failed(-low),
        _ => Phase::Unknown,
    }
}

/* ================================================================ */
/*  接続中の 1 件                                                    */
/* ================================================================ */

/// 取消 (D9) の進み具合。ESC 連打で二重に `launch_cancel` を出さないための印。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Cancel {
    /// 頼まれていない。
    No,
    /// ESC を受けたが `OS32_ERR_AGAIN` だった (WM がまだ取っていない)。
    /// 次のタイマでもう一度出す。
    Retry,
    /// `launch_cancel` が 0 を返した = 取消が積まれた。`DONE` を待つだけ。
    Armed,
}

/// `launch_req` が受け付けた 1 件 (token とそれに紐づく子 / 取消の進み具合)。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Attach {
    /// 表の照合鍵。要求者 ID ではなく token で照合する (票 §9 4)。
    pub token: i32,
    /// `RUNNING` で分かった子 ID。0 = まだ分からない (表示にだけ使う)。
    pub child: i32,
    /// 取消の進み具合。
    pub cancel: Cancel,
}

/// 1 回の `launch_poll` / `launch_cancel` の後に端末がすること。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    /// 何も変わらない (描き直しも要らない)。
    Idle,
    /// 表示が変わった (子 ID が分かった等)。最下行を描き直す。
    Redraw,
    /// `launch_cancel` を (もう一度) 出す。
    Retry,
    /// 接続モードを畳んでプロンプトへ戻る。
    Finish(Outcome),
}

/// プロンプトへ戻る理由。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Outcome {
    /// 子が終わった (取消で畳まれた場合もここ)。何も出さない。
    Done,
    /// 起動できなかった。`launch failed (<rc>)` を出す。
    Failed(i32),
    /// `launch_poll` 自体が負 / 意味不明な status。異常系だが固まらない。
    Lost(i32),
}

impl Attach {
    /// `launch_req` が返した token (> 0) から始める。
    pub fn new(token: i32) -> Self {
        Self {
            token,
            child: 0,
            cancel: Cancel::No,
        }
    }

    /// 取消を頼まれているか (最下行の表示に使う)。
    pub fn cancelling(&self) -> bool {
        self.cancel != Cancel::No
    }

    /// `launch_poll(token, &status)` の結果を読む (D4)。
    ///
    /// `rc` が負なら理由を問わずプロンプトへ戻す — `OS32_ERR_STALE` (表はもう
    /// IDLE) も含めて、接続モードに居座る理由が無い。
    pub fn poll(&mut self, rc: i32, status: i32) -> Step {
        if rc < 0 {
            return Step::Finish(Outcome::Lost(rc));
        }
        match phase(status) {
            Phase::Done => Step::Finish(Outcome::Done),
            Phase::Failed(code) => Step::Finish(Outcome::Failed(code)),
            Phase::Unknown => Step::Finish(Outcome::Lost(status)),
            Phase::Running(child) => {
                let changed = self.child != child;
                self.child = child;
                /* 取消が AGAIN で止まっていたなら、RUNNING になった今こそ
                 * 通る (D9: RUNNING → KILL(child) の PENDING)。 */
                if self.cancel == Cancel::Retry {
                    Step::Retry
                } else if changed {
                    Step::Redraw
                } else {
                    Step::Idle
                }
            }
            /* PENDING / TAKEN は何もしない。取消待ちなら再試行だけ出す。 */
            Phase::Pending | Phase::Taken => {
                if self.cancel == Cancel::Retry {
                    Step::Retry
                } else {
                    Step::Idle
                }
            }
        }
    }

    /// 接続モードで ESC を受けた (D9)。戻り値が真なら `launch_cancel` を出す。
    ///
    /// 既に出してある (`Retry` / `Armed`) なら何もしない — 連打しても表には
    /// `AGAIN` / `STALE` が返るだけで害は無いが、呼ぶ回数は 1 回にまとめる。
    pub fn escape(&mut self) -> bool {
        if self.cancel == Cancel::No {
            self.cancel = Cancel::Retry;
            true
        } else {
            false
        }
    }

    /// `launch_cancel(token)` の戻り値を読む (D9)。
    ///
    /// - `0`: 取消が積まれた。接続モードのまま `DONE` を待つ (子には注がない)。
    /// - `OS32_ERR_AGAIN`: WM がまだ取っていない。次のタイマで再試行。
    /// - それ以外 (`OS32_ERR_STALE` = 既に `DONE` / `FAILED`、不一致): 待っても
    ///   来ないのでプロンプトへ戻す。
    pub fn cancelled(&mut self, rc: i32) -> Step {
        if rc == 0 {
            self.cancel = Cancel::Armed;
            Step::Redraw
        } else if rc == ERR_AGAIN {
            self.cancel = Cancel::Retry;
            Step::Redraw
        } else {
            Step::Finish(Outcome::Done)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_decodes_into_the_abi_phases() {
        assert_eq!(phase(ST_PENDING), Phase::Pending);
        assert_eq!(phase(ST_TAKEN), Phase::Taken);
        assert_eq!(phase(ST_RUNNING + 3), Phase::Running(3));
        assert_eq!(phase(ST_RUNNING), Phase::Running(0));
        assert_eq!(phase(ST_DONE), Phase::Done);
        /* FAILED は `0x300 + (-rc)` — 符号を戻して元の rc にする。 */
        assert_eq!(phase(ST_FAILED + 13), Phase::Failed(-13));
        assert_eq!(phase(ST_FAILED + 9), Phase::Failed(-9));
        /* 知らない値と負の status は Unknown (固まらせない)。 */
        assert_eq!(phase(0x002), Phase::Unknown);
        assert_eq!(phase(0x400), Phase::Unknown);
        assert_eq!(phase(-11), Phase::Unknown);
        /* DONE に下位が乗って来ても DONE に倒す。 */
        assert_eq!(phase(ST_DONE + 1), Phase::Done);
    }

    #[test]
    fn a_short_lived_child_is_done_on_the_first_poll() {
        /* 票 §5 blocker 1: `rc == 0` の子は RUNNING を通らず DONE になる。 */
        let mut a = Attach::new(7);
        assert_eq!(a.poll(0, ST_DONE), Step::Finish(Outcome::Done));
        assert_eq!(a.child, 0);
    }

    #[test]
    fn running_then_done_walks_the_child_id_into_the_status_row() {
        let mut a = Attach::new(11);
        assert!(!a.cancelling());
        /* 取られるまでは何も起きない。 */
        assert_eq!(a.poll(0, ST_PENDING), Step::Idle);
        assert_eq!(a.poll(0, ST_TAKEN), Step::Idle);
        /* 子 ID が分かった周だけ描き直す。 */
        assert_eq!(a.poll(0, ST_RUNNING + 3), Step::Redraw);
        assert_eq!(a.child, 3);
        assert_eq!(a.poll(0, ST_RUNNING + 3), Step::Idle);
        assert_eq!(a.poll(0, ST_DONE), Step::Finish(Outcome::Done));
    }

    #[test]
    fn a_failed_launch_carries_the_negative_rc_back() {
        let mut a = Attach::new(2);
        assert_eq!(a.poll(0, ST_TAKEN), Step::Idle);
        assert_eq!(a.poll(0, ST_FAILED + 12), Step::Finish(Outcome::Failed(-12)));
    }

    #[test]
    fn a_negative_poll_returns_to_the_prompt_instead_of_hanging() {
        /* 既に IDLE (STALE) でも、知らない status でも接続モードに居座らない。 */
        let mut a = Attach::new(5);
        assert_eq!(a.poll(ERR_STALE, 0), Step::Finish(Outcome::Lost(ERR_STALE)));
        let mut a = Attach::new(5);
        assert_eq!(a.poll(0, 0x777), Step::Finish(Outcome::Lost(0x777)));
    }

    #[test]
    fn escape_cancels_once_and_waits_for_done() {
        /* D9: ESC → cancel 0 → 接続モードのまま DONE を待つ。 */
        let mut a = Attach::new(21);
        assert_eq!(a.poll(0, ST_RUNNING + 4), Step::Redraw);
        assert!(a.escape());
        assert!(a.cancelling());
        assert_eq!(a.cancelled(0), Step::Redraw);
        assert_eq!(a.cancel, Cancel::Armed);
        /* 2 度目の ESC は `launch_cancel` を出さない (1 回にまとめる)。 */
        assert!(!a.escape());
        /* 取消を積んだ後の poll は再試行を出さない。 */
        assert_eq!(a.poll(0, ST_RUNNING + 4), Step::Idle);
        assert_eq!(a.poll(0, ST_PENDING), Step::Idle);
        assert_eq!(a.poll(0, ST_DONE), Step::Finish(Outcome::Done));
    }

    #[test]
    fn again_retries_the_cancel_on_the_next_timer() {
        /* D9: PENDING / TAKEN への取消は AGAIN。次のタイマで自動再試行する。 */
        let mut a = Attach::new(33);
        assert!(a.escape());
        assert_eq!(a.cancelled(ERR_AGAIN), Step::Redraw);
        assert_eq!(a.cancel, Cancel::Retry);
        /* 取られていない間は poll のたびに再試行を促す。 */
        assert_eq!(a.poll(0, ST_PENDING), Step::Retry);
        assert_eq!(a.cancelled(ERR_AGAIN), Step::Redraw);
        assert_eq!(a.poll(0, ST_TAKEN), Step::Retry);
        /* RUNNING になれば通る。子 ID も同じ周で控える。 */
        assert_eq!(a.poll(0, ST_RUNNING + 6), Step::Retry);
        assert_eq!(a.child, 6);
        assert_eq!(a.cancelled(0), Step::Redraw);
        assert_eq!(a.poll(0, ST_RUNNING + 6), Step::Idle);
        assert_eq!(a.poll(0, ST_DONE), Step::Finish(Outcome::Done));
    }

    #[test]
    fn stale_cancel_returns_to_the_prompt() {
        /* D9: DONE / FAILED 済みへの取消は STALE。待っても来ないので戻る。 */
        let mut a = Attach::new(44);
        assert!(a.escape());
        assert_eq!(a.cancelled(ERR_STALE), Step::Finish(Outcome::Done));
    }
}
