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
                           unsigned long bytes_seen)
{
    /* **受信の有無を期限より先に見る。** 期限ちょうどに応答が届いた場合に
     * 「無音だった」と読み替えて戻してしまうと、せっかく揃った足並みを
     * 自分で壊す。 */
    if (bytes_seen > 0) {
        return SER_WD_LINKED;
    }
    /* 期限を過ぎても 1 バイトも来ていない = ホストは別の速度で喋っている
     * (こちらの応答が届いていない)。元へ戻すしかない。 */
    if (elapsed_ticks >= (unsigned long)SER_SWITCH_WATCHDOG_TICKS) {
        return SER_WD_REVERT;
    }
    return SER_WD_WAIT;
}
