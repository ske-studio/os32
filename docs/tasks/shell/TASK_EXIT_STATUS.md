# TASK_EXIT_STATUS — 終了コードの配線と `$?` (ゲスト試験ランナーの 1 段目)

> 発行: PM (Claude Code `claude-opus-5`、2026-09-16) / 状態: **計画 (2026-09-16)**

基点: `feat/gui` = `34cfc3f`。
引き継ぎ: [`../agents/HANDOVER_2026-09-16.md`](../agents/HANDOVER_2026-09-16.md) §7。
後続: 結果チャネル (TASK_TEST_RESULT、未起票)、ランナー (TASK_TEST_RUNNER、未起票)。

## 0. 目的

ゲストで試験を一括実行し、合否を機械が読める形で返すための 1 段目。
**外部プログラムと組み込みコマンドの終了コードをシェルまで正しく届け、`$?` とスクリプトの失敗停止で使えるようにする。**

## 1. 確認した事実 (2026-09-16、`34cfc3f`)

1. **終了コードと起動エラーが同じ値の空間に混ざっている**。常駐シェルの外部起動は
   `sh_launch(cmdline)` = `g_api->exec_run(cmdline)` (`userland/shell/shell.h:114`)。
   `exec_run` は正常終了で `exec_exit_status` (子が `exit(N)` に渡した値そのもの) を返し
   (`exec/exec.c:1552`)、起動失敗では `EXEC_ERR_*` (`-1`〜`-5`、`os32_kapi_shared.h:100`) を返し、
   park では **app_id `2`〜`5`** を返す (`exec/exec.c:1550`)。
   - **実害 1 (到達可能)**: 子が `exit(-1)` か `exit(-3)` で終わると、`try_exec_from_path`
     (`userland/shell/main.c:231`) はそれを「このディレクトリには無い」(`GENERAL` / `NOT_FOUND`) と読み、
     **PATH の次の候補で同じ名前のプログラムをもう一度実行する**。`/usr/bin` と `/host/bin` に同名がある
     普通の配置で起きる。
   - **実害 2**: `exit(2)`〜`exit(5)` は park と区別できない。
   - `exit(-2)` は `[Process crashed]` と表示される。
2. **組み込みコマンドの結果は捨てられている**。`execute_command` は `void` (`userland/shell/main.c:637`)、
   組み込みの表 `g_cmds[j].handler(argc, argv)` も戻り値を持たない (`main.c:291`)。
3. **`$?` が無い**。`env_expand` (`userland/shell/cmd_env.c:97`) は環境変数の展開だけ。
4. GUI の端末 (`SHELL_AS_APP` の `sh.bin`) は要求表経由で起動する。`launch_poll` の状態語は DONE に
   終了コードを載せず (`exec/launch.c:266`)、`sh_launch.inc:126` は DONE を無条件に `EXEC_SUCCESS` にする。
   要求表を処理する側 (`launch_report(token, rc)`) は `rc > 0` を「子の app_id」、`0` を DONE、負を FAILED と読む
   (`exec/launch.c:233-247`) ので、ここも終了コードと id が同じ引数に乗っている。
5. スクリプト (`userland/shell/cmd_script.c`) は行を順に `execute_command` するだけで、失敗で止まる手段が無い
   (`script_abort_flag` は既にある)。
6. 試験ランナーが最初に使うのは **CUI の常駐シェル** (`/api/cmd` → rshell → `execute_command`)。GUI 端末は後回しでよい。

## 2. 設計

### 2-1. カーネル: 起動の結果を「種別 + 値」で取れるようにする — 決裁 E1

**推奨 (a)**: KAPI を 1 本足す。`int exec_last_result(int *kind, int *code)`。
直前の `exec_run` の結果を `kind` = `EXITED / FAULT / PARKED / NOT_FOUND / NOMEM / INVALID / GENERAL`、
`code` = 終了コード (EXITED) / app_id (PARKED) / 0 で返す。`exec_run` の戻り値は**今のまま変えない** ([ABI2]、
既存の外部利用者 — `apps/` `game/` と Rust の bindings — の意味を変えない)。
シェルは `exec_run` の戻り値を見た直後にこれを呼び、**以後は `kind` だけで分岐する**。

(b) 案: `exec_run` の終了コードを `0x1000 + code` のように別帯へずらす。既存スロットの意味を変えるので不可 ([ABI2])。

(c) 案: カーネルは変えず、シェルが `exec_run` の戻り値で推測する。§1 の衝突は解けない。不可。

版数: 着地した時点で空いている次の版 (H2 が先に着地すれば v54)。手順はスキル `os32-kapi-add`。

### 2-2. シェル: 終了コードを持ち回る

- `execute_command` を `int` にし、最後に実行したコマンドの終了コードを返す。常駐シェルの静的変数
  `g_last_status` に入れる。
- 外部: `kind == EXITED` なら `code`、`FAULT` は `128 + 11` 相当の固定値 (`SH_STATUS_FAULT`、定数で)、
  `NOT_FOUND` は `127`、その他の起動失敗は `126`、`PARKED` は `0` (GUI アプリが生きたまま戻った = 起動成功)。
  値は POSIX シェルの慣習に合わせるが、**定数で定義して表を 1 か所に置く** ([C4])。
- **PATH 走査は `kind` で止める**: `NOT_FOUND` (と、候補の読み込みそのものが失敗した `INVALID` / `GENERAL`) の
  ときだけ次の候補へ進み、`EXITED` / `FAULT` / `PARKED` では**どんな値でも止まる** (§1 実害 1 の修正)。
- 組み込み: 表の handler を `int (*)(int, char **)` にする — 決裁 E2。
- `$?` を `env_expand` で展開する (10 進)。`$?` は環境変数表に書かない (子へ継承させない)。

### 2-3. 組み込みコマンドの戻り値 — 決裁 E2

**推奨 (a)**: この票で handler の型を `int` に変え、全組み込みが 0 / 非 0 を返す。型を変えるので
戻り値を忘れた組み込みはコンパイルが警告を出す (`-Wall` の `-Wreturn-type`)。機械的だが範囲は
`userland/shell/cmd_*.c` の全組み込み。「失敗」の基準は各コマンドが既に赤字のエラーを出している分岐。

(b) 案: handler は `void` のまま、失敗分岐だけ `sh_set_status(1)` を呼ぶ。差分は小さいが、呼び忘れた失敗は
**成功 (0) に見える** = ランナーが偽の合格を出す。[V4] に反するので推奨しない。

### 2-4. スクリプト

- `exit [N]`: スクリプトの実行を止め、`N` (省略時は `$?`) を `source` の終了コードにする。
- `set -e` / `set +e`: 立っている間、0 以外で終わった行でスクリプトを止める (`script_abort_flag`)。
  止めたときに `script: line N: status S` を 1 行出す。
- `source` 自身の終了コードは最後に実行した行のもの。

### 2-5. GUI 端末 (`SHELL_AS_APP`)

この票では**直さない**が、壊さない。`sh_launch.inc` の DONE は今どおり `EXEC_SUCCESS` → `$?` は 0。
要求表に終了コードを載せるのは試験ランナーに要らないので別票 (記録だけ §5)。

## 3. 範囲

| 層 | ファイル |
|---|---|
| KAPI | `sdk/kapi.json` (末尾に 1 本)、`exec/exec.c` (結果の記録)、`sdk/include/os32/os32_kapi_shared.h` (kind の定数)、`docs/KAPI_SPEC.md` |
| シェル | `userland/shell/main.c` (`execute_command`、`try_exec*`)、`userland/shell/cmd_*.c` (handler の型)、`userland/shell/cmd_env.c` (`$?`)、`userland/shell/cmd_script.c` (`exit` / `set -e`)、`userland/shell/shell.h` |
| 文書 | `docs/manpages/` のシェルの頁 (`$?`、`exit`、`set -e`)、`docs/POLICY_DEBUG.md` §4 (実害 1 の記録) |
| 試験 | `tools/tests/sh_status_host.c` + `sh_status_tdd.md` (名前は任意)。`tools/tests/sh_launch_host.c` の枠を読む |

## 4. 受入

### 4-1. ホスト試験

| ID | 反例 | 期待 |
|---|---|---|
| S1 | 子が `exit(0)` / `exit(1)` / `exit(-1)` / `exit(-3)` / `exit(2)` / `exit(255)` | `$?` = 0 / 1 / 子の値 (表の規則どおり) / … / 2 / 255。**`-1` と `-3` で PATH の次の候補を実行しない** |
| S2 | 候補 1 が `NOT_FOUND`、候補 2 が存在 | 候補 2 を実行 |
| S3 | 子が fault | `$?` = `SH_STATUS_FAULT`、`[Process crashed]` 表示は今どおり |
| S4 | GUI アプリが park で戻る | `$?` = 0 |
| S5 | 組み込みの成功 / 失敗 (`cd /nonexistent`、`cat` の無いファイル、`mkdir` の既存) | 0 / 非 0 |
| S6 | `set -e` のスクリプトで 2 行目が失敗 | 3 行目を実行しない、`source` の終了コードは非 0 |
| S7 | `exit 3` | 以降の行を実行しない、`$?` = 3 |
| S8 | `$?` を含む引数、`$?x`、`$` 単独、`$$` | 展開の規則が文書どおり、既存の `$VAR` 展開を壊さない |

### 4-2. ゲスト受入

`/api/cmd` で `false_test; echo $?` 相当 (終了コードを返す小さな試験バイナリを `userland/tests/` に置き、
`deploy.yaml` に登録 [V2]) → 画面の `lines` で値を確認。PATH に同名を 2 つ置いて `exit(-1)` が 1 回しか
走らないこと (実行回数をファイルに追記して数える)。

## 5. 決裁と、この票でしないこと

| ID | 問い | 推奨 | 決裁 |
|---|---|---|---|
| E1 | 起動結果の取り方 | (a) `exec_last_result` を KAPI に足す | 未 |
| E2 | 組み込みの戻り値 | (a) handler を `int` に変える | 未 |

しないこと: GUI 端末の要求表に終了コードを載せる (別票)、`if` / `&&` / `||` の構文 (試験ランナーには要らない)、
パイプラインの終了コード (最後の段にするか) — 触る場合は「最後の段」を既定として票に追記してから。
