# gui_gate.py の「GUI に入れたか」— ホスト試験の記録 (RED→GREEN)

- 経緯: [`docs/archive/settings/TASK_S5.md`](../../docs/archive/settings/TASK_S5.md) §6 の
  R2 の予備調査 (2026-09-29)、[`docs/POLICY_DEBUG.md`](../../docs/POLICY_DEBUG.md) §4-31。
  台本が rshell を閉じずに `os32gui` を打つと、4 文字ずつの text が `os32` / `gui` の
  2 コマンドになって GUI に入らず、`leave_gshell` の `ver` は CUI でも通るので
  **CUI のまま RESULT: OK** になっていた。
- 実行: `python3 -B tools/tests/test_gui_gate.py [--mutate]`
  (`make check-gui-gate-host`、`check-changed` / `check` で `--mutate` 付き)
- 対象: `tools/gui_gate.py` (実物を `import`。`post` / `get` / `_cmd_raw` / `time` を偽物に差し替える)

## 0. この試験が触らないもの

NP21/W・HTTP・実時間に触らない (時計は偽物、0.4 秒で終わる)。**実際のゲストで ESC が
rshell を閉じるか、`/api/status` の `scrn_ymax` / `grph_disp` がこの値になるかは
ここでは分からない** — NP21/W で台本を回して確かめる ([V4])。値は予備調査の実測
(CUI `scrn_ymax 400 grph_disp 0`、PEGC の GUI `scrn_ymax 480 grph_disp 1`) に合わせた。

## 1. 偽のゲスト (`FakeGuest`)

- rshell は `depth` 段重なる。ESC は内側 1 段を閉じて `[Remote shell closed]` を出し、
  最後の段が閉じたときだけプロンプトを出す (外側の rshell は黙っている)。
- rshell が有効な間の text は **1 回の POST が 1 コマンド** (§4-31 の罠そのもの)。
- CUI の text は行に溜まり RETURN で実行。`os32gui` で GUI へ (`gui_ok=False` で入らない)。
- GUI では Start → `ROW_CUI` → Yes (410, H/2+11) で CUI へ戻り、rshell が 1 段で立つ。

## 2. RED (直す前の gui_gate.py = feat/gui `0f0bc25d`)

`case_scenarios` を元の実物に当てた結果 (10 件の FAIL):

- `v11` / `v12g1` / `v12g4`: 「GUI に入っていない (0)」「GUI に入らないのに OK」
  「CUI にパスを打ち込む」— 元の不具合 (rshell 有効のまま始めると CUI のまま OK) を再現。
- `v12g4 --halt`: 2 回目に GUI へ入れない。

(`fresh` / `gui_entered` / `close` / `begin` は新しい関数の試験なので、元の実物では
属性が無く落ちる。)

## 3. GREEN

`SUMMARY 5/5 PASS` (fresh / gui_entered / close / begin / scenarios)。

## 4. 変異 (否定側、14 本すべて RED)

| # | 壊し方 | 当たる試験 |
|---|---|---|
| 1 | `gui_entered` が常に True (元の不具合) | begin, scenarios, gui_entered |
| 2 | `grph_disp` を見ない (9801 の CUI は 400 で GUI と同じ高さ) | gui_entered |
| 3 | `scrn_ymax` を見ない | gui_entered, begin |
| 4 | `begin_gui` が rshell を閉じずに `os32gui` | begin, scenarios |
| 5 | 印が出なくても閉じたと言う | close, begin |
| 6 | 重なった rshell の外側を閉じない | close |
| 7 | 印の数を見ない (末尾が同じ形の新しい印を見落とす) | fresh |
| 8 | 末尾の変化を見ない (画面が流れたときの新しい印を見落とす) | fresh |
| 9 | 印が末尾に無くても数える | fresh |
| 10 | GUI に入れなかった後に rshell を戻さない | begin |
| 11〜13 | v11 / v12g1 / v12g4 が入口の NG を無視する (元の不具合) | scenarios |
| 14 | `--halt` の 2 回目で rshell を閉じない | scenarios |

## 5. 未検証 (NP21/W で見ること)

- 実ゲストで ESC 1 回目に `[Remote shell closed]` が 5 秒以内に tvram に出ること。
- CUI のプロンプトでの 2 回目の ESC が画面を変えないか、変えても末尾 2 行に印を残さないこと
  (残すと重なりの判定で余分に ESC を 1 回送るだけ — 上限 2 回で止まる)。
- 9801 (`--h 400`) と PEGC (`--h 480`) の両方で、GUI 中の `grph_disp == 1` と `scrn_ymax == --h`。
