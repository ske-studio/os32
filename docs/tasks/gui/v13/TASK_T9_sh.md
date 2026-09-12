# T9 — shell script: 常駐シェルの内蔵コマンドとスクリプトを端末から (設計草案)

状態: **設計草案 第 4 版 (2026-09-12、PM) — 第 3 版の再レビューで blocker 1 件 (§7)。D3 / D4 を書き換えて再レビュー待ち。実装は発注していない。**
親: [PLAN.md](PLAN.md) §1 (決裁 B: … → T7 → T8 → **shell script** → 設定 S0〜)。
前提: K6C / K7 / T7 / T8 (端末、con_sink、kbd 待ちと poll の park、全画面)。すべて main `a9aa0e4`。

## 0. いま起きること (確認済み)

- `ls` `cat` `cd` `env` `run` (スクリプト) 等の内蔵コマンドは **常駐シェル `shell.bin` の中の C 関数** (`userland/shell/*.c`、
  `execute_command()` `main.c:658`、表 `ShellCmd`)。GUI 中は `os32gui` が常駐シェルを終わらせ、起動ループが gshell を同じ
  シェル帯 0x300000 に載せる (`cmd_sys.c:225`) ので、**GUI 中に常駐シェルは存在しない**。端末 (T7) が起動できるのは
  `/usr/bin` `/bin` の外部バイナリだけで、内蔵コマンドは打てない。
- `shell.bin` は他の外部プログラムと**同じソース・同じフラグ** (`PROGRAM_FLAGS`、crt0) で、違いはリンカスクリプト
  (`sdk/link/app_sys.ld` = 0x300000 帯) だけ (`build/programs.mk:36`)。65,680 B。
- 制約 (K5b D9-8、`exec/exec.c:1569,1652`): CPL=3 アプリからの `exec_run` は「子が終わるまで塞ぐ」入れ子で、
  **その子は park できない** (単一カーネルスタック、`ring3_park_reject_count`)。GUI 中に入れ子の子が `kbd_getchar` を
  呼ぶと `hlt` に落ちて協調型全体が止まる (T8-3 K の実装メモでも実測)。`exec_start` (塞がない起動) は owner 1 (WM) 専用。
- `exec_app_state(id)` (KAPI v44) は所有者制限なしで状態 (0 FREE / 1 RUNNING / 2 PARKED / 3 WAIT_KEY / 4 WAIT_POLL) を返す。

## 1. 設計 (PM 案、レビュー対象)

**方針: 常駐シェルのソースをそのまま CPL=3 の外部アプリ `sh.bin` としてもビルドし、端末から `sh` で起動する。**
内蔵コマンド・環境変数・スクリプト (`run`) は同じコードが動く。外部プログラムだけは入れ子 `exec_run` を使えないので、
**端末 (WM) に起動してもらい、終わるまで譲りながら待つ**。

| # | 決定 | 担当 |
|---|---|---|
| D1 | `build/programs.mk` に `userland/sh.bin` を足す: `SHELL_SRC` + crt0 を `sdk/link/app.ld` (0x500000) でリンク、`-DSHELL_AS_APP`。`build/app.conf` に `userland/sh` (KAPI は shell と同じ、heap は `mem` 相当の既定)、`userland/deploy.yaml` に `/bin/sh.bin`。`shell.bin` (常駐) は無変更で同じソース | ビルド系 |
| D2 | `SHELL_AS_APP` の条件分岐 (`userland/shell/*.c`): (a) `shell_rshell_init()` を呼ばない (シリアル / `rshell_active` のタイムアウトを触らない)、(b) `os32gui` `rshell` `filer` (TUI、カーソル位置依存) は `sh: cui only` で拒否、(c) 履歴ファイルは `~/.sh_history` 相当の別名 (常駐の履歴を壊さない)、(d) `exit` で `shell_run()` を抜けて 0 で終了、(e) プロンプトは `sh> ` (どちらで打っているか分かるように) | S (シェル) |
| D3 | **外部プログラムの起動はカーネル仲介の明示プロトコル** (KAPI v49、末尾追記 5 本)。**要求表は要求者 ID ごとに 1 本 (ID 2〜5 の 4 本)** で、con_sink のリングには載せない (あふれで消えない)。(1) `launch_req(const char *cmdline) -> i32 token / 負`: 呼び手は **OS32X 宣言 `OS32X_FLAG_LAUNCHER` (0x0020、app.conf 4 列目 `launcher`) を持つ CPL=3 アプリ** (`sh.bin` と **端末 `t5a_display`** に立てる)。カーネルは要求者を `res_owner_get()` で記録し (自己申告ではない)、その要求者の表が空でなければ `OS32_ERR_FULL`。(2) `launch_take(char *buf, u32 cap, i32 *requester) -> token / 0` と (3) `launch_report(token, i32 rc)`: **owner 1 (WM) 専用**。WM は OP_WAIT の周期で `launch_take` を見て (要求者 ID 昇順に 1 本ずつ)、あれば **既存の `run_program` (Run ダイアログと同じ入口: `cui only` / 宣言 / 4 本上限の規則)** で `exec_start` し、その戻り `rc` を `launch_report` で返す。カーネルは `rc > 0` (park した子) なら **子 ID を表に確定** (`RUNNING(id)`) し、その ID の `exec_reclaim_owned` で `DONE` に、`rc == 0` (park 前に終了した短命な子) は **即 `DONE`**、`rc < 0` は `FAILED(rc)`。(4) `launch_poll(token, i32 *status) -> 0`: **要求者だけ**が読める (`PENDING` / `RUNNING(child id)` / `DONE` / `FAILED(rc)`)。`DONE` / `FAILED` を読んだら表は解放。要求者が先に畳まれたら表は `exec_reclaim_owned` で捨てる (子は残る)。(5) `sys_yield` (D5) | K |
| D3a | **`sh` 側** (`try_exec()` `main.c:296`、`SHELL_AS_APP` 時): 候補パス (`/usr/bin/<名>.bin` → `/bin/<名>.bin`、既存の探索) を解決して `launch_req(絶対パス + 引数)`。`FAILED` は `sh: <名>: launch failed (rc)`。待ちは **`sys_yield()` → `launch_poll()` の繰り返し**で、`kbd_getchar` / `kbd_trygetchar` は呼ばない (子の打鍵を横取りしない)。`DONE` で `sh> ` に戻る | S |
| D4 | **端末 (T7-A) も同じ要求表で起動する**: T7 の E3 (候補パスの解決) の後、`session_launch` の代わりに **`launch_req(絶対パス + 引数)`** を出し、接続モードに入る。既存の 100ms タイマで `launch_poll(token)` を読み、`RUNNING(id)` で子 ID を知り、**`DONE` でプロンプトへ戻る** (短命な子は最初の poll で `DONE`)。`FAILED(rc)` は `launch failed (rc)` を出してプロンプトへ (gshell のモーダルは従来どおり出る)。`EXIT` レコードは表示用にとどめ、モード判定には使わない (あふれで消えても影響なし)。`START` レコードは**設けない**。`session_launch` は端末から使わなくなる (libos32gui の口は残す)。孫 (sh が起動した子) は sh の表で追われ、端末の表は sh の `DONE` まで `RUNNING` のまま | A (+ K は D3 の表) |
| D5 | **KAPI v49 `sys_yield(void)`**: GUI 中 (con_sink 有効) は `kbd_trygetchar` と同じ tick 制限 (共有の控え) で park するが、**印は専用の `parked_from_yield`** (状態は `WAIT_POLL` = 4 のまま — WM の扱いは同じ最下位)。`exec_resume` は印が `parked_from_yield` なら **注入リングを読まず EAX = 0** で起こす (`parked_from_poll` のときだけ `kbd_inject_take`)。`resume_check` の印対応表に `WAIT_POLL` → {`parked_from_poll`, `parked_from_yield`} を足す。CUI 中は `hlt` 1 回。カウンタ `ring3_yield_count` | K |
| D6 | スクリプト (`run file`): 各行は `execute_command()` なので内蔵はそのまま、外部行は D3 で端末経由。`$VAR` 展開・`if`/`goto` 等の既存機能はそのまま | — |
| D7 | 8MB では `sh` (段 2、数百ページ) + 子は入らない (T7 の 8MB と同じ、仕様どおり)。15MB 以上が対象 | — |

メモリ: `sh.bin` ≈ 66KB + 既定ヒープ、要求表 4 本 (要求者 / 子 ID / 状態 / cmdline 256B ≈ 1.1KB)、印 1 語 × 5。con_sink のレコード型は増やさない。KAPI v49 は `launch_req` / `launch_take` / `launch_report` / `launch_poll` / `sys_yield` の 5 本 (KAPI_SPEC §3-2 の予約を更新)。宣言ビット `OS32X_FLAG_LAUNCHER` 0x0020 (T8 の `gfx` / `cui` と同じ仕組み、app.conf `launcher`)。

## 2. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| S1 | 内蔵 | 端末で `sh` → `sh> ` → `ls /` `cat /etc/system.cfg` `cd /usr` `pwd` `env` の出力が端末に出る |
| S2 | 外部 | `sh> kbd_echo` → 子が端末経由で起動し打鍵が届き、`q` で戻ると **`sh> ` に戻る** (`sh` は待っている間 `WAIT_POLL` で譲り、子は `WAIT_KEY`) |
| S3 | スクリプト | `run /test/hello.sh` (内蔵 + 外部の混在、無ければ用意) が最後まで流れる |
| S4 | 拒否 | `sh> os32gui` / `filer` / `rshell` → `sh: cui only` |
| S5 | 終了 | `exit` で `sh` が終わり端末のプロンプト `> ` に戻る。ESC (接続モード) でも戻れる |
| S6 | CTRL+STOP | 子が走っている最中の CTRL+STOP は子だけを畳み、`sh> ` に戻る |
| S7 | 回帰 | regress 6 本、CUI の `shell.bin` は無変更 (同じソースなので `SHELL_AS_APP` 無しのビルドの同一性をサイズで確認)、Start → CUI mode |

## 3. レビューで見てほしい点 (第 4 版)

1. **同じソースの二重ビルド** (D1/D2) で常駐シェルを壊さない担保 — `#ifdef` の範囲を最小にし、`shell.bin` のサイズ / ハッシュが変わらないことを受入 S7 に入れる。
2. **D3 の権限と表**: 起動要求を出せるのは宣言 `LAUNCHER` を持つアプリ (`sh.bin`、`t5a_display`) だけ。表は要求者 ID ごとに 1 本で、状態は con_sink のリングに依存しない。
3. **D4**: 端末の起動を `session_launch` から `launch_req` に切り替える点 (Run ダイアログ / Start メニューは従来どおり `set_wm_launch`)。WM 側は `launch_take` の処理を SessionAction の `LAUNCH` と同じ `run_program` に合流させ、同じ周に両方あるときは SessionAction を先にする。
4. **D5**: `sys_yield` と `kbd_trygetchar` の tick 制限を共有する点、`WAIT_POLL` の印を 2 種に分ける点。

## 5. 独立レビュー第 1 版 (2026-09-12) — Request changes

- blocker 1: 第 1 版 D3 の「`exec_app_state(2..5)` の走査で子を追う」は、`exec_start` が park 前に終了した子を `0` で返し ID を回収済みにする既存契約のため、**短命な子を取り逃がして 2 秒後に `launch failed` になる**。→ 第 2 版: WM が `launch_report(rc)` で結果を明示的に返し、`rc == 0` は即 `DONE` (D3)。
- blocker 2: 第 1 版 D3 の「PRINT に `\x1e LAUNCH`」は con_sink のレコードに送信元 ID が無く、**任意のアプリが端末経由の非同期起動を使えてしまう**。→ 第 2 版: カーネルの要求表が `res_owner_get()` で要求者を記録し、宣言 `LAUNCHER` を持つアプリだけ受け付ける (D3)。端末は起動要求を解釈しない (D4)。
- 二重ビルドと `sys_yield` の方向性に blocker なし。

## 6. 独立レビュー第 2 版 (2026-09-12) — Request changes

- blocker 1: 第 2 版 D5 の `sys_yield` が `WAIT_POLL` の印をそのまま使うと、`exec_resume` が `parked_from_poll` の再開で `kbd_inject_take()` を行うため、**sh が譲っている間に子向けの打鍵が入ると sh がその 1 バイトを吸って捨てる**。→ 第 3 版: 専用の印 `parked_from_yield` を足し、resume で注入リングを読まない (D5)。
- blocker 2: 第 2 版 D4 の「自分以外に生存アプリがあれば接続維持」は、端末 + sh + GUI アプリ A のとき **sh が exit しても A が生きていてプロンプトへ戻れない**。→ 第 3 版: カーネルが `START id` レコードを積み、端末は自分が起動した sh の ID を確定して、その ID の `EXIT` だけで戻る (D4)。
- 前回の 2 件 (短命な子、任意アプリの起動) は `launch_report(rc)` と `LAUNCHER` + 要求表で解消と判定。

## 4. 範囲外

- 設定レジストリ (S0〜)。端末の複数化。`filer` の端末化 (カーソル位置レコードの解釈)。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。

## 7. 独立レビュー第 3 版 (2026-09-12) — Request changes

- blocker: 第 3 版 D4 の `START` レコードは con_sink の drop-oldest リングに載るため、**子が `exec_start` の戻り前に 8KB 超を出力すると `START` が捨てられ、端末は子 ID を永久に確定できない** (短命な大量出力なら `EXIT` だけ見える)。→ 第 4 版: `START` を廃止し、端末も要求表 (`launch_req` / `launch_poll`) で子 ID と `DONE` を問い合わせる (D3 の表を要求者 ID ごとに 4 本へ、D4)。制御情報はリングに載せない。
- `parked_from_yield` と「sh 自身の ID を追う」方向は妥当と判定。
