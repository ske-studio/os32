# TDD 記録 — CS4231 (MATE-X PCM) 再生ドライバ

票: [`docs/tasks/v3/TASK_PCM_CS4231.md`](../../docs/tasks/v3/TASK_PCM_CS4231.md) §2-3 / §3 E1
対象: `drivers/pcm_cs4231_math.c` (純粋部) と `drivers/pcm_cs4231.c` (I/O と状態機械)
走らせ方: `make check-pcm-cs4231-host` (= `python3 -B tools/tests/test_pcm_cs4231.py --target --mutate`)

## なぜホストで回すのか

NP21/W では**踏めない分岐**がこの層の中心にある。

| 見るもの | NP21/W で踏めない理由 |
|---|---|
| 1 周回った観測 (同じ半分で位置が戻る) | 実時間では tick が 10ms で回るので作れない。作れるのは「時計と DMA カウンタを手で置く」ホストだけ |
| 補充の余裕 (`REFILL_MARGIN` = 512 frame) を割った切り替え | 位置をピンポイントに置けない |
| drain の 3 段階と「出た + staged > 0 は完了しない」 | 末尾の 1 周ぶんを狙って観測できない |
| 初期化列 / 停止列 / RS の列の**順序** | 出てしまった音からは順序が読めない。I15→I14 の取り違えは NP21/W では症状すら出ない |
| 入口ガードで装置アクセスが **0 回** | 数えるには模型が要る |
| close / reclaim が各状態から **1 度だけ**解放すること | 二重解放はプールの `bad_free` にしか出ず、状態を跨いで撃ち分けられない |
| `dma_chan_setup` / `irq_register` / `kmalloc` の失敗の巻き戻し | エミュレータでは失敗させられない |

実物を**1 行も写さずに** `#include` する。`include/io.h` だけを
`tools/tests/pcm_hostshim/io.h` で差し替えて、ポート I/O と `irq_save` を模型へ回す
(`tools/tests/hostdrv_hostshim/io.h` と同じ作法)。`dma8237.h` / `irq_math.h` /
`pcm_cs4231.h` は**実物をそのまま**使うので、定数がずれればコンパイルで落ちる。

## 模型

- **装置**: 間接レジスタ 32 本 + R0 / R2 + 経路 `0F40h`。読み取り専用 (I11)、
  ID3-0 が固定の I12、**MCE の外では PEN しか書けない I9** まで真似る。
  `dev_init_busy` を立てると R0 の読みが `0x80` になり、コーデックへの書きが無視される
  (`0F40h` はコーデックの外なので INIT とは無関係 — ここを一緒に無視させると
  「巻き戻しで detach しない」の確認が空振りする)。
- **8237**: 残バイト数を `sim_set_pos(frame)` で置く。`-EAGAIN` も返せる。
- **プール / KHEAP**: リングはプール (16KB)、ステージングは `kmalloc` (16KB)。
  解放・leaked・失敗注入を数える。
- **時計**: `tick_count` と µs を手で進める。`sim_tick_per_read` を立てると
  ポートを読むたびに tick が 1 進む (期限切れの経路を有限で終わらせるため)。

## RED → GREEN

| # | 日付 | RED で見えたこと | GREEN にした変更 |
|---|---|---|---|
| 1 | 2026-09-23 | `close_dl`: `pcm_close_ticks(4096, 22050)` の期待を 51 と書いたが実装は 50 | **試験の期待が誤り**。票の式 `ceil((ceil(staged/2048) + 3) × H / 10) + 3` を手で解くと 50。期待値を直した (票の数として明記されているのは staged = 0 の 17 / 31 だけ) |
| 2 | 2026-09-23 | `seq`: 「`PCM_IFACE_CAL1 \| PCM_IFACE_PEN` を `a` に持つ op は無い」の検査が落ちる | **試験が誤り**。`PCM_IFACE_CAL1 \| PCM_IFACE_PEN` == `0x09` == `PCM_I_IFACE` で、たまたま同じ値だった。意味のない検査なので消し、末尾 2 op を `kind/a/b` で直に照合する形にした |
| 3 | 2026-09-23 | `open`: `dev_init_busy` のとき「経路 `0x1A` が残る」が落ちる | **模型が誤り**。INIT 中の書き無視をポート全部に掛けていた。`0F40h` は C バスの結線でコーデックの外 (DS139PP2 の INIT はコーデックの R0-R3 の話) なので、経路だけ先に通すよう直した |
| 4 | 2026-09-23 | `open` が終わらない (無限ループ) | `pcm_poll` は `tick_count` の差で期限を測るので、模型で時計が止まっていると期限が来ない。`sim_tick_per_read` を足し、期限切れの経路でだけ立てる |

実装側は RED を 1 件も出さなかった (設計 v10 が 9 往復ぶん詰まっていたため)。
**試験の側の誤りが 3 件、模型の不足が 1 件** — 記録としてはそちらが本体。

## 変異 (否定側)

`--mutate` は `drivers/pcm_cs4231_math.c` を**写しの上で 1 か所だけ**壊し、
どれか 1 つのケースが RED になることを見る。24 本。票 §2-3 が名指しする順序の変異 5 つ
(I14 を I15 より先 / MCE 無しの I9 / MODE2 前の I24 / DRS を待たない mask /
期限を毎 tick 入れ直す) はすべて RED。

**変異にできなかった 1 つ**: 完了条件の `c->stg.staged == 0U`。
`frames > 0` のときに `last_data_half` を置き直して段階を `WAIT` へ戻す規則
(変異 11) と重なっており、`pcm_obs` の中だけを見れば到達できない
(残りがあれば必ず補充され、段階が戻る)。**票どおり両方を残した上で**、
変異は観測できる側 —「完了の遷移は DRAINING のときだけ」— に置き換えた。
`staged == 0` は防御であって、単独で試験できる条件ではないと記しておく。

## ケース一覧

| ケース | 見るもの |
|---|---|
| `cont` | 連続性の判定表 (h0 × h1 × p の全組)。1 → 0 の折り返しが正常、同じ半分で戻れば喪失。**2 半周期以上の空白は保証外**であることも期待に書いてある |
| `refill` | `REFILL_MARGIN` の境目 (ちょうど 512 は通る、511 は通らない) |
| `drain` | drain の 3 段階の遷移 |
| `drain_short` | `write(1 frame) → close` の丸ごと 1 周 |
| `start` | `pcm_start` の配り方と drain の初期段階 (2048 未満は「読んでいる」から) |
| `rate` | レート → I8 (`0x5B` / `0x57`)、半周期、番犬の期間 |
| `close_dl` | close の期限式 (staged と rate) |
| `vol` | percent → 減衰とミュート、101 以上の拒否 |
| `stg` | ステージングの予約 / 公開 / 消費、物理末尾の 2 分割、**2047 + 2 + 消費 + 4096** の反例 |
| `pack` | カウンタ 3 本の詰め方と飽和 |
| `pos` | 残バイト数 → リング全体の frame 番号 |
| `obs` | 1 回の観測の統合 (切り替え・underrun の順・喪失・`-EAGAIN`・番犬・drain・完了) |
| `seq` | 初期化列 / start / 停止の入口 / 後続 tick / RS_RESTART / abort の**順序** |
| `open` | 検出・巻き戻し (NOSYS / NOMEM / BUSY / IO)、`irq_save` の釣り合い |
| `write` | frame 倍数の切り捨て、空き、満杯からの自動開始、owner 照合 |
| `close` | 未転送なしの即時解放、drain の 1 周、DRS 待ち、期限切れ → FAULTED → leaked |
| `rs` | 喪失 → RS_STOP → RS_RESTART → RUNNING、RS の期限切れ → STOP_REQ |
| `guard` | 入口ガードで装置アクセス **0 回**、PI の ack と `IRQ_HANDLED` |
| `reclaim` | 全状態からの回収が **1 度だけ**、他人の ID では動かない、FAULTED は leaked |
| `volume` | I6/I7 の両方に入ること、owner 照合 |
| `init` | 起動時の検出。装置が無くても CLOSED のまま、tick が装置を触らない |

## ここで確かめていないこと ([V4])

- 実際に音が出ること、PI の累積回数、左右の取り違え → 票 E2〜E5 (NP21/W、PM)
- INIT 中の書き無視・校正時間・XTAL2 の有無・auto-init ビット → 票 E6 (実機)
- `dma_chan_remaining` の合成そのもの → `tools/tests/test_dma8237.py`
