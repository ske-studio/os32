# K5b-W: アプリ 4 本の同時実行 — gshell (WM) 側の実装

> 発行: PM (2026-09-11。K5b-K の最終署名で**確定**) / レーン: W (Rust、gshell)
>
> **K5b-K が足した KAPI v44 (スロット 180〜185、`sdk/rust/os32api/src/kapi_generated.rs` に生成済み)**:
> `exec_start(cmdline) -> i32` (>0 = app_id で最初の OP_WAIT まで進んで park 済み / 0 = park 前に終了・回収済み / <0)、
> `exec_resume(app_id, wait_ret) -> i32` (app_id = また park / 0 = 終了 / <0。印なしフレームは `OS32_ERR_STALE`)、
> `exec_park() -> i32` (成立すれば戻らない。OP_WAIT 以外の op からは `OS32_ERR_INVAL` を返して戻る)、
> `exec_kill(app_id) -> i32` (走っている本人は `OS32_ERR_STALE` — CTRL+STOP 経路)、
> `exec_app_state(app_id) -> i32` (0 空き / 1 走行 / 2 park)、`snd_focus(app_id) -> i32`。
> すべて owner 1 (シェル帯) からのみ。**gshell の `build/app.conf` の要求 KAPI 版を 44 に上げるのは W レーン。**
> カーネルの制約: CUI の入れ子の子 (`exec_run` で立った) は park できない (WM へ戻れないため)。
> 前提: [K5b-K](TASK_K5B_kernel.md) の KAPI v44 と機構が本家に入っていること / 設計: [K5a §設計 D0〜D11](TASK_K5_multiapp.md)、決裁 (2026-09-11)
> 排他: `userland/gshell/**`、`userland/gshell/host/**`。`sdk/rust/os32api` の生成物は K5b-K の再生成結果を**そのまま使う** (手で触らない)。
> **触らない**: `kernel/**` `exec/**` `kapi/**` `sdk/kapi.json`、`libos32gui` (C レーン)。

## ゴール

WM が **GetMessage 方式**で 4 本のアプリを回す: 起動は塞がない `exec_start`、`OP_WAIT` の中で
D11 の規則 (入力優先は連続 4 回まで、`turn_used` で 1 ラウンド 1 回、フォーカス優先はその下、
ID 昇順の巡回) に従って `exec_park` / `exec_resume`、終了・fault・kill は ID 単位で回収。
スケジューラは持たない。音はフォーカスに追従して排他 (決裁 D9-4)。

## 作業

1. **スロット割当** (契約 T2a、F7): `OP_INIT` で 0〜3 を配る (いまは常に 0)。所有者 ID と
   スロットの対応表、5 本目は `ERR_FULL`。`gui_owner_exit` でスロットを返す。
2. **起動** (D4、決裁 D9-5): `run_program` の `exec_run` を `exec_start` に。戻り値 `>0` = park 済み
   (表に登録)、`0` = park 前に終了 (何もしない)、`<0` = エラー表示。`LAUNCH` の SessionAction は
   「アプリを 1 本増やす」意味になる (現行の「今のアプリを Quit してから」は不要になるので、S2 の
   手順を 4 本前提に読み替え、票に差分を書く)。
3. **`op_wait` の中の譲り合い** (D11): `input_ready(k)` / `derived_ready(k)` を実物で算出
   (`ring::pending` / `session::quit[].pending` / `timer::has_expired` / `configure_pending` /
   `damage::has_deliverable_paint` / `deadline`)。`ma_should_park` / `ma_pick` と**同じ規則**を
   Rust に写し (定数 `INPUT_STREAK_MAX = 4`、`turn_used`、`last_run` は WM 側)、park すべきなら
   `exec_park()` (戻らない)。WM top-level は `exec_resume(id, wait_ret)` で次を起こす。
   1 本のときは現行の `wm_cycle` + `sys_halt` ループのまま (回帰ゼロ、ケース 13b)。
4. **終了・fault・kill・CTRL+STOP** (D4): `gui_owner_exit(id)` でその ID の窓・サーフェス・タイマ・
   スロットを回収 (T4/U8)。止まっているアプリの Quit は `exec_kill(id)`。CTRL+STOP はフォーカス窓の
   アプリが走っているときだけ効く (T6)。
5. **音の排他** (決裁 D9-4、受入 G10): フォーカス切替 (`set_focus` / クリックでの前面化 / 終了で
   前面が変わる) のたびに `snd_focus(new_owner)` を呼ぶ (形は K5b-K の報告で確定)。
6. **可視・描画**: 4 本の窓が重なるときの露出計算は既存 (G4、`visible.rs`) で足りるはず。
   足りない箇所があれば票に書いて PM へ (勝手に契約を変えない)。
7. **ホスト試験** (`userland/gshell/host/wm_tests.rs`): `op_wait` の譲り合いを、モックした `exec_park` /
   `exec_resume` (`os32api::api_ptr()` で差し替え) で **モデルの 16 ケース 84 検査と同じ性質**を検査。
   特に: 5 本目 `ERR_FULL`、終了で 1 本分だけ回収、切替は `op_wait` の中でだけ (`exec_park` の
   呼び出し点が `op_wait` のループ先頭 1 点だけ)、1 本のときは `exec_park` が 0 回、
   `set_focus` を自分の 2 窓で交互に呼ぶアプリが他を飢えさせない (レビュアーの反例)。
   RED→GREEN を `tools/tests/k5b_gshell_tdd.md` に記録。

## 完了条件

`make programs` / `make check` (`check-gshell-host` を含む) EXIT=0、上記試験の GREEN と RED の記録、
`git diff --stat`。**配備・コミット・push・エミュレータ・ローカル AI 禁止。** 実機受入
(G1〜G6 / G9 / G10) は PM とテスター。

## 実機受入 (PM/テスター)

[K5 §K5b の G1〜G10](TASK_K5_multiapp.md#段階-k5b--実装-k5a-凍結後に発注)。観測は `gui_bench` の `CLICK n`
(tvram) とカーネルシンボル (`ring3_switch_count` 等) を `emu_read_mem` で。
