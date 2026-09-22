/* ======================================================================== */
/*  SERIAL_WATCHDOG.C — 速度を上げたあと会話が続いているかの判定            */
/*                                                                          */
/*  ここには副作用のある行を 1 つも置かない (KAPI も呼ばない)。切り出して   */
/*  あるのは、**実機でしか起きない失敗**をホストで試験するため。            */
/*    試験: tools/tests/test_serial_vfast.py (ケース watchdog)              */
/*    記録: tools/tests/serial_vfast_tdd.md                                 */
/* ======================================================================== */

#include "serial_watchdog.h"

int serial_watchdog_decide(unsigned long elapsed_ticks, int acked)
{
    /* **ack を期限より先に見る。** 期限ちょうどに届いた ack を「無音だった」
     * と読み替えて戻すと、せっかく揃った足並みを自分で壊す。
     *
     * ⚠ 見るのは `serial ack` という **明示の合図** だけ。受信バイト数でも
     * 往復した行数でもない (往復 1〜3 の設計はそこで穴が尽きなかった)。 */
    if (acked) {
        return SER_WD_LINKED;
    }
    /* 期限を過ぎても ack が来ない = ホストは別の速度で喋っている
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
    w->acked = 0;
    w->start_tick = tick;
    w->prev_mode = prev_mode;
    w->prev_baud = prev_baud;
}

void serial_watchdog_ack(struct serial_watchdog *w)
{
    /* **仕掛かっていないときは印を立てない。** 立てると、次の切替で arm する
     * 前の古い ack が残って即 LINKED になる (arm が 0 に戻すので実害は無いが、
     * 意味の無い代入はしない)。`serial ack` の応答自体は呼び手が冪等に返す。 */
    if (!w || !w->armed) return;
    w->acked = 1;
}

int serial_watchdog_poll(struct serial_watchdog *w, unsigned long tick)
{
    int d;

    if (!w || !w->armed) return SER_WD_WAIT;
    d = serial_watchdog_decide(tick - w->start_tick, w->acked);
    if (d == SER_WD_WAIT) return SER_WD_WAIT;
    /* 答えが出たら下ろす。**2 度は返らない** — REVERT を 2 回返すと
     * 戻したあとにもう一度 serial_init を呼んでしまう。 */
    w->armed = 0;
    return d;
}

int serial_watchdog_leave(struct serial_watchdog *w)
{
    if (!w || !w->armed) return SER_WD_WAIT;

    /* **抜けたあとは poll する者が居ない** (往復 4 B4)。番犬が仕掛かったまま
     * 忘れられると、確認の取れていない速度のまま会話が死ぬ。期限を待たずに
     * その場で戻す — 待っても誰も見に来ないのだから、待つ意味が無い。 */
    w->armed = 0;
    if (w->acked) {
        return SER_WD_WAIT;   /* 確認済み = そのままでよい */
    }
    return SER_WD_REVERT;
}
