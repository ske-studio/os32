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

## 5. 範囲外

- shell script、設定 S0〜。全画面プログラムと GUI アプリの同時描画 (排他が仕様)。VDM の端末化。
- 配備・コミット・push・エミュレータ・ローカル AI・ini・.env・`make` は禁止 (コーダー)。
