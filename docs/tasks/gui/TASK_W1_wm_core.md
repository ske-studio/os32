# W1: gshell 本体 — ウィンドウマネージャの核

> 発行: PM (2026-09-05) / レーン: W (Rust, `userland/gshell/` 新規) / 前提: K1 完了 (共有ヘッダ、`gui_register`) + H1 完了 (`gfx_stats`)
> 親: [TASKS.md](TASKS.md) / 契約: T2, T2a, T3, T4, U1, U2, U5, U8, G4, G6 / 設計: [DESIGN.md](DESIGN.md) §1 R1〜R4, §9.2
> 排他: `userland/gshell/**`。`build/programs.mk` / `userland/deploy.yaml` への追加は PM 経由

## ゴール

**シェル帯 (0x300000, CPL=0) に常駐する GUI シェル**を作る。アプリから見える WM の機能
(窓・Z 順・フォーカス・入力配送・損傷統合・commit・クローム) を全部ここに置く。
既存 `userland/rust/libos32gui` のウィンドウ管理部 (`Window` 配列、Z 順、フォーカス、
枠描画、`gui_pump`) を**移設**して核にする。契約の T / U をアプリの側から満たすのが W1、
アプリの側 (スタブとウィジェット) は C2。

v1.1 の gshell は CUI シェルから `gshell` コマンドとして起動できればよい (Level 0 起動は K4)。
起動中は CUI シェルと同じ帯域を使うので、**gshell は shell.bin と排他** (どちらか 1 本)。

## 構成

```
userland/gshell/
  Cargo.toml        staticlib, no_std, panic=abort (libos32gui と同じ)
  src/main.rs       起動: gfx_init(enter) → パレット (G6) → gui_register → デスクトップ → ループ
  src/wm.rs         Window 表 (16)、Z 順、フォーカス、所有者、Configure
  src/slot.rs       SHM スロット 4 本の割当と回収 (T2a)
  src/ring.rs       イベントリング (128 × 16B)、合成 (Pointer 畳み、Paint 統合)、OVERFLOW
  src/damage.rs     損傷矩形 (8/窓、32px 境界、隣接結合 = gfx_add_dirty_rect と同じ規則)
  src/chrome.rs     枠・タイトルバー・閉じるボタン・XOR ドラッグ枠 (2 色版は W2)
  src/input.rs      kbd_trygetkey / kbd_get_modifiers / mouse_poll → Key / Text / Pointer / Button
  src/timer.rs      8 本/アプリ、tick 比較、OP_WAIT の期限計算
  src/handler.rs    gui_call ハンドラ: op → 関数表 (match の巨大化を避ける)、owner 検証
  src/pump.rs       K2 が呼ぶ gui_pump(): input.rs → ring.rs への追記だけ
  src/desktop.rs    背景、(v1.2 でタスクバー)
```

- 描画は既存 libos32gfx を FFI (v2 C8)。C1 の G API が使えるようになったら共用してよい
  (gshell は CPL=0 なので経路は直接呼び出し、契約 U8)。
- 既存 libos32gui から持ってくるのは `types.rs` の `Window` / `MouseInfo` と `lib.rs` の
  ウィンドウ管理・枠描画・イベント配送。**ウィジェットは持ってこない** (C2 の領分)。

## 作業 (順に)

1. **骨格**: `gshell` コマンドで起動、`gfx_init`、G6 のパレットを `gfx_set_palette` で入れる、
   `gui_register(handler, pump)`、デスクトップを塗って `OP_WAIT` 相当のループ
   (`sys_halt`) で待つ。ESC で終了 (`gfx_shutdown` → CUI シェルへ戻る)。
2. **スロットとハンドラ**: `OP_INIT` で owner にスロットを割り当て (v1 は常に 0)、
   `proto_version` を照合 (`ERR_VERSION`)、以後の op は owner ↔ スロットを検証。
   `gui_owner_exit(owner)` (K1-5) で全回収。
3. **ウィンドウ**: `CREATE` 〜 `SET_FOCUS` (契約 U1)。要求は SHM の要求ブロックから読む。
   結果は戻り値 (ハンドル) と `Configure` イベント。座標の正はここ (アプリは持たない)。
4. **イベントリングと入力**: `input.rs` が `Key` (スキャンコード + ch + mods) と
   `Text` (印字可能文字、FEP は W2) と `Pointer` / `Button` を作り、フォーカス窓の
   所有者のスロットへ追記。`Pointer` は最新 1 件に畳む。`serial` を振る。
   `OP_POLL` は呼んだ owner のスロットの件数を返す (読み出しはアプリが SHM から直接)。
   `OP_WAIT(timeout)` はリングが空かつタイマ未到来なら `sys_halt` で待つ。
5. **損傷と commit**: `INVALIDATE` で損傷を統合し `Paint{window, rect}` を発行、
   `COMMIT` で損傷分を `gfx_present_rect` (H1 表経由)。全画面 present は WM だけ
   (起動時、フルスクリーン GFX からの復帰)。`gfx_stats().commits` が 1 周 1 回であることを
   ここで守る。
6. **クローム**: 枠・タイトルバー・閉じるボタン (色は G6 の役割名)。ドラッグ中は XOR 枠だけ、
   ドロップで 1 回 `Configure` + `Paint` (R2)。閉じるボタンは `Close` イベント (破棄は
   アプリ)。9801 バックエンド (`TEXT_OVERLAY` あり) ではタイトル文字を TVRAM で描いてよい
   (R4、任意)。
7. **マウスカーソル**: 既存スプライト機構 (libos32gfx) で。移動は損傷に含めない
   (カーソルだけ別経路で復元・描画)。
8. **タイマ**: `SET` / `KILL`、`Timer` イベント。`OP_WAIT` の期限は次のタイマまで。

## 鉄則

- **アプリへコールバックしない** (T1)。すべてイベント。
- **ハンドラの中で描かない、待たない。** `gui_call` は数十 µs で戻る。
- `pump` (K2 が syscall 境界で呼ぶ) は `input.rs → ring.rs` の追記だけ。KAPI を呼ばない、
  描画しない (K2 票の契約)。
- 640×400 / 16 色を書かない。`gfx_screen_info()` を起動時に 1 回読む。
- `get_tick` スピン禁止。待ちは `sys_halt` のみ。
- Rust: no_std、外部クレート無し、固定配列。

## 完了条件 (ゲート G2 の W 側)

- `gshell` 起動 → デスクトップが出る → ESC で CUI に戻る、を 10 回繰り返して
  `kselftest` / rshell が生きている。
- C2 の `gui_demo` (CPL=3、別プロセス) が窓 2 枚を出し、XOR ドラッグ・前面化・フォーカス・
  閉じるが動く。`gui_demo` を CTRL+STOP で kill しても窓が全部消える (owner 回収)。
- `gfx_stats()` で 1 周あたり `commits = 1`、`gui_call` の回数が `POLL + COMMIT + WAIT` の
  3 回 (+ 状態変更分) であること (P)。
- マウス移動 100 回に対しリングの `Pointer` が畳まれて数件になる。

## PM への連絡

- `build/programs.mk` に gshell のエントリ (shell と同じ 0x300000 リンク、`DEFINE_RUST_PROGRAM`
  の常駐版が要る)、`userland/deploy.yaml` に `/bin/gshell.bin`。
- 契約に無い op が要ると分かったら**実装せず** PM へ (末尾追記で入れる)。
