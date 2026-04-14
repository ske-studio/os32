# VBZ ベクター画像パイプライン改善

## 概要
画像→VBZ変換ツール (`img2vbz.py`) とPC-98上のベクタービューア (`vbzview.bin`) の
機能改善・品質向上・最適化タスク。

## 関連ファイル

| ファイル | 役割 |
|----------|------|
| `tools/img2vbz.py` | 画像/SVG → VBZ変換ツール (Python) |
| `programs/vbzview.c` | VBZビューア (OS32外部プログラム) |
| `programs/libos32gfx/gfx_bezier.c` | ベジェ曲線ライブラリ |
| `programs/libos32gfx/gfx_draw.c` | 描画プリミティブ (hline等) |
| `programs/libos32gfx/gfx_util.asm` | ASM最適化描画ルーチン |

---

## Phase 1: 減色アルゴリズム改善 → ✅ 完了

### 問題
Pillow `MEDIANCUT` は面積の広い色 (空の青等) にパレットが偏り、
キャラクターの固有色 (オレンジ・黄・肌色) が消失していた。

### 実施内容
- [x] **色相バランス量子化** (`quantize_huebalance`) 実装
  - HSV変換 → 12色相セクター分類
  - 有効色相にスロット最低1保証 + 残りを面積比で追加配分
  - 各グループ内 K-means (numpy自前実装、scikit-learn不要)
  - 最近傍マッピングでインデックス画像生成
- [x] **彩度ブースト前処理** (`saturation_boost`, デフォルト1.3倍)
- [x] **CLIオプション追加**: `--quantize mediancut|huebalance`, `--saturation-boost`
- [x] セル画風画像: パス数 4300→3175 (-26%), 色相カバレッジ 2→6 グループ
- [x] メカドラゴン: パス数 7055→6441 (-9%), 色相カバレッジ 2→9 グループ

---

## Phase 2: ズーム/パン機能 → ✅ 完了

### 実施内容
- [x] ビューポート変換 (`VX`/`VY` マクロ) による座標変換
- [x] ファイルデータのメモリ保持 → 再レンダリング対応
- [x] インタラクティブ操作: Z/X=ズーム, 矢印=パン, 1=リセット, ESC=終了
- [x] 最大16x (zoom_level=4) まで拡大可能

---

## Phase 3: 32bit化 → ✅ 完了

### 問題
Edge構造体・交差点バッファが `i16` のままで、ズーム時にオーバーフローの危険があった。

### 実施内容
- [x] `Edge` 構造体: `i16 x0,y0,x1,y1` → `int`
- [x] `g_intersect[]`: `i16[]` → `int[]`
- [x] `edge_add()`: `(i16)` キャスト除去
- [x] `sort_intersections()`: `i16*` → `int*`
- [x] `scanline_fill()`: `(i16)ix` キャスト除去

---

## Phase 4: scanline_fill 最適化 → ✅ 完了

### 問題
`gfx_scanline_fill()` のホットパスで、毎走査線 `gfx_hline()` 経由で
`gfx_add_dirty_rect()` KAPI間接呼び出しが約14万回発生し、オーバーヘッドが支配的だった。

### 実施内容
- [x] **dirty rect 一括登録化**: バウンディングボックスで `gfx_add_dirty_rect()` を1回だけ事前登録
  - KAPI間接呼び出し: 14万回 → 最大7000回
- [x] **`asm_gfx_hline()` 直接呼び出し**: `gfx_hline()` のC関数ラッパーをバイパス
  - Y境界チェック、dirty rect登録、関数呼び出しオーバーヘッドを削除
  - `y * pitch` の乗算もループ外で事前計算し加算で更新
- [x] **交差点ソート省略**: n==2 の場合 (99%のケース) はスワップのみ
- [x] **プロファイリング拡張**: Edge/Fill/Present/Other の4分割出力

---

## Phase 5: libos32gfx 分離 → ✅ 完了 (Phase 5-D で達成)

### 概要
vbzview.c にインライン化されていたスキャンラインフィルエンジンを
`programs/libos32gfx/geom/gfx_fill.c` に移動済み。

### 完了済み API
- [x] `GFX_EdgeTable` 構造体 + `gfx_edge_table_create()` / `gfx_edge_table_free()`
- [x] `gfx_edges_clear()` / `gfx_edge_add()`
- [x] `gfx_bezier3_to_edges()` (de Casteljau 平坦化)
- [x] `gfx_scanline_fill()` (DDA + AET, even-odd rule)
- [x] `libos32gfx.h` にAPI宣言追加済み
- [x] `vbzview.c` はライブラリAPIのみ使用

---

## Phase 6: 変換ツール追加改善

### 完了済み
- [x] **Floyd-Steinbergディザリング** (`--dither`)
- [x] **`--crop` オプション**: 入力画像の領域切り出し
- [x] **`-t` (turdsize) オプション**: ノイズ除去

### 未着手タスク
- [ ] 無彩色グループへの過剰配分防止 (犬画像で無彩色11スロット問題)
  - 各グループの上限キャップ (例: num_colors/2)
  - 無彩色の明度ヒストグラムに基づく適正スロット数算出
- [ ] パス簡略化の改善: `--epsilon` と Potrace `-t` パラメータの自動調整
