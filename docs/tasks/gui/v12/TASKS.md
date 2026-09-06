# GUI シェル v1.2 — 作業分担票

> 発行: PM (2026-09-06)  
> 親: [../DESIGN.md](../DESIGN.md)  
> 基底契約: [../API_CONTRACTS.md](../API_CONTRACTS.md)  
> v1.2 追加契約: [CONTRACTS.md](CONTRACTS.md)  
> 前提: GUI v1.1 G1〜G5 完了

---

## 0. 方針

v1.2 は HAL 拡張版ではなく、**v1.1 で完成した GUI 基盤をデスクトップ環境として完成させる版**とする。

PEGC / Cirrus / 9801 backend は原則 freeze し、H lane は regression 検証のみ。

**KAPI v42 は freeze。v43 は network / Host Services 用の予約で GUI は使わない。**

新機能は既存 `gui_call` と `libos32gui` 上に実装する。

v1.1 で問題になった「X4 で重い処理」「暗黙 ABI 変更」「共有 PT」「起動途中状態」のような跨層変更を避け、v1.2 は W / C 中心に進める。

---

## 1. レーンと票

| レーン | 言語 / 場所 | 票 | 内容 |
|---|---|---|---|
| **K** protocol | C / Rust, `sdk/include/os32/`, `sdk/rust/os32api/src/gui/` | [TASK_K5_v12_proto.md](TASK_K5_v12_proto.md) | op 65/66、Modal result、Session request、Quit reason。KAPI追加なし |
| **W** WM | Rust, `userland/gshell/` | [TASK_W3_desktop_session.md](TASK_W3_desktop_session.md) | taskbar / Start / clock / launcher / session state machine / halt |
| | | [TASK_W4_dialogs.md](TASK_W4_dialogs.md) | message / file-open / input dialog、completed result、sticky Modal |
| **C** library | Rust, `userland/rust/libos32gui/` | [TASK_C4_desktop_api.md](TASK_C4_desktop_api.md) | modal/session API、Icon16、shlib entry 95〜100 |
| **C** app | Rust, `userland/rust/filer/` | [TASK_C5_filer.md](TASK_C5_filer.md) | File Manager |
| **H** HAL | C, `gfx/`, `drivers/` | 新規票なし | 9801 / PEGC / Cirrus regression のみ |
| **PM / 検証** | Python / NP21/W | 本書 §5 | 既存 `/api/mouse` を使う自動 gate、契約照合、ROADMAP 更新 |

### レーン原則

- H lane は原則変更禁止。regression で backend bug が見つかった時だけ bug ticket を別に切る。
- `sdk/kapi.json` は v1.2 では変更しない。
- GUI op/event/struct、shlib jump table は末尾追記のみ。
- AppVTable の既存 field 配置は変更しない。
- popup Window ABI は v1.2 では作らない。

---

## 2. 依存と順序

```text
                  ┌────────► W3 ────────────────┐
G0 contract ─► K5 ┤                              ├──► G4 / G5
                  ├────────► W4 ──┐             │
                  └────────► C4 ──┼──► C5 ──────┘
                                  │
existing /api/mouse + gates ──────┘
```

### 並列開始可能

K5 が protocol 数値を commit した後、次の 3 本を並列化できる。

- W3
- W4
- C4

C4 は W4 implementation 完了を待たず wrapper を作れるが、結合 gate G2 は W4 後。

### C5 開始条件

- C4 の公開 API / shlib entry が固定済み。
- W3 `SESSION_REQUEST` が動作。
- W4 modal result が動作。

---

## 3. 衝突ゾーンの所有権

| 所有 | ファイル |
|---|---|
| K5 | `sdk/include/os32/os32_gui_shared.h`、`sdk/rust/os32api/src/gui/proto.rs` |
| W3/W4 | `userland/gshell/**`。W3/W4 間の同一 file は PM が順序を決める |
| C4 | `userland/rust/libos32gui/**`、`sdk/rust/os32api/src/gui/stub.rs` |
| C5 | `userland/rust/filer/**` |
| PM | `docs/tasks/gui/v12/**`、`tools/**`、`docs/ROADMAP.md` |
| H | `gfx/**`、`drivers/**` — freeze |

共有 library jump table は C4 だけが変更する。

既存 entry 0〜94 は変更禁止。v1.2 公開 entry は 95〜100。

`tools/check_gui_proto.py` は PM 所有。K5 の定数追加に合わせて PM が照合項目を更新する。

共有 build/deploy file (`build/programs.mk`, `userland/deploy.yaml`) は PM 経由。

---

## 4. 各票の要約

### K5 — protocol completion

固定:

```text
GUI_OP_MODAL_RESULT      = 65
GUI_OP_SESSION_REQUEST   = 66
GUI_MODAL_INPUT          = 4
GUI_SESSION_LAUNCH       = 1
GUI_SESSION_SWITCH_CUI   = 2
GUI_SESSION_SHUTDOWN     = 3
GUI_QUIT_REASON_REPLACE_APP = 1
GUI_QUIT_REASON_SWITCH_CUI  = 2
GUI_QUIT_REASON_SHUTDOWN    = 3
```

加える struct:

- `GuiReqModalResult` 4B
- `GuiRespModalResult` 260B
- `GuiReqSession` 260B

`GUI_PROTO_VERSION=1` / KAPI v42 を維持。

詳細: [TASK_K5_v12_proto.md](TASK_K5_v12_proto.md)

### W3 — desktop / session

- taskbar 24px
- Start / Programs(max 96) / File Manager / Run / CUI / Shut Down
- work area
- clock dirty rect
- SessionAction 1 件
- external app から session request
- sticky Quit
- nested `exec_run()` 禁止
- CUI cfg 更新失敗時の rollback
- `for (;;) sys_halt()` の terminal halt

詳細: [TASK_W3_desktop_session.md](TASK_W3_desktop_session.md)

### W4 — dialogs

- completed modal result / slot
- result 未 consume 中の next modal = `ERR_FULL`
- wrong/double consume = `ERR_STALE`
- Modal event sticky delivery
- File Open path result
- Input dialog + FEP
- owner cleanup

詳細: [TASK_W4_dialogs.md](TASK_W4_dialogs.md)

### C4 — client API

shlib:

```text
95  modal_open
96  modal_result
97  file_open
98  input_open
99  session_request
100 draw_icon16
```

`SHLIB_NFUNC=101`。

AppVTable は変更しない。File/Input は非同期。

Icon16 は 160B (`4bpp pixels 128B + 1bpp mask 32B`)。

詳細: [TASK_C4_desktop_api.md](TASK_C4_desktop_api.md)

### C5 — File Manager

- 2 pane
- lazy directory tree
- navigate / mkdir / rename / delete / rmdir
- file copy / same-FS move
- `.bin` launch は `SESSION_REQUEST`、直接 exec 禁止
- right-click menu は client surface overlay
- large copy は最大 16KB / event-loop cycle 程度に分割

KAPI v42 の既存 `sys_ls/stat/mkdir/rename/unlink/rmdir/open/read/write/close` だけを使う。

詳細: [TASK_C5_filer.md](TASK_C5_filer.md)

---

## 5. 検証層

v1.1 完了後に NP21/W ai-debug の **`/api/mouse` は実装済み**。v1.2 では新設作業ではなく、この API を gate automation に利用する。

最低操作:

- absolute move
- left down / up
- right down / up

併用:

- `/api/key`
- `/api/screenshot`
- `/api/status`

PM / 検証層は以下を維持・拡張する。

- `tools/check_gui_proto.py`
- `make check-shlib`
- `make check`
- GitHub Actions static gate
- `make external` が必要な SDK / shlib 更新の検出
- 9801 / PEGC / Cirrus 3 backend regression

### PM の成果物 (票なし、ここがチェックリスト)

| 成果物 | 時期 | 内容 |
|---|---|---|
| `tools/check_gui_proto.py` | **G0 の前** (v1.1 で未作成だった) | `os32_gui_shared.h` ⇄ `proto.rs` の定数 (op / event / modal / session / quit reason / flags / 上限) と構造体の大きさ・フィールド並びを照合。`make check` と GitHub Actions に載せる |
| `tools/gui_gate.py` | G1 まで | `/api/mouse` (`ax/ay` = シームレス絶対座標) + `/api/key` + `/api/screenshot` + `/api/status` で G1〜G5 の操作列を回す。v1.1 のドラッグ / 重なり回帰も含む |
| 配備登録 | C5 / C4 のテストアプリ着手時 | `build/programs.mk` (`DEFINE_RUST_PROGRAM`)、`userland/deploy.yaml`、`build/app.conf` |
| 契約照合 | K5 完了時 | C / Rust の値・size・offset、KAPI v42 / 180 関数のまま |
| ROADMAP / TASKS §10 | ゲート通過ごと | 記録 |

### 自動化で必ず mouse を使う項目

- Start button
- taskbar window button
- right-click context menu
- dialog button
- File Manager selection
- v1.1 drag/overlap regression

---

## 6. ゲート

| Gate | 名前 | 通過条件 |
|---|---|---|
| **G0** | v1.2 contract freeze | K5 値 / size / offset 固定、KAPI v42、v43不使用、個別5票存在 |
| **G1** | Desktop | taskbar / Start / clock / focus / Programs を `/api/mouse` で自動操作。clock partial present |
| **G2** | Dialog/API | message/file/input、path/text返却、FEP、sticky Modal、ERR_FULL/STALE、old/new shlib組合せ |
| **G3** | Filer | navigate / mkdir / rename / copy / move / delete / context menu / error path |
| **G4** | Session | app→別app、app→CUI、halt、CTRL+STOP後 pending action、nested exec なし |
| **G5** | Release | pc98 / PEGC / Cirrus + v1.1 G1〜G5 regression + shlib mismatch + `make check` |

---

## 7. 主要シナリオ

### 7.1 app replacement

1. `gui_demo` 起動。
2. Start -> Programs -> File Manager。
3. SessionAction `LAUNCH(/usr/bin/filer.bin)`。
4. `gui_demo` へ sticky `Quit(REPLACE_APP)`。
5. gui_demo 終了。
6. 既存 `exec_run()` が gshell top-level へ戻る。
7. filer 起動。

**WM handler / X3 / X4 内の nested `exec_run()` は 0 回。**

### 7.2 CUI switch

1. filer 起動中。
2. Start -> CUI mode。
3. sticky Quit。
4. filer 終了。
5. cursor hide / FEP callback解除 / gfx shutdown。
6. cfg `GUI=0` 更新成功。
7. shell switch。
8. `ver` 応答。

cfg 書込みを失敗注入した場合は shell switch せず GUI に残る。

### 7.3 System halt

1. app 起動中。
2. Start -> Shut Down。
3. sticky Quit。
4. app 終了。
5. GFX/FEP cleanup。
6. halt screen。
7. `for (;;) sys_halt()`。

タイマ IRQ 等が入っても desktop へ復帰しない。

### 7.4 Unresponsive app

1. `gui_busy`。
2. SessionAction を予約できる文脈で予約。
3. Quit を無視 / loop 継続。
4. CTRL+STOP。
5. owner 回収。
6. pending SessionAction 続行。
7. gshell / kernel 生存。

### 7.5 Modal overflow

1. event ring を人工的に満杯近くまで埋める。
2. File/Input dialog 完了。
3. completed result 保存。
4. Modal event は pending。
5. app が poll して空き生成。
6. Modal event 再配送。
7. `MODAL_RESULT` で value 取得。

### 7.6 Large file copy

1. 十分大きい file を copy。
2. 1 cycle 最大 16KB 程度。
3. copy 中も cursor / clock / event loop が進む。
4. Quit を受けたら copy state/fd を閉じて exit 可能。

---

## 8. v1.2 でやらないもの

- GUI terminal
- CUI output redirect
- multi-process GUI
- popup-window ABI
- drag & drop
- recursive directory copy/delete
- cross-FS move
- thumbnail
- arbitrary icon formats
- new GFX backend
- >640x480
- command-line args / PATH search in Run
- true power-off

---

## 9. 完了定義

v1.2 完成とは、CUI command 入力なしで:

```text
OS boot
 -> GUI
 -> Start
 -> File Manager
 -> file operations
 -> GUI app launch
 -> another app launch
 -> CUI switch / system halt
```

を完走できること。

同じ flow が:

- PC-9801 planar 640x400
- PEGC 640x480
- Cirrus 640x480

で成立し、v1.1 regression がないこと。

---

## 10. 進捗記録

| 日付 | 記録 |
|---|---|
| 2026-09-06 | v1.2 設計採用。`CONTRACTS.md` / `TASKS.md` 初版を発行 |
| 2026-09-06 | 設計再点検を反映: app→gshell の `SESSION_REQUEST`、shutdown の非復帰 halt、sticky Quit/Modal、modal consume 規則、KAPI v42 freeze / v43 network予約、既存 `/api/mouse` 前提、個別 K5/W3/W4/C4/C5 票を追加 |
| 2026-09-06 | **PM 点検 (実装前、コードと突き合わせ)**。**一致を確認**: op 64 / 80、EV_MODAL 11 / EV_QUIT 12、modal kind 0〜3、`GuiString` 256B、`GuiReqModal` 260B (→ `GuiRespModalResult` / `GuiReqSession` 260B は整合)、shlib entry 95 本 (0〜94)、KAPI v42 に `sys_time` (`os_time_t`) / `sys_ls`〜`sys_close` / `sys_halt` / `sys_switch_shell` / `exec_run` あり、`OS32_ERR_FULL` -13 / `STALE` -11 あり、libos32gui の U3 ループは `GUI_EV_QUIT` で戻る (v1.1 アプリも Quit で終了できる)。**ずれ・不足**: (1) `OS32_ERR_AGAIN` は無い → C4 の modal_result は「event 到着後だけ呼ぶ API」で確定 (未完成 query は公開しない)。(2) **`tools/check_gui_proto.py` は v1.1 で未作成** (`make check` にも無い) → G0 の前に PM が作る (C ⇄ Rust の定数 / size / offset 照合、CI にも載せる)。(3) **gshell は右ボタンを一切扱っていない** (`input.rs` は左のエッジのみ) → W3 に「右ボタンのエッジ取り込み + `wm_owns_edge` の扱い + Button イベント配送」を明記する (D4 / F5 の前提)。(4) gshell は今 `GUI_EV_QUIT` を送っていない → W3 の sticky Quit は新設 (既存経路なし)。(5) 現行の ESC (「CUI へ」) は cfg を書かない即時切替。S6 の SWITCH_CUI は `GUI=0` を永続化する → **ESC を SessionAction 経由にするか (永続化)、開発用の一時切替として残すかは要決定**。(6) 現行の上部バー (`ESC:CUI F1..F5`) と v1.2 の下部 taskbar の関係が未記載 → v1.2 で撤去 / デバッグ用に残すを W3 に書く。(7) `OS32ShlibHeader.version` は 1 のまま (nfunc で拒否) と明記する。(8) Programs は `/usr/bin` の全 `.bin` (テスト用も含む ~40 本) を並べる → v1.2 はそれでよいが、将来のフィルタ (manifest / 拡張子以外の印) を対象外として記す。(9) PM 側の成果物 (`check_gui_proto.py`、`/api/mouse` を使う gate スクリプト、filer / v12_api_test の deploy 登録) に票が無い → 本書 §5 に PM チェックリストとして列挙する。(10) `make external` は apps/game が libos32gui を使わない限り不要 (SDK の libos32gfx を変えたときだけ) と §9 に注記。実装はユーザー指示待ち |
| 2026-09-06 | **ユーザー決定**: ESC 即時切替は完成後 (G5) に廃止。CUI へ戻るのは Start → 確認ダイアログ → SessionAction の 1 本だけ (無言の 1 キーで CUI に落ちると GUI の状態が失われて危険)。開発中はデバッグ用に残す。上部バーも同じ扱い。**着手前の提案はすべて承認** → 反映: CONTRACTS S6 / D2 / D4 / V12-C / §8 / §9、W3 §4.1〜4.2 (デバッグ残置と撤去、右ボタン)、C4 (AGAIN 無し、shlib version 1)、本書 §5 の PM チェックリスト。**`tools/check_gui_proto.py` を作成** (定数 96 件 / 構造体 28 件、packed と union 対応、C 側に `GUI_SLOT_MAX` を追加して Rust と揃えた)、`make check-gui-proto` と GitHub Actions に組み込み。G0 の前提が揃った。実装 (K5 →) は次の指示で開始 |
