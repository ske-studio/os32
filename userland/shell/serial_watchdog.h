/* ======================================================================== */
/*  SERIAL_WATCHDOG.H — 速度を上げたあと会話が続いているかを見る番犬        */
/*                                                                          */
/*  速度を上げたあと **ホストと足並みが揃ったかはゲストには分からない**。    */
/*  013Ah が効かない機種、ケーブルが速度に耐えない、ホストが開き直しに       */
/*  失敗した — どれでも「こちらは N、ホストは別の速度」になり、戻すための    */
/*  `serial 9600` すら届かない。                                            */
/*                                                                          */
/*  だから **切替後に会話が成立しなければゲストが自力で元へ戻す**。          */
/*                                                                          */
/*  ⚠ **解除の条件は「1 バイト受信」ではない** (Codex レビュー往復 2 B2)。   */
/*  新速度で `ver` は届くが返答の EOT が落ちた場合、受信で解除してしまうと    */
/*  ゲストは新速度のまま、ホストは失敗と見て旧速度へ戻り、**二度と合わない**。 */
/*  速度不一致で化けたバイトやローカルのキー入力でも解除されてしまう。       */
/*                                                                          */
/*  解除は **4 つが揃った行を 1 つ処理したとき**だけ (往復 3 ①②③):          */
/*    (a) 改行 (`\n` / `\r`) で終端している — 化けた 1 バイト + 読み取り     */
/*        空振りの断片や ESC 中断を「行」と数えない                          */
/*    (b) 全バイトがシリアル由来 — ローカルキーが 1 バイトでも混じれば数えない */
/*    (c) 実行した — overflow で断った行は数えない                           */
/*    (d) 応答の EOT を **送り終えた** — `serial_putchar` が予算切れで諦めた  */
/*        ら「応答したつもり」になるので数えない                             */
/*  そして **切替行そのものは数えない** (①)。番犬を仕掛けるのは切替行の EOT  */
/*  を送り終えた後なので、数え始めるのはその次の行から。                     */
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
 * ホスト道具は「`serial N` を送る → 閉じる → N で開き直す → `ver` を投げて
 * 本文まで確かめる」までに数秒かかるので、往復に十分な余裕がある。
 * **短すぎるとホストが開き直す前に戻ってしまい**、長すぎると失敗したまま
 * 待たされる。 */
#define SER_SWITCH_WATCHDOG_TICKS 500

#define SER_WD_WAIT   0   /* まだ期限内 — 待つ */
#define SER_WD_LINKED 1   /* 往復が成立した — 解除 */
#define SER_WD_REVERT 2   /* 期限切れで往復なし — 元の設定へ戻す */

/* 番犬の状態。**数える場所を 1 か所に閉じ込めるため**に構造体で持つ
 * (Codex レビュー B1: 行頭の先読み経路だけ数え損ねていた)。 */
struct serial_watchdog {
    int armed;                    /* 1 = 足並みの確認待ち */
    unsigned long start_tick;     /* 切り替えた tick */
    unsigned long lines;          /* 切替後に **往復し終えた** 行の数 */
    unsigned long prev_mode;      /* 戻し先のモード */
    unsigned long prev_baud;      /* 戻し先の速度 */
};

/* 切替直後に仕掛ける。lines は 0 に戻る。 */
void serial_watchdog_arm(struct serial_watchdog *w, unsigned long tick,
                         unsigned long prev_mode, unsigned long prev_baud);

/* **シリアル由来の有効な 1 行を処理し、その EOT を送り終えた**ときに呼ぶ。
 * ここだけが解除の材料を増やす。仕掛かっていなければ何もしない。 */
void serial_watchdog_line_done(struct serial_watchdog *w);

/* その行を「往復した 1 行」と数えてよいか。上の (a)〜(d) の論理積そのもの。
 * 4 つの真偽の組み合わせ 16 通りをホスト試験で固定してある。 */
int serial_watchdog_line_qualifies(int terminated, int all_serial,
                                   int executed, int eot_sent);

/* いま戻すべきか。SER_WD_WAIT / _LINKED / _REVERT。
 * LINKED か REVERT を返したら番犬は下ろされる (2 度は返らない)。 */
int serial_watchdog_poll(struct serial_watchdog *w, unsigned long tick);

/* 判定そのもの (状態を持たない)。**往復が先**: 期限を過ぎていても 1 行でも
 * 往復し終えていたら LINKED (遅れて成立した往復を「無音」と読み替えない)。 */
int serial_watchdog_decide(unsigned long elapsed_ticks,
                           unsigned long lines_completed);

#endif /* __SERIAL_WATCHDOG_H */
