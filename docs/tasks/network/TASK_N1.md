# TASK_N1 — Host Services 基盤: ワイヤ v2、`net/link.c` の非ブロッキング化、KAPI v51、Agent v2

発行: PM (2026-09-14) / 状態: **実装中 (コーダー worktree)**。設計の正典は [TASK_N0.md](TASK_N0.md) 第 5 版 (§1a ABI、§1b ワイヤ v2、§2 状態機械、§3 TDD と受入)。本票は N0 §3 を実装単位に落としたもので、契約は N0 が勝つ。

## 0. 範囲 (HOST_SERVICES_PLAN §7 の N1)

| 段 | 成果物 | 試験 |
|---|---|---|
| 1 | `tools/host_agent.py` v2: 20B ヘッダ (LE 直列化、op ごとの payload 長検査)、3 way HELLO (候補 1 件、Agent nonce、`req_sess` / `req_epoch` の写し、`sess.txt` 永続採番と枯渇停止、`agent` 世代)、rid 台帳 (ACTIVE ≤ 2 / RELEASED / HOLE、`high_water`、規則 (1)〜(5)、epoch 切替で HOLE → RELEASED)、STATUS への再提示 / PROCESSING / TOMBSTONE、制御結果は RESPONSE flags bit0、RELEASE と bit0 ACK、WINDOW 待ちの配送開始、WDATA の (sess, rid, seq) 重複排除と累積 ACK、宣言長で本文完了 → RESPONSE、`GET` / `TIME` / `PING` / `ECHO <len>`。トランスポートは既存の NP2NETSOCK (4B BE 長 + フレーム) に加えて **UNIX ソケット** (`--unix <path>`、ホスト TDD 用) | `tools/tests/test_host_agent.py`: Python の贋 OS32 で N0 §3 の Agent 側項目 (台帳の (1)〜(5)、HOLE、墓標 8 件境界、3 way の各段の消失 / 遅延 / 旧 SYN、枯渇停止、flags 分離、WDATA 重複 / 欠落、RELEASE 先着) |
| 2 | `net/link.c` / `net/link.h`: v2 ヘッダ、ハンドル 2 本 + リング 1 本、N0 §2c の状態機械、`link_tick` (100Hz、`ne2k_timer_tick` の後、`kernel/isr_handlers.c`)、反射モードでは起動しない、制御 / 通常キューの交互送信 + RELEASE 専用スロット、NIC 受理 tick からの RTO、`T_probe` の STATUS、3 way HELLO と再同期、cli 区間 + `gen` 再確認、`host_read` の成功確定点。既存の同期自己試験 (`link_hello` / `link_request` / `link_l1_bulk` / `link_l2_stream` / `link_service_get`、`drivers/lgy98.c:132` の呼び手) は**非同期 API + hlt 待ちの上に書き直す** | 段 4 のハーネス |
| 3 | KAPI v51: `sdk/kapi.json` に 5 本を末尾追記 (`host_open` / `host_status` / `host_read` / `host_write` / `host_close`、スキル `os32-kapi-add` の手順、生成物は生成器で)、`kapi/kapi_host.c` に `__cdecl` ラッパー (ポインタ検査は `ring3_user_range_ok`、出力ポインタは全部検証してから書く)、`exec_reclaim_owned` に `host_owner_exit` (`launch_owner_exit` / `con_sink_owner_exit` と同じ位置)、`docs/KAPI_SPEC.md` §3-2 の v51 行を「実装済み」に | 段 4 のハーネスがラッパー経由 + ディスパッチャの早期検査 (`kapi_argptr`) 経由の両方で叩く |
| 4 | `tools/tests/net_link_host.c`: `net/link.c` を `#include`、NIC (RX キュー、TX 受理可否 = 1 tick 1 フレームの条件、tick) と `cli` / `sti` を贋物に、**実 Agent** (段 1 を UNIX ソケットでサブプロセス起動)。N0 §3 の全項目 (往復 2 の R1〜R10、往復 3 の B1〜B8、往復 4 の R1〜R4、その他) を**個別のケース名**で | `build/sdk.mk` に `check-net-link-host` を足し `check` の列に登録 |
| 5 | `userland/tests/host_test.c` (N0 §3 のゲスト項目: `GET /pattern/65536` の AGAIN ループと内容一致、`GET /notfound` = 404、`TIME`、`ECHO 5` + write、Agent 再起動 → STALE → close → open)、`build/programs.mk` / `build/app.conf` (api 51) / `userland/deploy.yaml` | ゲスト受入は PM / テスター (コーダーは触らない) |
| 6 | `tools/tests/n1_tdd.md` (RED → GREEN の記録、ケース名と N0 の指摘番号の対応表)、`docs/tasks/network/LINK_PLAN.md` §5-1 の進捗、本票 §3 の自己申告 | — |

既存の `check-net-l0`〜`l3` (`tools/net_l*_test.py`、ゲスト観測) は v2 で回帰させる (PM がゲストで実行)。`check-net-m2` の反射試験は回帰対象。

## 1. 規約

[C1] C89、[C2] kstr*、[C3] `__cdecl`、[C4] 定数は 3 層 (ワイヤ定数は `net/link.h`、KAPI 側の共有定数は `sdk/include/os32/os32_kapi_shared.h`)。[ABI1〜3] `kapi.json` が正典、末尾追記、版 50 → 51 (`make clean` → `make all` は PM / テスターが行う)。**ワイヤ上の構造は LE アクセサで読み書き** (構造体キャストと非アラインアクセスをしない — 移植性の習慣)。コーダーは commit / deploy / エミュレータ / ini / `.env` / `make all` / `make check` を触らない (自分のホスト試験ターゲットと Python 試験の直接実行は可)。

## 2. 受入 (PM)

`make check` (ホスト TDD 全部)、`kernel-lgy98-link` を配備して `host_agent.py` v2 を WSL2 で起動、`host_test` の全項目、`check-net-l0`〜`l3` と `m2` の回帰、GUI 配下で `gui_busy` と同時に `host_test`。Codex 実装レビュー (網羅指示) の後に main へ。

## 3. 記録

(コーダーの自己申告と PM の受入をここに)
