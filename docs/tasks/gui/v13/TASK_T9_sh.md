# T9 — shell script: 常駐シェルの内蔵コマンドとスクリプトを端末から (設計草案)

状態: **設計草案 (2026-09-12、PM) — 独立レビュー待ち。実装は発注していない。**
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
| D3 | **外部プログラムの起動** (`try_exec()` `main.c:296`、`SHELL_AS_APP` 時): `exec_run` の代わりに **con_sink の PRINT に起動要求を埋める**: `"\x1e" "LAUNCH " <cmdline> "\n"` (`\x1e` = RS、印字されない制御バイト。端末以外の読み手には無害な 1 行)。その後 **`sys_yield()` で譲りながら `exec_app_state(2..5)` を走査**し、「新しく非 FREE になった ID が現れ、それが FREE に戻る」まで待つ (上限: 現れないまま 200 tick ≒ 2 秒なら `sh: launch failed` を出して戻る)。待ちの間は `kbd_getchar` / `kbd_trygetchar` を呼ばない (子の打鍵を横取りしないため) | S |
| D4 | **端末 (T7-A)**: PRINT レコードの行頭が `\x1e LAUNCH ` なら表示せず、残りの cmdline を T7 の E3 (候補パス解決 → `session_launch`) にそのまま通す (`cui only` / `command not found` はいまの経路で端末に出る)。接続モードは維持 (`sh` が親として生きている間は EXIT でプロンプトに戻らない — `EXIT` の ID が `sh` 自身のときだけ戻す: 端末は「最後に自分が起動した ID」ではなく **`sh` を起動した直後の非 FREE ID** を覚える。簡単化: 接続中は EXIT を数え、`exec_app_state` で全部 FREE になったらプロンプト) | A |
| D5 | **KAPI v49 `sys_yield(void)`** (末尾追記): GUI 中 (con_sink 有効) は `kbd_trygetchar` と同じ tick 制限で `WAIT_POLL` に park し (注入リングは**触らない**)、resume で 0 を返す。CUI 中は `hlt` 1 回。**これが無いと D3 の待ちが協調型を止める** | K |
| D6 | スクリプト (`run file`): 各行は `execute_command()` なので内蔵はそのまま、外部行は D3 で端末経由。`$VAR` 展開・`if`/`goto` 等の既存機能はそのまま | — |
| D7 | 8MB では `sh` (段 2、数百ページ) + 子は入らない (T7 の 8MB と同じ、仕様どおり)。15MB 以上が対象 | — |

メモリ: `sh.bin` ≈ 66KB + 既定ヒープ。KAPI v49 は `sys_yield` の 1 本 (KAPI_SPEC §3-2 に予約)。

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

## 3. レビューで見てほしい点

1. **同じソースの二重ビルド** (D1/D2) で常駐シェルを壊さない担保 — `#ifdef` の範囲を最小にし、`shell.bin` のサイズ / ハッシュが変わらないことを受入に入れる。
2. **D3 の in-band 起動要求** (con_sink の PRINT に制御バイト) の是非 — 代案は KAPI `exec_start` を owner 1 以外にも許すことだが、単一カーネルスタックのため呼び手が塞がるので不可。もう 1 つの代案は端末が `sh` の PRINT を見ずに **`sh` 側から GUI セッション要求を出す**ことだが、CUI プログラムは GUI スロットを持たず `gui_call` が使えない。
3. **D3 の待ち (状態走査)** — 子の ID を確定せず「新しく非 FREE になった ID」で追う点。同時に他の GUI アプリが起動 / 終了すると誤検出しうる (v1.3 は端末 1 本 / 子 1 本なので許容するか)。
4. **D5 `sys_yield`** — `kbd_trygetchar` の譲りと同じ tick 制限を共有するか、別枠にするか。

## 4. 範囲外

- 設定レジストリ (S0〜)。端末の複数化。`filer` の端末化 (カーソル位置レコードの解釈)。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。
