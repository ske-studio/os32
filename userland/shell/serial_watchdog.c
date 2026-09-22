/* ======================================================================== */
/*  SERIAL_WATCHDOG.C — 速度を上げたあと会話が続いているかの判定            */
/*                                                                          */
/*  ここには副作用のある行を 1 つも置かない (KAPI も呼ばない)。切り出して   */
/*  あるのは、**実機でしか起きない失敗**をホストで試験するため。            */
/*    試験: tools/tests/test_serial_vfast.py (ケース watchdog)              */
/*    記録: tools/tests/serial_vfast_tdd.md                                 */
/* ======================================================================== */

#include "serial_watchdog.h"

int serial_watchdog_decide(unsigned long elapsed_ticks,
                           unsigned long lines_completed)
{
    /* **往復の成立を期限より先に見る。** 期限ちょうどに成立した往復を
     * 「無音だった」と読み替えて戻してしまうと、せっかく揃った足並みを
     * 自分で壊す。
     *
     * ⚠ 数えるのは「バイト」ではなく **往復し終えた行**。受信だけで解除すると、
     * 新速度で `ver` が届いたのに応答の EOT が落ちた場合に、ゲストは新速度の
     * まま・ホストは旧速度へ戻り、二度と合わなくなる (Codex レビュー B2)。
     * 化けたバイトやローカルのキー入力で解除されるのも同じ理由で困る。 */
    if (lines_completed > 0) {
        return SER_WD_LINKED;
    }
    /* 期限を過ぎても 1 往復もしていない = ホストは別の速度で喋っている
     * (こちらの応答が届いていない)。元へ戻すしかない。 */
    if (elapsed_ticks >= (unsigned long)SER_SWITCH_WATCHDOG_TICKS) {
        return SER_WD_REVERT;
    }
    return SER_WD_WAIT;
}

void serial_watchdog_arm(struct serial_watchdog *w, unsigned long tick,
                         unsigned long prev_mode, unsigned long prev_baud)
{
    if (!w) return;
    w->armed = 1;
    w->start_tick = tick;
    w->lines = 0;
    w->prev_mode = prev_mode;
    w->prev_baud = prev_baud;
}

void serial_watchdog_line_done(struct serial_watchdog *w)
{
    /* **仕掛かっていないときは数えない。** 数えてしまうと、次の切替で
     * arm する前の古い数が残って即 LINKED になる (arm が 0 に戻すので
     * 実害は無いが、意味の無い加算はしない)。 */
    if (!w || !w->armed) return;
    w->lines++;
}

int serial_watchdog_poll(struct serial_watchdog *w, unsigned long tick)
{
    int d;

    if (!w || !w->armed) return SER_WD_WAIT;
    d = serial_watchdog_decide(tick - w->start_tick, w->lines);
    if (d == SER_WD_WAIT) return SER_WD_WAIT;
    /* 答えが出たら下ろす。**2 度は返らない** — REVERT を 2 回返すと
     * 戻したあとにもう一度 serial_init を呼んでしまう。 */
    w->armed = 0;
    return d;
}
