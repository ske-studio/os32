# T9 — shell script: 常駐シェルの内蔵コマンドとスクリプトを端末から (設計草案)

状態: **設計草案 第 5 版 (2026-09-12、PM) — 第 4 版の Codex レビューで blocker 5 件 (§8)。D3〜D5 を書き換え D8 / D9 を追加して再レビュー待ち (Codex 往復 1/3)。実装は発注していない。**
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
| D1 | `build/programs.mk` に `userland/sh.bin` を足す: `SHELL_SRC` + crt0 を `sdk/link/app.ld` (0x500000) でリンク、`-DSHELL_AS_APP`、**`.o` は `userland/shell/sh_obj/` 等の専用出力先** (常駐の `SHELL_OBJ` と混ぜない)。`build/app.conf` に `userland/sh` (**KAPI 49**、heap は既定、4 列目 `launcher`)、端末 `t5a_display` も `launcher` + 49、gshell も 49。`userland/deploy.yaml` に `/bin/sh.bin`。`shell.bin` (常駐) は無変更で同じソース — **受入 S7 で `shell.bin` の SHA-256 が変更前後で一致**すること (サイズ一致では不十分) | ビルド系 |
| D2 | `SHELL_AS_APP` の条件分岐 (`userland/shell/*.c`): (a) `shell_rshell_init()` と **`shell_run()` 内の自動 `serial_init`** (`ui.c:485`) を呼ばない (シリアル / `rshell_active` のタイムアウトを触らない)、(b) `os32gui` `rshell` `filer` (TUI、カーソル位置依存) は `sh: cui only` で拒否、(c) 履歴ファイルは `~/.sh_history` 相当の別名 (常駐の履歴を壊さない)、(d) `exit` で `shell_run()` を抜けて 0 で終了、(e) プロンプトは `sh> ` (どちらで打っているか分かるように) | S (シェル) |
| D3 | **外部プログラムの起動はカーネル仲介の明示プロトコル** (KAPI v49、末尾追記 7 本、ABI の詳細は §1a)。**要求表は要求者 ID ごとに 1 本 (ID 2〜5 の 4 本)** で、con_sink のリングには載せない。(1) `launch_req(cmdline)`: 呼び手は OS32X 宣言 `OS32X_FLAG_LAUNCHER` (0x0020、app.conf `launcher`) を持つ CPL=3 アプリ (`sh.bin` と端末)。要求者は `res_owner_get()` で記録。**GUI 中 (con_sink 有効) でなければ `OS32_ERR_INVAL`** (CUI では WM が居ないので受けない)。(2) `launch_pending()`: 誰でも呼べる、未処理の要求 (起動 / 取消) の本数。**gshell は `should_park` の (a) に `launch_pending() > 0` を足し** (`st.launch_pending` / `has_top_level_work` と同列)、走っているアプリを park させて top-level へ戻る。(3) `launch_take(buf, cap, &requester, &kind, &arg)`: **owner 1 (WM) 専用、top-level でだけ呼ぶ**。`kind` = `LAUNCH` (cmdline を返す) / `CANCEL` (`arg` = 畳む子 ID)。取得済みの要求は再取得しない (状態 `PENDING` → `TAKEN`)。(4) `launch_report(token, rc)`: owner 1 専用。`LAUNCH` の `exec_start` の戻り `rc` を返す: `rc > 0` → `RUNNING(child)`、`rc == 0` (park 前に終了した短命な子) → `DONE`、`rc < 0` → `FAILED(rc)`。`CANCEL` は `exec_kill(child)` の後に `launch_report(token, 0)` → `DONE`。(5) `launch_poll(token, &status)`: 要求者だけ。(6) `launch_cancel(token)`: 要求者だけ、`RUNNING` の子の取消を要求表に積む (`kind = CANCEL`、`launch_pending` が増える)。(7) `sys_yield` (D5)。**回収**: `RUNNING(child)` の子が `exec_reclaim_owned(child)` を通ったら `DONE` (正常終了は AppSlot 解放より前の (n) 番目、`exec_kill` も同じ関数を通る — 番号は K で確定)。要求者が先に畳まれたら **その要求表の子も `kill` の連鎖 (D8) で畳む** (孤児を残さない) | K + W |
| D3a | **`sh` 側** (`SHELL_AS_APP` 時): 外部プログラムへ至る経路は **`try_exec()` (`main.c:296`) だけでなく内蔵 `exec` (`cmd_mnt.c:59`) と `filer` の起動 (`cmd_filer.c:314`) も `exec_run` を直接呼ぶ**ので、`exec_run` の呼び出しを 1 つの関数 `sh_launch(cmdline)` に集約し、`SHELL_AS_APP` では **`exec_run` を一切呼ばない** (`grep exec_run userland/shell` を受入 S7 で 0 件にする — `SHELL_AS_APP` のビルドで)。`sh_launch` = 候補パス解決 → `launch_req` → `sys_yield()` + `launch_poll()` の繰り返し → `DONE` で戻る / `FAILED(rc)` は `sh: <名>: launch failed (rc)`。待ちの間 `kbd_getchar` / `kbd_trygetchar` は呼ばない。`filer` は D2(b) で拒否 | S |
| D4 | **端末 (T7-A) も同じ要求表で起動する**: T7 の E3 の後、`session_launch` の代わりに **`launch_req(絶対パス + 引数)`** を出し token を保持して接続モードへ。100ms タイマで `launch_poll(token)` を読み、`RUNNING(id)` で子 ID を知り、**`DONE` でプロンプトへ戻る** (短命な子は最初の poll で `DONE`)。`FAILED(rc)` は `launch failed (rc)` を出してプロンプトへ。**接続モードの ESC は `launch_cancel(token)`** (子と、その要求表の孫まで D8 の連鎖で畳まれる) → `DONE` を待ってプロンプトへ (第 4 版までの「ESC で子を残したままプロンプト」は廃止。孤児と `ERR_FULL` の固着を防ぐ)。`EXIT` レコードは表示用にとどめモード判定には使わない | A |
| D5 | **KAPI v49 `sys_yield(void)`**: GUI 中は **必ず** `WAIT_POLL` に park する (tick 制限なし — 明示的な譲りは呼び手の意思。印は専用の `parked_from_yield`、resume は注入リングを読まず EAX = 0)。park できない文脈 (CPL=0、フレーム無し、入れ子 `exec_run` の子) では `hlt` 1 回して 0。CUI 中は `hlt` 1 回。カウンタ `ring3_yield_count`。**WM 側の公平性 (gshell `pick_poll`)**: `WAIT_POLL` 群は **ID 昇順の固定ではなく巡回** (前回起こした ID の次から)、かつ **同じ tick に同じアプリを 2 回起こさない** (WM が `get_tick` を控える。起こす相手が全員「この tick で起こし済み」なら `sys_halt` で次の tick を待つ) — sh の `sys_yield` と子の `kbd_trygetchar` が同時に `WAIT_POLL` でも tick ごとに交互に走り、飢餓しない。`kbd_trygetchar` の kernel 側 tick 制限は従来どおり (子は 1 tick 走り続けてから譲る) | K + W |
| D6 | スクリプト (`run file`): 各行は `execute_command()` なので内蔵はそのまま、外部行は D3 で端末経由。`$VAR` 展開・`if`/`goto` 等の既存機能はそのまま | — |
| D7 | 8MB では `sh` (段 2、数百ページ) + 子は入らない (T7 の 8MB と同じ、仕様どおり)。15MB 以上が対象 | — |
| D8 | **kill の連鎖**: `exec_kill(id)` (WM の CTRL+STOP 転送、SWITCH_CUI の畳み込み、D3 の CANCEL、要求者の退場) は、**その ID の要求表に `RUNNING(child)` があれば連鎖の末尾から畳む** (孫 → 子 → 本人)。CTRL+STOP の宛先はいまどおり「フォーカス窓のアプリ / 全画面の所有者」のままで、**カーネル側**で末尾へ転送する (`exec_abort_clear` → `exec_kill(focus)` の中)。端末にフォーカスがあるとき CTRL+STOP → 端末ではなく **走っている末尾 (kbd_echo) だけ**が畳まれ、`sh` はその `DONE` を受けて `sh> ` に戻る (受入 S6)。`sh` 自身を畳みたければもう 1 回 CTRL+STOP (次の末尾 = sh) | K |
| D9 | **接続モードの ESC = 取消** (D4): 端末は `launch_cancel(token)` を出して `DONE` を待つ。第 4 版までの「ESC でプロンプトへ戻るだけ」は廃止 (起動失敗でモーダルが出る場合も `FAILED` が返るので ESC は不要) | A |

メモリ: `sh.bin` ≈ 66KB + 既定ヒープ、要求表 4 本 (要求者 / 子 ID / 状態 / kind / token / cmdline 256B ≈ 1.1KB)、印 1 語 × 5。con_sink のレコード型は増やさない。

### 1a. KAPI v49 の ABI (K が実装、W / S / A が従う)

| 名前 | 引数 | 戻り | 権限 | 規則 |
|---|---|---|---|---|
| `launch_req` | `const char *cmdline` | token (> 0) / 負 | 宣言 `LAUNCHER` を持つ CPL=3 | cmdline は NUL 終端 1〜255B、超過 / 空 / GUI 外 / 入れ子 `exec_run` の子から / 自分の表が空でない → `OS32_ERR_INVAL` または `OS32_ERR_FULL`。token = (要求者 ID << 8) \| 世代 (1〜255、要求者ごとに増える) |
| `launch_pending` | — | 未処理 (`PENDING`) の要求数 | 誰でも | 0 なら WM は何もしない |
| `launch_take` | `char *buf, u32 cap, i32 *requester, i32 *kind, i32 *arg` | token / 0 (無し) / 負 | owner 1 | `cap < 256` → `OS32_ERR_INVAL`。`kind` = 1 LAUNCH (buf に cmdline) / 2 CANCEL (`arg` = 子 ID)。取得で `PENDING` → `TAKEN`。要求者 ID 昇順 |
| `launch_report` | `i32 token, i32 rc` | 0 / 負 | owner 1 | `TAKEN` 以外の token → `OS32_ERR_STALE`。LAUNCH: `rc > 0` は生きている非シェル ID でなければ `OS32_ERR_INVAL`、→ `RUNNING(rc)`; `rc == 0` → `DONE`; `rc < 0` → `FAILED(rc)`。CANCEL: → `DONE` |
| `launch_poll` | `i32 token, i32 *status` | 0 / 負 | 要求者 (token の ID と一致) | `status`: `0` PENDING / `1` TAKEN / `0x100 + child` RUNNING / `0x200` DONE / `0x300 + (-rc)` FAILED。`DONE` / `FAILED` を返した時点で表を解放 (再 poll は `OS32_ERR_STALE`) |
| `launch_cancel` | `i32 token` | 0 / 負 | 要求者 | `RUNNING` のときだけ受理 (`PENDING` / `TAKEN` は `OS32_ERR_AGAIN`)。`kind = CANCEL` として `PENDING` に戻す (`launch_pending` +1) |
| `sys_yield` | — | 0 | CPL=3 | D5。resume 後に印 `parked_from_yield` を消す |

回収通知: `exec_reclaim_owned(child)` の中で要求表の `RUNNING(child)` を `DONE` にする (正常終了 / `exec_kill` / fault / CTRL+STOP の 4 経路が通る、K6C の EXIT と同じ位置)。KAPI v49 は `launch_req` / `launch_take` / `launch_report` / `launch_poll` / `sys_yield` の 5 本 (KAPI_SPEC §3-2 の予約を更新)。宣言ビット `OS32X_FLAG_LAUNCHER` 0x0020 (T8 の `gfx` / `cui` と同じ仕組み、app.conf `launcher`)。

## 2. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| S1 | 内蔵 | 端末で `sh` → `sh> ` → `ls /` `cat /etc/system.cfg` `cd /usr` `pwd` `env` の出力が端末に出る |
| S2 | 外部 | `sh> kbd_echo` → 子が端末経由で起動し打鍵が届き、`q` で戻ると **`sh> ` に戻る** (`sh` は待っている間 `WAIT_POLL` で譲り、子は `WAIT_KEY`) |
| S3 | スクリプト | `run /test/hello.sh` (内蔵 + 外部の混在、無ければ用意) が最後まで流れる |
| S4 | 拒否 | `sh> os32gui` / `filer` / `rshell` → `sh: cui only` |
| S5 | 終了 | `exit` で `sh` が終わり端末のプロンプト `> ` に戻る。接続モードの ESC は `launch_cancel` で sh (と孫) を畳んでからプロンプトへ (D9)。その後の起動要求が `ERR_FULL` にならない |
| S6 | CTRL+STOP | 端末にフォーカスがある状態で子が走っている最中に CTRL+STOP → 連鎖の末尾 (子) だけが畳まれ `sh> ` に戻る (D8)。もう 1 回で sh が畳まれ端末のプロンプトへ |
| S7 | 回帰 | regress 6 本、CUI の `shell.bin` は変更前後で **SHA-256 一致**、`SHELL_AS_APP` ビルドの `exec_run` 参照 0 件 (nm)、Start → CUI mode (sh と子が生きていても D8 で畳まれる) |

## 3. レビューで見てほしい点 (第 5 版)

1. **D3 の受け渡し**: `launch_pending()` を `should_park` の理由に足して top-level で `launch_take` / `run_program` / `launch_report` を行う点 (K5b の `launch_pending` と同じ形)。
2. **D5 の公平性**: `sys_yield` は常に park、WM の `pick_poll` は巡回 + 同一 tick 1 回で、sh と子 (ポーリング型) が交互に走ることの確認。
3. **D8 の kill の連鎖**をカーネル側に置く点 (WM の宛先規則は不変)。要求者の退場で子も畳む (孤児を作らない)。
4. **D9 の ESC = 取消**で、第 4 版までの「戻り道」が要らなくなること (`FAILED` は poll で返る)。
5. §1a の ABI (token の符号化、状態遷移、エラー値) に穴がないか。

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

## 8. Codex レビュー 第 4 版 (2026-09-12、`codex exec -s read-only`、gpt-6-astra) — Request changes (往復 1/3)

blocker 5 件はいずれも PM が実コードで到達可能と確認:
1. 要求を WM top-level へ渡す経路が無い (`should_park` に起動要求が無い、`launch_take` は owner 1 専用) → D3 に `launch_pending()` と `should_park` の理由を追加。
2. `WAIT_POLL` 群で `pick_poll` が ID 昇順固定のため sh が子を飢餓させる → D5: `sys_yield` は常に park、WM は巡回 + 同一 tick 1 回。
3. 内蔵 `exec` (`cmd_mnt.c:59`) と `filer` (`cmd_filer.c:314`) が `exec_run` を直接呼ぶ → D3a: `exec_run` 呼び出しを `sh_launch` に集約し `SHELL_AS_APP` では 0 件。
4. CTRL+STOP がフォーカス窓 (端末) を畳み sh と子が孤児になる → D8: カーネル側で要求表の連鎖の末尾へ転送。
5. 接続モードの ESC で sh が残り要求表が `RUNNING` のまま固着 → D9: ESC = `launch_cancel`、`DONE` を待ってプロンプトへ。

non-blocker 3 件も反映: sh 専用の `.o` 出力先とハッシュ比較 (D1、S7)、`shell_run()` の自動 `serial_init` も `SHELL_AS_APP` で外す (D2a)、ABI 表 (§1a)、sh / 端末 / gshell の要求版を v49 に (D1)。宣言 `LAUNCHER` は認証ではなく協調的な宣言であり、`session_launch` (GUI アプリの経路) はそのまま — と明記。
