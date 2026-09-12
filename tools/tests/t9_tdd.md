# T9-K ホスト TDD の記録 (起動要求表 + sys_yield + exec_kill の連鎖)

票: [docs/tasks/gui/v13/TASK_T9_sh.md](../../docs/tasks/gui/v13/TASK_T9_sh.md) §1 D3 / D5 / D8、
§1a の ABI 表、§10 の non-blocker 1 / 2。
実行: `make check-launch-host` (= `python3 -B tools/tests/test_launch.py`) と
`make check-multiapp-model-host` (その中の `test_multiapp_impl.py`)。

実機・エミュレータ・`make` は**未実施** ([V4])。ここに書いてあるのは
ホストの gcc / i386-elf-gcc だけで踏めた範囲。

## 1. 何を実物で回すか

| ハーネス | 取り込む実物 | 見るもの |
|---|---|---|
| `tools/tests/launch_host.c` | `exec/launch.c` + `exec/appslot.c` | 要求表の遷移・権限・token の照合・連鎖の解決 |
| `tools/tests/multiapp_impl_host.c` (ケース 23 を追加) | 同上 + `kernel/kbd_inject.c` | `sys_yield` の park / resume (EAX 0・注入リング不変) と `exec_kill` の連鎖 |

カーネル帯の代わりにハーネスが持つのは 3 つだけ: 所有者 (`res_owner_get/set`)、
GUI 判定 (`con_sink_is_enabled`)、`kstrncpy`。スロットは実物の `AppSlot` を組み立てる
(`exec/exec.c` の起動経路は CR3 / pgalloc を引くのでホストへは持ち込めない)。

## 2. RED → GREEN

`exec/launch.c` を書いたあとに `launch_host.c` を書いたため、初回実行は最初から
全項目 GREEN だった。**「試験が仕様を捕まえているか」を別に確かめる**ため、
実装へ 3 つの欠陥を 1 つずつ埋め込んで RED を確認し、戻して GREEN に戻した
(いずれも票の non-blocker / blocker が実際に指摘した壊れ方)。

| 埋めた欠陥 | 落ちた検査 | 対応する票の指摘 |
|---|---|---|
| `row_finish` の孤児分岐を消す (孤児の完了を `IDLE` に落とさない) | `5k` `8c` | §10 non-blocker 1 (`ERR_FULL` の固着) |
| 回収通知で `child = 0` にしない | `2s` `4k` `7j` `7k` `8a` | §10 non-blocker 1 (再利用 ID への誤連鎖) |
| `launch_poll` が完了した表を解放しない | `2u` `2v` `3d` `3e` `4o` | §1a (完了は 1 度だけ渡す) |

いずれも `EXIT launch_host=1`。3 つとも戻した後の最終状態は `ALL PASS`。

## 3. 検査の並び (`launch_host.c`)

| 番号 | 見るもの |
|---|---|
| 1a〜1m | `launch_req` の門: GUI 外 / WM top-level / 宣言 `LAUNCHER` なし / 入れ子 `exec_run` の子 / NULL / 空 / 255B 超 → `OS32_ERR_INVAL`。通った後、同じ要求者の 2 本目は `OS32_ERR_FULL`、別の要求者は自分の表に積める |
| 2a〜2v | `take` (owner 1 専用・`cap < 256` は断る・昇順) → `report(rc>0)` (範囲外 / 死んだ ID は `INVAL` で `TAKEN` のまま) → `RUNNING` (`status = 0x100 + child`) → 子の回収通知で `DONE` + `child = 0` → `poll` が 1 度だけ渡して表は IDLE |
| 3a〜3f | `rc == 0` は即 `DONE` (park より前に終わった短命な子を取り逃がさない — §5 blocker 1)、`rc < 0` は `FAILED` (`status = 0x300 + (-rc)`) |
| 4a〜4o | `cancel`: `PENDING` / `TAKEN` は `AGAIN`、`RUNNING` は `KILL(child)` の `PENDING` へ (child は保持)、`take` で `kind = KILL` / `arg = 子`、回収通知で `DONE`、その後の `report` は `STALE` (§10 non-blocker 2)、`DONE` への `cancel` は `STALE` |
| 5a〜5n | 要求者の退場 = 孤児回収。`requester` が -1 になり、再利用 ID からの `launch_req` は回収が終わるまで `FULL`、完了は poll を待たず `IDLE`。子を持たない要求者の退場は即 `IDLE` |
| 6a〜6j | token: 要求者をまたいで単調増加、他人の token は `INVAL`、知らない / 0 / 負は `STALE`、ID を再利用しても古い token は当たらない |
| 7a〜7l | `launch_child` / `launch_chain`: 端末 → sh → 子 の 3 段、途中から、不正 ID、`max 0`、末尾を畳んだ後の縮み、壊れた表の環でも止まる |
| 8a〜8d | `launch_selftest()` (ブート時 kselftest が踏むのと同じ 3 項) が 0 を返し、表を空にして戻る |

## 4. 検査の並び (`multiapp_impl_host.c` ケース 23)

| 番号 | 見るもの |
|---|---|
| 23c〜23j | `sys_yield` の park: tick の間引き**なし**で成立、`ring3_yield_count` が増え `ring3_poll_yield_count` は動かない、状態は `WAIT_POLL` のまま、印は `parked_from_yield` だけ、`exec_app_state` は 4 のまま、WM top-level へ戻る |
| 23k〜23p | **譲っている間に届いた打鍵を吸わない**: 注入リングに 2 バイトある状態で resume → `EAX = 0`、`kbd_inject_pending()` は 2 のまま (§6 blocker 1 = `parked_from_yield` を足した理由) |
| 23q〜23D | 端末 → sh → 子 を要求表で組む (`req` → `take` → `report`)。`launch_child` で連鎖が読める |
| 23E〜23I | CTRL+STOP の形: WM が末尾を解決して `exec_kill(末尾)` → **1 本だけ**畳まれ、親の表が `child = 0` になる |
| 23J〜23O | `exec_kill(端末)` は子孫ごと、**末尾から**畳む (`ma_kill_order` が `[sh, 端末]`)。畳み終えた後に表が 1 本も残らない |
| 23P〜23U | 譲れない文脈: WM top-level と CUI の入れ子 `exec_run` の子。どちらも `OS32_ERR_INVAL` で `ring3_park_reject_count` に載り、入れ子の子からの `launch_req` も断られる |

`ma_resume_poll` と `ma_reclaim_res` と `ma_kill_chain` は、`exec/exec.c` の
`exec_resume` / `exec_reclaim_owned` / `exec_kill` の該当部分を**そのまま写した**形
(exec.c はカーネル一式を引くのでホストへ `#include` できない)。印から EAX の出所を導く
対応表そのものは `appslot_resume_source()` として実物に置いたので、写しているのは
「どこから値を取るか」ではなく「取った値をフレームへ書く」手順だけ。

## 4b. Codex 実装レビュー 往復 1/3 の修正 (2026-09-13)

blocker 3 件はいずれも `exec/launch.c` の §1a からの逸脱。直したあと、**3 点を元に戻すと
RED になる**ことを確認してから GREEN に戻した (下の「落ちた検査」は実際の出力)。

| 直したところ | 落ちた検査 (戻したとき) |
|---|---|
| `launch_take` が `buf == NULL` を `INVAL` にしていた | `2b2` `2b3` |
| `launch_cancel` の要求者不一致が `INVAL` だった | `4c` `5h2` |
| `launch_report(KILL)` が `TAKEN` のまま来たとき `child = 0` + `DONE` にしていた | `9e` `9f` `9h` `9i` `9j` `9l` `9o` `9p` |

3 つ目のために**ケース 9** を足した: `RUNNING` → `cancel` → `take` → `report(0)` (回収通知より
先) → poll は `RUNNING(child)` のまま → `owner_exit(child)` → poll が `DONE`。さらに
「report が先に来た後で要求者が退場しても、子の所有が残っているので孤児回収に載る」
(`9o` `9p`) — `child` を落としていた版ではこの子が誰にも回収されなくなる。

## 5. 最終実行 (2026-09-12)

```
$ python3 -B tools/tests/test_launch.py
HOST ILP32 GNU89 COMPILE PASS
launch request table (T9 K)
...
  ok   8d 自己診断は表を空にして戻る
ALL PASS
TARGET i386-elf GNU89 -Werror COMPILE PASS
EXIT launch_host=0

$ python3 -B tools/tests/test_multiapp_impl.py
HOST ILP32 GNU89 COMPILE PASS
...
  ok   23U その拒否も弾き数に載る
ALL PASS
TARGET i386-elf GNU89 -Werror COMPILE PASS
```

`tools/check_kapi_version.py` と `tools/check_constraints.py` も通した。
`make` (clean build / `make check` 全体 / `make external`) と実機は未実施。
