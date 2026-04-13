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

## Phase 4: gfx_draw.c 調査・最適化 → 未着手

### 目的
スキャンラインフィルの最大ボトルネックは `gfx_hline()` 呼び出し。
現在のC実装を調査し、ASM最適化の価値を判断する。

### タスク
- [ ] `gfx_draw.c` の `gfx_hline()` 実装を精査
  - 4プレーン×バイトマスク処理のループ構造
  - 先頭/末尾バイトのビットマスク処理
  - 中間バイトの一括書き込み方式
- [ ] プロファイリング: 7000パス画像の描画時間計測
- [ ] ASM化 `asm_gfx_hline()` の実装判断
  - `rep stosd` による中間バイト高速fill
  - 先頭/末尾ビットマスクのレジスタ操作
  - `gfx_util.asm` に追加

### 期待効果
- 描画速度2〜4倍改善の可能性 (7000パス画像の再レンダリング高速化)
- ズーム/パン操作のレスポンス向上

---

## Phase 5: libos32gfx 分離 → 未着手

### 目的
vbzview.c にインライン化されている汎用グラフィクス処理を
libos32gfx に移動し、他プログラムからも再利用可能にする。

### 前提
Phase 4 (gfx_draw.c 調査) 完了後に実施。hline の最適化方針が
スキャンラインフィルのAPIデザインに影響するため。

### タスク
- [ ] `gfx_fill.c` (新規) にスキャンラインフィルを移動
  - Edge構造体
  - `gfx_edges_init()` / `gfx_edges_clear()` / `gfx_edge_add()`
  - `gfx_scanline_fill()`
  - `gfx_sort_intersections()` (内部関数)
- [ ] `gfx_bezier.c` にベジェ平坦化→エッジ変換を追加
  - コールバック方式: `gfx_line` か `edge_add` かを切替可能に
  - 既存の `gfx_bezier3()` (描画) と `flatten_bezier3()` (エッジ) を統合
- [ ] `libos32gfx.h` にAPI宣言追加
- [ ] `vbzview.c` をライブラリ呼び出しにリファクタリング
- [ ] Makefile 更新

---

## Phase 6: 変換ツール追加改善 → 未着手

### タスク候補
- [ ] 無彩色グループへの過剰配分防止 (犬画像で無彩色11スロット問題)
  - 各グループの上限キャップ (例: num_colors/2)
  - 無彩色の明度ヒストグラムに基づく適正スロット数算出
- [ ] `--dither` オプション: 誤差拡散ディザリングで主観的色数を増加
- [ ] `--crop` オプション: 入力画像の領域切り出し (部分変換)
- [ ] パス簡略化の改善: `--epsilon` と Potrace `-t` パラメータの自動調整
