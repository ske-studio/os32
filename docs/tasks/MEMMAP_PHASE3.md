# Phase 3: カーネルデータ再配置

## 概要

拡張メモリ上のカーネルデータ（フォントキャッシュ、Unicode変換テーブル、GFXバックバッファ、KernelAPIテーブル）を先頭から詰めて配置し直す。
カーネル予約域 (0x200000-0x3FFFFF) を NOT PRESENT に設定する。

## 新配置マップ

```
  0x100000 ┌──────────────────────────────┐
           │ フォントキャッシュ (~292KB)    │ kcg.c: kanji_cache等
  0x149000 ├──────────────────────────────┤
           │ Unicode-JIS テーブル (128KB)  │ utf8.c + kernel.c vfs_read
  0x169000 ├──────────────────────────────┤
           │ GFX バックバッファ (128KB)    │ gfx_core.c: bb_b/r/g/i
  0x189000 ├──────────────────────────────┤
           │ KernelAPI テーブル (4KB)      │ exec.c: kapi pointer
  0x18A000 ├──────────────────────────────┤
           │ [空き: カーネル拡張用] ~472KB │
  0x200000 ├══════════════════════════════┤
           │ [予約: NOT PRESENT] 2MB      │ 将来拡張用
  0x400000 └──────────────────────────────┘
```

## 作業対象ファイル

| ファイル | 変更内容 |
|---------|---------|
| `include/memmap.h` | Phase 1 で変更済み (定数のみ) |
| `include/os32_kapi_shared.h` | `KAPI_ADDR` を 0x3F0000 → 0x189000 |
| `kernel/paging.c` | カーネル予約域を NOT PRESENT に設定 |
| `programs/libos32gfx/libgfx_internal.h` | `MEM_GFX_SURF_POOL` ダミー定義削除 |

## 変更詳細

### os32_kapi_shared.h — KAPI_ADDR 変更

```diff
-#define KAPI_ADDR         0x3F0000UL
+#define KAPI_ADDR         0x189000UL
```

**影響**: 外部プログラムは全て再ビルドが必要。`make all` で自動対応。

### paging.c — カーネル予約域保護

`paging_init()` の保護設定ブロックに追加:

```c
    /* カーネル予約域: Not-Present (将来拡張用) */
    paging_set_not_present(MEM_KERNEL_RESV_START, MEM_KERNEL_RESV_END);
```

### libgfx_internal.h — ダミー定義削除

```diff
-#define MEM_GFX_SURF_POOL 0x600000 /* Dummy, let's just use heap allocation in libos32 */
```

この定数は実際にはどこからも参照されていない (gfx_surface.c は `gfx_api->mem_alloc()` を使用)。

## コードが自動追従するファイル (変更不要)

以下のファイルは memmap.h / os32_kapi_shared.h のマクロ経由でアドレスを参照しているため、定数変更で自動追従する。

| ファイル | 参照定数 | 追従方法 |
|---------|---------|---------|
| `drivers/kcg.c` | `MEM_FONT_CACHE_BASE` | static ポインタ初期化子 |
| `lib/utf8.c` | `MEM_UNICODE_TABLE_BASE` | static ポインタ初期化子 |
| `gfx/gfx_core.c` | `MEM_GFX_BB_BASE` | static ポインタ初期化子 |
| `kernel/kernel.c` | `MEM_UNICODE_TABLE_BASE` | vfs_read の引数 |
| `exec/exec.c` | `KAPI_ADDR` | static ポインタ初期化子 |

## コミット方針

```
refactor(kernel): Phase 3 — カーネルデータ再配置 (フォント/Unicode/BB/KAPI → 0x100000-)
```

## 検証

```bash
make clean && make all

# KAPIアドレスの確認
grep KAPI_ADDR include/os32_kapi_shared.h
# → 0x189000

# NP21/Wで起動テスト
# 1. shell が正常に起動すること
# 2. 日本語文字表示が正常であること (フォントキャッシュ再配置の検証)
# 3. gfx_demo が動作すること (バックバッファ再配置の検証)
```

## リスク

- **KAPI_ADDR 変更**: 外部プログラムが旧アドレス (0x3F0000) の `.bin` で起動すると即クラッシュ。
  必ず `make all` で全て再ビルドすること。
- **フォントキャッシュ移動**: kcg.c のポインタが `static u8 *kanji_cache = (u8 *)(MEM_FONT_CACHE_BASE);` として
  初期化子で設定されるため、定数変更のみで安全に追従する。

## 依存関係

- Phase 1 (memmap.h 定数) + Phase 2 (paging.c コメント修正) が完了していること
