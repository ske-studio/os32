# sh_truncation TDD — シェルが入力を黙って切り詰める経路

票: [`docs/tasks/shell/TASK_SH_TRUNCATION.md`](../../docs/tasks/shell/TASK_SH_TRUNCATION.md)
基点: `feat/gui` = `a4f5429`
試験: `make check-sh-truncation-host` (= `python3 -B tools/tests/test_sh_truncation.py`)
    否定側もまとめて: `python3 -B tools/tests/test_sh_truncation.py --mutate`

## 0. この段 (§5 の段 1「足場」) で何をしたか

**挙動は 1 つも変えていない。** やったのは 3 つだけ:

1. `userland/shell/main.c` の `try_exec` / `try_exec_from_path` / `run_cmd_internal`
   (と `has_ext` / `has_slash` / `sh_is_cui_only`) を `userland/shell/sh_exec.inc` へ移し、
   `main.c` が `#include` する。トークン列は切り出す前と同一で、
   `userland/shell/` の `.o` は `cmd_base.o` の `__TIME__` 以外すべて md5 が一致する (§3)。
2. ホスト試験 `tools/tests/sh_truncation_host.c` + `test_sh_truncation.py` の骨を置いた。
   **登録表も `execute_command` も実物**を通す (`main.c` を丸ごと `#include` する)。
3. 票 §2-3 が要求する洗い出し表 (§2) を作った。

### RED → GREEN ではない — 「現状の記録」

段 1 の試験は**今の挙動を固定するだけ**で、直すべき欠陥を赤で落としていない。
反転させる予定の検査には `[EXPECTED_TO_CHANGE]` と書いてある。

| 検査 | いま記録している挙動 | 段 2 でどうなるか |
|---|---|---|
| `1c` / `1d` | `if <A> == <B>` の両辺が 256 文字以上で先頭 255 文字が同じとき、**条件が真になって右辺のコマンドが走る** | 断る。右辺を実行しない。上限超過を報告する (票 U1) |
| `1e` | 同じ反例の `!=` が**偽になって右辺が走らない** | 断る (走らない点は同じでも、理由がメッセージで分かる) |
| `2a`〜`2g` | 登録表・`execute_command` が実物であること、255 文字以下の比較は正しいこと | 変えない (足場の前提なので段 2 以降も緑のまま) |

段 2 で `1c` / `1d` / `1e` を票 §4 の U1 (「実行しない」「上限超過を報告する」
「スクリプト中なら後続行も実行しない」) の形へ書き換える。

### 足場の作り
`tools/tests/sh_shell_host.c` は `execute_command` を**スタブ**にしていて、
`exit` / `source` / `goto` を文字列で直接見ている。切り詰めの経路 (`execute_command`
→ `execute_single` → `parse_args_and_glob` → `run_cmd_internal` → 登録表 → `cmd_if`)
はそこでは通らないので、この票では別に `sh_truncation_host.c` を置いた:

- `#include` する実物: `main.c` (→ `sh_exec.inc` / `sh_args.inc` / `sh_launch.inc` /
  `sh_pipe.inc` / `sh_ls.inc`)、`cmd_base.c` / `cmd_dir.c` / `cmd_env.c` /
  `cmd_file.c` / `cmd_fs_shared.c` / `cmd_mnt.c` / `cmd_script.c` / `cmd_sys.c`、
  `sh_redraw.inc`
- 置き換えるのは KernelAPI 表 (贋 FS・贋ヒープ・`launch_req` の記録) と、
  `ui.c` の 3 本 (`shell_run` / `hist_save` / `hist_load`)、`sdk/crt/help.c` の 2 本だけ
- `cmd_filer.c` は `filer_draw.c` (GFX) を丸ごと引くので取り込んでいない。
  `SHELL_AS_APP` では `sh_is_cui_only` が `filer` を先に断つ。T16 / T23 は段 4 の担当。
- 子が起きたかどうかは `launch_req` の呼び出し回数で見る (GUI 外を模して
  `OS32_ERR_INVAL` を返すので、実際にプログラムは動かない)。

所要時間: 約 1.1 秒 (ホスト gcc ILP32 と i386-elf-gcc の 2 回のコンパイル込み)。

## 0-2. 段 2 (§5 の段 2「T1 単独 + §2-1 の中断規則」) — RED → GREEN

基点: `feat/gui` = `b3954f1`。

### 何を変えたか

| ファイル:行 | 変更 |
|---|---|
| `userland/shell/cmd_script.c` `strip_quotes` | 溢れたら**黙って切らずに `-1` を返す**。長さは**クォート除去後**で数える (`sh_args.inc` が既にクォートを落としているので、`strip_quotes` は取りこぼしの掃除役) |
| `userland/shell/cmd_script.c` `cmd_if` | `v1` / `v2` の幅を `IF_VALUE_MAX` (shell.h、[C4]) に。どちらかが収まらなければ**比較せずに断る** — `sh_refuse("if: left value" / "if: right value", IF_VALUE_MAX - 1)` の赤字 1 行を出し、コマンドは実行しない |
| `userland/shell/cmd_script.c` `script_exec` | `execute_command` の後に `sh_refused_take()` を見て、立っていたら `script_abort_flag` を立てて打ち切る (§2-1)。戻り値で「断って打ち切った」を親へ返す |
| `userland/shell/cmd_script.c` `script_source_file` | 断って打ち切ったら `SCRIPT_ERR_REFUSED` (-2) を返す |
| `userland/shell/cmd_script.c` `cmd_source` / `script_source_profile` (新規) | `source` は `-2` を受けたら印を立て直して親のスクリプトも打ち切る。起動スクリプトは**印を立て直さず**メッセージを出して続行する (R2) |
| `userland/shell/main.c` | 印の実体 `sh_refused_flag` と `sh_refuse()` / `sh_refuse_mark()` / `sh_refused_take()`。`execute_command` は `execute_command_line` を包む形にして、**いちばん外側の呼び出しだけ**が入口で印を消す |
| `userland/shell/main.c` `execute_command_line` | 既にあった `env_expand` 溢れの断りにも印を立てる (同じ §2-1 の規則) |
| `userland/shell/sh_exec.inc` | `foo.sh` を直に打った経路 (`run_cmd_internal`) も `-2` を印に変換して親へ伝える |
| `userland/shell/ui.c` | 起動スクリプト 2 本を `script_source_profile()` 経由に |
| `userland/shell/shell.h` | `IF_VALUE_MAX` / `SCRIPT_ERR_REFUSED` / 印の宣言 (寿命の規則もここに書いた) |

**印を消すのは 1 か所だけ** — いちばん外側の `execute_command` の入口。入れ子
(`if` / `time` が組み立てた行) で消すと内側の断りが `script_exec` に届かず、
段の後ろが `time …` のパイプで取りこぼす (変異 `nested_clear` が実際に落ちる)。
`script_exec` は 1 行ごとに読んで消す。対話 / `rshell` は誰も読まないので、
断った行の**次の**行は今までどおり動く。

### 段 1 の `[EXPECTED_TO_CHANGE]` をどう反転させたか

| 段 1 の検査 | 段 1 が記録した挙動 | 段 2 の検査 |
|---|---|---|
| `1c` | 条件が真になって右辺が走る | `1c` 右辺のコマンドを実行しない (`==`) |
| `1d` | 走った証拠 (`command not found`) | `1d` 何が上限を超えたか + 上限を 1 行で報告する |
| `1e` | `!=` が偽になって走らない | `1e` / `1f` 走らないのは同じだが、理由が赤字 1 行で分かる |
| `2a`〜`2g` | 足場が実物であること | そのまま (`2f` に「断りも出さない」を足しただけ) |

段 1 の記録は §0 に残してある (履歴)。

### RED → GREEN

段 2 の試験は 64 件。**GREEN 64 / FAIL 0** (`EXIT sh_truncation_host=0`)。

RED の記録は「段 2 の前の姿」に戻した版で取った (`--mutate` の否定側がそのまま
pre-fix の再現になっている。全ソースを段 1 の状態へ戻すと `SCRIPT_ERR_REFUSED`
などが無くてコンパイルが通らないので、規則ごとに 1 つずつ壊す形にした):

| 変異 (= 壊した規則) | 落ちた検査 |
|---|---|
| `compare_truncated` (段 2 の前の `strip_quotes`。黙って 255 で切る) | **24 件** 1c 1d 1f 1h 1i 1j 1k 3d 3g 4b 4c 4e 4f 5a 5b 5c 5d 6b 6c 6g 7b 8a 8b 8c |
| `no_mark` (断っても印を立てない = §2-1 が無い) | **11 件** 1j 1k 4c 4f 5b 5c 5d 6c 6g 8b 8c |
| `no_clear` (印を消し忘れる) | **8 件** 4b 4e 4h 4i 6d 6e 7d 7e |
| `nested_clear` (入れ子でも消す) | **1 件** 6g |
| `abort_interactive` (対話でも打ち切る) | **18 件** 1f 1h 1k 3a 3d 3e 3g 4b 4e 4g 4h 4i 6d 6e 7a 7c 7d 7e |
| `source_not_propagated` (入れ子 source の断りを親へ伝えない) | **1 件** 5c |
| `profile_aborts_boot` (起動スクリプトの断りで印を立て直す) | **1 件** 8d |

7 変異すべて RED。GREEN のまま通ったものは無い。

### 票 §2-1 が挙げた 6 経路の受け持ち

| 経路 | 検査 | 取りこぼし / 誤発火の両方を見たか |
|---|---|---|
| `if` / `time` 経由の入れ子 `execute_command` | 4a〜4f (取りこぼし) / 4g〜4i (誤発火) | はい |
| 入れ子の `source` (戻り値で親へ) | 5a〜5d / 5e・5f | はい |
| パイプの段 | 6a〜6c・6f・6g / 6d・6e | はい |
| `rshell` から呼ばれた `execute_command` | 7a〜7d | **rshell.c は取り込んでいない** (下の §4 を見よ) |
| 対話 (スクリプト外) | 7a〜7e | はい |
| 起動時の `/etc/profile` | 8a〜8e / 8f・8g | `ui.c` は取り込んでいない (下の §4) |

## 0-3. 段 2 の追補 — パイプの段で断ったら**後続の段も実行しない** (PM 決裁)

基点: `feat/gui` = `2ef6f6a` (段 2)。

### なぜ直すか (決裁の理由)

段 2 では「パイプの段で断っても**段の途中では止めない**」(bash の `false | cat` に寄せる)
という判断をして PM の裁定を仰いだ。**独立レビューが同じ箇所を指摘した**ので、
票 [`TASK_SH_TRUNCATION.md`](../../docs/tasks/shell/TASK_SH_TRUNCATION.md) §2 の
**「その場で赤字のエラーを 1 行出して、行全体を実行しない」**に合わせる。

反例:

```
<断られる段> | tee 重要ファイル
<断られる段> | echo x > 重要ファイル
```

段を続けると**断ったのに後段の書き込みが起きる**。`> file` は `apply_redirects` が
`FD_REDIR_WRITE` (= O_TRUNC) で開くので、**リダイレクト先が空で上書きされ得る**。
「切り詰めたら実行しない」という票の規則に正面から反する。

bash 寄せを採らないのはここが**シェルの通常の意味論の話ではない**ため —
`false` は「失敗した段」だが、断りは「**そもそも実行されなかった行**」で、
票 §2 の扱いは行単位と決まっている。

### 何を変えたか

| ファイル:行 | 変更 |
|---|---|
| `userland/shell/main.c:448` | `sh_refused_peek()` を足した。**読むだけで消さない** — `sh_refused_take()` を使うと断りが `script_exec` まで届かず、後続の**行**が走る |
| `userland/shell/main.c:611` | パイプの段ループの末尾で `if (sh_refused_peek()) break;`。段の後始末 (`sys_redirect_get_buf_len` → `reset_all_redirects` → `prev_buf` 記録) を**全部通してから**抜けるので、パイプバッファとリダイレクトの回収は今までどおり (`sh_pipeline_leave` / `sh_pipe_free` / `mem_free` はループの外) |
| `userland/shell/shell.h:77,84` | 印の寿命の規則に「段ループは peek で読むだけ」を足し、`sh_refused_peek()` を宣言 |
| `tools/tests/sh_shell_host.c:439` | 同じ実体のスタブ (この試験は `execute_command` をスタブにしているので使わないが、宣言に揃える) |

**印は消していない** (段 2 の規則のまま)。消すのはいちばん外側の `execute_command` の入口だけで、
スクリプト中ならこの行の後で `script_exec` が打ち切る — そこは 1 行も変えていない。

### 試験の足場に足したもの

段 2 の時点では**段が走ったかどうかを見る窓が無かった**。2 つ足した:

- `h_sys_write` が fd 1 / 2 への書き込みを出力に残すようにした。`echo` は `kprintf` ではなく
  `sys_write(1, ...)` を使うので、捨てていると「後続の段が走った」ことを検出できず
  **偽の GREEN** になる (実際、足す前は `6j` が pre-fix でも通ってしまった)。
- `h_sys_redirect_fd` がリダイレクト先を記録するようにした (`redir_opened()` / `g_redir_count`)。
  本物の `>` は O_TRUNC で開くので、**この呼び出しが起きたこと自体**が
  「リダイレクト先が空で上書きされた」ことを意味する。

### RED → GREEN

| | 件数 |
|---|---|
| 段 2 (`2ef6f6a`) の姿 + 新しい検査 = **RED** | GREEN 75 / **FAIL 4** (`6j` `6k` `6l` `6n`) |
| 直した後 = **GREEN** | **GREEN 79 / FAIL 0** (`EXIT sh_truncation_host=0`) |

RED で落ちた 4 件:

| 検査 | 見ているもの |
|---|---|
| `6j` | 断った段の**後続の段を実行しない** (痕跡 = 後段の `echo` の出力) |
| `6k` | 後続の段の**リダイレクト先を開かない** (= 空で上書きしない) |
| `6l` | 断った行はリダイレクトを 1 つも張らない |
| `6n` | 3 段の**真ん中**で断ったときも 3 段目は走らない |

誤発火の裏 (pre-fix でも GREEN のまま = 挙動を変えていないこと):

| 検査 | 見ているもの |
|---|---|
| `6m` | 断る**前**の段は今までどおり走る |
| `6o` / `6p` | 断りの無い 3 段パイプは全段走り、印を残さない |
| `6q` | 通るパイプの最終段の `> file` は今までどおり張る |
| `6r` / `6s` | `if` が**偽**で右辺を走らせないのは断りではない → 後続の段は走る |

### 変異 (否定側) — 8 本すべて RED

| 変異 | 落ちた検査 |
|---|---|
| `pipe_no_peek` (**新規**。段ループで印を見ない = この決裁の前の姿) | **4 件** 6j 6k 6l 6n |
| `compare_truncated` | 24 件 (段 2 と同じ) |
| `no_mark` | 11 件 (段 2 と同じ) |
| `no_clear` | **13 件** 4b 4e 4h 4i **4l** 6d 6e **6o 6p 6q 6s** 7d 7e |
| `nested_clear` | **1 件 4k** (下記) |
| `abort_interactive` | **25 件** 1f 1h 1k 3a 3d 3e 3g 4b 4e 4g 4h 4i 4l 6d 6e 6h 6m 6o 6p 6q 6s 7a 7c 7d 7e |
| `source_not_propagated` | 1 件 5c |
| `profile_aborts_boot` | 1 件 8d |

### `nested_clear` の証人が入れ替わったこと (足した検査 `4j`〜`4l`)

段 2 では「入れ子の入口で印を消さない」規則の**唯一の証人が `6g`** だった —
「断った段の**後ろ**の段が `time …` だと、そこで入れ子の `execute_command` が入口で印を消す」。
この決裁で**断った後の段を走らせなくなった**ので、その経路からは見えなくなった
(`6g` は今も GREEN だが、`time` の段に到達しないので規則を試していない)。

規則自体は `shell.h` の契約として残る (将来「断りの後に入れ子を呼ぶ」構文が増えたら効く) ので、
入れ子の深さを直に作って押さえる検査を足した:

- `4j` 入れ子の `execute_command` は今までどおり走る
- `4k` **入れ子の入口では印を消さない** (`g_exec_depth` を 1 つ上げて `sh_refuse_mark()` の後に呼ぶ)
- `4l` いちばん外側 (深さ 0) の入口では消す

`4k` が `nested_clear` の新しい証人。`g_exec_depth` は `main.c` の file-scope static だが、
この試験は `main.c` を丸ごと `#include` する同じ翻訳単位なので直に触れる。

### 隣の試験

| 試験 | 結果 |
|---|---|
| `python3 -B tools/tests/test_sh_shell.py` | ALL PASS (`EXIT sh_shell_host=0`) |
| `python3 -B tools/tests/test_sh_launch.py` | ALL PASS (`EXIT sh_launch_host=0`、`-Werror` 版のコンパイルも通る) |

### コンパイル

`i386-elf-gcc` で `userland/shell/main.c` を**常駐版**と `-DSHELL_AS_APP` 版の両方、
Makefile と同じフラグ (`-O2 -Wall -Wdeclaration-after-statement`) で **警告 0**。
段ループの `break` は `#ifdef` の外なので両方に同じように効く。

### この追補で確かめていないこと

- **実機 (NP21/W) では動かしていない** ([V4])。ゲスト受入 (票 §4 末尾) は未実施。
- **リンクしていない**: 段 2 と同じく `/usr/local/cross/i386-elf/lib` がこの環境に無く
  `-lc` / `-lgcc` / `-los32save` が解決できないので、`shell.bin` / `sh.bin` は作れていない。
  `make all` / `make check` / `make external` はこの段の担当外 (PM 指示で禁止)。
- **外部段を含むパイプでは見ていない**。`SHELL_AS_APP` では外部段を含むパイプが
  段ループへ入る前に丸ごと断られるので、試験の段は全部内蔵コマンド。
  常駐 `shell.bin` では外部段が段ループを通るが、その経路は実機でしか踏めない。
- `tee` は OS32 に無い。決裁の理由で挙げた `| tee 重要ファイル` は型の説明で、
  試験で使った反例は `| echo tail > /keep.txt` (同じ O_TRUNC の経路)。
- 段 3 / 段 4 の範囲 (T2〜T26) は手つかず。

## 1. 洗い出しの範囲と方法

対象: `userland/shell/` の全ファイルと `userland/lib/filer/`。

見たもの: `char [N]` の宣言、`strncpy` / `strncat` / `strcat` / `str_copy` /
`kstrncpy`、および `PATH_MAX_LEN` (256) / `CMD_BUF_SIZE` (4096) /
`SCRIPT_MAX_LINE` (256) / `SCRIPT_MAX_LINES` (128) / `TRY_EXEC_BUF_SIZE` (512) /
`ENV_NAME_MAX` (32) / `ENV_VALUE_MAX` (256) / `HIST_LINE_MAX` (512) /
`LAUNCH_CMDLINE_MAX` (256、カーネル側) / `SH_LS_NAME_MAX` (256) /
`FL_MAX_NAME_LEN` (64) / `FL_MAX_PATH_LEN` (256) / `FILER_NAME_LEN` (64) /
`FILER_MAX_PATH` (256) / `MAX_PIPE_STAGES` (8) / `MAX_ARGS` (256) の各所。

行番号は **切り出し後** (`sh_exec.inc` を作った後) のもの。票 §1 の行番号は
切り出し前の `main.c` を指しているので、`T3` / `T4` / `T17` はここの番号が正。

## 2. 洗い出し表

### 2-1. 票 §1 に挙がっている 22 件 (すべて再確認した)

| # | 場所 (ファイル:行) | 上限 (定数名と値) | 今の挙動 | 段 2〜4 で変えるか | 備考 |
|---|---|---|---|---|---|
| T1 | `cmd_script.c:349` `strip_quotes` / `:407` `cmd_if` の `v1[256]` `v2[256]` | 255 (`max - 1`) | 255 文字で切って比較。**条件が逆になる** | **段 2 — 済み** (`b3954f1` の次) | 票どおり。長さはクォート除去後 (`sh_args.inc:168,183` が既に落としている) |
| T2 | `cmd_script.c:150` (`li < SCRIPT_MAX_LINE - 1`)、`:121`、`:134-138` | `SCRIPT_MAX_LINE` 256 (実効 255) / `SCRIPT_MAX_LINES` 128 | 256 文字目以降を切る。129 行目以降は赤字を出して `break` し、`return 0` で先頭 128 行を実行 | **段 4** | 票どおり |
| T2' | `cmd_script.c:97` `sys_read(fd, raw_buf, raw_buf_size - 1)` | `SCRIPT_MAX_LINES * SCRIPT_MAX_LINE - 1` = 32767 | 32KB 超のスクリプトを**黙って**途中で切り、途中の行から先が無かったことになる | **段 4** | 票 U3 の「読み込み上限超のファイル」に対応。票の表には行が無いので分けて書いた |
| T3a | `sh_exec.inc:54,70,85,91` `try_exec` | `TRY_EXEC_BUF_SIZE` 512、実効 510 (`limit_args`) | 溢れた引数を落として**起動する**。クォート再付与で `"` `\` は 2 倍 | **段 3** | 票どおり (票の `main.c:140-170` は切り出し前の番号) |
| T3b | `cmd_mnt.c:41,53-56` `exec` | `cmdline[256]`、実効 255 | 同上 | **段 4** | 票どおり |
| T3c | `cmd_base.c:117,127-131` `time` | `cmd_buf[512]`、実効 510 | 同上 (組み立てた行を `execute_command` へ) | **段 4** | 票どおり |
| T4 | `sh_exec.inc:209-212` `run_cmd_internal` | `PATH_MAX_LEN - 5` = 251 | コマンド名を 251 で切って `.bin` を付ける → **別のファイルが起動する** | **段 3** | 票どおり |
| T5 | `main.c:386` `MAX_PIPE_STAGES`、`:388-411` `split_pipeline` | 8 段 / 段ごと `CMD_BUF_SIZE` 4096 | 9 段目以降と空の段を捨てる | **段 3** | 票どおり。段バッファ側 (`pos < seg_size - 1`) は 4096 で実質届かない |
| T6 | `sh_args.inc:84-85` `glob_cb` | — (`mem_alloc` 失敗) | 印を立てずに `return`。一致の**一部だけ**を渡す | **段 3** | 票どおり |
| T7 | `sh_args.inc:222,230` (`patlen < 255`)、`:216` (`i < PATH_MAX_LEN - 1`) | 255 / 255 | パターンとディレクトリ部を切る → **別のファイルに一致する** | **段 3** | 票どおり |
| T8 | `cmd_env.c:37-41` `str_copy` (`env_set` 経由)、`:193` (`ni < ENV_NAME_MAX - 1`)、`:227` (`vi < ENV_VALUE_MAX - 1`) | `ENV_NAME_MAX` 32 (実効 31) / `ENV_VALUE_MAX` 256 (実効 255) | 切った名前・値で**登録する** | **段 4** | 票の訂正どおり。**さらに 1 つ**: `set <32 文字以上の名前>=VALUE` は名前の途中で `=` を見失うが、**引数が 3 個以上あると** `argc >= 3` の fallback (`cmd_env.c:208-210`) に落ちて `argv[2]` を値として登録する (引数 2 個なら「値の表示」分岐) |
| T9 | `cmd_env.c:126-136` `env_expand` | `ENV_NAME_MAX - 1` = 31 | `${NAME}` も裸の `$NAME` も 31 文字で打ち切り、残り (と `}`) が**リテラルとして素通りする** | **段 4** | 票どおり |
| T10 | `rshell.c:123-124` (`rpos < 126`) | `rbuf[128]`、実効 126 | 126 文字で読み取りを止め、**その接頭辞を実行する**。残りは次の入力になる | **段 4** | 票どおり。断るときも EOT (0x04) を返すこと (票 §2-2) |
| T11 | `sh_launch.inc:63` `sh_launch` + `exec/launch.c` (`LAUNCH_CMDLINE_MAX` 256) | 256 | `launch_req` が `INVAL` を返し、`sh.bin` は「GUI 外」と読む (`sh_launch.inc:93-102`) | **段 3** | 票どおり。カーネルは変えない |
| T12 | `cmd_script.c:427-429` `cmd_if` の `join_args` | `CMD_BUF_SIZE` 4096 | 切れたコマンド行を実行する。到達するのは glob 展開で argv が伸びたとき | **段 4** | 票どおり |
| T13 | `main.c:423` `execute_command` / `:316` `execute_single` | `CMD_BUF_SIZE` 4096 | 空行と長大行を**同じ扱い**で黙って `return` | **段 3** | 票どおり (両方とも `>= CMD_BUF_SIZE`) |
| T14 | `ui.c:65` `hist_add` (`j < HIST_LINE_MAX - 1`)、`:480` `hist_load` (`li >= HIST_LINE_MAX` → 511) | `HIST_LINE_MAX` 512 (実効 511) | 512 バイト目以降を切って履歴に残す / 読み込む | **段 4** | 票どおり。`hist_load` は `:467` で `HIST_SIZE * HIST_LINE_MAX - 1` = 8191 バイトしか読まない (末尾の行も切れる) |
| T15 | `ui.c:645` (`cmd_len < CMD_BUF_SIZE - 4`)、`:666` (`cmd_len + utf8_len < CMD_BUF_SIZE - 1`)、`:705-706` | 4092 / 4095 | 4092 バイトで打鍵を黙って捨て、ENTER でその接頭辞を実行し履歴にも入れる | **段 4** | 票どおり。`/api/key text=` の機械注入では気付けない |
| T16 | `cmd_filer.c:215-217` (`i < FL_MAX_NAME_LEN - 1`)、`:144-153` `fl_path_join`、`:330-339`、`:73` `ft_load` | `FL_MAX_NAME_LEN` 64 (実効 63) / `FL_MAX_PATH_LEN` 256 / `FL_FILETYPES_MAXSZ` 8192 (実効 8191) | 63 バイトで切った名前で**別のファイルを起動する / 別のディレクトリへ入る**。`/etc/filetypes` の末尾行が切れて別コマンドに関連付く | **段 4** | 票どおり。`fl_path_join` は戻り値が無く、溢れても呼び手に伝わらない |
| T17 | `sh_exec.inc:125` (`di < PATH_MAX_LEN - 2`)、`ui.c:264` (`di < PATH_MAX_LEN - 1`) | 254 / 255 | 長い PATH 項目で `:` の区切りを見失い、**残りが別のディレクトリとして扱われる** | **段 3** (`sh_exec.inc`) / **段 4** (`ui.c`) | 票どおり |
| T18 | `cmd_script.c:289` (`pi < 254`)、`:323` (`len < 254`) | `prompt[256]` / `input[256]`、実効 254 | 255 文字目以降の打鍵を捨てて変数に入れる | **段 4** | 票どおり。入れた値は `env_set` でさらに 255 で切られる (T8) |
| T19 | `rshell.c:242-257` `resolve_host_path` | `host_path[256]`、実効 255 | `push` / `recv` で切れた別パスを `O_TRUNC` で作る | **段 4** | 票どおり。`host:` の 5 バイト (+ `/` 補完 1) を差し引くので、`host:` の後ろが 249〜250 文字で切れ始める |
| T20 | `ui.c:404-418` `hist_build_path`、`:519-525` profile パス | `PATH_MAX_LEN - HIST_FILE_ROOM` = 244 (常駐) / 240 (`sh.bin`、`HIST_FILE_ROOM` 16)、profile は `PATH_MAX_LEN - 12` = 244 | 長い `HOME` を切って**別ディレクトリの `.history` / `.profile`** を読み書きする | **段 4** | 票の「244 文字以上」は常駐側の値。`sh.bin` は `.sh_history` を使うので 240 |
| T21 | `ui.c:143,153-155` `name_store[40][64]`、`:210-215` `name_store[40][128]` | 63 / 126 (+ `/` 1) | 補完結果が切れて別の名前になり、`ui.c:327-335` / `:374-375` でバッファへ書き込まれる | **段 4** | 票どおり |
| T22 | `cmd_sys.c:161-169` (`sizeof(buf) - 1`)、`:186` (`sizeof(out) - CFG_LINE_RESERVE`) | `buf[1024]` (実効 1023) / `out[1152] - 64` = 1088 | `/etc/system.cfg` を 1023 バイトで読み、**切れたまま書き戻す** (1KB 超の設定が消える) | **段 4** | 票どおり |

### 2-2. 票が「変更不要と確認された」と書いているもの — 再確認の結果

| 場所 | 票の記述 | 再確認 | 備考 |
|---|---|---|---|
| `cmd_file.c` の `fs_join_path` 呼び手 | 「`-1` を全呼び手が見る」 | **そのとおり** | 呼び手は `cmd_file.c:178,179,256,267,351` の 5 か所。すべて `< 0` を見て `cp: path too long` で飛ばす |
| `cmd_dir.c` | 「変更不要」 | **そのとおり** | `old_dir[PATH_MAX_LEN]` は `strncpy` + 明示 NUL で、用途は `OLDPWD` の保存だけ。`size_buf[16]` は数値の整形 |
| `cmd_base.c` の `time` 以外 | 「変更不要」 | **そのとおり** | `np2_get_version` は `sizeof(buf)` を渡す。`pad[12]` は RTC の作業域 |
| `sh_ls.inc` | 「変更不要」 | **そのとおり** | `SH_LS_NAME_MAX` = `OS32_MAX_PATH` = `DirEntry_Ext.name` と同じ幅なので名前は切れない。溢れた**件数**は `sh_ls_dropped()` で呼び手へ伝わる |
| `split_pipeline` の段バッファ | 「変更不要」 | **そのとおり** | 段ごとの幅は `CMD_BUF_SIZE` 4096 で、`execute_command` の入口 (T13) が同じ上限で先に断るので届かない。段**数** 8 は T5 |
| `sh_stage_is_builtin` | 「変更不要」 | **そのとおり** | `main.c:272` の `name[PATH_MAX_LEN]` は先頭語を写すだけ。255 で切れても「内蔵か」の判定が保守側 (外部扱い) に倒れるだけで、切れた名前は実行に使われない |
| `sh_path_normalize` | 「変更不要」 | **ほぼそのとおり。1 点だけ違う** | 出力側は `seg_start + sn >= max` で `-1` を返すので票の言うとおり。ただし**入力側** (`cmd_fs_shared.c:114,120`、`tmp[PATH_MAX_LEN * 2]`) は cwd + 引数が 511 バイトを超えると**黙って切る**。呼び手は `fs_same_file` だけで、切れた綴りが偶然一致すると「同じファイル」と誤判定して `cp` が中止される (安全側に倒れる) — 段 2〜4 では変えないが、表には残す |
| カーネルのトークナイザ (`exec/exec.c`) | 「変更不要」 | **範囲外 (票 §3 でカーネルは変えない)** | 読むだけ |
| `cmd_fs_shared.c:215` `fs_append_basename` / `:261` `fs_parse_two_args` | 「呼び手が無い」 | **そのとおり** | `userland/` `tools/` 全体を検索して、宣言 (`cmd_fs_shared.h:42,49`) 以外の参照は 0。どちらも溢れを見ない (`fs_parse_two_args` は 255 で黙って切る)。**削除するか段 4 で決める** |

### 2-3. 票に無かったもの (この洗い出しで足した)

| # | 場所 (ファイル:行) | 上限 (定数名と値) | 今の挙動 | 段 2〜4 で変えるか | 備考 |
|---|---|---|---|---|---|
| T23 | `userland/lib/filer/filer_core.c:96-99` `dir_callback`、`:124-133` `build_full_path`、`:243`、`:320-325`、`:330` | `FILER_NAME_LEN` 64 (実効 63) / `FILER_MAX_PATH` 256 | T16 とまったく同じ穴が **`libos32filer` 側にもある**。63 バイトで切った名前で `filer_get_selected_path()` が**別のファイルのパス**を返し、ディレクトリ移動も別の場所へ行く。`build_full_path` は溢れても戻り値が無い | **段 4** (T16 と同じコミット) | 票 §3 は `userland/lib/filer/` を「名前の幅」として範囲に入れている。`cmd_filer.c` (TVRAM ファイラ) とは別のソースなので両方直す |
| T24 | `rshell.c:366-378` `cmd_push` | `xfer_buf[4096]` | `sys_read` が 1 回だけなので、**4096 バイトを超えるファイルは先頭 4KB だけがホストへ書かれ**、`Uploaded` と報告される。`cmd_recv` は `:303-319` で読み切るまで回すので直っている (同じ欠陥が `recv` 側だけ直された) | **段 4** | 入力そのものではなくファイル内容の切り詰めだが、「切り詰めたまま成功を返す」という同じ型 |
| T25 | `cmd_dir.c:167,169` `env_set("OLDPWD", ...)` / `env_set("PWD", ...)` | `ENV_VALUE_MAX - 1` = 255 | cwd が 255 バイトちょうどまでは収まるので**今は届かない**。`PATH_MAX_LEN` (256) を上げたら T8 の切り詰めが先に当たる | **変えない** | T8 を直せば自動的に閉じる。上限を上げるときの注意として残す |
| T26 | `ui.c:387-388` タブ補完の共通接頭辞 | `CMD_BUF_SIZE - 1` | 書き込みループは境界を見るが、直後の `buf[common_len] = 0;` は見ない。`common_len` は候補名の長さ (≤ 126) で抑えられているので**今は届かない** | **変えない** | T21 を直すときに一緒に見る |

**合計 26 件** (票の 22 件 + 新規 4 件)。票の記述と食い違ったのは 3 か所 —
T8 の `set NAME=VALUE` に第 3 引数があるときの経路、`sh_path_normalize` の入力側、
T20 の `sh.bin` 側の値 (240 であって 244 ではない)。いずれも上の表に書いた。

## 3. 等価の証明 (段 1 は挙動を変えていない)

`make -n programs` が出す実際のコンパイル行 (常駐 `userland/shell/*.o` と
`-DSHELL_AS_APP` の `userland/shell/sh_obj/*.o` の 2 組、計 24 個) を、切り出しの
前後で回して md5 を比べた。

| 結果 | 件数 |
|---|---|
| md5 が完全に一致 | **22 / 24** |
| 3 バイトだけ違う | 2 (`cmd_base.o` の常駐版と `sh.bin` 版) |

`main.o` は念のためもう一度、`git show a4f5429:userland/shell/main.c` を取り出して
**`-MMD -MP` まで含む make の実コンパイル行そのまま**でも比べた — 常駐版
`f3790e1f82566a8f3555b8c93d0472b6` / `sh.bin` 版 `37d0baa1bb2ec95339725f95069931f6` で
どちらも一致する。

`cmd_base.o` の差は `cmd_base.c:68` の `__DATE__` / `__TIME__` — `strings` の差分が
`11:20:47` → `11:22:02` の 1 行だけで、`.text` / `.data` / `.rodata` の
`objdump -s` は 3 セクションとも一致する。**切り出しとは無関係の、ビルド時刻の
埋め込みによる差**。

一致する理由はソースの作りからも言える: `sh_exec.inc` は `main.c` の該当区間
(旧 `main.c:120-346`) を **1 行も変えずに** 移し、`#include` を元の位置に置いた
だけなので、プリプロセスした後のトークン列が同一になる。`sh_launch.inc` /
`sh_pipe.inc` の `#include` を `try_exec` の**後ろ**に置く並びも崩していない
(先に置くと `sh_launch` が `try_exec` へ展開され得て、コード生成が変わる)。

## 4. 確かめていないこと

### 段 2 の時点

- **実機 (NP21/W) では 1 度も動かしていない** ([V4])。票 §4 末尾のゲスト受入
  (U1 を `/api/key text=` 注入か `set` + 展開で再現) は未実施。
- **リンクは通していない**: この環境に `/usr/local/cross/i386-elf/lib` が無く、
  `-lc` / `-lgcc` / `-los32save` が解決できないので `userland/shell.bin` /
  `userland/sh.bin` は作れていない (この票の変更とは無関係の環境側の欠け)。
  **コンパイルは常駐版・`-DSHELL_AS_APP` 版の両方で警告 0** を確認した。
- `rshell.c` は serial を握るのでホスト試験に取り込んでいない。7a〜7d は
  `rshell.c:149` と同じ「1 行ずつ `execute_command` を呼ぶ」形を並べて見ている
  だけで、rshell そのものは通していない (EOT = 票 §2-2 は段 4 の担当)。
- `ui.c` の `shell_run()` も取り込んでいない。8a〜8g は `shell_run` が呼ぶ
  `script_source_profile()` を実物で通しているが、`shell_run` の中の呼び出し
  そのもの (2 行) は試験で踏んでいない。
- `docs/POLICY_DEBUG.md` §4 への T1 の記録は**まだ書いていない** (票 §3)。

### 段 1 の時点

- 実機 (NP21/W) では 1 度も動かしていない。段 1 は挙動を変えないのでゲスト受入は
  段 2 以降 (票 §4 末尾の経路: `/api/key text=` 注入か、スクリプトの `set` + 展開)。
- `shell.bin` / `sh.bin` のリンクとバイナリ一致は見ていない (`.o` の md5 まで)。
  `make programs` を通したリンク後の SHA-256 比較は段 2 のコミットでまとめて見る。
- 表の「今の挙動」はコードの読みと `sh_truncation_host.c` の 2 例で確かめたもので、
  T2〜T26 の各行を試験で 1 件ずつ踏んではいない。踏むのは段 2〜4 の担当 (票 §4 の
  U1〜U20)。
