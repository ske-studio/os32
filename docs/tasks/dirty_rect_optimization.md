# dirty rect 描画パイプライン — 問題分析と改善計画

## 1. 背景

libpyxel Phase 2 テスト (pyxel_test TEST 5) にて、overlay方式 (cls不使用) での
FPS が期待値を大幅に下回る問題が発生。

| 方式 | 期待FPS | 実測FPS | 乖離 |
|------|--------:|--------:|------|
| cls 方式 | 9 | 2-3 | Test 6結果と乖離 |
| overlay 方式 | 18-25 | 3-5 | **Test 7 (25fps) と大きく乖離** |

bench_scale2x (Test 7) では save/restore 方式で 25fps を達成している。
libpyxel のアーキテクチャ固有の問題が存在する。

---

## 2. 描画パイプラインの全体像

```text
pyxel_test.c (テストプログラム)
  │
  ├─ pyxel_rect() / pyxel_circ() / pyxel_text()
  │     └→ pyxel_gfx.c: 座標を2倍 → gfx_fill_rect() + _dirty()
  │
  ├─ ui_text() / draw_ui_common()
  │     └→ kcg_draw_utf8() [直接BB書込] + gfx_fill_rect()
  │
  └→ _pyxel_present()
        └→ KAPI: gfx_present_dirty()
              ├→ VSYNC待ち (最大16.7ms)
              └→ _flush_dirty_queue()
                    └→ 各rect個別に 4プレーン×行数 の VRAM転送
```

---

## 3. 特定された問題点

### 問題1: dirty rect の過剰生成 (最重要)

**現象**: 1フレームあたり 20-30 個の dirty rect が生成される。

**原因**: pyxel_gfx.c の各描画関数が個別に `gfx_add_dirty_rect()` を呼ぶ。

```text
overlay フレームの dirty rect 数 (ボール8個):
  消去rect: 8個 (pyxel_rect × 8)
  描画circ: 8個 (pyxel_circ × 8)
  枠circb:  8個 (pyxel_circb × 8)
  ラベル:   2個 (pyxel_rect + pyxel_text)
  ─────────────────────
  合計:     26個   ← MAX_DIRTY_RECTS=32 に迫る
```

**影響**: MAX_DIRTY_RECTS (32) 超過時にフォールバックが発動し、
全体をバウンディングボックスに圧縮 → 全画面転送と同等になる。

**対策案**:

| 案 | 実装場所 | 概要 | 効果 |
|----|---------|------|------|
| A. マージ統合 | `gfx_add_dirty_rect()` | 追加時にオーバーラップ/隣接rectを統合 | ◎ 根本解決 |
| B. MAX拡大 | `gfx_internal.h` | MAX_DIRTY_RECTS を 64 や 128 に増加 | △ 応急処置 |
| C. フレーム単位蓄積 | `gfx_add_dirty_rect()` | 重複領域の面積増加を考慮した統合判定 | ◎ 最適 |

### 問題2: VRAM転送のループオーバーヘッド

**現象**: `_flush_dirty_queue()` が各 dirty rect を個別にループ転送する。

**原因**: `gfx_vram.c:88-101` のネストループ構造:

```c
for (i = 0; i < dirty_queue.count; i++) {    /* rect数分 */
    for (row = 0; row < r->h; row++) {        /* rect高さ分 */
        _memcpy_w(vb_base + phys_off, bb_b + base_off, words);  /* B */
        _memcpy_w(vr_base + phys_off, bb_r + base_off, words);  /* R */
        _memcpy_w(vg_base + phys_off, bb_g + base_off, words);  /* G */
        _memcpy_w(vi_base + phys_off, bb_i + base_off, words);  /* I */
    }
}
```

26個のrectで、各10-30行の高さがある場合:
- 合計ループ回数: 26 × 20 = **520回** × 4プレーン = **2,080回の _memcpy_w**

小さいrectが多数ある場合、`_memcpy_w` のセットアップ (rep movsw プロローグ)
が支配的になり、バイト効率が大幅に低下する。

**対策**: 問題1のマージで rect 数を減らすことが最も効果的。

### 問題3: VSYNC 待機の非効率

**現象**: `gfx_present_dirty()` 内の VSYNC ポーリングが毎フレーム実行される。

```c
while ((_in(0x60) & 0x20) == 0) { }   /* 最大16.7ms待ち */
```

**影響**: 転送処理自体が高速でも、VSYNC 待ちで最大 16.7ms 消費。
60fps (16.7ms/frame) が物理的上限。

**対策案**:
- ~~VSYNCスキップオプション (`gfx_present_dirty_nosync()`) を新設~~ → `gfx_present_nosync()` として実装済み
- ~~ティアリングを許容する高速パスとして使い分け~~
- **✅ 解決済み**: ページフリッピング実装により、全モード (200/400ライン) で
  VSYNC待ちなしでティアリングフリーな描画を実現。`gfx_init()` / `gfx_init_200()`
  いずれでも自動で有効化される。

### 問題4: 同一VRAM行への多重書込

**現象**: 同一 Y 座標範囲を含む複数の dirty rect があると、VRAM の同じ行に
複数回書き込みが発生する。

例: Y=100 の行に 3 つの dirty rect がある場合:
```text
rect A: (32, 80, 64, 40)   → Y=80-119 を転送
rect B: (128, 90, 96, 30)  → Y=90-119 を転送
rect C: (256, 100, 48, 20) → Y=100-119 を転送
```

Y=100-119 の範囲で 3 回 VRAM に書き込む。
VRAM は I/O マップドメモリなので、メインメモリの 10-20 倍遅い。

**対策**: マージによる rect 統合で多重書込を大幅に削減。

---

## 4. 改善計画

### Phase A: dirty rect マージ (カーネル `gfx_vram.c`)

`gfx_add_dirty_rect()` に rect マージロジックを追加する。

#### マージ判定基準

新しい rect R を追加する際、既存の rect E に対して:

1. **完全包含**: R が E に包含される → 何もしない (スキップ)
2. **オーバーラップ**: R と E が重なる → バウンディングボックスに統合
3. **隣接**: R と E が接している、またはギャップが閾値以下 → 統合
4. **独立**: 上記に該当しない → 新規追加

```text
統合判定 (擬似コード):
  gap_threshold = 32  /* 32px以内のギャップは統合 */
  for each existing rect E:
    if overlap(R, E) or adjacent(R, E, gap_threshold):
      E = bounding_box(R, E)
      return  /* 統合完了 */
  /* 該当なし → 新規追加 */
  queue.append(R)
```

#### 期待効果

| 状況 | マージ前 | マージ後 | 削減率 |
|------|--------:|--------:|-------:|
| ボール8個 overlay | 26 rect | 4-8 rect | 70-85% |
| cls + 全描画 | 1 rect (全画面) | 1 rect | 変化なし |
| UI更新のみ | 3 rect | 1-2 rect | 33-67% |

### Phase B: MAX_DIRTY_RECTS 拡大 (応急処置)

現在 32 → **64** に拡大。マージで rect 数が減るまでの安全策。

### Phase C: VSYNC スキップ対応 (オプション)

`gfx_present_dirty()` のVSYNC無し版を新設。
libpyxel の `pyxel_run()` が自身のフレームレート制御で VSYNC タイミングを管理し、
転送関数からは VSYNC 待ちを外す。

---

## 5. 検証計画

### 定量テスト

1. pyxel_test TEST 5 で CLS/OVLY 両方の FPS を計測
2. bench_scale2x の Test 7 と比較して乖離がなくなることを確認

### 合格基準

| テスト | 条件 | 目標 |
|--------|------|------|
| TEST 5 CLS モード | ボール8個 + cls() | ≥ 8 fps |
| TEST 5 OVLY モード | ボール8個 + overlay | **≥ 15 fps** |
| bench_scale2x Test 7 | 8スプライト save/restore | ≥ 25 fps (退行なし) |

---

## 6. 関連ファイル

| ファイル | 役割 |
|---------|------|
| [`gfx/gfx_vram.c`](../../gfx/gfx_vram.c) | dirty rect キュー管理 + VRAM転送 (変更対象) |
| [`gfx/gfx_internal.h`](../../gfx/gfx_internal.h) | MAX_DIRTY_RECTS 定義 (変更対象) |
| [`programs/libpyxel/pyxel_gfx.c`](../../programs/libpyxel/pyxel_gfx.c) | dirty rect 生成元 (変更なし) |
| [`programs/libpyxel/pyxel_core.c`](../../programs/libpyxel/pyxel_core.c) | `gfx_present_dirty()` 呼出元 |
| [`programs/tests/pyxel_test.c`](../../programs/tests/pyxel_test.c) | 検証テスト |
| [`docs/tasks/libpyxel/07_BENCH_RESULTS.md`](../libpyxel/07_BENCH_RESULTS.md) | ベンチマーク基準値 |
