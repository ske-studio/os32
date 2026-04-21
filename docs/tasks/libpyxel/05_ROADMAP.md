# 05: 実装ロードマップ

## フェーズ概要

```text
Phase 0: 性能検証ベンチマーク           ← スケーリングの実現可能性判断
Phase 1: リソース変換ツール             ← ホスト側Python
Phase 2: 描画基盤 + ゲームループ        ← libpyxel コア
Phase 3: スプライト・タイルマップ       ← blt / bltm
Phase 4: 入力・オーディオ              ← btn / play
Phase 5: デモアプリ・最適化             ← 統合テスト
```

---

## Phase 0: 性能検証ベンチマーク

**目的**: 2倍スケーリング転送が実用的なFPSを出せるか実測で判断する。

**成果物**:
- `tests/bench_scale2x.c` — マイクロベンチマークプログラム

**検証項目**:

| テスト | 内容 | 合格基準 |
|--------|------|---------|
| memcpy BB→VRAM | `rep movsw` で 96KB (512×384 の4プレーン) を転送 | < 40ms |
| LUT展開 | 256バイト→512バイトのLUT展開 (メモリ内) | 計測のみ |
| スケーリング全体 | BB 256×192 → VRAM 512×384 (LUT + VRAM書込) | < 50ms |
| gfx_present_rect | 既存APIで 512×384 領域を転送 | < 40ms |
| gfx_fill_rect 2×2 | 2×2ブロックの連続描画 (256×192回) | 計測のみ |

**判断基準**:
- 全画面スケーリング転送が 50ms 以下 → 方式採用 (20fps確保)
- 50ms 超過 → ダーティ領域方式に切り替え or 解像度縮小を検討

---

## Phase 1: リソース変換ツール

**目的**: `.pyxres` をOS32で直接ロード可能な `.os32res` に変換する。

**成果物**:
- `tools/pyxres2os32.py` — 変換スクリプト (Python 3)

**機能**:
1. ZIP解凍 → TOML解析
2. イメージバンクのパックトピクセル → プレーナー変換
3. タイルマップのバイナリ化
4. サウンド/ミュージックの可変長バイナリ化
5. `.os32res` フォーマットでの出力
6. Pyxel 2.x 公式パレット (16色) の検証

**テスト**:
- Pyxelのサンプル `sample.pyxres` を変換し、バイナリダンプで検証
- 変換後ファイルサイズの確認

**依存**: なし (ホスト側のみ)

---

## Phase 2: 描画基盤とゲームループ

**目的**: `pyxel_init` / `pyxel_run` / `pyxel_cls` / `pyxel_pset` の最小セットを実装。

**成果物**:
- `programs/libpyxel/pyxel.h`
- `programs/libpyxel/pyxel_core.c`
- `programs/libpyxel/pyxel_gfx.c`

**実装内容**:
1. `pyxel_init(256, 192)`: GFX初期化 + Pyxelカスタム16色パレット設定 (初期値はPyxelデフォルト、アプリから変更可能)
2. `pyxel_run(update, draw)`: メインループ (VSYNC同期)
3. `pyxel_cls(col)`: ゲーム領域クリア
4. `pyxel_pset(x,y,col)`: 点描画 (libos32gfx のスケーリング描画APIをラップ)
5. `pyxel_line` / `pyxel_rect` / `pyxel_rectb`: 描画プリミティブ (libos32gfxのスケーリングAPIをラップ)
   - `pyxel_rect` = **塗りつぶし矩形**、`pyxel_rectb` = **枠のみ矩形**
6. `pyxel_circ` / `pyxel_circb` / `pyxel_tri` / `pyxel_trib`: 図形プリミティブ
7. `pyxel_camera(x,y)` / `pyxel_clip(x,y,w,h)`: 描画制御
8. `pyxel_pal(c1,c2)` / `pyxel_pal_reset()`: パレットスワップ
9. `_pyxel_present()`: スケーリングVRAM転送

**テスト**:
- 画面に色付きの点・線・矩形・円を描画するミニデモ
- FPSカウンタ表示

**依存**: Phase 0 の結果による方式決定

---

## Phase 3: スプライトとタイルマップ

**目的**: `pyxel_blt` / `pyxel_bltm` を実装し、リソースデータからの描画を可能にする。

> **方針**: libos32gfx の `GFX_Surface` / `GFX_Sprite` システムを利用する。
> `.os32res` のイメージバンクを `GFX_Surface` として保持し、
> `pyxel_blt` は libos32gfx のスプライトブリットに座標変換を加えて実行する。
> 独自のスプライト管理は行わない。

**成果物**:
- `programs/libpyxel/pyxel_res.c` (リソースローダー)
- `pyxel_gfx.c` に `pyxel_blt` / `pyxel_bltm` / `pyxel_fill` 追加

**実装内容**:
1. `pyxel_load(path)`: `.os32res` ファイルを読み込み、`GFX_Surface` としてメモリ展開
2. `pyxel_blt(x,y,img,u,v,w,h,colkey)`:
   - イメージバンク (`GFX_Surface`) の矩形領域からスプライトを生成し描画
   - `colkey >= 0` の場合、`gfx_create_sprite` の透過色として使用
   - libos32gfx のスケーリング描画APIを利用
3. `pyxel_bltm(x,y,tm,u,v,w,h,colkey)`:
   - タイルマップの指定領域を描画
   - 各タイル(8×8)をイメージバンクから切り出して配置
4. `pyxel_fill(x,y,col)`: フラッドフィル

**テスト**:
- `sample.pyxres` から変換したデータでスプライト/タイル表示
- 透過色が正しく機能するか

**依存**: Phase 1 (変換ツール) + Phase 2 (描画基盤) + libos32gfx スプライトシステム

---

## Phase 4: 入力とオーディオ

**目的**: ゲームとして操作と音声が動作する状態にする。

**成果物**:
- `programs/libpyxel/pyxel_input.c`
- `programs/libpyxel/pyxel_snd.c`

**実装内容**:
1. `pyxel_btn(key)` / `pyxel_btnp(key, hold, repeat)`:
   - 毎フレーム `sys_kbd_get_state()` でキー状態をサンプリング
   - 前フレームとの差分でトリガー/リリース検出
   - hold/repeat のフレームカウント管理
2. `pyxel_play(ch, snd)`:
   - サウンドデータのノート配列をシーケンシャルに再生
   - FM (ch 0-2) / SSG (ch 3) への振り分け
   - **1 tick = 1/120秒** のタイミングでノート進行
   - OS32の100Hzタイマー (`tick_count`) から補間
   - エフェクト: Phase 1 では None(0) と FadeOut(3) のみ対応
     (4=Half-FadeOut, 5=Quarter-FadeOut は近似実装で対応可)
3. `pyxel_playm(msc)`:
   - ミュージックデータの4チャンネルシーケンスを同時再生
4. `pyxel_stop(ch)`:
   - 指定チャンネルの再生停止

**テスト**:
- キー入力でスプライトを移動
- サウンドの単音再生テスト

**依存**: Phase 2 + Phase 3

---

## Phase 5: デモアプリと最適化

**目的**: 実際のゲームに近いデモを作り、パフォーマンスを最適化する。

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

| Phase | 推定工数 | 備考 |
|-------|---------|------|
| Phase 0 | 0.5日 | ベンチマークプログラム作成 + 計測 |
| Phase 1 | 1日 | Python スクリプト |
| Phase 2 | 2-3日 | コア描画 + ゲームループ + 追加プリミティブ (tri/fill/camera/clip) |
| Phase 3 | 2日 | blt/bltm は実装難度やや高 |
| Phase 4 | 1.5日 | 入力は容易、サウンドシーケンサがやや重い |
| Phase 5 | 1-2日 | デモ + 最適化 |
| **合計** | **約8-10日** | |

---

## 関連ドキュメント

- [00_INDEX.md](00_INDEX.md) — ドキュメント索引
- [01_PYXEL_FORMAT.md](01_PYXEL_FORMAT.md) — `.pyxres` フォーマット解析
- [02_VRAM_PLANAR.md](02_VRAM_PLANAR.md) — VRAMプレーン方式と変換設計
- [03_SCALING.md](03_SCALING.md) — スケーリング性能検証と設計
- [04_API_MAPPING.md](04_API_MAPPING.md) — API マッピング仕様
- [06_IMPLEMENTATION_DETAILS.md](06_IMPLEMENTATION_DETAILS.md) — libpyxel APIリファレンスとモジュール構成
