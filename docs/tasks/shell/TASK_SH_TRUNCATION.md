# TASK_SH_TRUNCATION — シェルが入力を黙って切り詰める欠陥を全部断つ

> 発行: PM (Claude Code `claude-opus-5`、2026-09-16) / 状態: **計画 (2026-09-16)**

基点: `feat/gui` = `c271fc8`。
分割の経緯: [`TASK_EXIT_STATUS.md`](TASK_EXIT_STATUS.md) の設計レビュー 4 往復で、同じ系統の欠陥が
出続けた (Fable 1 往復 + Codex 2 往復 + Antigravity 1 往復)。**ユーザー決裁 2026-09-16 で 2 票に分割**し、
本票 (バグ修正) を先に緑にしてから終了コードの配線を載せる。
関連: [`../agents/HANDOVER_2026-09-16.md`](../agents/HANDOVER_2026-09-16.md) §7。

## 0. 目的

**シェルが入力を切り詰めたまま実行を続ける経路をすべて断つ。** 切り詰めたら実行せずに断る。

これは新機能ではなく**既存の不具合の修正**。`$?` の有無に関係なく今日のコードで起きる。
いちばん重いのは `if` の比較で、**条件が逆になって別の枝が走る**:

```
if "<256 文字以上で先頭 255 文字が同じ文字列 A>" == "<同 B>" rm -rf /data
```

`cmd_script.c` の `strip_quotes` が両方を 255 文字に切り詰めるので `A == B` が真になり、
**本来実行されない破壊的なコマンドが走る** (`cmd_script.c:349-357`、`cmd_if` の `v1[256]` / `v2[256]`)。

## 1. 確認した経路 (2026-09-16、`c271fc8`。すべて PM がコードで確認)

| # | 場所 | 今の挙動 | 起きること |
|---|---|---|---|
| T1 | `cmd_script.c:349-357` `strip_quotes` + `cmd_if` の `v1[256]` / `v2[256]` | 255 文字で切って比較 | **条件が逆になる**。`==` が偽であるべきところで真、`!=` は逆 |
| T2 | `cmd_script.c` の `script_load` (`SCRIPT_MAX_LINE` 256 / `SCRIPT_MAX_LINES` 128、`shell.h:25-26`) | 長い行を切り詰め、128 行超を捨てて**成功を返す** | 切れた行が実行される。捨てた行は無かったことになる |
| T3 | `main.c:140-170` `try_exec` (`TRY_EXEC_BUF_SIZE`) | 溢れた引数を落として**起動する** | 意図と違う引数でプログラムが走る。`exec` 組み込み (`cmd_mnt.c`) と `time` も同じ |
| T4 | `main.c:299-305` `run_cmd_internal` | コマンド名を `PATH_MAX_LEN - 5` で切って `.bin` を付ける | **別のファイルが起動する** (251 バイトの接頭辞が実在すると `Pextra` で `P.bin` が走る)。`try_exec_from_path` のパス連結も同様 |
| T5 | `main.c:606-620` `split_pipeline` (`MAX_PIPE_STAGES` 8) | 9 段目以降と空の段を捨てる | 実行されない段の失敗が見えない |
| T6 | `sh_args.inc:84` `glob_cb` | `mem_alloc` の失敗で印を立てずに戻る | 一致の**一部だけ**を渡す。`rm /tmp/item*` が 1 件だけ消えて成功に見える |
| T7 | `sh_args.inc:204-233` `parse_args_and_glob` | パターンを 255 文字、ディレクトリを `PATH_MAX_LEN-1` で切る | **別のファイルに一致する** (`prefix…suffix1*` が `suffix2` まで巻き込む) |
| T8 | `cmd_env.c` の `cmd_set` (`ENV_VALUE_MAX` / `ENV_NAME_MAX`) | 値と名前を切って**登録する** | 壊れた `PATH` が入る。エラーも出ない |
| T9 | `cmd_env.c:126-136` `env_expand` の `${NAME}` | 名前が 31 文字を超えるとループを抜け、残りと `}` が**素通りする** | 展開されない文字列がコマンド行に混ざる |
| T10 | `rshell.c:120-124` | 126 文字で読み取りを止め、**その接頭辞を実行する** | 途中まで実行される。残りが次の入力になる |
| T11 | `sh_launch.inc` + `exec/launch.c:138-146` (`LAUNCH_CMDLINE_MAX` 256) | 256 バイト超の行で `launch_req` が `INVAL` を返し、`sh.bin` は「GUI 外」と読む | 理由が取り違えられる |
| T12 | `cmd_script.c:427-430` `cmd_if` の `join_args` (`CMD_BUF_SIZE`) | 切れたコマンド行を実行する | 意図と違うコマンドが走る |
| T13 | `main.c:643` `execute_command` | 空行と `CMD_BUF_SIZE` 超の行を**同じ扱い**で黙って return | 長すぎる行が無言で消える |
| T14 | `ui.c:65,480` `hist_add` / `hist_load` (`HIST_LINE_MAX` 512) | 512 バイト超を切って履歴に残す | ↑キーで呼び出すと**切れた行が実行される** |

## 2. 方針

**「切り詰めたら実行しない」を 1 つの規則にする。**

1. 入力が上限を超えたと分かったら、**その場で赤字のエラーを 1 行出して、行全体を実行しない**。
   切り詰めた値で先へ進まない (比較・起動・登録・展開のいずれも)。
2. エラーの文言は「何が上限を超えたか」と「上限」を出す (例: `sh: argument too long (max 255)`)。
3. 上限そのものは**変えない** (バッファを増やす改修ではない)。[C4] に従い上限は既に定数なので、
   定数を参照してメッセージに出す。
4. 履歴 (T14) は実行経路ではないので、**切れた行は履歴に入れない**。
5. `rshell` (T10) は行末まで読み捨ててから断る。次の行から正常に戻ること。
6. `glob` の確保失敗 (T6) は行全体を断り、確保済みの文字列を解放する。

**網羅の要求**: 上の 14 件は 4 往復のレビューが積み上げたもので、同種の穴が他にもあり得る。
実装の前に `userland/shell/` の**入力が通る固定長バッファを 1 度全部洗い**、
「場所 / 上限 / 今の挙動 / 変更の有無」の表を `tools/tests/sh_truncation_tdd.md` に残す。
洗う対象: `char *[N]` / `char [N]` の宣言、`strncpy` / `kstrncpy` / `str_ncpy`、
`PATH_MAX_LEN` / `CMD_BUF_SIZE` / `SCRIPT_MAX_*` / `TRY_EXEC_BUF_SIZE` / `ENV_*_MAX` /
`HIST_LINE_MAX` / `LAUNCH_CMDLINE_MAX` / `NAME_CAP` / `MAX_PIPE_STAGES` / `MAX_ARGS` の各所。

## 3. 範囲

`userland/shell/` の `main.c` / `shell.h` / `sh_args.inc` / `sh_launch.inc` / `cmd_script.c` /
`cmd_env.c` / `cmd_mnt.c` / `rshell.c` / `ui.c`。**カーネルは変えない** (`exec/launch.c` の上限は読むだけ)。
文書は `docs/manpages/` のシェルの頁と `docs/POLICY_DEBUG.md` §4 (T1 と T4 の記録)。

## 4. 受入

ホスト試験 `tools/tests/sh_truncation_host.c` + `test_sh_truncation.py` + `sh_truncation_tdd.md`
(RED → GREEN を記録)。**実物の登録表と `execute_command` を通す** (スタブだけの試験にしない)。

| ID | 反例 | 期待 |
|---|---|---|
| U1 | `if` の両辺が 256 文字以上で先頭 255 文字が同じ (`==` と `!=` の両方) | **コマンドを実行しない**。上限超過を報告する。切り詰めた比較をしない |
| U2 | 255 文字ちょうど / 256 文字の境界 | 255 は通る、256 は断る |
| U3 | スクリプトに 256 文字超の行 / 129 行目 / 読み込み上限超のファイル | スクリプトを実行しない (`source` が失敗する) |
| U4 | `try_exec` / `exec` / `time` の再構築が溢れる長さ | **子を起こさない** |
| U5 | 251 バイトの接頭辞が実在する状態で長い名前を打つ (T4) | **`P.bin` を起動しない** |
| U6 | 9 段のパイプ、`echo ok \|`、`\| echo`、`a \|\| b` | 行全体を実行しない |
| U7 | `glob` の `mem_alloc` を N 回目で失敗させる | 行全体を断り、handler を呼ばず、確保済みを解放する |
| U8 | 256 文字超の glob パターン / `PATH_MAX_LEN` 超のディレクトリ部 | 一致を試みない |
| U9 | `set` の名前 32 文字 / 値 256 文字 | 登録しない |
| U10 | `${` + 32 文字以上の名前 + `}` | 展開エラー。素通りさせない |
| U11 | `rshell` に 127 文字以上の行 | 実行しない。次の行は正常に動く |
| U12 | `sh.bin` で 256 バイト以上のコマンド行 | `launch_req` を呼ばない |
| U13 | `CMD_BUF_SIZE` 超の行を `execute_command` へ | 空行と区別して断る |
| U14 | 512 バイト超の行を実行した後に ↑ キー | 履歴に切れた行が入っていない |
| R1 | `test_sh_shell.py` / `test_sh_launch.py` / `test_fs_kind_callers.py` / `test_hsync_*.py` | 退行なし |

ゲスト受入: 代表として U1 (条件の逆転) と U5 (別のファイルの起動) を実機で再現 → 修正後に断ることを確認する。

## 5. この票でしないこと

終了コードの配線と `$?` ([`TASK_EXIT_STATUS.md`](TASK_EXIT_STATUS.md))、上限そのものの拡大、
`if` / `&&` / `||` の構文の追加、カーネル側 (`exec/launch.c`) の上限の変更。
