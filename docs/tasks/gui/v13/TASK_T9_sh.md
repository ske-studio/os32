# T9 — shell script: 常駐シェルの内蔵コマンドとスクリプトを端末から (設計草案)

状態: **独立レビュー通過 (第 6 版、Codex 3 往復目で Approve、2026-09-12、§10)。non-blocker 4 件は実装要件に取り込み済み。K / ビルド系 → S / W / A の順で発注。**
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
| D3 | **外部プログラムの起動はカーネル仲介の明示プロトコル** (KAPI v49、末尾追記 **8 本**、ABI は §1a)。**要求表は要求者 ID ごとに 1 本 (ID 2〜5 の 4 本)**。表の欄は **配送状態と子の所有を分ける**: `phase` (IDLE / PENDING / TAKEN / RUNNING / DONE / FAILED)、`kind` (LAUNCH / KILL)、`child` (所有する子 ID、0 = 無し。**取消や退場の途中でも消さない**)、`token` (32bit、全体で単調増加)、cmdline。(1) `launch_req(cmdline)`: 宣言 `LAUNCHER` を持つ CPL=3 だけ、要求者は `res_owner_get()` で記録、GUI 外や入れ子 `exec_run` の子からは `OS32_ERR_INVAL`、自分の表が IDLE でなければ `OS32_ERR_FULL`。(2) `launch_pending()`: 誰でも、`PENDING` の要求数。**gshell は `should_park` の (a) に `launch_pending() > 0` を足す** (K5b の `launch_pending` と同列)。(3) `launch_take(...)`: owner 1 専用、top-level でだけ。`kind` = LAUNCH (cmdline) / KILL (`arg` = 畳む ID)。`PENDING` → `TAKEN`。(4) `launch_report(token, rc)`: owner 1 専用。LAUNCH: `rc > 0` → `child = rc`、`RUNNING`; `rc == 0` → `DONE`; `rc < 0` → `FAILED(rc)`。KILL: WM が `exec_kill(arg)` を終えてから呼ぶ (`rc` は無視) — 表は **`child` の回収通知で** `DONE` になる (`launch_report` は取得済みの印を消すだけ)。(5) `launch_poll(token, &status)`: 要求者だけ。(6) `launch_cancel(token)`: 要求者だけ、`RUNNING` のとき `kind = KILL(child)`、`phase = PENDING` (child は保持)。(7) `launch_child(id) -> child / 0`: **誰でも**、その ID の表が所有する子 (連鎖の次)。WM が CTRL+STOP の宛先を末尾へ解決するのに使う (D8)。(8) `sys_yield` (D5)。**回収通知**: `exec_reclaim_owned(x)` の中で (a) `child == x` の表を `DONE` に (phase が PENDING / TAKEN の KILL 途中でも)、(b) `requester == x` の表 (要求者の退場) は **`child != 0` なら `kind = KILL(child)`、`phase = PENDING`、requester を「孤児回収」印に**して残す (WM の top-level が `launch_take` で受けて畳む。孤児の子は WM が畳むまで生きているが、要求者 ID は再利用されうるので **表の所有は requester ではなく token で照合**し、再利用 ID からの `launch_req` は「孤児回収が完了するまで `OS32_ERR_FULL`」)。解放済み AppSlot の欄は読まない (通知は ID だけ) | K + W |
| D3a | **`sh` 側** (`SHELL_AS_APP` 時): 外部プログラムへ至る経路は **`try_exec()` (`main.c:296`) だけでなく内蔵 `exec` (`cmd_mnt.c:59`) と `filer` の起動 (`cmd_filer.c:314`) も `exec_run` を直接呼ぶ**ので、`exec_run` の呼び出しを 1 つの関数 `sh_launch(cmdline)` に集約し、`SHELL_AS_APP` では **`exec_run` を一切呼ばない** (`grep exec_run userland/shell` を受入 S7 で 0 件にする — `SHELL_AS_APP` のビルドで)。`sh_launch` = 候補パス解決 → `launch_req` → `sys_yield()` + `launch_poll()` の繰り返し → `DONE` で戻る / `FAILED(rc)` は `sh: <名>: launch failed (rc)`。待ちの間 `kbd_getchar` / `kbd_trygetchar` は呼ばない。`filer` は D2(b) で拒否 | S |
| D4 | **端末 (T7-A) も同じ要求表で起動する**: T7 の E3 の後、`session_launch` の代わりに **`launch_req(絶対パス + 引数)`** を出し token を保持して接続モードへ。100ms タイマで `launch_poll(token)` を読み、`RUNNING(id)` で子 ID を知り、**`DONE` でプロンプトへ戻る** (短命な子は最初の poll で `DONE`)。`FAILED(rc)` は `launch failed (rc)` を出してプロンプトへ。**接続モードの ESC は `launch_cancel(token)`** (子と、その要求表の孫まで D8 の連鎖で畳まれる) → `DONE` を待ってプロンプトへ (第 4 版までの「ESC で子を残したままプロンプト」は廃止。孤児と `ERR_FULL` の固着を防ぐ)。`EXIT` レコードは表示用にとどめモード判定には使わない | A |
| D5 | **KAPI v49 `sys_yield(void)`**: GUI 中は **必ず** `WAIT_POLL` に park する (tick 制限なし — 明示的な譲りは呼び手の意思。印は専用の `parked_from_yield`、resume は注入リングを読まず EAX = 0)。park できない文脈 (CPL=0、フレーム無し、入れ子 `exec_run` の子) では `hlt` 1 回して 0。CUI 中は `hlt` 1 回。カウンタ `ring3_yield_count`。**WM 側の公平性 (gshell `pick_poll`)**: `WAIT_POLL` 群は **ID 昇順の固定ではなく巡回** (前回起こした ID の次から)、かつ **同じ tick に同じアプリを 2 回起こさない** (WM が `get_tick` を控える。起こす相手が全員「この tick で起こし済み」なら `sys_halt` で次の tick を待つ) — sh の `sys_yield` と子の `kbd_trygetchar` が同時に `WAIT_POLL` でも tick ごとに交互に走り、飢餓しない。`kbd_trygetchar` の kernel 側 tick 制限は従来どおり (子は 1 tick 走り続けてから譲る) | K + W |
| D6 | スクリプト (`run file`): 各行は `execute_command()` なので内蔵はそのまま、外部行は D3 で端末経由。`$VAR` 展開・`if`/`goto` 等の既存機能はそのまま | — |
| D7 | 8MB では `sh` (段 2、数百ページ) + 子は入らない (T7 の 8MB と同じ、仕様どおり)。15MB 以上が対象 | — |
| D8 | **kill の連鎖と CTRL+STOP の宛先**: `exec_kill(id)` は **「id とその子孫 (要求表の `child` を末尾まで辿る) を末尾から回収」に固定**する (K)。WM は kill 後に `exec_app_state` で FREE になった ID を全部 `forget` する (子孫は WM の表からも消す)。**CTRL+STOP は WM 側で末尾へ解決**: `abort_target()` = フォーカス窓のアプリ (全画面なら所有者) から `launch_child()` を末尾まで辿った ID。`abort_targets_current(cur)` は **`abort_target() == cur` のときだけ真** — 端末 (cur) が子を持つなら偽になり、端末自身の syscall 出口では畳まれず、`redirect_abort(末尾)` → top-level で `exec_kill(末尾)` (末尾は子孫を持たないので 1 本だけ畳まれる)。`sh` はその `DONE` を受けて `sh> ` に戻る (S6)。もう 1 回 CTRL+STOP で次の末尾 (sh)、さらにもう 1 回で端末 | K + W |
| D9 | **接続モードの ESC = 取消**: 端末は `launch_cancel(token)` → WM が `exec_kill(child)` (子孫ごと) → 表は `child` の回収通知で `DONE` → 端末は poll で `DONE` を見てプロンプトへ。`DONE` / `FAILED` に対する `launch_cancel` は `OS32_ERR_STALE`、`PENDING` / `TAKEN` (起動がまだ WM に取られていない / 取られて exec_start 中) に対しては `OS32_ERR_AGAIN` で端末は次のタイマで再試行 | A |

メモリ: `sh.bin` ≈ 66KB + 既定ヒープ、要求表 4 本 (requester / child / phase / kind / token 32bit / cmdline 256B ≈ 1.1KB)、印 1 語 × 5。con_sink のレコード型は増やさない。KAPI v49 は **8 本**。

### 1a. KAPI v49 の ABI (K が実装、W / S / A が従う)

共通: 出力ポインタは NULL 可 (書かない)。失敗時は出力を書かない。CPL=3 のポインタは既存のディスパッチャ検証。

| 名前 | 引数 | 戻り | 権限 | 規則 |
|---|---|---|---|---|
| `launch_req` | `const char *cmdline` | token (> 0) / 負 | 宣言 `LAUNCHER` を持つ CPL=3 | cmdline は NUL 終端 1〜255B (超過 / 空 → `OS32_ERR_INVAL`)。GUI 外 / 入れ子 `exec_run` の子から → `OS32_ERR_INVAL`。自分の表が IDLE でない (孤児回収中を含む) → `OS32_ERR_FULL`。**token = 32bit の全体単調増加カウンタ** (0 と負は使わない、上位 8bit に要求者 ID は入れない; `0x7FFFFFFF` に達したら `OS32_ERR_FULL` で拒否 = 事実上到達しない)。表は token で照合するので、周回や ID 再利用で取り違えない |
| `launch_pending` | — | `PENDING` の要求数 | 誰でも | 0 なら WM は何もしない |
| `launch_take` | `char *buf, u32 cap, i32 *requester, i32 *kind, i32 *arg` | token / 0 (無し) / 負 | owner 1 | `cap < 256` → `OS32_ERR_INVAL`。`kind` = 1 LAUNCH (buf に cmdline) / 2 KILL (`arg` = 畳む ID)。`PENDING` → `TAKEN`。要求者 ID 昇順 |
| `launch_report` | `i32 token, i32 rc` | 0 / 負 | owner 1 | `TAKEN` 以外 → `OS32_ERR_STALE`。LAUNCH: `rc > 0` は生きている非シェル ID でなければ `OS32_ERR_INVAL` → `child = rc`, `RUNNING`; `rc == 0` → `DONE`; `rc < 0` → `FAILED(rc)`。KILL: `rc` は無視、`TAKEN` の印だけ消す (`DONE` は `child` の回収通知で) |
| `launch_poll` | `i32 token, i32 *status` | 0 / 負 | 要求者 (表の requester と一致) | `status`: `0` PENDING / `1` TAKEN / `0x100 + child` RUNNING / `0x200` DONE / `0x300 + (-rc)` FAILED。`DONE` / `FAILED` を返した時点で表を IDLE に (再 poll は `OS32_ERR_STALE`) |
| `launch_cancel` | `i32 token` | 0 / 負 | 要求者 | `RUNNING` → `kind = KILL(child)`, `PENDING`; `PENDING` / `TAKEN` → `OS32_ERR_AGAIN`; `DONE` / `FAILED` / 不一致 → `OS32_ERR_STALE` |
| `launch_child` | `i32 id` | 子 ID / 0 | 誰でも | その ID の表の `child` (phase を問わず)。不正 ID → 0 |
| `sys_yield` | — | 0 | CPL=3 | D5。park できない文脈では `hlt` 1 回。resume 後に印 `parked_from_yield` を消す |

回収通知: `exec_reclaim_owned(x)` の中 (K6C の `con_sink_owner_exit` と同じ位置。**正常終了は AppSlot 解放の後、`exec_kill` は前**に通るので、通知は ID だけを使い AppSlot の欄を読まない) で、`child == x` の表を `DONE` に、`requester == x` の表を孤児回収 (`KILL(child)`, `PENDING`) に。

## 2. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| S1 | 内蔵 | 端末で `sh` → `sh> ` → `ls /` `cat /etc/system.cfg` `cd /usr` `pwd` `env` の出力が端末に出る |
| S2 | 外部 | `sh> kbd_echo` → 子が端末経由で起動し打鍵が届き、`q` で戻ると **`sh> ` に戻る** (`sh` は待っている間 `WAIT_POLL` で譲り、子は `WAIT_KEY`) |
| S3 | スクリプト | `source /test/hello.sh` (`userland/tests/data/hello.sh`、内蔵 `echo` `pwd` `env` + 外部 `klibc_test` の混在) が最後まで流れ `script done` で `sh> ` に戻る |
| S4 | 拒否 | `sh> os32gui` / `filer` / `rshell` → `sh: cui only` |
| S5 | 終了 | `exit` で `sh` が終わり端末のプロンプト `> ` に戻る。接続モードの ESC は `launch_cancel` で sh (と孫) を畳んでからプロンプトへ (D9)。その後の起動要求が `ERR_FULL` にならない |
| S6 | CTRL+STOP | 端末にフォーカスがある状態で子が走っている最中に CTRL+STOP → 連鎖の末尾 (子) だけが畳まれ `sh> ` に戻る (D8)。もう 1 回で sh が畳まれ端末のプロンプトへ |
| S7 | 回帰 | regress 6 本、CUI の常駐 `shell.bin` は変更前後で**同一** — `cmd_ver` が `__DATE__` / `__TIME__` を埋めるため SHA-256 は再現しないので、同一フラグの `SHELL_OBJ` 12 本のうち `cmd_base.o` 以外がバイト一致し、`cmd_base.o` の差が `__TIME__` 文字列だけであること (S の実装メモ、着地時に PM が clean build で再確認)。`SHELL_AS_APP` ビルドの `exec_run` 参照 0 件、Start → CUI mode (sh と子が生きていても D8 で畳まれる) |

## 3. レビューで見てほしい点 (第 6 版)

1. **D8**: `exec_kill(id)` = id と子孫の回収に固定し、CTRL+STOP は WM が `launch_child` で末尾を解決して `exec_kill(末尾)` (1 本だけ)。`abort_targets_current` は末尾 == cur のときだけ真。
2. **D3 の表**: 配送状態 (`phase` / `kind`) と子の所有 (`child`) を分け、取消・退場の途中でも `child` を保持。要求者の退場は「孤児回収」として WM の top-level が畳む (カーネルは回収文脈から kill しない)。
3. **token**: 32bit 全体単調増加、表は token で照合、枯渇は拒否。
4. **D9**: `launch_cancel` の `AGAIN` / `STALE` の規則と端末の再試行。
5. §1a の残り (NULL 出力、失敗時の出力なし、回収通知の順序)。

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

## 9. Codex レビュー 第 5 版 (2026-09-12) — Request changes (往復 2/3)

blocker 4 件、いずれも PM が妥当と判断して第 6 版に反映:
1. CTRL+STOP が端末の OP_WAIT 内で `abort_targets_current` 真 → 端末自身が畳まれ、カーネル側の転送 (第 5 版 D8) に到達しない → **WM 側で末尾を解決** (`launch_child`、`abort_targets_current` は末尾 == cur のときだけ)。
2. 「id と子孫を回収」と「末尾だけ停止」を同じ `exec_kill` に割り当てると WM の `forget` と CANCEL の `DONE` が壊れる → `exec_kill` は子孫ごとに固定、CTRL+STOP は末尾 1 本、kill 後は FREE を走査して forget。
3. 取消で `RUNNING(child)` から外れると要求者退場時に孤児 → `child` を配送状態と分けて保持、退場は孤児回収 (KILL) を WM に渡す。
4. 8bit 世代の token が 255 回で周回 → 32bit 全体単調増加、token で照合、枯渇は拒否。

non-blocker: KAPI は 8 本に統一、NULL 出力と失敗時の規則、回収通知は ID だけ (正常終了は AppSlot 解放後、kill は前)、clean build 双方で SHA-256 比較。

## 10. Codex レビュー 第 6 版 (2026-09-12) — **Approve** (往復 3/3)

blocker なし。non-blocker 4 件は実装要件として各票に入れる:
1. **要求表の回収完了処理** (K): 子の回収通知で `DONE` と同時に `child = 0`。孤児回収 (要求者退場) の完了時は poll を待たず `IDLE`。子を持たない要求者の退場も表を解放。再利用 ID への誤連鎖と `ERR_FULL` 固着を防ぐ。
2. **KILL 後の `launch_report`** (K / W): `launch_take` → `exec_kill(child)` → 回収通知で `DONE` → `launch_report` の順になると `TAKEN` でないので `STALE` を返す。WM はこれを再試行せず正常として扱い、カーネルは回収通知で取得済み情報も掃除する (ABI は変えない)。
3. **sh の行入力** (S): `shell_run` の `ime_getkey` (FEP を通る) ではなく、`SHELL_AS_APP` では **`kbd_getkey` / `kbd_getchar` の注入入力に統一** (FEP 確定は gshell 側で済んでいる。UTF-8 の後続バイトも同じ経路)。二重処理の疑いを消す。
4. **WM の新しい起動口** (W): `launch_take` の LAUNCH は `run_program` (`begin_start` / `end_start`、全画面判定・復帰) を通す。`exec_start` の直呼びはしない。

## 11. 実装メモ (ビルド系、D1 / D1a、2026-09-12)

- `build/programs.mk`: `SH_OBJDIR = userland/shell/sh_obj`、`SH_OBJ` を `-DSHELL_AS_APP` でそこへ吐き、`userland/sh.elf` は `PROGRAM_LDFLAGS` (= `app.ld` 0x500000) + `FILER_DRAW_OBJ` + `-los32save`。常駐の `SHELL_OBJ` / `userland/shell.elf` は 1 文字も変えていない (S7 の SHA-256 一致の根拠)。
- 4 列目 `launcher` → `--launcher` を `userland/%.bin` レシピに追加。`sdk/mkos32x.py` に `OS32X_FLAG_LAUNCHER = 0x0020` と `--launcher` (票 K と重複したら同内容なので片方を捨てる)。
- `build/app.conf`: `userland/sh 49 0 launcher` / `t5a_display 49 0 launcher` / `gshell 49`。`tools/check_manifests.py` は `launcher` を書式として許すだけ (呼び出しとの突き合わせは KAPI v49 着地後)。
- `programs:` に `sh`、`clean-programs` で `sh_obj/` と `userland/sh.{elf,raw,bin}` を掃除。`userland/deploy.yaml` に `/bin/sh.bin` (tags `programs`)。
- 未実施: 実ビルド・配備・実機 ([V4])。検証は `make -n sh` / `make -n programs` の dry-run と `check_gfx_flag()` / `check_constraints.py` の直接実行まで。`make check-manifests` の §2 は `userland/sh.bin` が実在してから通る。

## 12. 実装メモ (K、2026-09-12、着地 feat/gui)

- 要求表は `exec/launch.c` + `include/launch.h` (添字 = 要求者 ID、照合は token)。ワイヤ側の定数
  (`LAUNCH_KIND_*` / `LAUNCH_ST_*` / `LAUNCH_CMDLINE_MAX` / `LAUNCH_TOKEN_MAX`) と
  `OS32X_FLAG_LAUNCHER 0x0020` は `sdk/include/os32/os32_kapi_shared.h` が正典 ([C4])。
- KAPI v49 = 8 本、スロット 193〜200 (`0x30C`〜`0x328`)。`sys_yield` の実体は `exec_sys_yield`。
  データフィールドは `0x32C` / `0x330` へ移った (`docs/KAPI_SPEC.md` の v49 節が正典)。
- D8: `exec_kill` は `launch_chain()` で末尾まで辿り、**末尾から** `exec_kill_one` を回す。
  CTRL+STOP は WM が `launch_child()` で末尾を解決して渡す (末尾は 1 本だけ畳まれる)。
- D5: 第 4 の park 点。印 `parked_from_yield`、状態は `WAIT_POLL` のまま、間引きなし。
  「印 → EAX の出所」の対応表は `appslot_resume_source()` に閉じた (`exec_resume` は分岐するだけ)。
- §10 non-blocker 1 / 2 は実装済み: 回収通知で `DONE` + `child = 0`、孤児の完了は `IDLE`、
  子を持たない要求者の退場も解放、KILL 後の `launch_report` は `STALE` (WM は正常扱い)。
- ホスト TDD: `tools/tests/test_launch.py` (新規、`make check-launch-host`) と
  `test_multiapp_impl.py` ケース 23。記録は `tools/tests/t9_tdd.md`。kselftest に 3 項。
- **Codex 実装レビュー 往復 1/3 の blocker 3 件を修正** (2026-09-13、いずれも §1a からの逸脱):
  (1) `launch_take` は `buf == NULL` を断らない — 共通契約「出力ポインタは NULL 可」に従い
  cmdline のコピーだけ飛ばす (`cap` を見るのは `buf` が非 NULL のときだけ)。
  (2) `launch_cancel` の要求者不一致は `INVAL` ではなく **`OS32_ERR_STALE`** (§1a「不一致 → STALE」)。
  `AGAIN` だけが再試行の合図、という端末側 (D9) の読み分けを壊さないため。
  (3) `launch_report(KILL)` が `TAKEN` のまま来ても `child` を落とさず、印だけ消して `RUNNING` へ
  戻す。`DONE` を付けるのは常に回収通知 — 落とすと「生きている子の所有が表から消え、以後の
  退場でも回収されない」孤児ができた。試験は `2b2`/`2b3`・`4c`・`5h2` とケース 9 (16 検査) を追加。
- **未実施**: `make` (clean build / `check` 全体 / `external`)、配備、実機。`build/app.conf` は
  ビルド系レーンの担当なので触っていない (sh / 端末 / gshell の要求版 49 は未設定)。

### 12a. Codex 実装レビュー (K 側、2026-09-13、`codex exec -s read-only`) — 往復 1/3: Request changes → 修正済み

blocker 3 件 (すべて `exec/launch.c` の §1a 逸脱): (1) `launch_take` が `buf == NULL` を INVAL にしていた → NULL 可、(2) `launch_cancel` の要求者不一致が INVAL → STALE、(3) `launch_report(KILL)` が TAKEN のまま来ると `child = 0` + DONE にしていた → 印だけ消して RUNNING に戻す (child 保持、DONE は回収通知だけ)。
non-blocker: kill 連鎖の途中要素を飛ばす経路は正常系で到達しない、cmdline の NUL より先の未マップページは既存ディスパッチャと同じ扱い (呼び手の fault kill)、ホスト試験は `exec.c` の転記ハーネス。
`launch_poll` の不一致は INVAL のまま (§1a は cancel だけ STALE と規定)。

**往復 2/3 (`dc8405e`): Approve** — 3 件の修正を確認、新たな blocker なし。non-blocker は前回と同じ 3 点。
## 13. 実装メモ (S、2026-09-13)

- D2 は全て `#ifdef SHELL_AS_APP`: (a) `shell_rshell_init` と `shell_run` の `serial_init`+`rshell`、(b) `run_cmd_internal` の頭で `os32gui`/`rshell`/`filer` を `sh: cui only`、(c) `.sh_history` (`HIST_FILE_ROOM` も広げた — 旧 `max - 12` では 1 バイト溢れる)、(d) 内蔵 `exit` が `sh_exit_flag` を立て `shell_run` が抜ける、(e) `sh> `。§10-3 の行入力は `kbd_getkey` (`ime_getkey` と同じ u16 形式なので行編集の変換は不要)。
- D3a: `exec_run` は `shell.h` の `#define sh_launch(c) (g_api->exec_run(c))` **1 か所だけ**になり `grep exec_run userland/shell/*.c` は 0 件。`SHELL_AS_APP` の実体は `userland/shell/sh_launch.inc` (`.c` にすると wildcard の `SHELL_SRC` 経由で常駐にも空の `.o` が混ざる)。
- 票に無い判断 2 つ: ①`gfx_shutdown` を `sh_gfx_restore()` にして `SHELL_AS_APP` では**呼ばない** (所有者検査が無く、CPL=3 の sh が呼ぶと GUI の表示ごと止まる)。②`FAILED` が `NOT_FOUND`/`GENERAL` のときは印字しない (PATH 候補の数だけ同じ行が出る)。
- 常駐の回帰: `dee101b` の `userland/shell/` を同じフラグでコンパイルして `.o` を突合、12 本中 11 本バイト一致、`cmd_base.o` の差は `__TIME__` の 1〜2 バイトのみ。**`cmd_ver` が `__DATE__`/`__TIME__` を埋めるので `shell.bin` の SHA-256 はそもそも再現しない** — S7 はここを除いた比較に。
- 未実施: `make` 全般・リンク・配備・実機 ([V4])。通したのは両フラグの単体コンパイル、`check-sh-launch-host` (新規、`check`/`.PHONY` 登録、28 項目 ALL PASS、記録 `tools/tests/t9_tdd.md`)、`check_constraints.py`。
- 残る穴 (W/A へ): ①`try_exec` 2b は相対名 (`ls.bin`) を `launch_req` へ渡すので、`run_program` が要求者でなく WM の cwd で解決すると当たらない。②リダイレクト/パイプは sh 自身の FD に掛かるため `sh> ls > f` は外部コマンドに効かない。

### 13a. 実装レビュー (往復 1/3) の修正 (S、2026-09-13)

- blocker 1: `SHELL_AS_APP` の `redraw_line` を `userland/shell/sh_redraw.inc` に分け、**コンソール座標を一切引かない**形にした。いま出ている行の写し (`sh_drawn`) を持ち、純粋な延長なら差分バイトだけ、それ以外は `\n` + プロンプト + 行全体、行末より前は桁数ぶんの BS で戻す。`shell_run` が直接印字する 3 か所 (ASCII / UTF-8 追加、行末 BS) と候補一覧の後にも `sh_mark_drawn` / `sh_drop_drawn` で写しを合わせる (常駐では両マクロとも空)。
- blocker 2: `cmd_script.c` の `script_exec` が**各行の前に** `sh_exit_flag` を見て抜ける (`#ifdef SHELL_AS_APP`)。`goto` の巻き戻しでも毎行通り、ネストした `source` は内側から順に戻って `script_source_file` が各段で解放する。行ループ末尾の判定 (ui.c) はそのまま。→ §13 の「残る穴 ③」は解消。
- check-manifests: `docs/07_shell.md` の基本コマンド表に `exit` と「`sh.bin` のみ」の注記を追加 (§1c は「なし」)。non-blocker も対応 — GUI 外の `launch_req` INVAL は `sh: external programs need the GUI terminal` を**1 度だけ**出して次の PATH 候補へ回す (起動が通れば印を寝かせる)。UTF-8 の BS は既存問題として触っていない。
- 試験: `check-sh-shell-host` を新設 (`check` / `.PHONY` 登録、19 項目、RED → GREEN を `t9_tdd.md` に記録)。`check-sh-launch-host` は 36 項目へ増え、こちらも RED を採り直した。常駐の `.o` は 12 本中 11 本が `f753f2d` とバイト一致、`cmd_base.o` の差は `__TIME__` 1 バイトのみ。

### 13b. 実装レビュー (往復 2/3) の修正 (S、2026-09-13)

- blocker 1: `exit` の印を `shell_run` の行ループの**入口** (最初の `show_prompt` と `sh_getkey` の前) で見るようにし、ループ末尾の判定は外した。これで起動時の `/etc/profile` / `$HOME/.profile` 内の `exit` でも入力待ちに入らずそのまま終わる。
- blocker 2: 行の写しに**画面カーソルのバイト位置** `sh_drawn_pos` を足し、差分印字は「前方一致 かつ 新カーソルが行末 かつ **描画済みカーソルも行末**」のときだけにした。`redraw_line` は自分が置いたカーソル位置を写しへ残す。加えて内容を変えずにカーソルだけ動かす LEFT / RIGHT / HOME は `sh_drop_drawn()` で写しを捨て、次回を行の作り直しに倒す (経路の見落としで表示が壊れないように)。non-blocker のハーネス `goto` オフセットも `cmd + 5` へ直し、RED で本当に無限ループになることを確認した。

## 14. 実装メモ (W、2026-09-13)

- 起動口は単独ループ (`lib.rs` `standalone_loop`) の `session_handoff` の**直後**、
  `multiapp::resume_one` (park 判定) の**前**。`exec_start` / `exec_resume` から戻った後で、
  WM が owner 1 で走っている唯一の地点 — `launch_take` / `launch_report` / `exec_kill` はここだけ。
- `drain_launch_requests` (`lib.rs`) は `launch_pending() > 0` の間 `launch_take` を回し、
  LAUNCH は `run_program`、KILL / 孤児 (`LAUNCH_REQ_ORPHAN`) は `multiapp::kill_for_request`
  (= `exec_kill` → `FREE` を全部 `forget`、D8)。`launch_report` の `STALE` は再試行しない (§10 2)。
  要求表経由の失敗はモーダルを出さない (`session_launch` の経路は無変更)。
- D5 の巡回と「同じ tick に 1 回」は `multiapp::pick_poll` + `Multi` の `poll_last` /
  `poll_tick` / `poll_woken` (票の「`GuiState` に持つ」から変更 — 同種の控えが `Multi` にある)。
  印は `mark_resumed` で付けるので `pick` を 2 度呼んでも答えは変わらない。
- `abort_target` は `launch_child` を末尾まで辿る (`chain_tail`、環で止まる)。
  `abort_targets_current` は末尾 == cur のときだけ真 — ただし**宛先なし (`f == 0`) は従来どおり真**
  (偽にすると窓が 1 枚も無い周の CTRL+STOP がどこにも届かない)。
- 検証はホストのみ: `make check-gshell-host` 65 passed (T9-W 9 本追加、RED 7 → GREEN)、
  `check_constraints.py` / `check_gui_proto.py` / C 側 3 本の回帰。記録は `tools/tests/t9_tdd.md` §6。
  **`make` / 配備 / 実機は未実施** ([V4])。`cargo clippy` は着手前から `lib.rs:127` で落ちる (追加分は 0)。
- **実装レビュー 1 の blocker 2 件を修正 (2026-09-13)**: (1) `run_program` に依頼元
  (`LaunchVia::Wm` / `Table`) を渡すようにし、要求表経由の入口の拒否 (`cui only`) は
  モーダルを出さず **`OS32_ERR_INVAL` (負)** を返す — `RUN_REFUSED` = 0 のままだと表が `DONE` に
  なり、要求者が「起動して正常終了した」と読む。(2) 「起こした tick」は `pick` 時ではなく
  `mark_resumed` が `get_tick` を読んだ時刻を **1 本ごと** (`App::poll_tick`) に持つ形へ。
  集合の一括消去をやめたので、選んでから再開するまでに PIT が進んでも 2 回起こさない。
  追加した unsafe には SAFETY コメント。試験は 68 passed (T9-W 12 本、RED 2 → GREEN)。

## 15. 実装メモ (A、2026-09-13)

- 端末の起動は `session_launch` → **`launch_req(cmd)`** (`guest.rs:launch`)。token は受理した時点で
  必ず控える (落とすと誰も poll せず表が `ERR_FULL` で固着する)。`ERR_FULL` は従来どおり `busy` で
  行を残し、他の負は `launch_req failed (rc)` を出してプロンプトのまま。
- 要求表の読み方と取消の進み具合は新設の純粋モジュール `userland/rust/t5a_display/src/launch.rs`
  (`Phase` / `Attach` / `Step` / `Outcome`)。KAPI を呼ぶのは `guest.rs` だけ。
- 既存の 100ms タイマ (`TIMER_SINK`) の周で `launch_poll` を 1 回。`RUNNING` で子 ID を控えて
  最下行が `[running id=N] ESC=cancel`、`DONE` / `FAILED` / 負の `rc` / 未知の status はプロンプトへ。
- D9: 接続モードの ESC = `launch_cancel` 1 回 (`Attach` の印で連打をまとめる)。`AGAIN` は次のタイマで
  自動再試行し、`0` の後は `[cancelling id=N] wait` のまま `DONE` を待つ。`STALE` (もう完了している) でも token は捨てず、次の poll で `DONE` / `FAILED` を消費してからプロンプトへ (Codex 往復 1 の blocker: 捨てると表が IDLE に戻らず以後 `ERR_FULL`)。
- D4: `Record::Exit` は `session.apply` に渡すだけ (表示のみ)。`prompt::Event::Exit` は遷移表から削除し
  `Done` / `Failed` / `CancelRequested` に置き換えた。`exit` / 端末終了時の取消は**出さない** (孤児は
  カーネルの `launch_owner_exit` が回収、§10 non-blocker 1)。
- 検証はホストのみ: host 試験 48 → **62** (RED→GREEN の記録は `tools/tests/t9_tdd.md` §A)、
  `cargo check --release -p t5a_display`、`check_constraints.py`。`make` / 配備 / 実機は未実施 ([V4])。

## 16. Codex 実装レビュー (W / A / S、2026-09-13) — 往復 1/3: Request changes → 各レーンへ差し戻し

- **A blocker**: ESC → `launch_cancel` が STALE のとき token を捨ててプロンプトへ戻ると、DONE / FAILED の表が poll で消費されず IDLE に戻らない → 以後の `launch_req` が FULL に固着。→ STALE でも poll で完了を消費してから戻る。
- **W blocker 1**: `run_program` の `cui only` 拒否 (`RUN_REFUSED = 0`) をそのまま `launch_report(token, 0)` すると要求者に DONE が届く。→ 要求表経由の拒否は負の rc、モーダルは出さない。
- **W blocker 2**: pick 時の tick で「起こし済み」を記録すると、pick と再開の間に tick が進んだとき同 tick に 2 回起こせる。→ 実際に起こした tick を ID ごとに記録する。
- **S blocker 1**: TAB 補完後の `redraw_line()` が `console_get_cursor` に依存し、GUI 中 (K6C-2 で座標が進まない) は入力位置が戻らない。→ SHELL_AS_APP では座標に依存しない再描画。
- **S blocker 2**: `source` 内の `exit` が行ループを止めない (`goto loop` で永久)。→ 各行の前に exit の印を見て資源を解放して抜ける。
- ゲート (テスター): `make clean` / `all` (72s) / `external` exit=0、`make check` は check-manifests §1c (`docs/07_shell.md` に内蔵 `exit` が無い) で exit=2 → S へ。
- non-blocker: A の STALE 試験は次回起動を見ていない、W の unsafe に SAFETY 注記、S の t9_tdd に RED 記録が無い、CUI 直起動時の `launch_req failed (-9)` の連発、UTF-8 の BS (既存)。
- 実装レビュー往復 1/3 の blocker 修正 (2026-09-13): ESC の `launch_cancel` が `OS32_ERR_STALE` を
  返しても **token を捨てない** — 完了した表は `launch_poll` が消費して初めて `IDLE` に戻る
  (`include/launch.h`) ので、捨てると以後 `launch_req` が `FULL` で固着する。取消待ちのまま poll を
  続け、`DONE` / `FAILED` を消費してからプロンプトへ戻す (host 試験にカーネル要求表の写しを置いて確認)。

**往復 2/3 (W `25e3c46` / A `ba8e540`): Approve** — 3 件の修正を確認、新たな blocker なし。non-blocker: W の mock が NULL 出力を拒否し INVAL を -2 にしている (実契約は NULL 可 / -9)、A の試験模型が取消直後を RUNNING で返す (実契約は PENDING → TAKEN)。いずれも実装の挙動には影響しない。S は別途 (往復 1 の blocker 2 件を修正中)。

**往復 2/3 (S `3103643`): Request changes** — 前回 2 件は修正済み。残る blocker: (1) `.profile` 内の `exit` の後に shell_run が無条件に 1 回入力待ちに入る (行ループ入口でも印を見る)、(2) `hel` → LEFT → TAB (CUI 直起動) で写しの画面カーソルを持たないため「末尾への延長」と誤判定 (写しにカーソル位置を持つ / 移動時は写しを捨てる)。non-blocker: ハーネスの goto 引数オフセット (`cmd + 7`)。
ゲート (テスター、`3103643`): `make all` / `external` / `check` すべて exit=0 (check 36s)。
