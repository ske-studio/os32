# タスク05: スクロールエンジン NASM 高速化

作成日: 2026-04-24
更新日: 2026-04-24
依存: [04_SCROLL_OPT.md](04_SCROLL_OPT.md) (Phase 1〜3 完了済み)

---

## 1. 目的

差分スクロールエンジン (Phase 1〜3) の C 実装をベースに、
ボトルネックとなる BB シフト・タイル描画・上層BG再描画を最適化し、
H-diff 77ms/frame を目標 **33ms (30fps)** まで削減する。

---

## 2. 現状のコードフロー分析

### 2.1 `tilemap_compose_scroll()` の処理フロー

```
tilemap_compose_scroll()
├─ BG0 のスクロール差分 (dx_total, dy_total) を算出
├─ 8px非境界 or 大ジャンプ → フォールバック (全面btf再描画)
│
├─ gfx_dirty_suppress = 1
│
├─ [H-diff] dx_total != 0 の場合:
│  ├─ bb_shift_horiz(shift_bytes)   ← C: 4plane × 384行 × memmove
│  └─ draw_column_btf() × 露出列   ← C: gfx_blit/gfx_blit_transparent 経由
│
├─ [V-diff] dy_total != 0 の場合:
│  ├─ bb_shift_vert(shift_lines)    ← C: 4plane × memcpy/memset
│  └─ draw_row_btf() × 露出行      ← C: gfx_blit/gfx_blit_transparent 経由
│
├─ [上層BG再描画] has_upper_bg の場合:
│  └─ redraw_upper_bgs()            ← BG1〜3 の非透明タイルを全セル走査 ★高コスト
│
├─ [ダーティタイル処理] detect_dirty() + 全セル走査
│
├─ gfx_dirty_suppress = 0
│
├─ gfx_add_dirty_rect(384×384) ← ★全面dirty — 差分効果を部分的に無駄にしている
│
└─ prev_scroll 更新
```

### 2.2 ボトルネック内訳 (H-diff 77ms の推定)

| 処理 | 推定コスト | 備考 |
|------|-----------|------|
| `bb_shift_horiz` (1列=8px) | ~15ms | 384行 × 48byte/行 × 4plane の memmove |
| `draw_column_btf` (1〜2列) | ~35ms | 24タイル × BG0 × gfx_blit 呼び出し |
| `redraw_upper_bgs()` | ~20ms | 全576セル × BG1〜3 走査 (多くは TRANSPARENT でスキップ) |
| `flush` → VRAM転送 | — | `gfx_present_dirty` 内で別途計上 |

> **ボトルネック順位**: タイル描画 > BBシフト > 上層BG走査 > dirty rect管理

---

## 3. 実装フェーズ

### Phase 4 — NASM: bb_shift_horiz / bb_shift_vert 高速化

**ファイル**: `programs/libtilemap/asm_tilemap.asm`

#### 4.1 現在の C 実装

```c
/* bb_shift_horiz: 1行あたり memmove + memset (48byte 未満) × 384行 × 4plane */
for (p = 0; p < 4; p++) {
    for (row = 0; row < tile_h; row++) {
        memmove(line, line + shift_bytes, tile_bytes - shift_bytes);
        memset(line + tile_bytes - shift_bytes, 0, shift_bytes);
    }
}

/* bb_shift_vert: 1行あたり memcpy (48byte) × (384 - shift)行 × 4plane */
for (p = 0; p < 4; p++) {
    for (row = ...) {
        memcpy(dst, src, tile_bytes);   /* 48byte = 12 DWORD */
    }
    for (row = ...) {
        memset(dst, 0, tile_bytes);     /* 露出行クリア */
    }
}
```

#### 4.2 NASM 最適化方針

- 各行を `rep movsd` (12 DWORD) で転送
- 水平シフト: 方向に応じて DF=0/DF=1 切り替え
- 垂直シフト: 行の走査順で上→下 / 下→上 を使い分け
- プレーンループは NASM 内で 4 回アンロール (C⇔NASM 遷移を削減)

#### 4.3 API 設計

C 側の `bb_shift_horiz` / `bb_shift_vert` と同一引数構成。
呼び出し元を差し替えるだけで適用可能にする。

```c
void __cdecl asm_bb_shift_horiz(
    u8 *planes[4],   /* gfx_fb.planes */
    int pitch,       /* gfx_fb.pitch (80) */
    int origin_y,    /* タイル領域開始Y */
    int tile_h,      /* タイル領域高さ (384) */
    int base_byte,   /* 水平開始オフセット (origin_x >> 3) */
    int tile_bytes,  /* 水平幅バイト数 (48) */
    int shift_bytes  /* シフト量 (正=右方向, 負=左方向) */
);

void __cdecl asm_bb_shift_vert(
    u8 *planes[4],
    int pitch,
    int origin_y,
    int tile_h,
    int base_byte,
    int tile_bytes,
    int shift_lines  /* シフト量 (正=上方向, 負=下方向) */
);
```

#### 4.4 期待改善

| 処理 | C版 | NASM版 | 改善 |
|------|-----|--------|------|
| bb_shift (H, 1列) | ~15ms | ~3ms | 5× |
| bb_shift (V, 1行) | ~15ms | ~3ms | 5× |

---

### Phase 5 — NASM: タイルブリット (draw_column / draw_row) 高速化

**ファイル**: `programs/libtilemap/asm_tilemap.asm`

#### 5.1 問題

現在の `draw_column_btf` / `draw_row_btf` は各タイルごとに
`gfx_blit()` / `gfx_blit_transparent()` を関数呼び出ししている。

```c
/* draw_column_btf: 1列 = 24行 × BG_COUNT × gfx_blit 呼び出し */
for (row = 0; row < TILEMAP_ROWS; row++) {
    for (bg = 0; bg < BG_COUNT; bg++) {
        prepare_tile_surface(&tmp_surf, tile, attr, flip_buf);
        if (tile->opacity == TILE_OPAQUE)
            gfx_blit(dx, dy, &tmp_surf, NULL);
        else
            gfx_blit_transparent(dx, dy, &tmp_surf, NULL);
    }
}
```

関数呼び出しオーバーヘッド (引数構築 + GFX_Surface 一時構築) が支配的。

#### 5.2 最適化方針: ストリーム描画ルーチン

既存の `asm_tile_pair_opaque` のアーキテクチャを拡張し、
1列/1行分のタイルを連続ストリームで BB に直接書き込む。

```c
/* 1列分 (24タイル) の不透明タイルをBBに連続描画 */
void __cdecl asm_draw_column_opaque(
    u8 *bb_planes[4],     /* バックバッファプレーン配列 */
    int bb_pitch,         /* BBピッチ (80) */
    int dst_byte_x,       /* 描画先Xバイトオフセット */
    int dst_y_start,      /* 描画先Y開始行 */
    const u8 *tile_data,  /* タイルデータ先頭 (planes連続) */
    int tile_psz,         /* TILE_PLANE_SZ (32) */
    int tile_count        /* タイル数 (≤24) */
);
```

**BG0 のみ不透明の場合** (1BG スクロールの典型パターン):
- `gfx_blit` の関数呼び出しを完全排除
- タイル間でレジスタを保持し、dst ポインタを `+= pitch * TILE_H` で進める
- フリップ付きタイルは C 側で `flip_buf` を事前準備してから渡す

#### 5.3 段階的アプローチ

1. **Step A**: `asm_draw_column_opaque` — 不透明タイル1列連続描画
2. **Step B**: `asm_draw_row_opaque` — 不透明タイル1行連続描画
3. **Step C**: 透明タイル対応 (colorkey マスク処理) — 必要に応じて

> Step C は BG0 が全面不透明の典型ケースでは不要。
> 上層BGの透明タイルは Phase 5.5 で別途対応。

#### 5.4 期待改善

| 処理 | C版 | NASM版 | 改善 |
|------|-----|--------|------|
| draw_column (1列, 24タイル) | ~35ms | ~12ms | 3× |

---

### Phase 5.5 — 上層BG再描画の最適化

**ファイル**: `programs/libtilemap/tilemap_compose.c`

#### 5.5.1 問題

現在の `redraw_upper_bgs()` は **全 576 セル × 3 BG** を走査しており、
差分スクロールの利点を部分的に打ち消している。

```c
/* 現状: BG1〜3 の非透明タイルを全面走査 */
for (row = 0; row < TILEMAP_ROWS; row++)
    for (col = 0; col < TILEMAP_COLS; col++)
        for (bg = 1; bg < BG_COUNT; bg++) { ... }
```

#### 5.5.2 最適化方針

**A. 走査範囲の限定**:
- BBシフトで移動した範囲 + 露出列/行の交差領域のみ上層BGを再描画
- シフト方向と量から「上層BGの再描画が必要な領域」を事前計算

**B. 上層BG ダーティ追跡**:
- BG1〜3 の各セルに `upper_needs_redraw` フラグを導入
- BBシフト時にシフト範囲のフラグを立て、再描画後にクリア

**C. dirty rect の精密化** (Phase 4 と連動):
- 現在の `gfx_add_dirty_rect(384×384)` (全面) を排除
- `drawn_tracking_mark` による列/行単位の精密 dirty rect に統一

#### 5.5.3 期待改善

| 処理 | 現行 | 最適化後 | 改善 |
|------|------|---------|------|
| upper BG 再描画 (1列シフト時) | ~20ms (全面) | ~2ms (1列分) | 10× |
| dirty rect → VRAM | 全面転送 | 列単位転送 | 2〜5× |

---

### Phase 6 — H-diff + V-diff 同時処理 (斜めスクロール)

**ファイル**: `programs/libtilemap/tilemap_compose.c`

#### 6.1 現状

`tilemap_compose_scroll` は H-diff と V-diff を **順次処理**:
1. 水平 BB シフト → 露出列描画
2. 垂直 BB シフト → 露出行描画

斜めスクロール時、コーナー部分 (露出列×露出行の交差領域) が
2重描画される場合がある。

#### 6.2 改善方針

```
┌────────────────────┬───────┐
│                    │       │
│  保持領域           │ 露出列 │
│  (BB シフト後)      │(再描画)│
│                    │       │
├────────────────────┼───────┤
│      露出行 (再描画) │  角   │
└────────────────────┴───────┘
```

- 角領域 (corner) は露出列描画時にカバー済み → 露出行描画でスキップ
- または H→V の順に処理し `drawn_tracking_mark` の重複を利用してスキップ

#### 6.3 前提

Phase 4/5 完了後に着手。

---

## 4. 実装スケジュール

| Phase | 内容 | 難易度 | 優先度 | 推定効果 |
|-------|------|--------|--------|---------|
| Phase 4 | BB シフト NASM化 | 中 | 高 | BBシフト 5× 削減 |
| Phase 5 | タイルブリット NASM化 | 高 | 最高 | タイル描画 3× 削減 |
| Phase 5.5 | upper BG + dirty rect 精密化 | 中 | 高 | VRAM転送量 大幅削減 |
| Phase 6 | 斜めスクロール | 低 | 中 | コーナー重複排除 |

**目標**: Phase 4 + 5 + 5.5 完了で **H-diff ≈ 20〜33ms/frame (30fps 達成)** を見込む。

---

## 5. テスト方法

`tile_bench v` および `tile_bench s` で計測:

```
[Scroll] x10
btf (full redraw)         365 ticks (3650 ms)
compose_scroll H-diff      77 ticks ( 770 ms)  ← Phase 4+5+5.5 後: 目標 20〜33 ticks
compose_scroll V-diff      ?? ticks             ← 計測後に記入
```

---

## 6. 注意事項

- V-diff 実装にはデバッグ用 `kprintf` が残存 (`tilemap_compose.c` L745-748, L763, L769)
  → Phase 4 着手前に除去すること
- `tilemap_compose_scroll()` 末尾の `gfx_add_dirty_rect(384×384)` (L818-820) は
  Phase 5.5 で精密化するまでは安全側として残す

---

## 7. 参考

- [asm_tilemap.asm](../../programs/libtilemap/asm_tilemap.asm) — 既存 NASM ルーチン (348行)
- [tilemap_compose.c](../../programs/libtilemap/tilemap_compose.c) — 差分スクロール実装 (838行)
- [04_SCROLL_OPT.md](04_SCROLL_OPT.md) — Phase 1〜3 実装記録
