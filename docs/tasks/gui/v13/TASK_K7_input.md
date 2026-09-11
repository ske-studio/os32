# K7 — 入力統合: GUI 中の `kbd_getchar` を端末アプリの打鍵で満たす (設計草案)

状態: **設計草案 (2026-09-12、PM) — 独立レビュー待ち。実装は発注していない。**
親: [PLAN.md](PLAN.md) §1 (決裁 B: K5 → K6 console → 端末アプリ → **K7 入力統合** → 既存 CUI コマンドを端末で流す)。
前提: K5b (協調型 4 本、`exec_park` / `exec_resume`)、K6C (con_sink、端末アプリ `t5a_display`)。

## 0. いま起きること (確認済み)

- GUI モード中、IRQ1 は **raw リングにだけ**積む (`drivers/kbd.c:242`、WM が `kbd_trygetrawkey` で読む)。cooked リング `kbd_buf` は空のまま。
- CUI プログラムを gshell の Run で起動すると (K5b で可能)、そのプログラムが `kbd_getchar()` (KAPI、`drivers/kbd.c:351`) を呼んだ瞬間に
  カーネルの `for(;;) { if (kbd_count) …; hlt; }` に入り、**戻らない**。syscall の中なので OP_WAIT の park も起きず、
  協調型の全体 (gshell と他の 3 本) が止まる。脱出は CTRL+STOP (IRQ1 の abort 要求) だけで、それも `hlt` ループが要求を見なければ効かない。
- 端末アプリは K6C-A で出力側だけ持つ。打鍵は gshell からフォーカス窓へ `GUI_EV_KEY` / `GUI_EV_TEXT` (FEP 確定文字、UTF-8) で届く
  (`userland/gshell/src/ring.rs:117-121`)。

## 1. 設計 (PM 案、レビュー対象)

| # | 決定 | 根拠 |
|---|---|---|
| D1 | **kbd の待ちを第 2 の park 点にする**: GUI モード中、CPL=3 の呼び手 (syscall フレームあり) が `kbd_getchar` / `kbd_getkey` で cooked が空なら `hlt` せず **`exec_park` と同じ手順で止める** (状態 `APP_STATE_WAIT_KEY`、印 `parked_from_kbd`)。`resume` は `wait_ret` に**その文字**を入れるので、アプリから見ると `kbd_getchar()` が普通に値を返したように見える (OP_WAIT の `wait_ret` と同じ仕掛け、`exec/exec.c:1600` 付近) | K5b の切替点は「park されたフレームからのみ resume」(G7)。印を分けて、WM が「鍵待ち」と「OP_WAIT 待ち」を区別できるようにする。`hlt` ループを残すと協調型が壊れる |
| D2 | **打鍵の注入は KAPI 1 本** `kbd_inject(const u8 *utf8, u32 len)` (v47、末尾追記)。呼べるのは **con_sink の読み手** (= 端末アプリ、`g_reader`) だけ。カーネルは注入リング (256B、静的) に積む。GUI モード中の `kbd_getchar` / `kbd_trygetchar` / `kbd_getkey` は**この注入リングだけ**を見る (IRQ1 の cooked は GUI 中は使わない、現状どおり) | 端末アプリは CPL=3 で他アプリを起こせない (owner 1 専用)。注入と起床を分けると WM の規則 (D11) をそのまま使える |
| D3 | **起床は WM (gshell) の OP_WAIT の中**: `exec_app_state` に `WAIT_KEY` を足し、`multiapp::ready_to_run` を「`WAIT_KEY` かつ注入リングが空でない」でも真にする (`kbd_inject` の戻り値か `con_sink_stat` 拡張で pending を知らせる)。選択規則は D11 のまま (ID 昇順、入力優先の streak 上限 4)。resume 時にカーネルが注入リングから 1 文字 (UTF-8 なら 1 バイト) を取り `wait_ret` に入れる | プリエンプションは足さない (ユーザー決裁)。鍵待ちのアプリが複数なら D11 が決める |
| D4 | **FEP / 日本語**: 端末アプリは `GUI_EV_TEXT` の UTF-8 をそのまま `kbd_inject` に渡す。`kbd_getchar` は 1 バイトずつ返す (既存 CUI コマンドは UTF-8 バイト列を読む前提: §4-27) | 変換は gshell 側に既にある。カーネルで再変換しない |
| D5 | **CTRL+STOP**: `WAIT_KEY` で止まっているアプリは `exec_kill` で畳める (K5c の経路、フォーカス窓宛)。注入リングはアプリ終了 (`exec_reclaim_owned`) と CUI 復帰で捨てる | 既存の回収に並べるだけ |
| D6 | **CUI モードは無変更**: `kbd_gui_mode` が 0 なら従来の cooked + `hlt`。rshell のタイムアウト経路も無変更 | 回帰 6 本と v86 を守る |
| D7 | **`kbd_getkey` (スキャンコード付き)** は GUI 中は下位 8 bit だけ意味を持つ (スキャンコードは 0)。`kbd_is_pressed` / `kbd_get_modifiers` は raw 側の状態をそのまま | 端末経由ではスキャンコードが無い。必要になったら `GUI_EV_KEY` を別リングで注入する拡張を後で |

メモリ: 注入リング 256B + AppSlot の印 1 語 × 5。`MEMORY_BUDGET.md` に計上。

## 2. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| I1 | 止まらない | gshell で端末を出し、Run で `kbd_getchar` を呼ぶ CUI プログラム (`input_test` 等、無ければ小さな試験プログラム) を起動 → gshell と他アプリが動き続ける (`ring3_switch_count` が増える、クリックが届く) |
| I2 | 打鍵が届く | 端末窓にフォーカスして文字を打つ → CUI プログラムが受け取り、その出力が端末窓に出る (K6C の経路)。日本語は FEP 確定文字が UTF-8 バイト列で届く |
| I3 | CTRL+STOP | 鍵待ちのプログラムをフォーカス窓宛の CTRL+STOP で畳める。`fault_kill_count` は増えない |
| I4 | 回帰 | regress 6 本、v86 -t、CUI の `kbd_getchar` (シェルの入力) が従来どおり。8MB / 15MB |
| I5 | 切替点 | `ring3_resume_bad_frame_count` 0、`parked_from_kbd` 無しの resume を拒否する負例 (kselftest) |

## 3. レビューで見てほしい点

1. D1 の「syscall の中から park」は OP_WAIT と同じ機構だが、`kbd_getchar` は **`hlt` を前提に書かれた呼び手** (rshell タイムアウト等) を持つ。GUI 中だけ分岐する境界が正しいか。
2. D2 の権限 (con_sink の読み手だけが注入できる) で足りるか。端末が 2 本のときの挙動 (busy 側は注入も拒否)。
3. D3 で「鍵待ちのアプリが注入リングを共有する」ことの是非 (端末 1 本 / CUI 1 本の v1.3 では問題にならないが、複数 CUI が同時に鍵待ちなら最初の 1 本が全部取る)。

## 4. 範囲外

- 端末から CUI コマンドを起動する UI (次段「既存 CUI コマンドを端末で実際に流す」)。full-screen GFX 復帰。shell script。
- `kbd_getkey` のスキャンコード完全再現。
