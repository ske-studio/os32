# multiapp_model — GUI アプリ 4 本同時実行 (K5a) のホスト TDD

対象票: [`docs/tasks/gui/v13/TASK_K5_multiapp.md`](../../docs/tasks/gui/v13/TASK_K5_multiapp.md) §K5a-8
実行: `python3 -B tools/tests/test_multiapp_model.py` (`make check` への登録は PM)
三点セット: `multiapp_model_host.c` (模型 + 検査本体) / `test_multiapp_model.py` (ビルドと実行) /
このファイル (何を見ているか + RED→GREEN の記録)。

## 何を検証するか — と、何を検証しないか

K5a は**読取専用の設計段階**で、`kernel/` `exec/` は 1 行も変えていない。したがって
`test_app_band_pde.py` のように「実ソースをホストで動かす」ことはできない
(動かす実装がまだ無い)。ここで試験するのは**設計そのもの = 状態遷移の規則**で、
`multiapp_model_host.c` に純粋な状態機械として書いてある。K5b で `kernel/multiapp.c` を
書くときは、この遷移表を仕様として写し、実体 (`struct addrspace` / `setjmp` /
`pgalloc` / `res_owner_set`) を足す。

**この試験が言えないこと** (V4): 実際のカーネルが正しいこと、ゲストで 4 本動くこと、
メモリ見積が実測と合うこと。ゲスト検証は K5b の G1〜G8。

模型が持つ 5 つの規則:

| # | 規則 | 由来 |
|---|---|---|
| R1 | ID は 1 = シェル帯、2〜5 = アプリ。同時に生きられる非シェル ID は 4 本、5 本目は `ERR_FULL`。GUI アプリと CUI の入れ子 exec は**同じ 1 つの池**を使う | 票 §K5a-3 / G3、契約 T2a |
| R2 | 走っているのは常に 1 本。park (OP_WAIT の中) → WM top-level → resume で別の 1 本、の往復しか無い。プリエンプションは無い | 票「同時」の意味 (2026-09-10 ユーザー確認) |
| R3 | park できるのは「走っているアプリが `gui_call(OP_WAIT)` の中に居る」ときだけ | 契約 T2a「譲り合いの点は `OP_WAIT` だけ」、票 G7 |
| R4 | 終了 (exit / fault / kill) はその ID の資源だけを回収する | 契約 T4 / U8、票 G2 / G5 |
| R5 | GUI アプリの起動は WM の top-level からだけ。CUI の入れ子 `exec_run` は従来どおり | v1.2 契約 S2 |
| R6 | 同時に ready が複数居るときの選択は**決定的**: 入力群 > 導出群、入力群の中はフォーカス優先、それ以外は `last_run` の次から ID 昇順の巡回 | 票 D11 (独立レビュー 2026-09-10 の指摘 1)、契約 T3 / U5 |

## 作り

`test_pgalloc_range.py` と同じ仕掛け:

- `gcc -std=gnu89 -m32 -march=i386 -ffreestanding -Wall -Wextra -Werror
  -Wdeclaration-after-statement -nostdlib -static` で ILP32 として直接ビルドし、
  そのまま実行する (libc に依存しない。出力と終了は `int 0x80` を直接叩く)
- 最後に **クロスコンパイラ (`i386-elf-gcc -O2`)** でも同じソースをコンパイルし、
  K5b でそのまま `kernel/` へ移せる方言 ([C1] C89/GNU89) であることを確かめる
- OS32 のヘッダは 1 つも `#include` しない (模型はカーネルの定数に依存しない)。
  対応する実物の定数はコメントで対にしてある (`MA_SHELL_ID` = `GUI_SHELL_OWNER`、
  `MA_SLOT_MAX` = `GUI_SLOT_MAX` ほか)

検査は 14 ケース・75 個の `check()`。1 つでも落ちれば非ゼロ終了する。

## RED → GREEN の記録

**手順**: 先に 11 ケース全部の検査を書き、模型の遷移関数を**全部スタブ**
(`return MA_ERR_INVAL`) にした状態から始めた。以下は実際に走らせた出力の要約で、
各行の「残 FAIL」は `python3 -B tools/tests/test_multiapp_model.py | grep -c '^  FAIL'`
の値。

| 回 | 入れた遷移 | 期待した色 | 実際 (残 FAIL) |
|---|---|---|---|
| 0 | (スタブのみ) | RED | ビルドは通り、実行が **SIGSEGV**。ケース 8b が `ma_app()` の NULL を辿った |
| 0' | 8b の検査を NULL 安全に直した (模型は未実装のまま) | RED | **49** (この時点の全 55 検査のうち 49 が FAIL、4 が pass、2 は未到達) |
| 1 | R1: ID 池 (`ma_alloc_id`) / `ma_start` / `ERR_FULL` / `ERR_NOMEM` / `ma_live` | 1a-1d, 7a-7d, 10a, 10c が GREEN | **36** |
| 2 | R2/R3: `ma_gui_call` / `ma_gui_return` / `ma_park` / `ma_resume` | 1e, 3a-3h, 4a-4b が GREEN | **25** |
| 3 | R4: `ma_res_add` / `ma_reclaim` / `ma_exit` (親が生きていればその段へ戻る) | 2a-2g, 8d-8g が GREEN | **12** |
| 4 | `ma_fault` (= `ma_exit(-1)`、畳むのは走っている 1 本だけ) | 5a-5e が GREEN | **9** |
| 5 | SHM スロット (`ma_alloc_slot`、`ma_start` が配る) | 6a-6d が GREEN | **7** |
| 6 | `ma_abort_request` / `ma_abort_check` / `ma_kill` | 4c, 9a-9g が GREEN | **0** — ALL PASS |
| 7 | ケース 11 (R5: S2 のゲート) を**追加**。模型は未対応 | RED | **5** (11a-11e) |
| 8 | `ma_start` の先頭に `if (gui && cur != SHELL) return MA_ERR_STATE;` | GREEN | **0** — ALL PASS |
| 9 | ケース 12〜14 (R6: D11 の選択規則) を**追加**。`input_ready` / `derived_ready` / `last_run` / `focus` は足したが `ma_pick` / `ma_should_park` はスタブ (`return 0`) | RED | **8** (12a-12c, 12e, 13c-13e, 14a) |
| 10 | `ma_pick_group` / `ma_pick` / `ma_should_park` を入れる | GREEN | **0** — ALL PASS (75 検査) |

回 0' の RED で受かっていた 4 個は `2f 終了した ID は空く` `3d park に失敗した間 CR3 は
動かない` `10b 起動失敗では CR3 を載せ替えない` `10d 起動失敗では回収を回さない` — どれも
「〜しない」を見る否定形で、**何も実装されていないので自明に成立する**。これらは他の検査が
GREEN になって初めて意味を持つので、単独では根拠にしない。未到達の 2 個 (`6c` `6d`) は、
ケース 6 がスロット重複を検出して早期 return したため。

回 0 (スタブのみ) の SIGSEGV は「模型がまだ何も返さない」ことの現れであって、
設計の欠陥ではない — 検査側の NULL 参照なので検査を直した (模型は未実装のまま)。

回 7 と回 9 は「後付けの RED」ではなく、**新しい規則 (R5 / R6) を足すときに検査を先に
書いて赤を見た**記録。回 6 の時点で `ma_start` は走っているアプリの中からでも GUI アプリを
起動できてしまい、契約 S2 に反していた — その穴を検査が実際に検出している。

回 9 の RED でも、否定形の 4 個 (`12d 導出群ではフォーカスを優先しない`
`12f ready が 1 本も無ければ誰も起こさない` `13a 自分に入力があれば park しない`
`13b 他に ready が居なければ park しない`) はスタブの `return 0` で自明に受かっていた。
うち `12d` は回 10 の後に**巡回の順とフォーカスが食い違う配置**へ書き直し、
「フォーカスを優先していれば別の ID が選ばれる」ことを実際に区別できるようにした。

## ケースと票の対応

| ケース | 見るもの | 票の受入 |
|---|---|---|
| 1 | ID は 2..5 が小さい順、5 本目は `ERR_FULL`、既存 4 本もページも減らない | G3 |
| 2 | 終了で回収されるのは 1 本分だけ。他のアプリの資源・ページは不変。`gui_owner_exit` はその ID で呼ばれる | G2 |
| 3 | `gui_call` の外 / `OP_POLL` / `OP_COMMIT` の中では park できない。`OP_WAIT` の中だけ。CR3 が動くのは resume の 1 回だけ | G7、契約 T2a |
| 4 | 走っている間は別のアプリを起こせない・kill も走らない (プリエンプション無し) | 「同時」の意味 |
| 5 | fault で畳まれるのは 1 本。他 2 本の資源が前後で完全に同じ | G5 |
| 6 | スロット 0..3 と ID が 1 対 1。park/resume で動かない。畳んだ ID とスロットが次へ回る | 契約 T2a / T2 |
| 7 | 入らない要求は `ERR_NOMEM`。拒否は空きページを 1 枚も動かさない (切り詰めない) | 票 §K5a-5 |
| 8 | CUI の入れ子は ID = 段 (2,3,4)、スロットを取らない、1 段ずつ親へ戻る。GUI 4 本のときは入れ子も `ERR_FULL` | G8、票 §K5a-3 |
| 9 | CTRL+STOP は走っているアプリ宛にしか立たない。止めてあるアプリは WM の kill でだけ畳める | G4、契約 T6 |
| 10 | 起動失敗は CR3 も owner も回収も動かさない | 票 §K5a-4 |
| 11 | GUI の起動は WM top-level からだけ。CUI の入れ子は従来どおり | 契約 S2 |
| 12 | 選択規則: 入力群のフォーカス最優先 / 入力群の巡回 / 導出群の巡回 / 導出群ではフォーカスを優先しない / 入力は導出より先 / ready ゼロなら誰も起こさない | 票 D11、受入 G9 |
| 13 | park 判定: 自分に入力なら戻る / 他に ready が無ければ戻る (1 本のときの回帰ゼロ) / 他に入力があれば譲る / 他も導出だけなら譲る / 自分が ready でなければ譲る | 票 D11 (1) |
| 14 | 飢餓なし: 導出群だけの 4 本が 2,3,4,5 の順にちょうど 1 回ずつ走り、1 周したら先頭へ戻る | 票 D11 |

## 模型が**わざと**持っていないもの

- 時間・タイマ割込み・優先度・タイムスライス。R2 のとおり切替は park/resume の
  往復だけで、スケジューラは存在しない。持たせると「足場を作らない」という
  票の決めに反する。R6 の巡回は**選ぶ順を決定的にするだけ**で、時間で横取りする
  仕組みではない (走っているアプリは自分が `OP_WAIT` に入るまで走り切る)。
- `ready` の**計算**そのもの。`input_ready` / `derived_ready` は試験が直接立てる。
  実物では WM が `wake_ready()` (`handler.rs:396-417`) と `session::quit[]` から
  毎周期算出する — その算出が正しいかは W レーンの担当で、ここでは
  「2 群に分かれた後の選び方」だけを固定している。
- 物理アドレス・ページテーブル・`setjmp`。ページは**枚数**しか持たない
  (「入らなければ拒否する」だけを見るのに必要十分)。番地の正しさは K5b で
  `paging_app_band_selftest()` の系列が見る。
- SHM の中身・イベントリング・窓の重なり。gshell 側 (W レーン) の担当。
