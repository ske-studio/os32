# GUI シェル v1.2 — 作業分担票

> 発行: PM (2026-09-06)  
> 親: [../DESIGN.md](../DESIGN.md)  
> 基底契約: [../API_CONTRACTS.md](../API_CONTRACTS.md)  
> v1.2 追加契約: [CONTRACTS.md](CONTRACTS.md)  
> 前提: GUI v1.1 G1〜G5 完了

---

## 0. 方針

v1.2 は HAL 拡張版ではなく、**v1.1 で完成した GUI 基盤をデスクトップ環境として完成させる版**とする。

PEGC / Cirrus / 9801 バックエンドは原則 freeze し、H レーンは回帰検証のみとする。

KAPI v42 は freeze する。

新機能は可能な限り既存 `gui_call` と `libos32gui` 上に実装する。

v1.1 で問題になった「X4 で重い処理」「暗黙の ABI 変更」「共有 PT」「起動途中状態」のような跨層変更を避け、v1.2 は W / C 中心に進める。

---

## 1. レーンと票

| レーン | 言語 / 場所 | 票 | 内容 |
|---|---|---|---|
| **K** プロトコル | C / Rust, `sdk/include/os32/`, `sdk/rust/os32api/src/gui/` | `TASK_K5_v12_proto.md` | `MODAL_RESULT` / `INPUT` / Quit reason の末尾追記。KAPI 追加なし |
| **W** WM | Rust, `userland/gshell/` | `TASK_W3_desktop_session.md` | taskbar / Start / clock / launcher / session state machine |
| | | `TASK_W4_dialogs.md` | message / file-open / input dialog 完成、結果保持 |
| **C** ライブラリ | Rust, `userland/rust/libos32gui/` | `TASK_C4_desktop_api.md` | modal 結果 API、icon16、client menu helper |
| **C** アプリ | Rust, `userland/rust/filer/` | `TASK_C5_filer.md` | File Manager |
| **H** HAL | C, `gfx/`, `drivers/` | 新規票なし | 9801 / PEGC / Cirrus の回帰のみ |
| **PM / 検証** | Python / NP21/W | 本書 §5 | mouse 注入、自動ゲート、契約照合、ROADMAP 更新 |

### レーン原則

- H レーンは原則変更禁止。回帰で実装バグが見つかった場合のみ bug ticket を別に切る。
- KAPI v42 は変更しない。
- GUI プロトコル番号、イベント、共有ライブラリジャンプ表は既存値を変更せず末尾追記のみ。
- AppVTable の既存フィールド配置は変更しない。

---

## 2. 依存と順序

```text
             ┌── K5 ──┬── W4 ──┐
G0 契約凍結 ─┤        │        ├── C4 ──► C5 ──┐
             │        └────────┘               │
             └──────── W3 ──────────────────────┼──► G4 / G5
                                               │
mouse injection / test harness ────────────────┘
```

### 並列開始可能

- K5
- W3
- PM mouse injection

### K5 完了後

- W4
- C4

### C5 開始条件

C5 は C4 / W4 の公開 API が固まってから開始する。

---

## 3. 衝突ゾーンの所有権

| 所有 | ファイル |
|---|---|
| K | `sdk/include/os32/os32_gui_shared.h`、対応する Rust proto 定義 |
| W | `userland/gshell/**` |
| C4 | `userland/rust/libos32gui/**`、`sdk/rust/os32api/src/gui/stub.rs` |
| C5 | `userland/rust/filer/**` |
| PM | `docs/tasks/gui/v12/**`、`tools/**`、`docs/ROADMAP.md` |
| H | `gfx/**`、`drivers/**` — 原則 freeze |

共有ライブラリのジャンプ表は C4 のみが変更する。

既存 entry 0〜94 は変更禁止。新規 API は 95 以降へ追記する。

---

## 4. 各票のゴール

### K5 — v1.2 protocol completion

追加するもの:

- `GUI_OP_MODAL_RESULT = 65`
- `GUI_MODAL_INPUT = 4`
- `GuiReqModalResult`
- `GuiRespModalResult`
- `GUI_QUIT_REASON_*`

既存値は一切変更しない。

`GUI_PROTO_VERSION = 1` を維持する。

KAPI v42 を維持する。

#### 完了条件

- `check_gui_proto.py` が通る。
- C / Rust 定義が一致する。
- v1.1 クライアントが無変更でビルドできる。
- 既存の `GUI_OP_MODAL_OPEN = 64` と既存イベント番号が不変。

---

### W3 — desktop / session

新規モジュール候補:

```text
desktop.rs     既存: 背景 + taskbar
startmenu.rs   Start メニュー
session.rs     pending launch / CUI / shutdown
```

#### 作業

1. taskbar 24 px
2. Start button
3. window buttons
4. clock
5. work-area
6. Start menu
7. `/usr/bin/*.bin` program list
8. pending launch
9. replace-current-app state machine
10. CUI switch
11. shutdown
12. desktop context menu

#### 必須設計条件

アプリ実行中に WM の `gui_call` / X3 / X4 から `exec_run()` してはならない。

launch / CUI / shutdown はすべて pending 化し、現在の `exec_run()` が戻ってからトップレベルで実行する。

X4 では入力と session action の予約までに留める。

Start メニューの `/usr/bin` 列挙は X3 またはトップレベルで行い、毎フレーム実行しない。

#### 完了条件

- taskbar がアプリ Window 上限 16 と SHM スロット 4 本を消費しない。
- 640×400 / 640×480 の両方で作業領域が正しい。
- clock 更新で全画面 present しない。
- Start から現在アプリとは別のアプリを選んでも nested `exec_run()` が発生しない。

---

### W4 — standard dialogs

#### 作業

1. MessageBox
2. File Open
3. Input
4. completed result / slot
5. `MODAL_RESULT`
6. owner cleanup
7. overflow 時にも path / text を失わない
8. ESC / Cancel
9. dialog 下地の damage 復元
10. Input dialog で FEP を使用可能にする

#### 完了条件

- 親ループを入れ子にしない。
- File Open で 255 B までのフルパスが返る。
- Input で UTF-8 文字列が返る。
- FEP 入力が Input dialog でも動く。
- owner kill 後に modal state / completed result が残らない。
- イベントリング overflow が起きても completed result 自体は失われない。

---

### C4 — desktop client API

追加候補:

- `modal_open`
- `modal_result`
- `open_file_dialog`
- `input_dialog`
- `draw_icon16`
- client-side menu helper

共有ライブラリジャンプ表へ末尾追記する。

既存 95 本の番号は完全固定する。

#### 互換条件

新しいアプリ + 古い shlib:

- `nfunc too short` で拒否する。

古いアプリ + 新しい shlib:

- 正常起動する。

版不一致試験は v1.1 で未実走だったため、この票で必ず実機実行する。

#### 完了条件

- `make check-shlib`
- jump table 旧 0〜94 不変
- 新 API は 95 以降
- 古い `gui_demo` が新 shlib で起動
- 新テストアプリが旧 shlib を安全に拒否

---

### C5 — File Manager

新規アプリ:

```text
/usr/bin/filer.bin
```

画面イメージ:

```text
┌──────────────────────────────────────────────┐
│ File Manager                                 │
├────────────────┬─────────────────────────────┤
│ [-] /          │ name        size   type     │
│   [-] usr      │ gui_demo    ...    BIN      │
│     bin        │ ...                         │
│   etc          │                             │
│   sys          │                             │
├────────────────┴─────────────────────────────┤
│ /usr/bin                                     │
└──────────────────────────────────────────────┘
```

#### 必須

- navigate
- launch
- mkdir
- rename
- delete
- rmdir
- copy
- move
- context menu
- confirmation dialog

v1.2 では TreeView ABI を増やさず、既存 listbox を使う。

`.bin` 起動は W3 の pending launch / replace-current-app 経路を使う。ファイラー自身のイベント処理中に別アプリを nested exec しない。

#### 対象外

- drag & drop
- recursive directory copy
- thumbnail

---

## 5. 検証層

v1.2 では**マウス操作の自動化を必須前提**にする。

v1.1 で残った「NP21/W ai-debug にマウス注入 API がなく、ドラッグ / 重なりを自動確認できない」をここで解消する。

最低 API:

```text
/api/mouse?x=...&y=...
/api/mouse?buttons=...
```

必要操作:

- move
- left down / up
- right down / up

相対移動は不要。

これを `os32-cycle` / GUI ゲートから使用する。

PM / 検証層は併せて以下を維持する。

- `check_gui_proto.py`
- `make check-shlib`
- `make check`
- `make external` が必要な SDK / shlib 更新の検出
- 9801 / PEGC / Cirrus 3 バックエンドの回帰

---

## 6. ゲート

| Gate | 名前 | 通過条件 |
|---|---|---|
| **G0** | v1.2 契約凍結 | `CONTRACTS.md` 確定、KAPI v42 維持、GUI ABI 末尾追記確認 |
| **G1** | Desktop | taskbar / Start / clock / window focus をマウスで自動操作 |
| **G2** | Dialog | message / file / input、path / text 返却、FEP、overflow / owner cleanup |
| **G3** | Filer | navigate / mkdir / rename / delete / copy / launch |
| **G4** | Session | app→別 app、app→CUI、shutdown、CTRL+STOP 後の pending action |
| **G5** | Release | pc98 / PEGC / Cirrus + v1.1 G1〜G5 + shlib mismatch + `make check` |

---

## 7. G4 セッション試験の具体例

### 7.1 アプリ置換

1. `gui_demo` 起動
2. Start → Programs → File Manager
3. `gui_demo` へ `Quit(REPLACE_APP)`
4. `gui_demo` 終了
5. gshell の既存 `exec_run()` が戻る
6. `filer` 起動

**WM ハンドラ内の nested exec が 1 回も発生しないこと。**

### 7.2 CUI 切替

1. `filer` 起動中
2. Start → CUI mode
3. `filer` へ Quit
4. `filer` 終了
5. FEP render callback 解除
6. `gfx_shutdown()`
7. `/etc/system.cfg` を `GUI=0`
8. `shell.bin` へ切替
9. `ver` 応答

### 7.3 Shutdown

1. アプリ実行中
2. Start → Shut Down
3. Quit 配送
4. アプリ終了
5. FEP render callback 解除
6. `gfx_shutdown()`
7. `sys_halt()`

### 7.4 応答しないアプリ

1. `gui_busy`
2. session action を予約
3. アプリが Quit に応答しない場合 CTRL+STOP
4. app 回収
5. pending session action を続行
6. gshell / kernel が生存

---

## 8. v1.2 でやらないもの

次は v1.3 以降とする。

- GUI terminal
- CUI output redirect
- multi-process GUI
- popup-window ABI
- drag & drop
- recursive directory copy
- thumbnail
- arbitrary icon formats
- new GFX backend
- higher resolution support

---

## 9. 完了定義

v1.2 完成とは、次の一連の操作を **CUI コマンド入力なし**で完走できること。

```text
OS 起動
  → GUI
  → Start メニュー
  → File Manager
  → ファイル操作
  → GUI アプリ起動
  → 別アプリへ切替
  → CUI へ戻る / shutdown
```

かつ同じ手順が以下の 3 バックエンドで成立すること。

- PC-9801 planar 640×400
- PEGC 640×480
- Cirrus 640×480

さらに v1.1 G1〜G5 の既存回帰、Ring3 表示面隔離、PC98 fallback BB、共有ライブラリ保護を維持すること。
