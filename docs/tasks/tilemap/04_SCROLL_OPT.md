# タスク04: スクロール最適化 (差分スクロールエンジン)

作成日: 2026-04-24

---

## 1. 目的

フルフレーム再描画 (全576タイル) から差分更新へ移行することで、
30fps スクロールを実現する。

PC-98のVRAMバス帯域制限 (16bit bus @ 8MHz相当) により、
フルフレーム更新は ~365ms/frame (3fps相当) かかる。

---

## 2. 実装フェーズ

### Phase 1 — 列単位ダーティ追跡 ✅ 完了

**概要**: 全画面 `gfx_add_dirty_rect(384x384)` を廃止し、
描画した列のみダーティ登録する。

**実装ファイル**:
- `programs/libtilemap/tilemap_compose.c`
  - `drawn_tracking_reset()` / `drawn_tracking_mark()` / `flush_drawn_dirty()`
  - `tilemap_present()` を `gfx_present_dirty()` に変更

**ベンチマーク結果** (`tile_bench` x30, 2-Layer Mixed, Delta 1-tile move):
| 方式 | ticks | ms |
|------|-------|----|
| compose_btf | 26 ticks | 260ms |
| compose_btf_fast | 30 ticks | 300ms |
| compose_ftb | 36 ticks | 360ms |

---

### Phase 2 — 差分水平スクロール ✅ 完了

**概要**: タイルマップのスクロール量から dx を算出し、
バックバッファ(BB)を `memmove` でシフトして露出列のみ再描画する。

**実装ファイル**:
- `programs/libtilemap/tilemap_compose.c`
  - `bb_shift_horiz(shift_bytes)` — BB水平バイトシフト
  - `draw_column_btf(screen_col, map_col)` — 露出列の再描画
  - `tilemap_compose_scroll()` — 差分スクロールの統合エントリ
- `programs/libtilemap/tilemap_bg.c`
  - `tilemap_scroll_sync()` — prev_scroll 状態の明示的リセット

**ベンチマーク結果** (`tile_bench v`, Scroll x10, H-diff):
| 方式 | ticks | ms/frame |
|------|-------|----------|
| btf (full redraw) | 365 ticks | 365ms |
| compose_scroll H-diff | 77 ticks | 77ms |

→ **約4.7倍高速化** (full redraw 比)

---

### Phase 3 — 差分垂直スクロール ✅ 完了

**概要**: 垂直方向の差分更新 (`bb_shift_vert`) を実装。
 Triple Fault は `make clean` 漏れによる構造体サイズの不整合(`gfx_fb` の破壊)が原因であったため修正済み。また、差分更新時の VRAM 同期漏れを防ぐため、各描画関数に `drawn_tracking_mark()` を追加。

**実装ファイル**:
- `programs/libtilemap/tilemap_compose.c`
  - `bb_shift_vert(shift_lines)` — BB垂直ラインシフト
  - `draw_row_btf(screen_row, map_row)` — 露出行の再描画

---

## 3. バグ調査記録 — V-diff Triple Fault

### 3.1 現象

- `tile_bench v` (垂直差分スクロールのみ実行) でトリプルフォルトが発生
- H-diff は正常に 77 ticks で完了
- V-diff の1フレーム目から `p0=0, pitch=16` と `gfx_fb` が破壊されている

### 3.2 デバッグ出力 (シリアルコンソール)

```
[Scroll] x10
btf (full redraw)         365 ticks (3650 ms)
avg: 365 ms/frame
compose_scroll H-diff      77 ticks (770 ms)
avg: 77 ms/frame
V: dy=16 p0=0 pitch=16
V: shift done
V: draw r=23 mr=0
V: draw done
V: dy=16 p0=0 pitch=32
...
(以降 pitch が 16ずつ増加し続け、やがてアクセス違反でクラッシュ)
```

### 3.3 仮説

`gfx_fb` グローバル変数 (`GFX_Framebuffer`) が、
V-diffループ実行中に上書きされている。

症状:
- `p0 = 0` (NULLポインタ)
- `pitch` が 16 ずつ累積増加 → 毎フレーム加算されている

**有力仮説**:
スタックのローカル変数 or BB内部の書き込み範囲外 → `gfx_fb` へのオーバーライト。

### 3.4 試みた対策

| 対策 | 結果 |
|------|------|
| prev_scroll同期 (`tilemap_scroll_sync()` 追加) | 改善なし |
| ベンチの V-diff 単独テスト (`tile_bench v` 引数追加) | クラッシュ再現確認 |
| `bb_shift_vert` のforループ内変数宣言をC89準拠に修正 | 改善なし |
| デバッグkprintf (bb_shift_vert前後) | `V: shift done` まで正常 |
| デバッグkprintf (bench setup) | DBG0〜5 が表示されない → ループ内で破壊 |

### 3.5 次のアクション

- [ ] `bb_shift_vert` 内部の書き込み範囲計算を検証
  - `_tilemap.origin_y`, `gfx_fb.pitch`, `base_byte`, `tile_bytes` の実値ログ
  - `(origin_y + row + shift_lines) * pitch + base_byte` がBB範囲内か確認
- [ ] `gfx_fb` アドレスと `bb_shift_vert` の書き込み先アドレスの重複を確認
- [ ] H-diff終了時点の `gfx_fb` の正常性を確認 (DBGポイント追加)

---

## 4. 参考: ベンチマーク全体結果

`tile_bench` (全テスト, 2026-04-24 時点)

### Full Draw
| シナリオ | compose_btf | compose_btf_fast | compose_ftb |
|---------|-------------|-----------------|-------------|
| 1-Layer Opaque x5 | ~133 ticks | ~134 ticks | ~153 ticks |
| 4-Layer Heavy x3  | ~125 ticks | ~99 ticks | ~130 ticks |

### Delta Update (1-tile move x30, 2-Layer Mixed)
| 方式 | ticks |
|------|-------|
| compose_btf | 26 ticks |
| compose_btf_fast | 30 ticks |
| compose_ftb | 36 ticks |

### Flip (Full Draw x3)
| 方式 | ticks |
|------|-------|
| No flip | 95 ticks |
| H-flip / V-flip / HV-flip | ~138-139 ticks |

### Scroll (x10)
| 方式 | ticks | ms/frame |
|------|-------|----------|
| btf (full redraw) | 365 ticks | 365ms |
| compose_scroll H-diff | 77 ticks | **77ms** |
| compose_scroll V-diff | 未計測 (クラッシュ) | — |

---

## 5. 今後の最適化候補

- **Phase 4**: NASM による `bb_shift_vert` 高速化 (rep movsd)
- **Phase 5**: `draw_row_btf` の NASM タイルブリット最適化
- **Phase 6**: H-diff と V-diff の同時処理 (斜めスクロール)
