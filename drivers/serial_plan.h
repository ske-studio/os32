/* ======================================================================== */
/*  SERIAL_PLAN.H — シリアルの「速度をどう出すか」の純粋な判定              */
/*                                                                          */
/*  I/O も tick も触らない決め事だけを drivers/serial.c から切り出してある。 */
/*  実機でしか踏めない分岐 (8253 の整数分周、V･FAST の分周表、TxRDY の      */
/*  待ち予算) をホストで試験するため — **NP21/W は通信速度を模擬しない**    */
/*  ので、ここは「エミュレータで確認済み」が通用しない。                     */
/*                                                                          */
/*    試験: tools/tests/test_serial_vfast.py                                */
/*    記録: tools/tests/serial_vfast_tdd.md                                 */
/*    資料: docs/hw/undocumented/io_rs.md (0130h〜013Ah)                    */
/*    票  : docs/tasks/realhw/TASK_SERIAL_VFAST.md                          */
/* ======================================================================== */

#ifndef __SERIAL_PLAN_H
#define __SERIAL_PLAN_H

#include "serial.h"   /* ポート番地とステータスビット (互換 / FIFO の両方) */

/* ======== 通信モードと初期化の結果 ========
 * **値の正典は共有の契約ヘッダ** `sdk/include/os32/os32_kapi_shared.h`。
 * カーネルとユーザランド (シェルの `serial` コマンド) の両方が同じ値を
 * 見るので、片方に写さない ([C4])。ここは短い別名を作るだけ。
 *
 * **REFUSED は「何もしなかった」** — ハードウェアには 1 バイトも書いていない
 * ので、いまの設定がそのまま生き残る。呼び手は速度を変えていない前提で
 * 続けてよい (Codex レビュー blocker 2)。 */
#include "os32_kapi_shared.h"

#define SER_MODE_COMPAT  KAPI_SER_MODE_COMPAT
#define SER_MODE_VFAST   KAPI_SER_MODE_VFAST

#define SER_INIT_VFAST   KAPI_SER_INIT_VFAST
#define SER_INIT_COMPAT  KAPI_SER_INIT_COMPAT
#define SER_INIT_REFUSED KAPI_SER_INIT_REFUSED


/* ======================================================================== */
/*  TxRDY を待つ予算                                                        */
/*                                                                          */
/*  直す前は「100 回スピン → `hlt` で次の 10ms tick まで寝る」だった。      */
/*  1 文字ごとに最悪 10ms 寝るので、9600 で 490B/s、38400 では 233B/s と     */
/*  **回線より遅くなる** (票 §0)。回線が 1 文字を押し出す時間は baud で     */
/*  決まるのだから、その時間だけ見てから寝ればよい。                        */
/*                                                                          */
/*    8N1 の 1 文字 = start 1 + data 8 + stop 1 = 10 ビット                 */
/*    予算 = 2 文字分 = 2 × 10 × 1e6 / baud [µs]                            */
/*      9600   → 2083µs / 38400 → 520µs / 115200 → 173µs                   */
/*                                                                          */
/*  予算のあいだは `cpu_delay_us(SER_TX_POLL_US)` を挟んで TxRDY を見る。    */
/*  超えたら従来どおり `hlt` (最大 SER_TX_HLT_RETRY 回) — 相手が            */
/*  ハードウェアフロー制御で止めている場合に CPU を焼かないため。           */
/* ======================================================================== */
#define SER_CHAR_BITS         10UL    /* 8N1 の 1 文字 = 10 ビット */
#define SER_TX_BUDGET_CHARS    2UL    /* 予算は 1 文字時間の 2 倍 */
#define SER_US_PER_SEC   1000000UL

/* ポーリングの刻み。短すぎると I/O ポートの読みで CPU を占有し、長すぎると
 * 115200 の予算 (173µs) を数回で使い切る。5µs なら 115200 でも 30 回以上見る。
 * cpu_delay_us の精度は ±10% 程度 (kernel/cpu_calibrate.h) なのでこれ以上
 * 細かくしても意味がない。 */
#define SER_TX_POLL_US        5UL
/* 予算の下限。**0 にしてはいけない** — 1 回も見ないうちに hlt へ落ちると
 * 直した意味が消える。刻みの 8 倍を下限にする。 */
#define SER_TX_BUDGET_MIN_US 40UL
/* 予算の上限。異常に遅い速度を渡されても 1 文字で 50ms は待たない
 * (従来の hlt 1 回分 = 10ms の 5 倍)。 */
#define SER_TX_BUDGET_MAX_US 50000UL
/* 予算を使い切ったあと `hlt` で待つ回数 (従来と同じ)。 */
#define SER_TX_HLT_RETRY      5

/* 8253 カウンタ#2 は 16 ビット。 */
#define SER_COUNT_MAX    0xFFFFU
/* 8251 の ×16 モード (MOD_CLKx16) なので、ボーレートはクロック/16/count。 */
#define SER_CLK_DIVISOR  16UL
/* 速度を渡されなかったときの既定 (include/config.h の SYS_SERIAL_BAUD と
 * 同じ値。ドライバはカーネル設定ヘッダを見ないのでここに持つ)。 */
#define SER_BAUD_DEFAULT 9600UL

/* 決めた結果。**要求どおりに出ないことがあるので呼び手に見せる** ([V4])。 */
struct serial_plan_out {
    int  mode;             /* SER_MODE_COMPAT / SER_MODE_VFAST */
    unsigned long actual;  /* 実際に出る速度 */
    u16  count;            /* 8253 カウンタ#2 (COMPAT のときだけ / VFAST は 0) */
    u8   div;              /* 013Ah bit3-0 (VFAST のときだけ / COMPAT は 0) */
    u8   exact;            /* 1 = 要求どおり出る / 0 = ずれている */
};

/* V･FAST の分周値 (013Ah bit3-0)。表に無い速度は 0。 */
int serial_vfast_div(unsigned long baud);

/* 速度の出し方を決める。
 *   has_fifo   0136h の判定 (serial_fifo_detected)
 *   clk        8253 のタイマクロック (TIMER_CLK_1997 / TIMER_CLK_2458)
 *   want_vfast 呼び手が V･FAST を明示的に頼んだか (票の決裁: 起動時の
 *              既定 9600 は互換のまま。V･FAST は `serial N` で明示的に入る)
 * V･FAST に入るのは **has_fifo かつ want_vfast かつ表にある速度** のときだけ。
 * それ以外は 8253 の整数分周 (割り切れなければ exact = 0)。 */
void serial_plan(unsigned long baud, int has_fifo, unsigned long clk,
                 int want_vfast, struct serial_plan_out *out);

/* TxRDY を待つ予算 [µs]。上の式そのもの。 */
u32 serial_tx_budget_us(unsigned long baud);

/* 0136h を 2 回読んだ値から FIFO 搭載を判定する (資料 304〜323 行)。
 * bit6 が反転し、かつ bit5 がどちらも 0 なら搭載。
 * 未実装ポートの 0xFF も 0x00 固定も、この判定なら非搭載に落ちる。 */
int serial_fifo_detected(u8 first, u8 second);

/* モードに対応するポートとビットマスク。**互換と FIFO でビット位置が違う**
 * ので、選択子を通して取り違えを 1 か所に閉じ込める。 */
unsigned int serial_data_port(int mode);
unsigned int serial_cmd_port(int mode);
u8 serial_txrdy_mask(int mode);
u8 serial_rxrdy_mask(int mode);
u8 serial_err_mask(int mode);

#endif /* __SERIAL_PLAN_H */
