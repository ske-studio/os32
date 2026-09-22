# serial_vfast — RED → GREEN の記録

票: [`docs/tasks/realhw/TASK_SERIAL_VFAST.md`](../../docs/tasks/realhw/TASK_SERIAL_VFAST.md)
対象: `drivers/serial_plan.c` (速度の決め事) / `userland/shell/serial_watchdog.c`
      (切替後の番犬) / `drivers/serial.c` `userland/shell/rshell.c` (I/O と配線)
試験: `tools/tests/test_serial_vfast.py` + `tools/tests/serial_vfast_host.c`
実行: `make check-serial-vfast-host` (`check-par` の列)

## なぜホストで試験するのか

**NP21/W は通信速度を模擬しない** (`np21w-src/src/io/serial.c`: `013Ah` の速度表は
持つが、`commng` に速度を伝えるだけでビットの流れる時間は再現しない)。だから
「エミュレータで確認済み」がここでは通用しない — 分周表を 1 行ずらしても、
`0132h` の RxRDY を `0032h` のビット位置で見ても、NP21/W では動いてしまう。

実機に出す前に押さえられるのは「決め事」だけなので、決め事を
`drivers/serial_plan.c` に切り出してそこを試験する。I/O を伴う部分
(`0138h` / `013Ah` を実際に叩く順番、IRQ ハンドラの汲み出し) は
**実機での確認が要る** (票 §3 の S5 / S6)。ここで緑でも実機合格ではない ([V4])。

## RED

1. ハーネス (`serial_vfast_host.c`) と `test_serial_vfast.py` を先に書いた。
   `drivers/serial_plan.c` が無いのでコンパイルが通らない = RED。

   ```
   serial_vfast_host.c:18:10: fatal error: ../../drivers/serial_plan.c:
   No such file or directory
   ```

2. 実装後の否定側 (`--mutate`)。実物を 1 か所だけ壊して、どれも RED になることを見る。
   **変異は写しの上で行う** (一時ディレクトリの `drivers/serial_plan.c` を先に
   引かせる) ので、実物のソースは 1 バイトも動かない = `check-par` で並列に回せる。

   | # | 壊し方 | 結果 |
   |---|---|---|
   | 1 | V･FAST の表を 1 行ずらす (115200 のつもりで 57600 が出る) | RED (3 件) |
   | 2 | 表に無い速度 (4800) を足す = 9600 の戻しが V･FAST に入らない | RED (2 件) |
   | 3 | FIFO の RxRDY を互換のビット位置 (bit1) で見る | RED (1 件) |
   | 4 | FIFO の TxRDY を TxEMP (bit0) と取り違える | RED (1 件) |
   | 5 | 予算の下限を外す (速い速度で 0µs → 直す前の `hlt` 待ちに戻る) | RED (1 件) |
   | 6 | `want_vfast && has_fifo` を `||` にする (明示指定なしでも V･FAST に入る) | RED (2 件) |
   | 7 | 割り切れない分周を「ちょうど出る」と答える (38400 → 41600 を見逃す) | RED (4 件) |
   | 8 | 番犬が期限を受信より先に見る (期限ちょうどに届いた応答を無音と読み替える) | RED (1 件) |
   | 9 | 番犬の期限を 1000 倍にする (事実上いつまでも戻さない) | RED (1 件) |

## GREEN

```
HOST GNU89 -Werror compile PASS (real drivers/serial_plan.c)
TARGET i386-elf GNU89 -Werror PASS      (drivers/serial_plan.c + drivers/serial.c)
SUMMARY 10/10 PASS
MUTATION 1..9 すべて RED (どれも実行時。コンパイルエラーに逃げていない)
```

## 何を見ているか

| ケース | 見るもの | 根拠 |
|---|---|---|
| `vfast_table` | 速度 → `013Ah` bit3-0 の分周。表に無い速度は **0** | `io_rs.md` 352〜380 行 / NP21/W `speedtbl` |
| `compat_exact` | 2.4576MHz なら 9600=16 / 19200=8 / **38400=count 4 ちょうど** | 実機 Ra266 (TASK_FDC_REALHW §9-1) |
| `compat_inexact` | **1.9968MHz の 38400 は count 3 → 実効 41600 (+8.3%)**、`exact=0` | `drivers/serial.h` の表 |
| `mode_choice` | V･FAST に入るのは **FIFO 搭載 × 明示指定 × 表にある速度** の 3 つ揃ったときだけ | 票 §2 の決裁 |
| `tx_budget` | 予算 = 2 × 10 ビット ÷ baud (9600→2083µs / 115200→173µs)、**0 にしない** | 票 §2 |
| `status_bits` | 互換 `0032h` は bit0 TxRDY / bit1 RxRDY、FIFO `0132h` は bit1 TxRDY / bit2 RxRDY | `io_rs.md` 0132h / NP21/W `rs232c_i132` |
| `fifo_detect` | `0136h` を 2 回読んで bit6 が反転 + bit5 が 0。**0xFF も 0x00 も非搭載へ落ちる** | `io_rs.md` 304〜323 行 |
| `real_hw_story` | 9600 起動 → `serial 115200` (`013Ah` に 0x81) → `serial 9600` で戻す | 票 §3 |
| `refuse_inexact` | **出せない速度を適用しない**: 1.9968MHz の 38400 は拒否、2.4576MHz の 38400 は count 4 で適用、FIFO 無しの 115200 は**どちらのクロックでも**拒否。起動時の 9600 は両クロックで exact = 拒否の分岐を通らない | Codex レビュー blocker 2a |
| `watchdog` | 切替後 500 tick 無音なら REVERT、1 バイトでも来れば LINKED、**受信は期限より先に見る** | Codex レビュー blocker 2b |

## 資料と NP21/W が食い違ったところ

| 箇所 | 資料 (`io_rs.md`) | NP21/W | 採ったもの |
|---|---|---|---|
| `0132h` bit3〜7 | bit5 パリティ / bit4 オーバーラン / bit7 ブレーク / bit3 不明 | bit3 パリティ / bit4 オーバーラン / bit5 フレーミング / bit6 ブレーク (「Vol.2 の記載は誤り」と明記) | **どちらでも共通に立つ bit3〜5 (0x38) をエラーとして見る**。エラーの種別で分岐していないので、これで両方の解釈に耐える |
| `0132h` への書き込み | 記述なし ([READ] のみ) | `0x132` の out を `0032h` と同じハンドラ `rs232c_o32` に繋ぐ (`rs232c_bind`) | **NP21/W に従う**。資料 `0138h` の関連欄も `0030h` / `0032h` を挙げている。実機で確かめる必要がある (票 S5) |
| FIFO モードの割り込みマスク | 記述なし (`0136h` は「参照」で許可/禁止ではない) | `0035h` 以外でマスクしない | **両モードとも `0035h`** を使う |
| `0136h` の読み | 「割り込み要因の取得」 | 読むと `irqflag` を畳んで `pic_resetirq(4)` | **FIFO モードの IRQ ハンドラの最後に 1 回読む**。読まないと同じ要因で割り込みが上がり続ける |
| `013Ah` の速度表 | 7 段 (9600〜115200) | `speedtbl` が完全に一致 | 食い違いなし |

## Codex レビュー (4600a9d、Request changes) の 3 件をどう閉じたか

| # | 指摘 | 閉じ方 | ホストで見ているか |
|---|---|---|---|
| 1 (P1) | 切替中の IRQ4 とリングの競合 (`ser_head=0; ser_tail=0` の直後に ISR が書くと 1 バイト残る / ポート・マスクの逐次差し替え中に ISR が新ポートを古いビットで読む) | `serial_init_ex` の切替全体を `irq_save()` + `irq_disable(4)` → … → `irq_enable(4)` + `irq_restore()` で囲んだ。ISR 末尾の `0035h` 再許可は直値をやめて `s_mask_ien` (初期化が決めた値) を書く。**`kprintf` は危険区間の外へ出した** — rshell 中は kprintf がシリアルへ流れ、`serial_putchar` の `_halt()` が IF=0 で永久に止まるため | いいえ (I/O と割り込みの順序。実機・エミュレータでしか見えない) |
| 2 (P1) | `--fast` の失敗で戻れなくなる | 3 段: (a) ドライバが exact でない 8253 速度を **適用しない** (`SER_INIT_REFUSED`、ハードウェアには 1 バイトも書かない)、(b) ゲストの番犬が切替後 500 tick 無音なら元へ戻して EOT を返す、(c) ホスト道具が `ver` の往復で確かめ、失敗なら元の速度へ戻って再確認し終了コード 1 | (a) `refuse_inexact` / (b) `watchdog` で見ている。(c) は実機 |
| 3 (P2) | `--fast` の受動待ちで成功時も 15 秒止まる | `sync` の受動待ちをやめ、`ver` の往復 (5 秒) に置き換えた。成功なら 1 秒で返る。EOT の取りこぼしに依存しない | いいえ (ホスト道具。実機で計る) |

## まだ試験していないこと

- **`0138h` / `013Ah` を実際に叩く順番**と、そのあと 8251 のモード/コマンドが
  `0132h` で効くこと。NP21/W では速度が変わらないので確かめられない (票 S3 / S5)。
- **IRQ ハンドラの FIFO 汲み出し** (16 バイト / `0136h` の ack)。
  **16 バイトで抜けたあと残量があるときに再割り込みが来ることは資料で証明
  できない** (`0136h` は「割り込み要因の取得」としか書かれておらず、
  FIFO の残量と割り込みの関係は「詳細不明」)。NP21/W は受信リングの使用量が
  3/4 を超えたときに `irqflag = 2` を立てる実装だが、実機がそうとは限らない。
  実機で大きなファイルを `hexdump` して取りこぼしが無いかを見る (Codex
  レビューの非 blocker 指摘 1)。
- **`0132h` のエラービット `SER_FSTS_ERR` = 0x38 の bit3**。資料は bit3 を
  「不明」とし、NP21/W は「パリティ」とする。どちらでも「エラーが立ったら
  コマンドを打ち直す」だけなので害は出にくいが、**資料で証明できていない**
  (Codex レビューの非 blocker 指摘 2)。
- **`0132h` へのコマンド書き込み**。資料は `[READ]` としか書かず、根拠は
  NP21/W の `rs232c_bind` (`0x132` の out を `rs232c_o32` に繋ぐ) だけ。
  実機で 8251 のモード/コマンドが `0132h` で効くかは確かめていない
  (Codex レビューの非 blocker 指摘 3)。
- **`cpu_delay_us` を挟んだ送信ループの実効速度**。目標は 9600 で 900B/s 以上、
  115200 で 8KB/s 以上 (票 S5) — 実機でしか測れない。
- **切替区間の排他そのもの** (`irq_save` / `irq_disable(4)` / `s_mask_ien`)。
  競合は「ISR が特定の 2 命令の間に入る」ときだけ起きるので、ホストでは作れない。
  見るなら実機で `serial 115200` → `serial 9600` を繰り返して 1 バイトのずれが
  出ないことを確かめる (票 S5 / S6)。
- **番犬が実際に戻すところ** (`ser_wd_poll` → `serial_init`)。判定は
  `watchdog` で押さえたが、rshell のループへの配線と EOT の返しは実機。
  **FIFO 非搭載機を用意できないので、`serial 115200` が拒否される経路も実機待ち。**
- **ホスト道具の `ver` 往復と失敗時の戻り** (`switch_speed`)。pyserial と
  実際の COM ポートが要る。
