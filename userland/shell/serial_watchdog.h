/* ======================================================================== */
/*  SERIAL_WATCHDOG.H — 速度を上げたあと会話が続いているかを見る番犬        */
/*                                                                          */
/*  速度を上げたあと **ホストと足並みが揃ったかはゲストには分からない**。    */
/*  013Ah が効かない機種、ケーブルが速度に耐えない、ホストが開き直しに       */
/*  失敗した — どれでも「こちらは N、ホストは別の速度」になり、戻すための    */
/*  `serial 9600` すら届かない (Codex レビュー blocker 2b)。                */
/*                                                                          */
/*  だから **切替後に 1 バイトも受信しなければゲストが自力で元へ戻す**。     */
/*  ホストが新しい速度で 1 バイトでも届けられたら番犬は解除する。            */
/*                                                                          */
/*  ここは**依存を 1 つも持たない**。シェル (userland/shell/rshell.c) から   */
/*  使い、ホスト試験 (tools/tests/serial_vfast_host.c) は .c をそのまま      */
/*  #include して回す。                                                     */
/*    試験: tools/tests/test_serial_vfast.py (ケース watchdog)              */
/*    記録: tools/tests/serial_vfast_tdd.md                                 */
/*    票  : docs/tasks/realhw/TASK_SERIAL_VFAST.md                          */
/* ======================================================================== */

#ifndef __SERIAL_WATCHDOG_H
#define __SERIAL_WATCHDOG_H

/* 期限 [tick]。PIT は 100Hz なので 500 tick = 5 秒。
 * ホスト道具は「`serial N` を送る → 閉じる → N で開き直す → `ver` を投げる」
 * までに 1 秒程度しかかからないので、往復に十分な余裕がある。
 * **短すぎるとホストが開き直す前に戻ってしまい**、長すぎると失敗したまま
 * 待たされる。 */
#define SER_SWITCH_WATCHDOG_TICKS 500

#define SER_WD_WAIT   0   /* まだ期限内 — 待つ */
#define SER_WD_LINKED 1   /* 受信があった — 解除 */
#define SER_WD_REVERT 2   /* 期限切れで無音 — 元の設定へ戻す */

/* 切替後の番犬の判定。`elapsed_ticks` は切替からの経過 tick、`bytes_seen` は
 * 切替後に受け取ったバイト数。**受信が先**: 期限を過ぎていても 1 バイトでも
 * 来ていたら LINKED (遅れて届いた応答を「無音」と読み替えない)。 */
int serial_watchdog_decide(unsigned long elapsed_ticks,
                           unsigned long bytes_seen);

#endif /* __SERIAL_WATCHDOG_H */
