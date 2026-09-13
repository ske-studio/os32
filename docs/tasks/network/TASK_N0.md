# N0 — Host Services の KAPI v51 (ABI 確定) と非ブロッキング化の設計

状態: **設計 第 2 版 (往復 1 の blocker 12 件を反映: ワイヤを v2 (rid 付き 20B ヘッダ、宣言長、rid ごとの WDATA/ACK、epoch の生成) に改訂し、駆動・排他・配送の直列化を契約に。往復 2/3 待ち)**。前提: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) (§2 サービス一覧、§3 KAPI 案、§7 票、§9 の決裁は**推奨案で進める**: 印刷 v1 は to-file 既定、Agent は WSL2、LGY-98 は N3 受入後に既定へ、KAPI は v51、CLIP は含め PUT は後回し、HTML は text)、[LINK_PLAN.md](LINK_PLAN.md) (L0〜L3 の契約: EtherType 0x88B5、16B ヘッダ、Stop-and-Wait の REQUEST/RESPONSE、L1 絶対値 WINDOW、L2 8KB ストリーム Go-Back-N)、`net/link.{c,h}` (現状は同期版 `link_request` / `link_service_get`、`link_stream_read`)、`docs/KAPI_SPEC.md` §3-1 (追加手順) / §3-2 (予約表)、T9 §1a (ABI 表の書式)、S0-K (v50: CPL=3 ポインタの範囲検証 `ring3_user_range_ok`、owner 回収の位置)。
規約: [ABI1〜3] (kapi.json が正典、末尾追記、版を上げて `make clean`)、[C1] C89、[C2] kstr*、[C4]。

## 0. 範囲

N0 は**設計だけ** (コードは書かない): (a) KAPI_SPEC §3-2 の予約を v43 (欠番) → **v51** に改訂 (済)、(b) **リンク層ワイヤ v2** (§1b) — 往復 1 で「要求と本文の対応付け」「epoch」「WDATA の ACK」「本文の終端」「TIME の形式」がワイヤ契約の欠落だと分かったので、LINK_PLAN §4 の形式を v2 に改訂する、(c) v51 の 5 本の ABI (§1a)、(d) `net/link.c` の非ブロッキング化 = **駆動・排他・状態機械** (§2)、(e) Host Agent 側の対応 (§2c、N1 の一部として `host_agent.py` の v2 対応を含める — N2 はサービス追加だけに)、(f) N1 のホスト TDD と受入 (§3)。実装は N1 (K + ホスト)。

## 1. KAPI_SPEC §3-2 の改訂 (済)

v43 = 欠番、v51 = `host_open` / `host_status` / `host_read` / `host_write` / `host_close` (slot 208〜212、data_fields 0x35C / 0x360)。全体 clean rebuild (SDK・shlib・apps / game・配備先の旧バイナリも作り直す = F0 の方針。旧 v50 の CPL=3 バイナリは data_fields の旧位置から関数スタブの番地を読むので互換しない)。エラー番号は固有を取らない。

## 1b. リンク層ワイヤ v2 (LINK_PLAN §4 の改訂、N0 で行う)

ヘッダを 16B → **20B**: `op u8`, `flags u8`, `epoch u16`, `seq u32`, `ack u32`, `length u16`, **`rid u32`** (要求 ID)、`agent u16` (HELLO だけ意味を持つ Agent の世代、他は 0)。EtherType 0x88B5 は不変。op 1〜7 は既存、**8 = WDATA** を追加。

| op | 向き | seq / ack / rid の意味 | 備考 |
|---|---|---|---|
| HELLO (1) | 双方 | `epoch` = OS32 が `link_hello` ごとに生成 (前回 + 1、初回は `tick_count` の下位 15bit + 1、0 は使わない)、`agent` = Agent が起動時に乱数で決めた世代 | Agent は OS32 の epoch を採用して返す。OS32 は返答の `agent` を控える |
| REQUEST (2) | OS32→Host | `rid` = ハンドルごとに OS32 が採番 (epoch 内で単調増加、0 は使わない)、`seq` = 転送 seq | 本文が要る要求は要求行に**宣言長**を持つ (`CLIP PUT <len>`、`PRINT DATA <job> <len>`、`PUT /file/<p> <len>`) |
| ACK (6) | 双方 | Host→OS32: `rid` + `ack` = 受け取った REQUEST / WDATA の seq (累積)。OS32→Host: `rid` + `ack` = 受け取った DATA の seq (累積、既存) | **ACK は「転送の受領」だけ**を意味し、サービスの完了は RESPONSE |
| RESPONSE (3) | Host→OS32 | `rid`、本文は `status u16` + `length u32` の 6B **固定** (TIME / PING も同じ形にする = 往復 1 の B10) | 同じ `rid` の REQUEST 再送には**再実行せず**同じ RESPONSE を再提示 (Agent は epoch 内の rid → 結果を保持、上限 8) |
| DATA (4) / EOF (5) | Host→OS32 | `rid`、`seq` = そのストリーム内 1〜 | **OS32 が WINDOW (rid 付き、credit > 0) を送ったストリームだけ**流す (§2b の直列化) |
| WINDOW (7) | OS32→Host | `rid`、`ack` = 順序どおり受けた最終 seq、payload = credit_pages | ストリームの開始許可を兼ねる |
| WDATA (8) | OS32→Host | `rid`、`seq` = その要求の本文内 1〜 | Host は (epoch, rid, seq) で重複排除して追記、ACK (rid, seq 累積) を返す |

- **epoch / agent**: OS32 は `epoch` と `agent` が控えと違うフレームを捨てる (HELLO 以外)。Agent は `epoch` が違う非 HELLO フレームを捨て、HELLO には常に応答。**再同期の契機**: (i) REQUEST の転送 ACK が `LINK_TRIES` 回来ない、(ii) HELLO 応答の `agent` が控えと違う (Agent 再起動)、(iii) 起動時。再同期 = 新 epoch で HELLO → 生存ハンドル (FREE 以外) を STALE、TX / リング / 配送状態を破棄。周回 (epoch 0x7FFF → 1) は 15bit なので実用上起きないが、OS32 側は「違う = 捨てる」だけなので周回でも壊れない。

## 1a. KAPI v51 の ABI (N1 の K が実装、N3 の C / N4 の W が従う)

共通: CPL=3 のポインタ / 長さは v50 と同じ (`ring3_user_range_ok` = 帯 + 長さ。帯外は `INVAL`、帯内の非 present はディスパッチャの規則で kill)。出力ポインタは NULL 可 (書かない)。失敗時は出力を書かない。**どの呼び出しも待たない** (応答・リンク・TX 空きを待つのは呼び手の `sys_yield` / タイマ)。NIC が無い / リンク層未初期化 → `NOSYS`。h が範囲外 / FREE / 他人 → `INVAL`。

| slot | 名前 | 引数 | 戻り | 規則 |
|---|---|---|---|---|
| 208 | `host_open` | `const char *req, u32 len` | h (0 / 1) / 負 | 要求行 1〜1400B (超過 / 0 → `INVAL`)。**要求行はカーネル所有のハンドル領域へ写す** (再送のため。呼び手は戻った後 req を解放してよい)。HELLO 未確立 / 再同期中 → `STALE`。空きハンドル無し → `FULL`。TX スロット (§2a) 塞がり → `AGAIN` (ハンドルは割り当てない)。成功で SENT (REQUEST を 1 回送る。再送は §2)。所有者 = `res_owner_get()` |
| 209 | `host_status` | `i32 h, u32 *status, u32 *length` | 0 / `AGAIN` / 負 | RESPONSE 未着 → `AGAIN`。着いていれば `status` / `length` (本文の宣言長)。STALE → `STALE` (close まで) |
| 210 | `host_read` | `i32 h, void *buf, u32 cap` | 長さ (> 0) / 0 = 完了 / `AGAIN` / 負 | `cap == 0` / `buf == NULL` → `INVAL`。RESPONSE 未着 → `AGAIN`。`length == 0` → 0。**リングの所有者でなければ** (他方が所有中) → `AGAIN`。所有者になったら (§2b) リングから最大 cap を写し、消費した分だけ credit が回復 (WINDOW は `link_tick` が送る)。**完了 = 受信バイト数 == 宣言長 かつ 全部読んだ**とき 0 (EOF フレームは補助。往復 1 の B5)。データ未着 → `AGAIN` |
| 211 | `host_write` | `i32 h, const void *buf, u32 len` | 受け付けた長さ (≤ len、≤ 1400) / `AGAIN` / 負 | 要求行に宣言長を持つ要求だけ (無い要求 / 宣言長を超える → `INVAL`)。**未 ACK の WDATA が 1 本ある間は `AGAIN`** (Stop-and-Wait、ハンドルごと)。カーネルは送った WDATA の写し (1400B) を持ち、ACK が来なければ再送 (§2)。TX 塞がり → `AGAIN`。RESPONSE 着後 → `INVAL` |
| 212 | `host_close` | `i32 h` | 0 / 負 | 任意の状態から FREE。リングの所有者なら未読分を捨てて所有権を放す。ホストへは何も送らない (以後届く同じ rid のフレームは `rid` が生存ハンドルに無いので `link_tick` が捨てる = 往復 1 の B3 の discard 印は不要)。二重 close → `INVAL` |

- **owner 回収**: `exec_reclaim_owned(x)` の中 (con_sink / launch と同じ位置) で `host_owner_exit(x)` = 所有者が x のハンドルを内部解放 (公開 `host_close` の現在 owner 検査は使わない、往復 1 non-blocker)。CPL=0 の利用者 (常駐シェルは owner 1、CPL=0 外部アプリも owner を持つ) も同じ。
- 同期版 `link_service_get()` / `link_request()` は自己試験用に**状態機械の上に**書き直す (`SENT` にして `link_tick` を回す)。

## 2. `net/link.c` の非ブロッキング化 (N1 の設計)

### 2a. 駆動と排他 (往復 1 の B1 / B2)
- **駆動元**: `kernel/isr_handlers.c` の 100Hz タイマから `link_tick()` を呼ぶ (`ne2k_timer_tick` の直後)。IRQ5 (`ne2k_irq`) はフレームをドライバのソフトウェアキューに積むだけ (現行)。`link_tick` が (1) 受信キューを空になるまで dispatch、(2) 再送タイマ、(3) WINDOW の送出、(4) HELLO / 再同期、を **1 周回 = 有限の予算** (受信 dispatch は最大 16 フレーム、送信は TX 空きの範囲) で行う。KAPI からは `link_tick` も `link_poll` も呼ばない。起動時の HELLO は `link_init` の後に `link_tick` が自動で始める (自己試験に依存しない)。
- **排他**: リンク層の状態 (ハンドル表、リング、TX スロット) の更新者は **`link_tick` (タイマ文脈) だけ**。KAPI は `cli` / `sti` (IF の保存・復元) の短い区間で状態を**スナップショット / 更新**する: `host_read` はリングからカーネルの中継バッファ (ハンドルごと 1400B) へ cli 区間で写してから、IF=1 でユーザー領域へ写す (ユーザー領域への写しで #PF が起きてもロックや途中状態を残さない)。`host_open` / `host_write` は要求 / WDATA をカーネル領域に写してから cli 区間で TX スロットに積む (送信そのものは `link_tick`)。IRQ 内で待つロックは無い。
- **TX スロット**: 送信バッファは 1 本 (`link_txbuf`) だが、**送信要求のキュー (4 本: REQUEST ×2、WDATA ×2、WINDOW / ACK は優先して直接送る)** を持ち、`link_tick` が NIC の TX 空きを見て順に送る。KAPI は「キューに空きが無い」ときだけ `AGAIN`。

### 2b. 配送の直列化 (往復 1 の B4 / B6)
- ハンドル 2 本、リング 1 本。**リングの所有権**は「RESPONSE が着いて `length > 0` のハンドルのうち、OS32 が最初に WINDOW を送ったもの」= `host_read` が最初に呼ばれた側 (所有権は `host_read` の cli 区間で取り、`link_tick` がその rid の WINDOW を送る)。Agent は **WINDOW を受けた rid だけ**ストリームを流す (RESPONSE は要求ごとに即返す = 状態は先に分かる)。所有権は **close まで** (完了しても未読が残る間は放さない)。他方は `host_read` で `AGAIN`。
- Agent 側: 受付済み要求 (RESPONSE 済み、本文待ち) を rid ごとに保持 (上限 2 + 再提示用の結果 8)、WINDOW が来たら配送開始、close 相当は無い (rid が新しい要求で置き換わる / epoch 変化で全部捨てる)。**キュー待ちは REQUEST の再送回数に数えない** (転送 ACK は即返るので再送は起きない)。

### 2c. 状態機械
ハンドル: `state` (FREE / SENT / RESP / DONE / STALE)、`owner`、`rid`、`req_copy[1400]` + `req_len`、`tx_seq`、`status`、`length`、`recv_bytes`、`read_bytes`、`ring_owner` (bool)、`wseq` (次の WDATA seq)、`wcopy[1400]` + `wlen` + `wacked`、`last_tx_tick`、`retries`。

| 事象 (link_tick / KAPI) | 遷移 / 動作 |
|---|---|
| `host_open` | FREE → SENT (要求を写し、TX キューへ) |
| 転送 ACK (rid、ack ≥ tx_seq) | REQUEST の再送を止める (状態は SENT のまま = サービス処理待ち。**サービスの完了は待ち切らない**、往復 1 の B7: HTTP は 10 秒かかる) |
| 転送 ACK が `LINK_RTO` 内に来ない | REQUEST 再送 (`req_copy`、同じ rid / seq)。`LINK_TRIES` 超 → 再同期 (§1b) → STALE |
| RESPONSE (rid 一致) | SENT → RESP (`status` / `length`)。`length == 0` → DONE。重複 RESPONSE は無視 |
| WINDOW 送出 | `ring_owner` かつ受信残 (`recv_bytes < length`) のとき、credit = 既存式 (リング空き) で `link_tick` が送る (初回 = 開始許可) |
| DATA (rid = ring_owner の rid、seq 順次) | リングへ push、`recv_bytes += n`、ACK (累積) を返す。順序外 (gap) は捨てて再 ACK (Go-Back-N、既存)。`ring_owner` でない rid / 生存ハンドルに無い rid → 捨てる |
| EOF (rid 一致) | 補助印。完了判定は `recv_bytes == length` (欠落中なら Go-Back-N で回復するまで未完了) |
| `host_read` | RESP で ring_owner でなければ取得を試みる (他方が所有中なら AGAIN)。リングから写す。`read_bytes == length` で 0 (DONE) |
| `host_write` | SENT で `wacked` なら WDATA を写して TX キューへ (`wacked = 0`)。未 ACK → AGAIN。ACK (rid, seq) で `wacked = 1`。RTO で再送 (同 seq、Host は重複排除)、TRIES 超 → 再同期 |
| Agent が本文を受け切る (宣言長) | RESPONSE を返す → RESP / DONE |
| 再同期 (epoch 変化) | FREE 以外 → STALE、リングと TX キューを破棄、`ring_owner` を放す |
| `host_close` / `host_owner_exit` | 任意 → FREE (`ring_owner` なら放す) |

### 2d. Agent 側 (N1 に含める `host_agent.py` の v2 対応)
20B ヘッダ、`agent` 世代の乱数、rid ごとの結果保持 (再提示)、WINDOW 待ちのストリーム開始、WDATA の重複排除と累積 ACK、宣言長で本文完了 → RESPONSE、TIME / PING を標準ヘッダに。サービス (PRINT / CLIP / PUT) の追加は N2。既存の `check-net-l0`〜`l3` はゲスト観測用として v2 で回帰させる (ホストハーネスの合否とは分けて記録)。

## 3. N1 のホスト TDD と受入 (往復 1 の B12)

- `tools/tests/net_link_host.c`: `net/link.c` を `#include`、NIC (キュー / TX / tick) と `cli` / `sti` を贋物に。**KAPI から poll を呼ばず、贋のタイマ入口 `link_tick` だけで進む**ことを固定 (KAPI 呼び出しの間に tick を挟まなければ状態が進まないケース)。**割込み挿入**: リング更新 / TX 構築の途中で贋 IRQ が DATA / WINDOW を積む → 消失・破壊が無い。**実 Agent のワイヤ形式** (Python の `host_agent.py` をサブプロセスで起動し、贋 NIC の両端を UNIX ソケットで結ぶ) で: 2 要求 (A の本文配送中に B を open → B は RESPONSE 済み・本文待ち、A の close 後に B が流れる)、連続 close → 再 open の遅延フレーム破棄、ACK 消失 (REQUEST / WDATA の再送と Agent の重複排除)、EOF 消失 (宣言長で完了)、最終 DATA 消失 (Go-Back-N で回復するまで未完了)、Agent 再起動 (agent 世代の変化 → 再同期 → STALE)、HTTP 相当の遅い応答 (転送 ACK は即、RESPONSE は後) で再送が起きない。owner 回収: 親へ戻った状態で x のハンドルだけ解放され他 owner は残る。ポインタ検査は link.c 単体に加えて実 KAPI ラッパー (`kapi/kapi_host.c`) を通す (`paging_bounds_host` の作法)。
- ゲスト (`kernel-lgy98-link`、WSL2 の `host_agent.py` v2): `userland/tests/host_test.c` (CPL=3) で `GET /pattern/65536` の内容一致 (AGAIN ループ、`sys_yield`)、`GET /notfound` = 404、`TIME` (標準ヘッダ)、`CLIP PUT 5` + write + RESPONSE (N2 の CLIP は無いので Agent の echo サービス `ECHO <len>` を N1 に足して往復)、GUI 配下で `gui_busy` と同時に走らせて WM が止まらない、CTRL+STOP 後に次の open が通る (owner 回収)、Agent を再起動して STALE → close → open が通る。
- 配備: `kernel-lgy98-link` (既定ビルドではない)。`make check` で既定カーネルの回帰も担保。

## 4. レビューで見てほしい点

1. §1b のワイヤ v2 (20B ヘッダ、rid、epoch / agent、WDATA / ACK の意味、宣言長、TIME の標準化) が往復 1 の B3 / B5 / B7 / B8 / B9 / B10 / B11 を閉じるか。
2. §2a の駆動 (`link_tick` だけが更新者、KAPI は cli 区間のスナップショット、中継バッファ経由のユーザーコピー) が B1 / B2 を閉じ、IRQ 内で待たないか。
3. §2b の直列化 (WINDOW = 開始許可、所有権は close まで) が B4 / B6 を閉じ、PRINT OPEN → DATA → CLOSE / CLIP GET / CLIP PUT の呼び出し列が §1a で書けるか (§5 に例)。
4. 再送の分離 (転送 ACK と RESPONSE) と再同期の契機、STALE の範囲 (生存ハンドルだけ)。
5. §3 の試験が B12 の要求 (タイマ入口だけで進む、割込み挿入、実 Agent、owner) を満たすか。
6. LINK_PLAN §4 の改訂範囲 (既存 L0〜L3 の試験の書き換え量) と、HOST_SERVICES_PLAN §2 / §3 の同期。

## 5. 呼び出し列の例 (N3 が従う)

```
GET:      h = host_open("GET http://x/", n) ; loop { rc = host_status(h,&st,&len); AGAIN → sys_yield }
          ; loop { n = host_read(h,buf,4096); AGAIN → sys_yield; 0 → break; >0 → write } ; host_close(h)
CLIP PUT: h = host_open("CLIP PUT 5", 10) ; loop { n = host_write(h,"hello",5); AGAIN → sys_yield }
          ; loop { host_status(h,&st,&len) ; AGAIN → sys_yield } ; host_close(h)        (len == 0)
PRINT:    h1 = open("PRINT OPEN rep text") → status/read で "job 7" → close(h1)
          h2 = open("PRINT DATA 7 4096") → write ×3 (1400/1400/1296) → status (200) → close(h2)   (繰り返し)
          h3 = open("PRINT CLOSE 7") → status/read "pages 1" → close(h3)
```

## 6. ユーザー判断が要る点

- HOST_SERVICES_PLAN §9 の 6 項目は推奨案で進める。異論があれば N1 着手前に。
- ワイヤ v2 は L0〜L3 の既存試験 (エミュレータ合格済み) を書き換える。合格実績を一度捨てて v2 で取り直すことの承認 (N1 の受入に含める)。

## 7. レビュー記録

| 版 | 判定 | 要旨 |
|---|---|---|
| 第 1 版 | Request changes | 12 件: B1 非同期の駆動元が無い、B2 KAPI と IRQ の排他が無い、B3 要求と DATA/EOF の対応付けが無く discard 印も不成立、B4 Agent の逐次処理はストリームの直列配送を保証しない、B5 EOF だけでは完了を確定できない、B6 EOF とリング解放の混同、B7 REQUEST 再送の保持・重複排除・期限が無い、B8 WDATA の ACK 待ちに回復と識別が無い、B9 送信本文の終端が無い、B10 TIME の形式、B11 epoch の生成と旧世代拒否が無い、B12 試験骨子が進まない状態機械を合格にできる。→ 第 2 版: ワイヤ v2 (rid / agent 付き 20B ヘッダ、宣言長、WDATA の rid+seq、TIME 標準化、epoch 生成と再同期の契機)、`link_tick` だけが更新者 + cli 区間 + 中継バッファ、WINDOW を開始許可にした直列化と close までの所有権、転送 ACK と RESPONSE の分離、実 Agent を結ぶホスト試験 |
