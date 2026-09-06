# GUI シェル v1.2 — 追加契約

> 発行: PM (2026-09-06)  
> 親契約: [../API_CONTRACTS.md](../API_CONTRACTS.md)  
> 親設計: [../DESIGN.md](../DESIGN.md)  
> 作業分担: [TASKS.md](TASKS.md)
>
> v1.1 の G / T / U 契約は変更しない。本書は v1.2 で必要な事項だけを追加する。  
> 原則: 既存番号・構造体・共有ライブラリのジャンプ表は変更せず、**末尾追記のみ**。

---

## 0. v1.2 の範囲

v1.2 は HAL 拡張版ではなく、v1.1 で完成した GUI 基盤を**デスクトップ環境として完成させる版**とする。

v1.1 で実機通過した 9801 / PEGC / Cirrus の 3 バックエンド、Ring3 分離、共有ライブラリ、FEP、モーダル、入力ポンプを基盤として固定し、v1.2 では次を完成させる。

- タスクバー
- Start メニュー
- ランチャー
- 時計
- 標準ダイアログの結果返却
- ファイルマネージャ
- GUI セッション終了 / CUI 切替
- マウス操作を含む自動回帰検証

v1.2 では KAPI v42 を変更しない。新機能は既存 `gui_call(op, arg)` と GUI 共有プロトコル、`libos32gui.shlib` の末尾追記で実現する。

---

## 1. V12-S: セッションモデル

### S1. 外部 GUI アプリは同時に 1 本

v1.2 でも実行モデルは協調型シングルフォアグラウンドアプリとする。

- gshell はシェル帯に常駐する。
- 外部アプリは `exec_run()` で 1 本だけ実行する。
- 1 アプリは複数ウィンドウを所有できる。
- タスクバーのウィンドウボタンは「プロセス切替」ではなく、そのアプリ内のウィンドウの raise / focus を行う。
- v1.2 では複数外部プロセスの同時 GUI 実行を導入しない。

### S2. アプリ実行中の別アプリ起動

WM の `gui_call` ハンドラ、X3、X4 から直接 `exec_run()` を呼んではならない。

別アプリ起動要求が発生した場合:

1. `next_launch_path` を gshell に保存する。
2. 現在のアプリへ `Quit{reason=REPLACE_APP}` を配送する。
3. 現アプリが終了し、既存の `exec_run()` が gshell のトップレベルへ戻る。
4. トップレベルループが `next_launch_path` を見て次のアプリを起動する。

アプリが Quit に応答しない場合は既存の CTRL+STOP で回収する。回収後も保留中の起動要求は維持する。

### S3. セッションアクション

gshell は次の内部状態を 1 つ持つ。

- `NONE`
- `LAUNCH(path)`
- `SWITCH_CUI`
- `SHUTDOWN`

アプリ実行中に要求された場合は S2 と同じく、まず `Quit` を配送し、アプリ終了後にトップレベルで実行する。

`SWITCH_CUI`:

- `/etc/system.cfg` の `GUI=0` を永続化する。
- GUI の FEP 描画コールバックと GFX を安全に終了する。
- `sys_switch_shell("/sys/shell.bin")` で CUI シェルへ切り替える。
- 切替だけのための不要な再起動は行わない。

`SHUTDOWN`:

- アプリ終了後に FEP 描画コールバックを解除する。
- `gfx_shutdown()` する。
- `sys_halt()` する。

### S4. X4 bounded-work

syscall 境界ポンプ X4 では以下を禁止する。

- VFS ディレクトリ走査
- ファイル読み書き
- `exec_run`
- `system.cfg` 更新
- モーダルのディレクトリ再走査
- Start メニュー項目の生成

X4 は入力を取得して**要求を保留するところまで**とする。

VFS、起動、終了処理、メニュー構築などは X3 または gshell トップレベルで行う。

---

## 2. V12-D: デスクトップ

### D1. タスクバー

タスクバーは WM 自身の UI とし、アプリ用 Window / Surface / SHM スロットを消費しない。

- 高さ: 24 px
- 画面下端固定
- 左: Start
- 中央: 通常トップレベルウィンドウのボタン
- 右: 時計
- WM 内部描画 (契約 U8)
- タスクバー領域のマウス入力はアプリへ配送しない

クライアント作業領域:

```text
work_height = screen_height - 24
```

- 640×400 → 640×376
- 640×480 → 640×456

既存アプリとの互換性のため、ウィンドウが作業領域外へ存在すること自体は fault にしない。新規配置・ドラッグ時だけ作業領域へクランプする。

### D2. Start メニュー

Win95 風の WM 内部メニューとする。

最低項目:

- Programs
- File Manager
- Run...
- CUI mode
- Shut Down

`Programs` は `/usr/bin` の `.bin` を列挙する。

ディレクトリ走査はメニューを開いた X3 で行い、そのセッション中はキャッシュする。毎フレーム走査しない。

### D3. 時計

`sys_time` / tick を用い、1 秒単位で更新する。

時計更新だけで全画面再描画してはならない。時計矩形のみ dirty にする。

---

## 3. V12-M: 標準ダイアログ完成

### M1. `GUI_OP_MODAL_RESULT`

既存:

```c
#define GUI_OP_MODAL_OPEN 64
```

に続けて、末尾追記で:

```c
#define GUI_OP_MODAL_RESULT 65
```

を追加する。

要求:

```c
typedef struct {
    u16 dialog;
    u16 _pad;
} GuiReqModalResult;
```

応答:

```c
typedef struct {
    i16 result;
    u16 dialog;
    GuiString value;
} GuiRespModalResult;
```

`value` の意味:

- Message box: 空
- File Open: 選択したフルパス
- Input: 入力された UTF-8 文字列

最大 255 B。

### M2. 完了結果の保持

WM は各 GUI スロットにつき最低 1 件の completed-modal result を保持する。

ダイアログ完了時:

1. completed result を保存する。
2. その後 `Modal{dialog,result}` イベントをリングへ積む。

したがってイベントリングが overflow しても、選択パスや入力文字列そのものを失わない。

`MODAL_RESULT(dialog)` 成功後に結果を consume する。

owner 回収時には completed result も破棄する。

### M3. Input dialog

追加:

```c
#define GUI_MODAL_INPUT 4
```

prompt は既存 `GuiReqModal.message` を使用する。

入力値は `MODAL_RESULT` で返す。

v1.2 では初期値指定・パスフィルタ・複数選択は対象外。

---

## 4. V12-C: クライアント側追加

KAPI v42 は変更しない。

`gui_call()` が既に汎用入口なので、v1.2 の追加は GUI プロトコル内だけで完結させる。

`GUI_PROTO_VERSION` も 1 のままとし、互換な末尾追記だけを行う。

`libos32gui.shlib` のジャンプ表にも末尾追記のみ行う。

最低追加 API:

- `modal_open`
- `modal_result`
- `file_open_dialog`
- `input_dialog`
- `draw_icon16`
- client-side menu helper

互換条件:

- 古いアプリ → 新しい shlib: そのまま動く。
- 新しいアプリ → 古い shlib: 必要 `nfunc` が不足するため既存 bind 検査で拒否する。
- AppVTable の既存フィールド配置は変更しない。

---

## 5. V12-I: アイコン

v1.2 のアイコンは 16×16 を標準とする。

形式:

- 16×16
- 4 bpp = GUI システム 16 色
- 1 bit transparency mask
- 拡大縮小なし

PNG / BMP / ICO デコーダや任意サイズアイコンは v1.4 以降とする。

WM と libos32gui で同じ論理形式を使用する。

---

## 6. V12-F: ファイルマネージャ

Win3.1 風 2 ペインとする。

左:

- ディレクトリ一覧
- インデント + `[+]` / `[-]` による簡易ツリー

右:

- ファイル一覧

v1.2 必須操作:

- ディレクトリ移動
- `.bin` 起動
- mkdir
- rename
- delete / rmdir
- file copy
- 同一 FS 内 move
- context menu
- file open dialog との連携

専用 TreeView ABI は作らない。既存 listbox + クライアント側ロジックで実装する。

drag & drop、再帰ディレクトリコピー、サムネイルは対象外。

---

## 7. V12-P: 性能・安全条件

- X4 で VFS / exec をしない。
- 時計更新で fullscreen present しない。
- Start メニューのディレクトリ走査は open 時のみ。
- taskbar はアプリ用 Window 上限 16 を消費しない。
- taskbar はアプリ用 SHM スロット 4 本を消費しない。
- PC98 / PEGC / Cirrus の 3 バックエンドで同じデスクトップ機能を提供する。
- v1.1 の G1〜G5 回帰をすべて維持する。
- v1.1 で成立した Ring3 表示面隔離、PC98 fallback BB、PCD/PWT 保持を変更しない。

---

## 8. v1.2 対象外

以下は v1.3 以降へ送る。

- 複数外部アプリの同時実行
- プリエンプティブ GUI
- GUI ターミナル
- CUI 出力リダイレクト
- 任意ウィンドウ外へ出るアプリ用 popup-window ABI
- drag & drop
- 再帰ディレクトリコピー
- サムネイル
- PNG / BMP / ICO 等の任意アイコン形式
- 新しい GFX バックエンド
- 640×480 を超える解像度
