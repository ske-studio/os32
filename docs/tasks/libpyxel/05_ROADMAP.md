# 05: 実装ロードマップ

## フェーズ概要

```text
Phase 0: 性能検証ベンチマーク           ✅ 完了
Phase 1: リソース変換ツール             ✅ 完了
Phase 2: 描画基盤 + ゲームループ        ✅ 完了
Phase 3: スプライト・タイルマップ       ⬚ 未着手 (スタブのみ)
Phase 4: 入力・オーディオ              🔶 入力完了 / オーディオ未着手
Phase 5: デモアプリ・最適化             🔶 テストスイートあり / デモ未着手
```

---

## Phase 0: 性能検証ベンチマーク — ✅ 完了

**目的**: 2倍スケーリング転送が実用的なFPSを出せるか実測で判断する。

**成果物**:
- `tests/bench_scale2x/main.c` — マイクロベンチマークプログラム ✅
- [07_BENCH_RESULTS.md](07_BENCH_RESULTS.md) — 計測結果レポート ✅

**結果サマリ** (NP21/W, 15.9MHz):

| 方式 | 実測FPS | 判定 |
|------|--------:|------|
| 全画面cls + 再描画 | 9 fps | ❌ 非推奨 |
| fill_rect消去 + dirty | 18 fps | ⚠️ 改善余地 |
| **save/restore + dirty** | **25 fps** | **✅ 採用** |
| small dirty (32×32) | 222 fps | ✅ 理想的 |

**決定事項**:
- スプライトオーバーレイ方式 (save/restore + dirty rect) を推奨描画方式として採用
- スケーリングバッファ方式は不採用 (83ms/frame = 12fps上限)
- 2倍座標直接描画方式を採用 (`_px`/`_py` マクロで変換)

---

## Phase 1: リソース変換ツール — ✅ 完了

**目的**: `.pyxres` をOS32で直接ロード可能な `.os32res` に変換する。

**成果物**:
- `tools/pyxres2os32.py` — 変換スクリプト (Python 3) ✅
- `tools/test_pyxres2os32.py` — 自動テストスクリプト ✅

**実装済み機能**:
1. ✅ ZIP解凍 → TOML解析 (tomllib / tomli)
2. ✅ 末尾値圧縮の展開 (行方向・列方向)
3. ✅ イメージバンクのパックトピクセル → プレーナー変換
4. ✅ タイルマップの (tx, ty) ペアバイナリ化
5. ✅ サウンド/ミュージックの可変長バイナリ化
6. ✅ 空セクションのスキップ (実エントリのみ出力)
7. ✅ `.os32res` フォーマットでの出力 (PX32 ヘッダ)
8. ✅ 16色範囲外インデックスの検出・警告

**テスト**:
- ✅ ミニマル `.pyxres` 生成 → 変換 → バイナリアサーション (全PASS)
- ✅ プレーナー変換結果の手計算との一致確認

**依存**: なし (ホスト側のみ)

---

## Phase 2: 描画基盤とゲームループ — ✅ 完了

**目的**: `pyxel_init` / `pyxel_run` / `pyxel_cls` / `pyxel_pset` の最小セットを実装。

**成果物**:
- `programs/libpyxel/pyxel.h` ✅
- `programs/libpyxel/pyxel_internal.h` ✅
- `programs/libpyxel/pyxel_core.c` ✅
- `programs/libpyxel/pyxel_gfx.c` ✅

**実装済み機能**:
1. ✅ `pyxel_init(256, 192)`: GFX初期化 + Pyxel 16色パレット設定
2. ✅ `pyxel_run(update, draw)`: メインループ (100Hz VSYNC同期)
3. ✅ `pyxel_quit()`: GFXシャットダウン + TVRAM復帰
4. ✅ `pyxel_cls(col)`: ゲーム領域クリア (`gfx_fill_rect`)
5. ✅ `pyxel_pset(x,y,col)` / `pyxel_pget(x,y)`: ピクセル操作 (`gfx_fill_rect` / `gfx_get_pixel`)
6. ✅ `pyxel_line` / `pyxel_rect` / `pyxel_rectb`: 描画プリミティブ
7. ✅ `pyxel_circ` / `pyxel_circb`: 円描画
8. ✅ `pyxel_tri` / `pyxel_trib`: 三角形描画 (`gfx_fill_tri` / `gfx_line` × 3)
9. ✅ `pyxel_text`: テキスト描画 (`kcg_draw_utf8`)
10. ✅ `pyxel_camera(x,y)` / `pyxel_clip(x,y,w,h)` / `pyxel_clip_reset()`: 描画制御
11. ✅ `pyxel_pal(c1,c2)` / `pyxel_pal_reset()`: パレットスワップ
12. ✅ `_pyxel_present()`: dirty rect VRAM転送 (`gfx_present_dirty`)
13. ✅ `pyxel_frame_count` / `pyxel_fps`: フレーム統計

**アーキテクチャ決定事項**:
- **全描画関数はlibos32gfxに完全委譲** — `pyxel_gfx.c` に独自描画ロジックなし
- 2倍座標変換は `_px`/`_py` マクロ (libos32gfx側にスケーリング概念なし)
- dirty rect はlibos32gfxの各プリミティブが内部で自動登録

**テスト**:
- ✅ `tests/pyxel_test.c` — 7ページの包括テストスイート (プリミティブ/入力/パレット/カメラ/ベンチ等)

**依存**: Phase 0 の結果による方式決定 → 完了

---

## Phase 3: スプライトとタイルマップ — ⬚ 未着手

**目的**: `pyxel_blt` / `pyxel_bltm` を実装し、リソースデータからの描画を可能にする。

> **方針**: libos32gfx の `GFX_Surface` / `GFX_Sprite` システムを利用する。
> `.os32res` のイメージバンクを `GFX_Surface` として保持し、
> `pyxel_blt` は libos32gfx のスプライトブリットに座標変換を加えて実行する。
> 独自のスプライト管理は行わない。

**現状**: `pyxel_blt` / `pyxel_bltm` / `pyxel_fill` / `pyxel_load` はスタブ (void戻り)。

**成果物**:
- `programs/libpyxel/pyxel_res.c` (リソースローダー)
- `pyxel_gfx.c` に `pyxel_blt` / `pyxel_bltm` / `pyxel_fill` 実装

**実装内容**:
1. `pyxel_load(path)`: `.os32res` ファイルを読み込み、`GFX_Surface` としてメモリ展開
2. `pyxel_blt(x,y,img,u,v,w,h,colkey)`:
   - イメージバンク (`GFX_Surface`) の矩形領域からスプライトを生成し描画
   - `colkey >= 0` の場合、`gfx_create_sprite` の透過色として使用
   - 2倍座標変換を適用
3. `pyxel_bltm(x,y,tm,u,v,w,h,colkey)`:
   - タイルマップの指定領域を描画
   - 各タイル(8×8)をイメージバンクから切り出して配置
4. `pyxel_fill(x,y,col)`: フラッドフィル

**テスト**:
- `sample.pyxres` から変換したデータでスプライト/タイル表示
- 透過色が正しく機能するか

**依存**: Phase 1 (変換ツール) + Phase 2 (描画基盤) + libos32gfx スプライトシステム

---

## Phase 4: 入力とオーディオ — 🔶 入力完了 / オーディオ未着手

**目的**: ゲームとして操作と音声が動作する状態にする。

### 入力 — ✅ 完了

**成果物**:
- `programs/libpyxel/pyxel_input.c` ✅

**実装済み**:
1. ✅ `pyxel_btn(key)`: オンデマンドポーリング方式 (KAPI `kbd_is_pressed`)
2. ✅ `pyxel_btnp(key, hold, repeat)`: 立上りエッジ検出 + ホールドリピート
3. ✅ `pyxel_btnr(key)`: リリース (立下り) 検出
4. ✅ フレーム境界でのキー状態差分管理 (`_pyxel_update_input`)

> **設計決定**: 128キー全ポーリング方式は KAPI呼出128回/フレームのオーバーヘッドが
> 大きすぎるため廃止。現在はオンデマンド方式 (呼ばれたキーのみ取得) を採用。

### オーディオ — ⬚ 未着手

**成果物**:
- `programs/libpyxel/pyxel_snd.c` — 未作成

**現状**: `pyxel_play` / `pyxel_playm` / `pyxel_stop` はスタブ (pyxel_gfx.c 内)。

**実装内容**:
1. `pyxel_play(ch, snd)`:
   - サウンドデータのノート配列をシーケンシャルに再生
   - FM (ch 0-2) / SSG (ch 3) への振り分け
   - **1 tick = 1/120秒** のタイミングでノート進行
   - OS32の100Hzタイマー (`tick_count`) から補間
   - エフェクト: None(0) と FadeOut(3) のみ対応
2. `pyxel_playm(msc)`:
   - ミュージックデータの4チャンネルシーケンスを同時再生
3. `pyxel_stop(ch)`:
   - 指定チャンネルの再生停止

**依存**: Phase 3 (リソースデータからサウンド定義を取得)

---

## Phase 5: デモアプリと最適化 — 🔶 テストスイートあり / デモ未着手

**目的**: 実際のゲームに近いデモを作り、パフォーマンスを最適化する。

### 現状

- ✅ `tests/pyxel_test.c` — 7ページの包括テストスイート (プリミティブ/入力/パレット/カメラ/ベンチマーク/ネイティブ/スプライト)
- ⬚ `programs/apps/pyxeldemo/` — Pyxel公式サンプル移植は未着手

**成果物**:
- `programs/apps/pyxeldemo/` — Pyxelのサンプルゲーム移植

**実装内容**:
1. Pyxel公式サンプル (例: スプライトの移動、タイルスクロール) のC言語移植
2. プロファイリングとボトルネック特定
3. 必要に応じて:
   - アセンブラ最適化 (スケーリング転送のインナーループ)
   - ダーティ領域最適化の導入
   - メモリ使用量の削減

**依存**: Phase 1〜4 すべて

---

## 工数見積もり (参考)

| Phase | 推定工数 | 状態 | 備考 |
|-------|---------|------|------|
| Phase 0 | 0.5日 | ✅ 完了 | ベンチマーク実施・方式決定 |
| Phase 1 | 1日 | ✅ 完了 | Python スクリプト + テスト |
| Phase 2 | 2-3日 | ✅ 完了 | 全プリミティブ + ゲームループ + テストスイート |
| Phase 3 | 2日 | ⬚ 未着手 | blt/bltm は Phase 1 に依存 |
| Phase 4 | 1.5日 | 🔶 半完 | 入力完了、サウンドシーケンサ未着手 |
| Phase 5 | 1-2日 | 🔶 一部 | テストスイートあり、デモ未着手 |
| **残り** | **約4.5-5.5日** | | Phase 1 + 3 + 4(audio) + 5(demo) |

---

## 関連ドキュメント

- [00_INDEX.md](00_INDEX.md) — ドキュメント索引
- [01_PYXEL_FORMAT.md](01_PYXEL_FORMAT.md) — `.pyxres` フォーマット解析
- [02_VRAM_PLANAR.md](02_VRAM_PLANAR.md) — VRAMプレーン方式と変換設計
- [03_SCALING.md](03_SCALING.md) — スケーリング性能検証と設計
- [04_API_MAPPING.md](04_API_MAPPING.md) — API マッピング仕様
- [06_IMPLEMENTATION_DETAILS.md](06_IMPLEMENTATION_DETAILS.md) — libpyxel APIリファレンスとモジュール構成
- [07_BENCH_RESULTS.md](07_BENCH_RESULTS.md) — Phase 0 ベンチマーク結果
