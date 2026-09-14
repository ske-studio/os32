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
| 7 | **移植性調査** `docs/tasks/portability/SURVEY_N1.md` (ユーザー指示 2026-09-14): ワイヤ v2 と `link.c` / KAPI v51 の実装で触れた・見つけた **CPU アーキテクチャ依存** (他アーキテクチャ、例えば ARM への移行時の注意点) を**詳細に**調査して記す。観点: (a) 直列化とアライメント (ワイヤ / ディスク上の構造を LE アクセサで読んでいるか、構造体キャスト・非アラインアクセスの残存箇所を `net/` `drivers/ne2000.c` `fs/` `kapi/` で grep して列挙)、(b) 割込み制御 (`cli` / `sti` / `hlt` の直書きの箇所と抽象化の有無)、(c) ポート I/O (`in` / `out` の箇所 = ARM では MMIO)、(d) メモリ順序とキャッシュ (NIC リング・DMA バッファ・共有バッファで x86 の強い順序に依存している箇所、[HW2] の 64KB 境界)、(e) システムコールの引数渡し (`int 0x80` + ユーザースタックからの引数コピー、`kapi_argptr` の早期検査 = レジスタ渡しの ISA でどう変わるか)、(f) 物理番地の前提 (`memmap.h` 以外に絶対番地を書いた箇所)、(g) タイマと割込みコントローラ (100Hz `link_tick` の前提)、(h) エンディアンと型幅 (`u16` / `u32` の仮定、`int` の幅)。各項目に「N1 で直した」「残っている (場所と理由)」「移植時にやること」を書く。調査は**コードを grep して具体的な行を挙げる** (推測で書かない) | — |
| 6 | `tools/tests/n1_tdd.md` (RED → GREEN の記録、ケース名と N0 の指摘番号の対応表)、`docs/tasks/network/LINK_PLAN.md` §5-1 の進捗、本票 §3 の自己申告 | — |

既存の `check-net-l0`〜`l3` (`tools/net_l*_test.py`、ゲスト観測) は v2 で回帰させる (PM がゲストで実行)。`check-net-m2` の反射試験は回帰対象。

## 1. 規約

[C1] C89、[C2] kstr*、[C3] `__cdecl`、[C4] 定数は 3 層 (ワイヤ定数は `net/link.h`、KAPI 側の共有定数は `sdk/include/os32/os32_kapi_shared.h`)。[ABI1〜3] `kapi.json` が正典、末尾追記、版 50 → 51 (`make clean` → `make all` は PM / テスターが行う)。**ワイヤ上の構造は LE アクセサで読み書き** (構造体キャストと非アラインアクセスをしない — 移植性の習慣)。コーダーは commit / deploy / エミュレータ / ini / `.env` / `make all` / `make check` を触らない (自分のホスト試験ターゲットと Python 試験の直接実行は可)。

## 2. 受入 (PM)

`make check` (ホスト TDD 全部)、`kernel-lgy98-link` を配備して `host_agent.py` v2 を WSL2 で起動、`host_test` の全項目、`check-net-l0`〜`l3` と `m2` の回帰、GUI 配下で `gui_busy` と同時に `host_test`。Codex 実装レビュー (網羅指示) の後に main へ。

## 3. 記録

### 3-1. コーダーの自己申告 (2026-09-14、worktree `agent-ad14aebaf0154a34d`、基点 `af48990`)

段 1〜7 を実装した。TDD の記録 (RED → GREEN、ケース名 ↔ TASK_N0 §3 の指摘番号の対応表、
決めたこと、既存 `check-net-l0`〜`l3` が読むシンボルの v2 での意味) は
[`tools/tests/n1_tdd.md`](../../../tools/tests/n1_tdd.md)、移植性調査 (段 7) は
[`docs/tasks/portability/SURVEY_N1.md`](../portability/SURVEY_N1.md)。

**試験で確認したこと** (全部コーダーの手元で実行、出力は n1_tdd.md §5):

- `make check-host-agent` (`python3 -B tools/tests/test_host_agent.py`) — **25/25 PASS**。
  実 `host_agent.py` に、フレームを直接組む贋 OS32 をぶつけた。往復 2 の R1〜R5 / R7、
  往復 3 の B1〜B7、往復 4 の R1〜R3 と HELLO 各段の消失、payload 長検査、
  WDATA 重複排除、WINDOW の配送許可、TIME の標準形。
- `make check-net-link-host` (`python3 -B tools/tests/test_net_link.py --target`) —
  **29/29 PASS** + ホスト GNU89 `-Werror` + `i386-elf-gcc -Werror` の両コンパイル。
  実 `net/link.c` + 実 `kapi/kapi_host.c` を `#include` し、NIC (RX キュー・
  **1 tick 1 フレーム**の TX 受理・tick)、`cli`/`sti` (IF=0 中のタイマ延期と復元直後の
  tick、リング更新 / TX 構築の途中の贋 IRQ5)、ディスパッチャの早期検査
  (`kapi_argptr` は**生成物から読んで** `-D` で渡す) を贋物にし、対向は
  **実 Agent をサブプロセス**か台本。往復 2 の R1 / R3 / R4 / R6 / R8 / R9 / R10、
  往復 3 の B1 / B6 / B7 / B8 と TOMBSTONE / NO_SLOT、往復 4 の R4 と HELLO 各段、
  CPL=0 経路、owner 回収、`host_read` の「最後の 1 回は正の長さ・次が 0」、
  `host_write` の宣言長契約、リングの排他、EOF だけでは完了にしない、Go-Back-N。
- ソース本文の番人 2 件: `exec_reclaim_owned` の `host_owner_exit` の位置、
  `timer_handler` が `ne2k_timer_tick()` の直後に `link_tick()` を呼び、
  KAPI / `net/link.c` の中からは呼ばず、反射モードで `link_init` を止めていること。
- `python3 tools/check_kapi_version.py` — v51 で 4 か所一致、`KAPI_SPEC.md` の関数表も一致。
- 個別コンパイル (`i386-elf-gcc -Wall -Wextra -Werror`): `net/link.c`、`kapi/kapi_host.c`、
  `exec/exec.c`、`kernel/isr_handlers.c`、`drivers/lgy98.c`、`kapi/kapi_generated.c`。
  `userland/tests/host_test.c` はユーザランドのフラグで警告 0。

**確認していないこと** ([V4] — 実行していないので合否を主張しない):

- `make all` / `make clean` / `make check` / `make external` を**一度も走らせていない**
  (票の禁止事項)。KAPI 構造体が変わるので [ABI3] の clean rebuild は PM の担当。
- 配備 (`make deploy*`) とエミュレータ (NP21/W、`/api/*`、MCP) は**一切触っていない**。
- `userland/tests/host_test.c` は**コンパイルしか確かめていない**。実機で 1 度も
  走らせていないので、ゲスト項目 (`GET /pattern/65536` の AGAIN ループ、404、TIME、
  `ECHO 5` + write、Agent 再起動 → STALE → close → open) は**未検証**。
  Agent 再起動のケースは運用者が Agent を落とす必要があるので `host_test stale` で分けた。
- `check-net-l0`〜`l3` と `check-net-m2` は**実行していない** (ゲスト観測)。
  読むシンボルの意味は保つよう作ったが、実測での回帰は取っていない。
  v1 で合格していた L0〜L3 の実績は、ワイヤが変わったので**取り直しが要る**。
- 実 NIC / 実エミュレータ上でのタイミング (100Hz で RTO 200ms・T_probe 1 秒が
  足りるか、1 tick 1 フレームで L1 の 200 フレームが時間内に流れるか) は**未測定**。
  ホスト試験の贋 NIC は「1 tick 1 フレーム」を再現しているが、実機の遅延は入っていない。
- GUI 配下 (`gui_busy` と同時) は未検証。
- `host_agent.py` の実 HTTP 経路 (`GET http://...`) はホスト試験では `--offline` で
  止めているので、**実際にネットワークへ出る経路は 1 度も動かしていない**。

**PM が着地時に注意すべき共有ファイル**: `sdk/kapi.json` (末尾 5 本 + version 51 +
includes に `kapi_host.h`)、`build/sdk.mk` (`check-host-agent` / `check-net-link-host` を
`check` の列と `.PHONY` に追加)、`build/kernel.mk` (`kapi/kapi_host.c` を KERNEL_SRC へ)、
`build/app.conf` / `userland/deploy.yaml` (`host_test`)、`docs/KAPI_SPEC.md` (題名 v51・
§3-2 の v51 行・§4 の関数表 5 行 + data_fields を 0x35C / 0x360 へ)、`README.md` /
`docs/INDEX.md` (版数)、`sdk/include/os32/os32_kapi_shared.h` (`KAPI_VERSION`)、
`exec/exec.c` (`exec_reclaim_owned` の 1 行)、`kernel/isr_handlers.c` (`timer_handler`)。

### PM 記録 (2026-09-14)

- 着地 `995bb19` (基点 `af48990`、削除ファイル無し、conflict 無し) + `f5dca53` (`INC_KAPI` に `-Inet`: `kapi/kapi_host.c` の `link.h` が見つからず最初の `make all` が落ちた)。
- テスター: `make clean` → `make all` exit 0 (71s) → `make check` exit 0 (53s、`check-host-agent` / `check-net-link-host` を含む) → `make external` exit 0 (7s)。合否は `tools/emu_agent/logs/n1-build2/steps.jsonl` の obs で判定 (モデルの最終出力は unparseable だったが obs は 3 本とも exit=0)。
- 未実施: ゲスト受入 (§2)、Codex / 設計者の実装レビュー。v1.4 の体制 (ROLES §0) により、ゲスト受入は実装 PM の最初の仕事。

### PM ゲスト受入 (2026-09-14、kernel-lgy98-link を NHD 配備、Agent v2 を WSL2 で待受)

| 項目 | 結果 | 根拠 |
|---|---|---|
| 配備 | OK (`/boot/vmkernel.lz4` 475,863 B 一致) | `os32-cycle deploy`、session `n1-deploy` |
| kselftest | **86 / 1** (従来 87 / 0) | 落ちた項目 = `db_v50_selftest` bit 0 (`KAPI_SLOT_COUNT != 208`、`db_error_code` が末尾) — v51 の追記で崩れる検査。**修正対象 F1** |
| 起動時自己試験 (Agent 側) | L0 10 往復、L1 102,400 B、L2 131,072 B、L3 65,536 B / 404 / 実 HTTP 559 B / TIME 19 B すべて業務 RESPONSE まで通り、DATA も最後まで ACK (pcap: rid 11 ack=200、rid 12 ack=256、rid 13 ack=128) | `scratchpad/n1_link.pcap` (Agent 受信側)、`host_agent.log` |
| `check-net-l3` | OK (0 failures) | session `n1-lnet2` |
| `check-net-l0` | FAIL: `rt_ok=16` (期待 10) | `link_rt_ok` が業務 RESPONSE の総数 (L1〜L3 を含む) に変わった。**F2**: L0 専用の計数を足すか試験の期待を改める |
| `check-net-l1` / `l2` | FAIL: `l1_recv=131/200`、`l1_bytes=66114`、`l2_read=66114/131072` — **配送は完了しているのに計数が途中で止まる** (L1 と L2 が同じ 66114) | pcap の ACK 進行と矛盾 → **F3**: 計数の更新経路の欠陥 (配送ではない) |
| `link_retransmits=32` | pcap 上は REQUEST 16 / RELEASE 16 で再送ゼロ | **F4**: open / close 直後、NIC が初回送信を受理する前に RTO 期限を数えて `retries` / `retransmits` を増やしている疑い (`net/link.c` の `since(last_tx_tick)` / `since(rel_tick)` が未送信でも走る)。契約「RTO は NIC 受理 tick から」に反し再送予算が 1 減る |
| WINDOW | 1 tick に 2 通 (同じ ack で credit 14 → 20) が常態 | **F5** (non-blocker): 制御枠「rid ごと 1 周回 1 本」に反する。credit の揺れ 14 / 16 / 20 の由来も記録する |
| `hsync` → `host_test` (CPL=3、KAPI v51) | **PASS 26 / 26** (GET 65536 の AGAIN ループ、404、TIME、ECHO + write、不正引数) | `/api/cmd` の出力 |
| `host_test stale` (Agent 再起動 → STALE → close → open) | **未実施**: NP2NETSOCK は NP21/W が connect する向きで、Agent を落とすと再接続の有無が不明 (実施には NP21/W 再起動を伴う)。ホスト TDD の R6 ケースで代替 | — |
| `stat` (S3 の観測手段) | `/` = `dev=1(hd0) ino=2`、`settings.db` ino=273 / `.bak` ino=272 (別 inode)、`/nope` はエラー行 | `/api/cmd` の出力 |
| GUI 配下で `gui_busy` と同時の `host_test` | 未実施 (F1〜F4 の修正後に) | — |

F1〜F5 は Codex の実装レビュー所見と合わせて N1 のコーダーへ戻す (修正票 = 本票 §4)。
