# N0 — Host Services の KAPI v51 (ABI 確定) と非ブロッキング化の設計

状態: **設計 第 1 版 (Codex 設計レビュー 往復 1/3 待ち)**。前提: [HOST_SERVICES_PLAN.md](HOST_SERVICES_PLAN.md) (§2 サービス一覧、§3 KAPI 案、§7 票、§9 の決裁は**推奨案で進める**: 印刷 v1 は to-file 既定、Agent は WSL2、LGY-98 は N3 受入後に既定へ、KAPI は v51、CLIP は含め PUT は後回し、HTML は text)、[LINK_PLAN.md](LINK_PLAN.md) (L0〜L3 の契約: EtherType 0x88B5、16B ヘッダ、Stop-and-Wait の REQUEST/RESPONSE、L1 絶対値 WINDOW、L2 8KB ストリーム Go-Back-N)、`net/link.{c,h}` (現状は同期版 `link_request` / `link_service_get`、`link_stream_read`)、`docs/KAPI_SPEC.md` §3-1 (追加手順) / §3-2 (予約表)、T9 §1a (ABI 表の書式)、S0-K (v50: CPL=3 ポインタの範囲検証 `ring3_user_range_ok`、owner 回収の位置)。
規約: [ABI1〜3] (kapi.json が正典、末尾追記、版を上げて `make clean`)、[C1] C89、[C2] kstr*、[C4]。

## 0. 範囲

N0 は**設計だけ** (コードは書かない): (a) KAPI_SPEC §3-2 の予約を v43 (欠番、使わない) → **v51** に改訂、(b) v51 の 5 本の ABI を §1a 形式で確定、(c) `net/link.c` の非ブロッキング化の状態機械と owner 回収を N1 が実装できる粒度で決める、(d) N1 のホスト TDD と受入の骨子。実装は N1 (K)。

## 1. KAPI_SPEC §3-2 の改訂 (N0 で行う)

- v43 の行を「**欠番** (予約していたが v44〜v50 が先に実装された。使わない)」に改める。
- v51 の行を追加: 「予約 (N1 で実装)」、`host_open` / `host_status` / `host_read` / `host_write` / `host_close` (slot 208〜212)、data_fields は 5 本ぶん後ろへ (`sbrk_heap_limit` 0x35C、`shm_base` 0x360)。全体 clean rebuild (F0 の方針)。
- エラー番号: 固有番号は取らない (`IO` -1 / `INVAL` -9 / `NOSYS` -10 / `STALE` -11 / `AGAIN` -14 を写像。§1a)。

## 1a. KAPI v51 の ABI (N1 の K が実装、N3 の C / N4 の W が従う)

共通: CPL=3 のポインタ / 長さは v50 と同じ `ring3_user_range_ok` (帯 + 長さ) で検証し、拒否は `OS32_ERR_INVAL`。出力ポインタは NULL 可 (書かない)。失敗時は出力を書かない。**どの呼び出しも待たない** (応答・リンク・TX 空きを待つのは呼び手の `sys_yield` / タイマ)。GUI 配下では T8 D8 のポーリング型 (`WAIT_POLL`) と同じ作法。

| slot | 名前 | 引数 | 戻り | 権限 | 規則 |
|---|---|---|---|---|---|
| 208 | `host_open` | `const char *req, u32 len` | h (0 / 1) / 負 | CPL=3 / CPL=0 | 要求行は 1〜1400B (超過 / 0 → `INVAL`。NUL は含めない = len が長さ)。リンク未確立 (HELLO 未完 / 再同期中) → `STALE`。**同時ハンドルは 2** (0 と 1。空きが無ければ `OS32_ERR_FULL`)。ただし **本文 (RESPONSE の L2 ストリーム) を持てるのは 1 本だけ**: 先に応答本文が流れ始めたハンドルがストリームを占有し、もう 1 本は「要求送信済み・応答待ち」のまま (ホストは順に処理するので実質直列)。`host_open` は REQUEST を **1 回送って戻る** (再送は `link_poll` の状態機械)。TX スロット (リンク層の送信バッファ 1 本) が塞がっていれば `AGAIN` (ハンドルは割り当てない)。所有者は `res_owner_get()` で記録 |
| 209 | `host_status` | `i32 h, u32 *status, u32 *length` | 0 / `AGAIN` / 負 | 所有者 | RESPONSE 未着 → `AGAIN`。着いていれば `status` (HTTP 相当 u16) と `length` (本文長、0 = 本文なし)。h が不正 / 他人の / 閉じた → `INVAL`。リンク断 (epoch が変わった = HELLO 再同期) → `STALE` (ハンドルは閉じられるまで STALE を返し続ける) |
| 210 | `host_read` | `i32 h, void *buf, u32 cap` | 読んだ長さ (> 0) / 0 = EOF / `AGAIN` / 負 | 所有者 | L2 ストリームから最大 cap (`cap == 0` → `INVAL`)。RESPONSE 未着 → `AGAIN`。本文長 0 → 即 0。データ未着で EOF でもない → `AGAIN`。**消費した分だけ credit が回復**する (WINDOW はリンク層が `link_poll` で送る)。他方のハンドルがストリームを占有中 → `AGAIN` (それが閉じられるまで) |
| 211 | `host_write` | `i32 h, const void *buf, u32 len` | 受け付けた長さ (≤ len) / `AGAIN` / 負 | 所有者 | `PRINT DATA` / `CLIP PUT` / `PUT` の本文。**v1 は 1 回の呼び出しで 1 フレーム** (≤ 1400B) を REQUEST (op = DATA 相当の新 op `WDATA`、seq 付き) で送り、受け付けた長さを返す (呼び手は残りを次で送る)。TX スロットが塞がっていれば `AGAIN`。ホスト側の受付は ACK (既存の L0 ACK) で、ACK 未着の間は次を送らない = Stop-and-Wait (ホストは余裕があるので credit は要らないが、**順序保証は ACK で取る**)。`len == 0` → `INVAL`。応答 (RESPONSE) が既に届いたハンドルへの write → `INVAL` |
| 212 | `host_close` | `i32 h` | 0 / 負 | 所有者 | 未読ストリームを捨てる (ホストへは何も送らない。次の要求で RESPONSE/DATA が混ざらないよう **ハンドルの seq 範囲を控え、閉じた後に届くその範囲の DATA / RESPONSE は `link_poll` が捨てる**)。ハンドルを返す。不正 → `INVAL` |

- **owner 回収**: `exec_reclaim_owned(x)` の中 (con_sink / launch と同じ位置) で `host_owner_exit(x)` = その所有者のハンドルを `host_close` 相当で全部閉じる (プロセスの異常終了で本文が読まれないままでもリンク層は詰まらない)。
- **カーネル内の駆動**: 既存の `link_poll()` (IRQ5 + 100Hz ウォッチドッグ) が RESPONSE / DATA / EOF / ACK / WINDOW / HELLO を処理し、ハンドルの状態を進める。KAPI は状態を読む / 1 フレーム送るだけ。
- 同期版 `link_service_get()` / `link_request()` は自己試験用に残す (KAPI にはしない)。CPL=0 (常駐シェル) からも同じ 5 本を使う (`date -sync` 等)。

## 2. `net/link.c` の非ブロッキング化 (N1 の設計、状態機械)

ハンドル `HostReq[2]`: `state` (FREE / SENT / RESP / STREAM / EOF / STALE)、`owner`、`req_seq` (送った REQUEST の seq)、`status`、`length`、`read_pos`、`epoch` (open 時のリンク epoch)、`last_tx_tick` (再送用)、`tx_retries`。

| 事象 (link_poll / KAPI) | 遷移 |
|---|---|
| `host_open` | FREE → SENT (REQUEST 送信、`req_seq` 記録、`last_tx_tick`)。TX 塞がり → AGAIN で FREE のまま |
| RESPONSE (ack == req_seq) | SENT → RESP (`status` / `length` 保存)。`length == 0` → EOF (本文なし) |
| DATA (ストリーム) | RESP → STREAM (最初の DATA) / STREAM のまま。L2 の順次消費 (既存 `link_stream_push`、`link_l2_gaps` の Go-Back-N)。**ストリームは 1 本**なので、RESP の 2 本目は他方が EOF/閉じるまで DATA を受けても捨てる (ホストは直列に処理するので実際には起きない = 起きたら計器に数える) |
| EOF フレーム | STREAM → EOF (残りは `host_read` で読み切る) |
| 再送タイマ (`link_poll` 内、100Hz) | SENT で `now - last_tx_tick > LINK_RTO` (既存の同期版と同じ値) → REQUEST 再送 (`tx_retries++`、上限 `LINK_TRIES` で **STALE**)。DATA の欠落は既存の Go-Back-N (ACK で相手に再送させる) |
| HELLO 再同期 (epoch 変化) | 全ハンドル → STALE (呼び手は close して open し直す) |
| `host_close` | 任意 → FREE。STREAM 中なら `link_stream` を捨て、**その `req_seq` の DATA / EOF を以後捨てる**印 (`discard_seq`) を 1 つ持つ |
| owner 退場 | `host_owner_exit` = 所有ハンドルを close |

- `link_request` (同期版) の内部は「SENT にして `link_poll` を回す」形に書き直し、同じ状態機械を通す (自己試験 L0〜L3 が既存の `make check-net-l*` で回帰する)。
- `host_write` の WDATA: 新 op (LINK_OP_WDATA = 8) を LINK_PLAN のワイヤ表に追記 (`host_agent.py` 側は N2 で受ける: `PRINT DATA` 等の本文として `seq` 順に追記、ACK を返す)。**LINK_PLAN §1 のフレーム形式表の改訂は N0 で行う** (op 8 の追加だけ、既存の 1〜7 は不変)。

## 3. N1 のホスト TDD と受入 (骨子)

- `tools/tests/net_link_host.c`: `net/link.c` を `#include` し、NIC 送受信を贋物 (フレーム列の注入、TX の記録、tick の進行) に差し替えて状態機械を固定: open → RESPONSE → DATA×n → EOF → read の順次消費と credit、AGAIN の各点、再送と STALE、epoch 変化で STALE、close 後の遅延 DATA の破棄、2 本目のハンドルの待ち、owner_exit、WDATA の Stop-and-Wait (ACK 前の write は AGAIN)、CPL=3 ポインタの範囲検証 (v50 の `paging_bounds_host` の作法)。既存の L0〜L3 の自己試験も同じハーネスで通す。
- ゲスト (`kernel-lgy98-link` を配備、Host Agent は WSL2 の `host_agent.py`): `userland/tests/host_test.c` (CPL=3) で `GET /pattern/65536` の内容一致 (AGAIN ループ、`sys_yield`)、`GET /notfound` = 404、`TIME`、GUI 配下で `gui_busy` と同時に走らせて WM が止まらない、異常終了 (CTRL+STOP) 後に次の open が通る (owner 回収)。
- 配備: `kernel-lgy98-link` は既定ビルドではない (§9-3 は N3 受入後に既定へ)。N1 の受入は LGY-98 有効カーネルで行い、既定カーネルへの回帰 (`make all` の kernel) も `make check` で担保する。

## 4. レビューで見てほしい点

1. §1a の 5 本が「待たない」を守り、GUI の協調 (`sys_yield` / `WAIT_POLL`) と矛盾しないか。同時 2 ハンドル / ストリーム 1 本の規則が LINK_PLAN の L2 (再結合バッファ無し、順次消費) と整合するか。
2. 状態機械 (§2) の穴: RESPONSE の遅延と再送の競合、close 後の遅延 DATA、epoch 変化、2 本目の DATA の扱い、`host_write` の ACK 待ちと `host_open` の TX スロット共有。
3. owner 回収の位置と、CPL=0 (常駐シェル) からの利用 (owner が無い) の扱い。
4. WDATA (op 8) の追加が既存 L0〜L3 と `host_agent.py` を壊さないか。
5. KAPI_SPEC §3-2 の改訂 (v43 欠番、v51) と data_fields の移動、[ABI1〜3]。
6. N1 のホスト TDD が §2 の各遷移と失敗を踏めるか。

## 5. ユーザー判断が要る点

- HOST_SERVICES_PLAN §9 の 6 項目は推奨案で進める (印刷 to-file 既定、Agent は WSL2、LGY-98 は N3 後に既定へ、v51、CLIP は含め PUT は後回し、HTML は text)。異論があれば N1 着手前に。
