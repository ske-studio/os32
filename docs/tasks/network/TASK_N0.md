# N0 — Host Services の KAPI v51 (ABI 確定) と非ブロッキング化の設計

状態: **設計 第 3 版 (往復 2 の 11 件を反映: セッション ID と HELLO の nonce、rid 内の単一 seq 空間、STATUS 照会による RESPONSE の再提示と生存確認、RELEASE、結果の墓標、自己試験の非同期化、TX の公平化、反射モードの排他、正典の同期。往復 3/3 = 最終待ち)**。前提: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) (§2 サービス一覧、§3 KAPI 案、§7 票、§9 の決裁は**推奨案で進める**: 印刷 v1 は to-file 既定、Agent は WSL2、LGY-98 は N3 受入後に既定へ、KAPI は v51、CLIP は含め PUT は後回し、HTML は text)、[LINK_PLAN.md](LINK_PLAN.md) (L0〜L3 の契約: EtherType 0x88B5、16B ヘッダ、Stop-and-Wait の REQUEST/RESPONSE、L1 絶対値 WINDOW、L2 8KB ストリーム Go-Back-N)、`net/link.{c,h}` (現状は同期版 `link_request` / `link_service_get`、`link_stream_read`)、`docs/KAPI_SPEC.md` §3-1 (追加手順) / §3-2 (予約表)、T9 §1a (ABI 表の書式)、S0-K (v50: CPL=3 ポインタの範囲検証 `ring3_user_range_ok`、owner 回収の位置)。
規約: [ABI1〜3] (kapi.json が正典、末尾追記、版を上げて `make clean`)、[C1] C89、[C2] kstr*、[C4]。

## 0. 範囲

N0 は**設計だけ**: (a) KAPI_SPEC §3-2 (済)、(b) **リンク層ワイヤ v2** (§1b) = LINK_PLAN §4 の改訂 (旧表は参照に置き換える)、(c) v51 の ABI (§1a)、(d) `net/link.c` の駆動・排他・状態機械 (§2)、(e) Agent 側の v2 対応は N1 に含める (§2d)、(f) N1 の TDD と受入 (§3)、(g) **HOST_SERVICES_PLAN §2 / §3 の同期** (要求文法 `PRINT DATA <job> <len>`、TIME の標準形、close / STALE の契約、N1 / N2 の分担。往復 2 の R11 — N0 完了条件)。

## 1. KAPI_SPEC §3-2 の改訂 (済)

v43 = 欠番、v51 = 5 本 (slot 208〜212、data_fields 0x35C / 0x360)。全体 clean rebuild (SDK・shlib・apps / game・配備先の旧バイナリを作り直す)。

## 1b. リンク層ワイヤ v2 (LINK_PLAN §4 の改訂)

ヘッダ **20B、明示的に直列化** (LE アクセサで書く。C 構造体の padding に依存しない): `op u8` @0, `flags u8` @1, `epoch u16` @2, `seq u32` @4, `ack u32` @8, `length u16` @12, `rid u32` @14, `sess u16` @18。EtherType 0x88B5。`sess` = **セッション ID** (OS32 が起動ごとに生成: `sys_time` の秒 + `tick_count` を混ぜた 16bit、0 は使わない) を**全フレーム**に載せ、`epoch` は HELLO ごとの世代。

| op | 向き | 意味 |
|---|---|---|
| HELLO (1) | 双方 | `seq` = OS32 の **nonce** (HELLO ごとに +1)。Agent は受けた HELLO の `seq` / `epoch` / `sess` をそのまま返し、payload に `agent u16` (Agent 起動時の乱数世代) を載せる。**OS32 は自分が最後に送った nonce と一致する応答だけ採用** (遅延応答は無視)。Agent は同じ `sess` で `epoch` が**今の世代以上**の HELLO だけ採用 (小さい = 遅延した旧 HELLO は無視、同じ = 冪等に応答)。`sess` が変わった HELLO = OS32 再起動 → Agent は全状態を捨てて採用 |
| REQUEST (2) | OS32→Host | `rid` (epoch 内で単調増加、0 は使わない)、`seq` = **0** (rid 内の単一 seq 空間の先頭)。本文が要る要求は要求行に宣言長 |
| WDATA (8) | OS32→Host | `rid`、`seq` = **1〜** (REQUEST と同じ空間の続き)。Agent は (sess, epoch, rid, seq) で重複排除 |
| ACK (6) | 双方 | `rid` + `ack` = 順序どおり受けた最終 seq (**累積**、REQUEST = 0、WDATA = 1〜。往復 2 の R2)。Host→OS32 は REQUEST / WDATA の受領、OS32→Host は DATA の受領 (既存)。未送信 seq への ACK / 古い ACK は無視 |
| RESPONSE (3) | Host→OS32 | `rid`、本文 6B 固定 (`status u16` + `length u32`)。TIME / PING / ECHO も同じ |
| STATUS (9) | OS32→Host | `rid`。Agent は **結果があれば RESPONSE を再提示**、処理中なら RESPONSE (`status = 102`、`length = 0`、これは「処理中」の印で完了ではない)、知らない rid なら RESPONSE (`status = 410`) (往復 2 の R3 / R6) |
| RELEASE (10) | OS32→Host | `rid`。OS32 がハンドルを閉じた通知 (close / owner 退場)。Agent はその rid の受付済み要求・配送状態・結果を捨て、ACK (rid, ack = 0) を返す。**転送 ACK が来るまで再送** (往復 2 の R4) |
| DATA (4) / EOF (5) | Host→OS32 | `rid`、`seq` = ストリーム内 1〜。OS32 が WINDOW (rid) を送ったストリームだけ |
| WINDOW (7) | OS32→Host | `rid`、`ack` = 受けた最終 seq、payload = credit_pages。開始許可を兼ねる |

- **照合規則** (往復 2 の R1): HELLO 以外のフレームは `sess` と `epoch` が控えと一致するものだけ受け付ける (両端とも)。`agent` は HELLO の payload だけに載り、通常フレームには載せない。
- **Agent の要求保持** (往復 2 の R4 / R5): 受付済み (RESPONSE 前) と配送中 (RESPONSE 後、本文未完) の要求は **rid ごとに最大 2** (OS32 のハンドル数と同じ)。**追放は RELEASE を受けたときだけ** (活動中の要求を勝手に捨てない)。3 本目の REQUEST が来たら RESPONSE (`status = 503`、`length = 0`) で断る (OS32 側は RELEASE が届いていないことを意味するので再送する)。完了した結果は rid ごとに保持し、**`rid ≤ high_water − 8` の結果は捨てて墓標にする**: その rid の REQUEST / STATUS には RESPONSE (`status = 410`) を返し、**再実行しない**。`high_water` = 受け付けた最大 rid。
- **再同期の契機** (往復 2 の R6 / R7): (i) REQUEST / WDATA / RELEASE の転送 ACK が `LINK_TRIES` 回来ない、(ii) 生存ハンドル (SENT / RESP) があるのに **`T_probe` (1 秒) 以上フレームを受けていない → STATUS 照会**、それに `k` 回応答が無い、(iii) HELLO 応答の `agent` が控えと違う (Agent 再起動)、(iv) 起動時。再同期 = 新 epoch (+1) で HELLO (新 nonce) → 応答を得たら生存ハンドルを STALE、リング / TX キューを破棄。epoch は 16bit で +1 するだけ (`sess` が起動を分けるので周回時の旧フレームは `sess` / `epoch` の等値照合で弾ける。「壊れない」とは言わない = 周回は 65535 回の再同期後で、そのときは旧フレームが同じ値になり得るが、そのフレームは数秒前のものでしかない)。

## 1a. KAPI v51 の ABI (N1 の K が実装、N3 の C / N4 の W が従う)

共通: CPL=3 のポインタ / 長さは v50 と同じ (`ring3_user_range_ok`、帯外は `INVAL`、帯内の非 present はディスパッチャの規則で kill)。複数の出力ポインタは**全部を先に検証**してから書く。出力ポインタは NULL 可。失敗時は出力を書かない。どの呼び出しも待たない。NIC 無し / 未初期化 → `NOSYS`。h が範囲外 / FREE / 他人 → `INVAL`。**STALE のハンドルへの status / read / write → `STALE`** (close だけが通る)。

| slot | 名前 | 引数 | 戻り | 規則 |
|---|---|---|---|---|
| 208 | `host_open` | `const char *req, u32 len` | h (0 / 1) / 負 | 要求行 1〜1400B (超過 / 0 → `INVAL`)、カーネル所有の領域へ写す。HELLO 未確立 / 再同期中 → `STALE`。空き無し → `FULL`。TX キュー満杯 → `AGAIN`。成功で SENT (REQUEST seq 0 をキューへ) |
| 209 | `host_status` | `i32 h, u32 *status, u32 *length` | 0 / `AGAIN` / 負 | RESPONSE (102 以外) 未着 → `AGAIN`。着いていれば `status` / `length` |
| 210 | `host_read` | `i32 h, void *buf, u32 cap` | 長さ (> 0) / 0 = 完了 / `AGAIN` / 負 | `cap == 0` / `buf == NULL` → `INVAL`。RESPONSE 未着 → `AGAIN`。`length == 0` → 0。リングの所有者でなければ取得を試み、他方が所有中 → `AGAIN`。**1 回に写す量 = min(cap, リングの連続可用, 1400)**。最後のバイトを写した呼び出しは正の長さを返し、**その次**の呼び出しが 0 (`read_bytes == length` のとき)。未着 → `AGAIN` |
| 211 | `host_write` | `i32 h, const void *buf, u32 len` | 受け付けた長さ (≤ min(len, 1400, 残り宣言長)) / `AGAIN` / 負 | 宣言長の無い要求 / `len == 0` / `buf == NULL` / 宣言長超過 → `INVAL`。**REQUEST (seq 0) の転送 ACK が来るまで `AGAIN`** (rid 内の seq は順に ACK される)。未 ACK の WDATA が 1 本ある間 `AGAIN`。TX キュー満杯 → `AGAIN`。RESPONSE 着後 → `INVAL`。累計は u32 で宣言長以下に抑える (overflow 無し) |
| 212 | `host_close` | `i32 h` | 0 / 負 | 任意の状態から FREE。リング所有者なら未読分を捨てて放す。**RELEASE (rid) をキューへ** (転送 ACK まで `link_tick` が再送、保留 TX の同 rid の REQUEST / WDATA は無効化)。二重 close → `INVAL` |

- **owner 回収**: `exec_reclaim_owned(x)` の中で `host_owner_exit(x)` = owner が x のハンドルを内部解放 (RELEASE も送る)。
- **同期版** `link_service_get()` / `link_request()` / L1 / L2 の自己試験は **非同期 API + `sys_yield` 相当 (IF=1 の hlt 待ち) の上に書き直し**、状態機械を進めるのは `link_tick` だけ (往復 2 の R8)。

## 2. `net/link.c` の非ブロッキング化 (N1 の設計)

### 2a. 駆動と排他
- **プロトコルの進行はタイマ**: 100Hz の `link_tick` (`ne2k_timer_tick` の後) が受信 dispatch (最大 16 フレーム / 周回)、再送、WINDOW / ACK / STATUS / RELEASE の送出、HELLO / 再同期を行う。KAPI からは `link_tick` を呼ばない。**反射モード (`LGY98_FLAG_REFLECT`) では `link_tick` を起動しない** = RX キューの消費者は反射かリンク層のどちらか 1 つ (往復 2 の R10、M2 の反射試験を回帰対象に)。
- **KAPI の更新は割込み禁止区間**: ハンドル表 / リング / TX キューへの KAPI からの読み書きは `cli` / `sti` (IF 保存復元) の短い区間で行い、区間内で待たない。`host_read` はリング → 中継バッファ (ハンドルごと 1400B) を cli 区間で写し、IF=1 でユーザー領域へ写す (写した後に cli 区間で `read_bytes` を進める = ユーザー領域の #PF で途中状態を残さない)。`host_open` / `host_write` は要求 / WDATA をカーネル領域に写してから cli 区間でキューへ積む。IF=0 中に来たタイマは延期されるだけで、IF 復元直後の tick が処理する。
- **TX の公平化** (往復 2 の R9): 送信キューは制御 (WINDOW / ACK / STATUS / RELEASE、rid ごとに **1 周回 1 本に集約**) と通常 (REQUEST / WDATA) の 2 本で、`link_tick` は **交互** (制御 1 本 → 通常 1 本) に NIC へ渡す。NIC が busy で受けなければ次の周回に持ち越す (制御通知も捨てない)。**RTO は NIC が送信を受理した tick から数える** (キューに積んだ時点ではない)。

### 2b. 配送の直列化
- ハンドル 2 本、リング 1 本。リングの所有権は最初に `host_read` した側 (cli 区間で取得) が **close まで**持ち、`link_tick` がその rid の WINDOW を送る。Agent は WINDOW を受けた rid だけ流す。他方は `AGAIN`。
- Agent 側: 受付済み / 配送中は rid ごと最大 2、追放は RELEASE だけ (§1b)。

### 2c. 状態機械
ハンドル: `state` (FREE / SENT / RESP / DONE / STALE)、`owner`、`rid`、`req_copy[1400]` + `req_len`、`req_acked`、`status`、`length`、`recv_bytes`、`read_bytes`、`ring_owner`、`wseq`、`wcopy[1400]` + `wlen` + `wacked`、`decl_len` + `wsent_total`、`last_tx_tick`、`retries`、`last_rx_tick`。

| 事象 | 遷移 / 動作 |
|---|---|
| `host_open` | FREE → SENT、REQUEST (seq 0) を通常キューへ |
| 転送 ACK (rid, ack ≥ 0) | `req_acked = 1`、REQUEST の再送停止 (状態は SENT のまま) |
| 転送 ACK 不着 (`LINK_RTO` × `LINK_TRIES`、NIC 受理 tick から) | REQUEST 再送 (同 rid / seq 0)。上限超 → 再同期 |
| RESPONSE (rid 一致、`status != 102`) | SENT → RESP。`length == 0` → DONE (**0 長の成功応答も RESPONSE の再提示対象**)。重複は無視 |
| RESPONSE (`status == 102`) | 処理中の印。`last_rx_tick` を更新するだけ |
| RESPONSE (`status == 503`) | Agent の受付枠が無い = RELEASE 未達。RELEASE を再送してから REQUEST を再送 (SENT のまま) |
| RESPONSE (`status == 410`) | 墓標 (Agent が結果を捨てた) → その旨の status を呼び手へ (RESP、`length = 0`) |
| `req_acked` で RESPONSE 未着が `T_probe` | STATUS (rid) を制御キューへ (1 秒ごと)。`k` (5) 回応答が無い → 再同期 |
| WINDOW 送出 | `ring_owner` かつ `recv_bytes < length` のとき credit = リング空き (制御キュー、rid ごと 1 周回 1 本) |
| DATA (ring_owner の rid、順次) | リングへ、`recv_bytes += n`、ACK (累積) を制御キューへ。gap は捨てて再 ACK (Go-Back-N)。非所有 / 未知 rid は捨てる |
| EOF | 補助。完了は `recv_bytes == length` |
| `host_read` | 所有権取得 → 中継 → ユーザーへ → `read_bytes` 更新。`read_bytes == length` で 0 (DONE) |
| `host_write` | `req_acked` かつ `wacked` → WDATA (seq = `wseq`) を写して通常キューへ (`wacked = 0`)。ACK (rid, ack ≥ seq) で `wacked = 1`、`wseq++`。RTO で再送 (同 seq)、上限超 → 再同期 |
| Agent が本文を受け切る | RESPONSE → RESP / DONE |
| 再同期 (新 epoch、新 nonce の HELLO 応答) | SENT / RESP / DONE → STALE、リング / TX キュー破棄、`ring_owner` を放す。FREE は不変 |
| `host_close` / `host_owner_exit` | 任意 → FREE、RELEASE (rid) を通常キューへ (転送 ACK まで再送、上限超 → 再同期)、同 rid の保留 TX を無効化 |

### 2d. Agent 側 (N1 に含める `host_agent.py` の v2 対応)
20B ヘッダ (LE 直列化)、`sess` / `epoch` / nonce の HELLO 規則、`agent` 世代、rid ごとの受付 2 + 結果保持 + 墓標 (`high_water − 8`)、STATUS への再提示 / 102 / 410、RELEASE の処理と ACK、WINDOW 待ちの配送開始、WDATA の (sess, epoch, rid, seq) 重複排除と累積 ACK、宣言長で本文完了 → RESPONSE、TIME / PING を標準ヘッダに、N1 の受入用 `ECHO <len>` サービス。PRINT / CLIP / PUT は N2。既存 `check-net-l0`〜`l3` は v2 で回帰 (ゲスト観測)、ホストハーネスと分けて記録。

## 3. N1 のホスト TDD と受入

- `tools/tests/net_link_host.c`: `net/link.c` を `#include`、NIC (RX キュー、TX 受理の可否 = 1 tick 1 フレームの条件も、tick) と `cli` / `sti` を贋物に。**タイマ入口だけで進む**、**割込み挿入** (リング更新 / TX 構築の途中に贋 IRQ5 が積む、IF=0 中のタイマ延期と復元直後の tick)、**実 Agent** (`host_agent.py` v2 をサブプロセス、UNIX ソケットで結ぶ) で: 非ゼロの `agent` 世代での HELLO → REQUEST → RESPONSE → 本文完了 (R1)、REQUEST ACK の遅延重複 + WDATA 喪失の同時注入 (R2)、転送 ACK 後の RESPONSE 消失 → STATUS で再提示 (0 長も) (R3)、A / B の close 順 × 配送状態 (WINDOW 未送信 / 配送途中 / 本文送信途中) で RELEASE が正しい rid を捨てる (R4)、8 件境界を越えた遅延 REQUEST が 410 で再実行されない、長寿命 A + 多数の短命 B (R5)、2 ハンドル ACK 済みで Agent 再起動 → STATUS 無応答 → 再同期 → STALE (R6)、旧 HELLO / 旧応答の遅延、同じ初期 tick の再起動 (`sess` で区別)、epoch 境界 (R7)、自己試験の実行中のタイマ挿入 (R8)、credit 0 の A と B の並行で NIC が 1 tick 1 フレーム (R9)、反射モードで `link_tick` が起動しない (R10)、HTTP 処理中 (102) の別要求の ACK / WINDOW、最終 DATA 消失、EOF 消失、owner 回収 (親へ戻った状態)、ポインタ検査は実 KAPI ラッパー (`kapi/kapi_host.c`) 経由。既存 M2 反射試験と L0〜L3 を回帰対象に。
- ゲスト (`kernel-lgy98-link`、WSL2 の `host_agent.py` v2): `userland/tests/host_test.c` で `GET /pattern/65536` (AGAIN ループ)、`GET /notfound` = 404、`TIME`、`ECHO 5` + write、GUI 配下で `gui_busy` と同時、CTRL+STOP 後の open、Agent 再起動 → STALE → close → open。

## 4. レビューで見てほしい点

1. §1b の HELLO (nonce / sess / epoch / agent) が R1 / R6 / R7 を閉じるか、`sess` の生成 (`sys_time` + tick) の衝突。
2. rid 内の単一 seq 空間と累積 ACK (R2)、STATUS / 102 / 410 / 503 と RELEASE (R3 / R4 / R5) の相互作用 (RELEASE 未達 + 新 REQUEST、STATUS と RESPONSE の競合)。
3. §2a の駆動・排他・公平化 (R8 / R9 / R10) と、KAPI が cli 区間で更新することの整理。
4. §3 の試験が R1〜R11 の反例を個別に踏むか。
5. LINK_PLAN §4 と HOST_SERVICES_PLAN §2 / §3 の同期 (R11) を N0 の完了条件にしたこと。

## 5. 呼び出し列の例 (N3 が従う)

```
GET:      h = host_open("GET http://x/") ; loop { host_status(h,&st,&len); AGAIN → sys_yield }
          ; loop { n = host_read(h,buf,4096); AGAIN → sys_yield; 0 → break; >0 → 書く } ; host_close(h)
CLIP PUT: h = host_open("CLIP PUT 5") ; loop { n = host_write(h,"hello",5); AGAIN → sys_yield }
          ; loop { host_status ; AGAIN → sys_yield } ; host_close(h)
PRINT:    h1 = open("PRINT OPEN rep text") → status/read "job 7" → close
          h2 = open("PRINT DATA 7 4096") → write ×3 → status (200) → close   (繰り返し)
          h3 = open("PRINT CLOSE 7") → status/read "pages 1" → close
```

## 6. ユーザー判断が要る点

- HOST_SERVICES_PLAN §9 の 6 項目は推奨案で進める。
- ワイヤ v2 で L0〜L3 の合格実績を取り直す (N1 の受入に含める)。

## 7. レビュー記録

| 版 | 判定 | 要旨 |
|---|---|---|
| 第 2 版 | Request changes | 11 件: R1 通常フレームの agent が矛盾、R2 REQUEST と WDATA の ACK 空間衝突、R3 転送 ACK 後の RESPONSE 消失、R4 close 通知が無く Agent の受付 2 件を置換できない、R5 結果 8 件では重複排除を保証できない、R6 ACK 済み・読出し中の Agent 再起動を検出できない、R7 HELLO の鮮度と epoch 再使用、R8 同期自己試験の再入、R9 制御通知の無条件優先で飢餓、R10 反射モードの RX 争奪、R11 正典に旧契約が残る。→ 第 3 版: sess / nonce / agent、rid 内単一 seq、STATUS (102 / 410 / 503) と RELEASE と墓標、T_probe の生存確認、自己試験の非同期化、交互送信と NIC 受理からの RTO、反射モードの排他、正典同期を完了条件に |
| 第 1 版 | Request changes | 12 件: B1 非同期の駆動元が無い、B2 KAPI と IRQ の排他が無い、B3 要求と DATA/EOF の対応付けが無く discard 印も不成立、B4 Agent の逐次処理はストリームの直列配送を保証しない、B5 EOF だけでは完了を確定できない、B6 EOF とリング解放の混同、B7 REQUEST 再送の保持・重複排除・期限が無い、B8 WDATA の ACK 待ちに回復と識別が無い、B9 送信本文の終端が無い、B10 TIME の形式、B11 epoch の生成と旧世代拒否が無い、B12 試験骨子が進まない状態機械を合格にできる。→ 第 2 版: ワイヤ v2 (rid / agent 付き 20B ヘッダ、宣言長、WDATA の rid+seq、TIME 標準化、epoch 生成と再同期の契機)、`link_tick` だけが更新者 + cli 区間 + 中継バッファ、WINDOW を開始許可にした直列化と close までの所有権、転送 ACK と RESPONSE の分離、実 Agent を結ぶホスト試験 |
