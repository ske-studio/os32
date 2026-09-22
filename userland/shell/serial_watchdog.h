/* ======================================================================== */
/*  SERIAL_WATCHDOG.H — 速度を上げたあと会話が続いているかを見る番犬        */
/*                                                                          */
/*  速度を上げたあと **ホストと足並みが揃ったかはゲストには分からない**。    */
/*  013Ah が効かない機種、ケーブルが速度に耐えない、ホストが開き直しに       */
/*  失敗した — どれでも「こちらは N、ホストは別の速度」になり、戻すための    */
/*  `serial 9600` すら届かない。だから **確認が取れなければゲストが自力で    */
/*  元へ戻す**。                                                            */
/*                                                                          */
/*  ⚠ **合図は明示的にする** (Codex レビュー往復 4 で設計変更)。             */
/*  往復 1〜3 では「受信した」「1 行往復した」を証拠に使おうとして、その     */
/*  たびに穴が出た — 切替行自身を数える / 断片を数える / EOT の送信失敗を    */
/*  数える / ローカルキーで解除される / 本文が落ちて EOT だけ通る……。       */
/*  **暗黙の推定は尽きない**ので、推定をやめる:                             */
/*                                                                          */
/*    arm   : rshell 経由の `serial N` が速度を変えた瞬間 (EOT の前後も      */
/*            成否も問わない)                                               */
/*    解除  : **新速度で `serial ack` という行を受けて実行したときだけ**      */
/*    戻す  : 期限 (SER_SWITCH_WATCHDOG_TICKS) まで ack が来なかったとき、   */
/*            または **arm 中に rshell を抜けるとき**                        */
/*                                                                          */
/*  `serial ack` はホストが投げる専用の合図で、ゲストは `ACK <baud> <mode>`  */
/*  を 1 行返す。**arm されていなくても同じ応答** (冪等) — ホストは何回でも  */
/*  投げてよく、どこかの回で読めればそれが確認になる。                       */
/*                                                                          */
/*  ローカル CUI で打った `serial N` は arm しない。**戻す相手が居ない**     */
/*  ので、勝手に速度が戻ると手元の操作の方が驚く。                           */
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
 * ホスト道具は切替後 **1.5 秒以内**に新速度で `serial ack` を投げはじめ、
 * 0.5 秒ごとに 4 秒まで繰り返す (tools/rshell_serial.py)。つまり ack の
 * 機会は期限内に 8 回ほどある。**短すぎるとホストが開き直す前に戻り**、
 * 長すぎると失敗したまま待たされる。 */
#define SER_SWITCH_WATCHDOG_TICKS 500

#define SER_WD_WAIT   0   /* まだ期限内 — 待つ */
#define SER_WD_LINKED 1   /* ack が来た — 解除 */
#define SER_WD_REVERT 2   /* 期限切れで ack なし — 元の設定へ戻す */

struct serial_watchdog {
    int armed;                    /* 1 = ack 待ち */
    int acked;                    /* 1 = `serial ack` を受けて実行した */
    unsigned long start_tick;     /* 切り替えた tick */
    unsigned long prev_mode;      /* 戻し先のモード */
    unsigned long prev_baud;      /* 戻し先の速度 */
};

/* 切替直後に仕掛ける。acked は 0 に戻る。 */
void serial_watchdog_arm(struct serial_watchdog *w, unsigned long tick,
                         unsigned long prev_mode, unsigned long prev_baud);

/* **`serial ack` の行を受けて実行した**ときに呼ぶ。ここだけが解除の材料。
 * 仕掛かっていなければ何もしない (`serial ack` 自体は冪等に応答する)。 */
void serial_watchdog_ack(struct serial_watchdog *w);

/* いま戻すべきか。SER_WD_WAIT / _LINKED / _REVERT。
 * LINKED か REVERT を返したら番犬は下ろされる (2 度は返らない)。 */
int serial_watchdog_poll(struct serial_watchdog *w, unsigned long tick);

/* rshell を抜けるときに呼ぶ。**arm 中で未確認なら即座に REVERT**
 * (Codex レビュー往復 4 B4: 抜けたあとは poll する者が居ないので、
 * 番犬が仕掛かったまま忘れられて会話が死ぬ)。
 * 戻り SER_WD_REVERT = 戻すこと / SER_WD_WAIT = 何もしなくてよい。 */
int serial_watchdog_leave(struct serial_watchdog *w);

/* 判定そのもの (状態を持たない)。**ack が先**: 期限を過ぎていても ack を
 * 受けていたら LINKED (遅れて届いた ack を「無音」と読み替えない)。 */
int serial_watchdog_decide(unsigned long elapsed_ticks, int acked);

#endif /* __SERIAL_WATCHDOG_H */
