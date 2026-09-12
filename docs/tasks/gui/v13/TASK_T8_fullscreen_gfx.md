# T8 — full-screen GFX 復帰: 端末から起動した GFX プログラムが全画面を使い、終了で GUI に戻る

状態: **決裁済み (2026-09-12、ユーザー): D1a 十分 / D2 落とす / D3 OK / D4 採用 / D5〜D7 OK。K / ビルド系 / W の 3 票を発注。**
親: [PLAN.md](PLAN.md) §1 (決裁 B: … → K7 → T7 → **full-screen GFX 復帰** → shell script → 設定 S0〜)。
前提: K5b (協調型 4 本)、K6C / K7 (端末、con_sink、kbd 待ちの park)、T7 (端末からの起動)。すべて main `3b7677b`。

## 0. いま起きること (確認済み)

- GFX プログラム (`gfx200_test` / `blit_test` / `tile_bench` / `gfx_demo200` など) は `libos32gfx_init(api)` → KAPI `gfx_init` /
  `gfx_init_200` で **画面を丸ごと取る** (`gfx/gfx_core.c:341`: バックエンド再選択、両ページの VRAM ゼロクリア、パレット初期化)。
  gshell 配下の GUI アプリは `libos32gfx_attach` で取り付くだけで `gfx_init` を呼ばない (gotcha §4-20)。
- `exec_start` は「最初の park」まで戻らない (`exec/exec.c:1548`)。K7 以前は GFX プログラムが `kbd_getchar` で `hlt` に入り
  走り切るまで戻らなかったので、抜けた後に gshell の `rc <= 0` の枝 (`userland/gshell/src/lib.rs:281`) が `gfx_init` +
  `invalidate_all_clients` + `composite_full` で GUI を戻した。
- **K7 の後**: `kbd_getchar` で park する (`WAIT_KEY`) ので `exec_start` は `rc > 0` で戻り、gshell が合成・present を続けて
  **GFX プログラムの画面を上書きする**。`rc > 0` の枝は復帰処理をしない (W-1) ので、プログラムが抜けても GUI は戻らない。
- OS32X ヘッダには **`OS32X_FLAG_GFX` (0x0001、`mkos32x --gfx`) が既にある**が、いまは誰も立てず、カーネルも見ていない。
  `OS32X_FLAG_FORCE_CPL0` (0x0004、`--cpl0`) は v86 / VDM 系の CPL=0 プログラムに付いていて `appslot_launch_is_app` が見ている。

## 1. ユーザー決裁 (2026-09-12)

- **VRAM を直接触る系 (v86 / VDM = `--cpl0`) は GUI からの直接実行を禁止**し、CUI に降りてから実行させる。
  VRAM を触らない DOS 的な CUI プログラム (CPL=3、出力は console.c 経由) は制限しない。
- **ユーザーランドは全部ビルドし直してよい** → ヘッダの宣言ビットを使う。
- 実装コストが高ければ「CUI のみの機能」で構わない (→ 下記のとおり低コストなので GUI からの全画面を実装する)。

## 2. 設計 (決裁反映後)

| # | 決定 | 担当 |
|---|---|---|
| D1 | **画面の所有者をカーネルが 1 つ持つ** (`g_gfx_owner`: 1 = シェル帯 (WM) / 2〜5 = アプリ)。CPL=3 の非シェル ID が `gfx_init` / `gfx_init_200` を呼んだら所有者をその ID にし、その ID の回収 (`exec_reclaim_owned`: 正常終了 / kill / fault / CTRL+STOP) で WM に戻す。**GUI 中 (con_sink 有効中) の `exec_start` は `OS32X_FLAG_FORCE_CPL0` のプログラムを `OS32_ERR_INVAL` で拒否** (CUI 専用。K5b A1 の「CPL=3 アプリが生きていたら拒否」を「GUI からは常に拒否」に広げる)。`gfx_init` を呼ばずに VRAM へ直接書く CPL=3 プログラムは「行儀の悪いプログラム」として扱い、守らない | K |
| D1a | **宣言ビット**: `OS32X_FLAG_GFX` を「全画面 GFX を使う」の宣言として使う。`build/app.conf` に 4 列目 `gfx` (省略 = 無し) を足し、`mkos32x` が `--gfx` を付ける。全画面を使う既存プログラム (`gfx200_test` / `blit_test` / `blit_test2` / `tile_bench` / `gfx_demo200` / `demo_tile` / `rotate_test` / `mgx_test` / `hello_gfx` 系 / `apps/` `game/` の GFX 物 — `libos32gfx_init` / `gfx_init` を呼ぶもの) に立てる。`tools/check_manifests.py` で「`gfx_init` を呼ぶのに宣言が無い」を検出できるなら足す (少なくとも app.conf の列の検査)。**カーネルは GUI 中に宣言の無い CPL=3 が `gfx_init` を呼んだら `OS32_ERR_INVAL` で拒否** (黙って画面を壊さない)。CUI 中は従来どおり何でも通す | ビルド系 + K |
| D2 | **落とす (ユーザー決裁)**。WM の present はカーネルで捨てない。全画面中に描かないのは WM の規律 (D4a) だけで守る | — |
| D3 | **KAPI v48**: `gfx_screen_owner(void) -> i32` (誰でも呼べる、値は D1)。WM は `exec_start` / `exec_resume` から戻った直後にこれを見て全画面モードに入る / 抜ける | K |
| D4 | **gshell の全画面モード**: 所有者 ≠ WM の間は (a) 合成・present をしない、(b) 入力はいつもどおりフォーカス窓 (= 端末) へ配る (端末が `kbd_inject`)、(c) マウスはタスクバー / Start を含めて無視、(d) **CTRL+STOP は所有者宛** (`exec_abort_clear` → `exec_kill(owner)`、K5c の分岐に 1 条件)。所有者が WM に戻ったら **`gfx_init` → パレット復元 → `install_system_palette` → `lease::reapply` → `invalidate_all_clients` → `composite_full`** (いまの `rc <= 0` の枝を関数にして両方から呼ぶ)。**入口の追加**: Run ダイアログ / 端末からの起動前に OS32X ヘッダを読み (`sys_open` + 40B 読み)、`FORCE_CPL0` なら `cui only: <名>` を出して起動しない (カーネルの拒否は最後の砦)。`FLAG_GFX` なら起動直後から全画面モードに入る (所有者の問い合わせと二重) | W (+ 端末 A は入口だけ) |
| D5 | **復帰は WM の `gfx_init` 任せ**: 復帰の `gfx_init` は WM が最初に選んだバックエンドで立ち上げ直す。プログラムが `gfx_init_200` にしていても 400 ラインに戻る。Cirrus のリレーは §4-21 の作法どおり | — |
| D6 | **プログラム側のコード変更は無し** (宣言ビットは app.conf / 再ビルドで付く)。GUI アプリ (`libos32gfx_attach`) は所有者を取らない | ビルド系 |
| D7 | 端末 (T7-A) は入口 (D4 の `cui only`) 以外は無変更。全画面中も接続モードで打鍵を注入し、EXIT でプロンプトへ | A |

メモリ: 所有者 1 語 + カウンタ 1 語。`MEMORY_BUDGET.md` に計上。KAPI v48 は `gfx_screen_owner` の 1 本 (KAPI_SPEC §3-2 に予約済み)。

将来 (この票の範囲外、記録のみ): VDM のコンソール出力を con_sink に流せば「文字だけの DOS プログラムを端末窓で」も可能になる。
宣言ビットは Start メニューの一覧や `ls` の表示 (種別) にも使える。

## 3. 独立レビューの残点 → ユーザー決裁 (2026-09-12): 1 = D1a で十分、2 = D2 は落とす、3 = D4(d) 採用、4 = D5 のまま (F7 で実測)

1. D1a の「宣言の無い CPL=3 の `gfx_init` を GUI 中は拒否」で、既存プログラムの取りこぼし (宣言し忘れ) は `check_manifests` で網を張るが、それで十分か。
2. D2 を保険として残すか落とすか。
3. D4(d) CTRL+STOP の宛先を所有者に切り替える例外 (K5c の「フォーカス窓宛」に対する)。
4. D5 の Cirrus / PEGC 480 ライン構成の復帰 (§4-21 の再 init の罠) — 受入 F6 で実測する。

## 4. 受入 (ゲスト、PM / テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| F1 | 200 ライン | 端末に `gfx200_test` + Enter → 画面全体がテストパターン (WM が上書きしない)。端末経由でキーを打つ → プログラムが終了 → **デスクトップ・端末窓・他の窓が全面再描画され、パレットが戻る**。`gfx_screen_owner` は 3 → 1 |
| F2 | 400 ライン | `blit_test` または `tile_bench` で同じ |
| F3 | CTRL+STOP | 全画面中に CTRL+STOP → プログラムだけ畳まれ (`appslot_reclaim_count` +1、`fault_kill_count` 不変)、GUI が戻る |
| F4 | 共存 | 端末のほかに `gui_demo` を出した状態で F1 → 復帰後に `gui_demo` の窓も描き直されている |
| F5 | CUI 専用の拒否 | 端末に `v86` + Enter → `cui only: v86` で起動しない。Run ダイアログからも同じ。CUI からは従来どおり `v86 -t` が通る |
| F6 | 宣言なし | 宣言ビットを外した試験バイナリ (テスト用に 1 本、`app.conf` で `gfx` 無し) を GUI 中に起動 → `gfx_init` が `ERR_INVAL` で画面は無事。CUI 中は動く |
| F7 | 回帰 | regress 6 本、v86 -t、Start → CUI mode。可能なら PEGC 構成で F1 (§3-4) |

## 4a. 実装メモ (ビルド系、2026-09-12)

- `build/app.conf` に 4 列目 `gfx` (省略 = 無し) を追加。`build/programs.mk` の `userland/%.bin` が `--gfx` を付ける。`sdk/mkos32x.py` は無変更 (`--gfx` = 0x0001 を単体実行で確認)。
- 立てた 14 本 = `gshell` / `bench` / `bench_scale2x` / `blit_test` / `blit_test2` / `demo_tile` / `font_test` / `gdi_test` / `gfx200_test` / `gfx_demo200` / `hal_test` / `hello_gfx` / `rotate_test` / `tile_bench`。`hal_test` `gdi_test` は行が無かったので `7 0 gfx` で新設。
- 票の候補のうち `mgx_test` `asset_demo` は gfx を呼ばないので立てず、逆に票が GUI 側に挙げた `gdi_test` は `libos32gfx_init` を直接呼ぶ単独 GFX なので立てた (`programs.mk` の注記どおり)。`demo_tile` / `tile_bench` は `tilemap_init` 経由。
- `tools/check_manifests.py` に §2b を追加: 4 列目の書式と「gfx_init 系を呼ぶのに宣言が無い」を検出。除外リストは持たず、`userland/lib` の呼び出しグラフを不動点まで辿る (コメントは除去)。宣言だけあって呼ばないものは `[--]` の警告。
- `gshell` にも宣言を立てた: シェル帯の WM 自身が復帰時に `gfx_init` を呼ぶので、K が「宣言の無い CPL=3」で弾く実装にした場合に GUI 復帰が死ぬのを避ける。`apps/` `game/` は submodule 未チェックアウトのため未対応 (各リポジトリ側で `--gfx`)。
## 4a. 実装メモ (K、2026-09-12)

- 所有者の表は `exec/appslot.c` (`g_gfx_owner` / `gfx_init_reject_count`)。判定材料 (走っている ID /
  `cpl3` / OS32X `hdr_flags`) が全部 AppSlot にあり、純関数 `appslot_gfx_claim_check()` としてホストで試験できるため。
- KAPI の門は `sdk/kapi.json` の `"target"` で `gfx_init` / `gfx_init_200` → `gfx_kapi_init(_200)`
  (`gfx/gfx_core.c`)。戻り型は `void` のまま、生成物は手で触っていない ([ABI1])。**KAPI v48 = `gfx_screen_owner` (スロット 192 / offset 0x308)**。
- D1 の cpl0 拒否は `appslot_cpl0_admit(is_shell, gui)` に `gui` を足して広げた (K5b A1)。CUI の `exec_run` は無変更。
- ホスト試験は `tools/tests/multiapp_impl_host.c` ケース 20 (33 検査、RED→GREEN 5 通り) → `tools/tests/t8_tdd.md`。
  kselftest に 2 項 (`test_gfx_owner`)。**`make`・配備・実機は未実施** ([V4]) — 受入 F1〜F7 は PM / テスターへ。

## 4a. 実装メモ (W、2026-09-12)

- 全画面モード = `userland/gshell/src/fullscreen.rs` (印 / 所有者 / 入る時点のパレット)。門は `wm::composite_rect`
  `composite_full` `queue_present` `flush_present` `flush_screen_dirty`・`input::capture_mouse`・`cursor::show` `move_to`・
  `fep::post_cycle` の入口 1 条件ずつ (dirty は溜める)。
- 入口 = `os32x.rs` の純関数 `classify` (cpl0 > gfx > 無し) + `sys_open` 40B 読み。`run_program` が cpl0 を `cui only: <名>` で断り
  (戻り値 0 = `RUN_REFUSED`)、gfx は `exec_start` 前に `arm`。所有者の問い合わせは `after_exec` (`exec_start` / `exec_resume` 直後)。
- 復帰 = `lib.rs::restore_screen` (`gfx_init` → `invalidate_all_clients` → パレット → `install_system_palette` → `lease::reapply` →
  露出 → `composite_full`)。`rc <= 0` の既存経路も同じ関数。CTRL+STOP の宛先は `multiapp::abort_target` (全画面中は所有者)。
- 端末 (D7) は `prompt::classify` (同じ判定の写し。共有ライブラリは増やさない) で `FORCE_CPL0` を `cui only: <名>` にして起動しない。
- ホスト試験は gshell 4 本 + `os32x` 2 本 (48 pass、実装を外すと RED を確認)、端末 1 本 (49 pass)。`app.conf` の gshell を KAPI 48 に。
  **`make`・配備・実機は未実施** ([V4]) — 受入 F1〜F7 は PM / テスターへ。

## 5. 範囲外

- shell script、設定 S0〜。全画面プログラムと GUI アプリの同時描画 (排他が仕様)。VDM の端末化。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。

## 6. 実機受入の記録 (PM / テスター、2026-09-12、K `f19dd00` + B `d73ceea` + W `732b07d` を NHD 配備、vmkernel 460,792 B、gshell 166,568 B、API v48、kselftest 66 / 0、15MB)

| 受入 | obs | 判定 |
|---|---|---|
| **F1** 前半 | 端末に `gfx200_test` + Enter → 200 ライン (400 ライン画面に縦 2 倍) のテストパターンが全画面に出て WM は上書きしない (`g_gfx_owner` 3)。端末経由の Space で `kbd_getchar` の park から復帰し次段 (FPS 計測) へ | **合格** |
| **F1** 後半 | FPS 計測ループが `kbd_trygetchar()` をポーリングしており、GUI 中は park しない (K7 R1 どおり) ため **WM に制御が戻らず端末からキーを注入できない**。プログラムは終了せず、CTRL+STOP でしか抜けられない | **不合格 (設計の穴 → §7 判断待ち)** |
| **F3** | 全画面中 (ポーリング中) に CTRL+STOP → 所有者だけ畳まれ (`appslot_last_reclaim_id` 3、`g_gfx_owner` 1)、デスクトップ・端末窓・プロンプト (`EXIT` レコード) まで復帰。**`fault_kill_count` が +1** (走行中のアプリへの abort は `exec_exit(EXEC_ERR_FAULT)` 経由で fault 扱い。park 中の kill は増えない) | **合格** (カウンタの註付き) |
| **F5** | 端末に `v86` + Enter → **起動してしまう** (使い方を表示して終了)。`v86.bin` の flags は 0x0 = CPL=3 プログラムで、V86 へは KAPI `v86_*` で入る。`FORCE_CPL0` の判定では捕まらない | **不合格 → T8-2** (`OS32X_FLAG_CUI_ONLY` 0x0010 を app.conf `cui` で立て、入口判定 + `exec_start` 拒否 + `v86_*` KAPI の GUI 中拒否) |
| 事故 | F6 の準備で CUI から `hsync` を実行 → HostDrv の古いビルドで NHD が戻った (§4-33)。`make deploy` → `hsync` で復旧、サイズ照合済み | — |
