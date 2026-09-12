# T8 — full-screen GFX 復帰: 端末から起動した GFX プログラムが全画面を使い、終了で GUI に戻る (設計草案)

状態: **設計草案 (2026-09-12、PM) — 独立レビュー待ち。実装は発注していない。**
親: [PLAN.md](PLAN.md) §1 (決裁 B: … → K7 → T7 → **full-screen GFX 復帰** → shell script → 設定 S0〜)。
前提: K5b (協調型 4 本)、K6C / K7 (端末、con_sink、kbd 待ちの park)、T7 (端末からの起動)。すべて main `3b7677b`。

## 0. いま起きること (確認済み)

- GFX プログラム (`gfx200_test` / `blit_test` / `tile_bench` / `gfx_demo200` など) は `libos32gfx_init(api)` → KAPI `gfx_init` /
  `gfx_init_200` で **画面を丸ごと取る** (`gfx/gfx_core.c:341`: バックエンド再選択、両ページの VRAM ゼロクリア、パレット初期化)。
  gshell 配下の GUI アプリは `libos32gfx_attach` で取り付くだけで `gfx_init` を呼ばない (gotcha §4-20) — つまり
  **「CPL=3 の非シェルが `gfx_init` を呼ぶ」= 全画面を取りに来た**、という区別が既にある。
- `exec_start` は「最初の park」まで戻らない (`exec/exec.c:1548`)。K7 以前は GFX プログラムが `kbd_getchar` で `hlt` に入り
  **走り切るまで戻らなかった**ので、画面はプログラムのもので、抜けたら gshell が `rc <= 0` の枝 (`userland/gshell/src/lib.rs:281`)
  で `gfx_init` + `invalidate_all_clients` + `composite_full` を行い GUI が戻った (v1.2 の LAUNCH 経路、監査 (e))。
- **K7 の後**: `kbd_getchar` で park する (`WAIT_KEY`) ので `exec_start` は `rc > 0` で戻り、gshell は普通に合成・present を続ける →
  **GFX プログラムの画面を WM が上書きする**。逆にプログラムが resume して描くと WM の画面を壊す。どちらが正か決まっていない。
  さらに `rc > 0` の枝は復帰処理をしない (W-1 の理由でそれが正しい) ので、プログラムが抜けても GUI は戻らない。
- 端末 (T7) から起動した場合、打鍵は端末 → `kbd_inject` → resume で届く (画面がどちらのものでも入力経路は同じ)。

## 1. 設計 (PM 案、レビュー対象)

| # | 決定 | 根拠 |
|---|---|---|
| D1 | **画面の所有者をカーネルが 1 つ持つ** (`g_gfx_owner`: 0 = 誰も / 1 = シェル帯 (WM) / 2〜5 = アプリ)。CPL=3 の非シェル ID が `gfx_init` / `gfx_init_200` を呼んだら所有者をその ID にする (呼び手は `res_owner_get()` で分かる)。その ID の回収 (`exec_reclaim_owned`: 正常終了 / kill / fault / CTRL+STOP) で所有者を WM (1) に戻す | 「gfx_init を呼んだ」が全画面の合図であることは既存の区別 (§4-20) と一致。WM が exec の状態から推定するより 1 箇所で確実 |
| D2 | **所有者がアプリの間、WM からの present を捨てる** (`gfx_present` / `gfx_present_dirty` / `gfx_present_rect` の KAPI ラッパで `res_owner_get() == 1 && g_gfx_owner > 1` なら何もしない、`gfx_counters` に捨てた回数を積む)。WM の描画関数 (バックバッファへの描き込み) は止めない — バックバッファは WM のものなので壊れない | WM の規律だけに頼らず、カーネルで「画面はいまアプリのもの」を守る。捨てた回数はデバッグの手がかり |
| D3 | **KAPI v48**: `gfx_screen_owner(void) -> i32` (誰でも呼べる、値は D1)。WM は OP_WAIT の周期 (`exec_start` / `exec_resume` から戻った直後) にこれを見て**全画面モード**に入る / 抜ける | 状態の問い合わせ 1 本で済む。con_sink の EXIT は端末しか読めないので使わない |
| D4 | **gshell の全画面モード**: 所有者 ≠ WM の間は (a) 合成・present をしない (D2 の二重防御)、(b) 入力はいつもどおりフォーカス窓 (= 端末) へ配る (端末が `kbd_inject` する、K7 の経路)、(c) マウスはタスクバー / Start を含めて**無視**する (画面に何も無い)、(d) **CTRL+STOP はフォーカス窓ではなく画面の所有者宛**にする (`exec_abort_clear` → `exec_kill(owner)`、K5c の分岐に 1 条件足す)。所有者が WM に戻ったら **`gfx_init` → パレット復元 → `install_system_palette` → `lease::reapply` → `invalidate_all_clients` → `composite_full`** (いまの `rc <= 0` の枝と同じ手順を関数にして、`rc <= 0` のときと「全画面モードから抜けたとき」の両方から呼ぶ) | 抜けたときの手順は v1.2 から実績がある (W-1 で入口だけ狭めた)。全画面中に窓を動かしても意味が無いので入力は端末だけで足りる |
| D5 | **モード (200 ライン / PEGC 480 ライン / Cirrus) の復元**: 復帰の `gfx_init` は WM が最初に選んだバックエンドで立ち上げ直す (`gfx_select_and_init_backend` はそのたびに選ぶ)。プログラムが `gfx_init_200` にしていても WM の `gfx_init` で 400 ラインに戻る。Cirrus のリレーは §4-21 の「窓を畳まず再 init」の作法どおり | 既存の `gfx_init` の性質に乗る。新しい復元経路は作らない |
| D6 | **プログラム側は無変更**: `gfx_init` を呼ぶ既存の GFX プログラムがそのまま全画面で動く。`libos32gfx_attach` を使う GUI アプリは所有者を取らない | ユーザーランドの互換を守る |
| D7 | 端末 (T7-A) は無変更。全画面中も接続モードのまま打鍵を注入し、EXIT でプロンプトへ戻る (画面は D4 が戻す) | — |

メモリ: 所有者 1 語 + カウンタ 1 語。`MEMORY_BUDGET.md` に計上。KAPI v48 は `gfx_screen_owner` の 1 本 (KAPI_SPEC §3-2 に予約)。

## 2. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| F1 | 200 ライン | 端末に `gfx200_test` + Enter → 画面全体がテストパターン (WM が上書きしない、スクリーンショット)。端末経由でキーを打つ → プログラムが `gfx_shutdown` / `gfx_init` を通って終了 → **デスクトップ・端末窓・他の窓が全面再描画され、パレットが戻る** (スクリーンショット)。`gfx_screen_owner` は 3 → 1 |
| F2 | 400 ライン | `blit_test` または `tile_bench` で同じ。捨てた present の回数 (`gfx_counters`) が全画面中だけ増える |
| F3 | CTRL+STOP | 全画面中に CTRL+STOP → プログラムだけ畳まれ (`appslot_reclaim_count` +1、`fault_kill_count` 不変)、GUI が戻る |
| F4 | 共存 | 端末のほかに `gui_demo` を出した状態で F1 → 復帰後に `gui_demo` の窓も描き直されている (W-1 の露出と同じ観点) |
| F5 | 回帰 | regress 6 本、v86 -t、CUI から直接 `gfx200_test` (所有者の仕組みは GUI 中だけ効く)、Start → CUI mode |

## 3. レビューで見てほしい点

1. D1「`gfx_init` を呼んだら所有者」で足りるか — `gfx_init` を呼ばずに VRAM (0xA8000〜) へ直接書く CPL=3 プログラムは所有者にならず WM に上書きされる (それは「行儀の悪いプログラム」として許容するか、VRAM 写像の時点で所有者にするか)。
2. D2 で WM の present をカーネルが捨てることの是非 (WM の規律 D4(a) だけで十分か。二重防御が「WM が黙って描けない」原因調査を難しくしないか — カウンタで補う案)。
3. D4(d) CTRL+STOP の宛先を所有者に切り替える点 (K5c の「フォーカス窓宛」の例外になる)。
4. D5 の「復帰は WM の `gfx_init` 任せ」で Cirrus / PEGC の 480 ライン構成が本当に戻るか (§4-21 の再 init の罠)。

## 4. 範囲外

- shell script、設定 S0〜。全画面プログラムと GUI アプリの同時描画 (排他が仕様)。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。
