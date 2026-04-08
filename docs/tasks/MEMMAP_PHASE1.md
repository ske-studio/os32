# Phase 1: memmap.h 定数再定義

## 概要

memmap.h の定数を全て新メモリマップに合わせて再定義する。
**この段階ではコードの動作変更は行わない** (定数の値を変えるのみ)。

## 作業対象

### ファイル: `include/memmap.h`

## 変更内容

### 削除する定数

| 定数 | 旧値 | 削除理由 |
|------|------|----------|
| `MEM_GFX_SURF_POOL` | 0x100000 | カーネルGFX内に実装なし (ゴースト) |
| `MEM_EXEC_DEFAULT_HEAP_SIZE` | 0x80000 | 未使用 |
| `MEM_EXEC_DEFAULT_STACK_SIZE` | 0x10000 | 未使用 |

### 変更する定数

| 定数 | 旧値 | 新値 | 理由 |
|------|------|------|------|
| `KHEAP_SIZE` | 0x30000 (192KB) | 0x4F000 (316KB) | デッドゾーン回収 (0x40000-0x8EFFF) |
| `MEM_FONT_CACHE_BASE` | 0x300000 | 0x100000 | 先頭詰め配置 |
| `MEM_UNICODE_TABLE_BASE` | 0x350000 | 0x149000 | フォントキャッシュ直後 |
| `MEM_GFX_BB_BASE` | 0x380000 | 0x169000 | Unicodeテーブル直後 |
| `MEM_EXEC_MAX_SIZE` | 0x180000 (1.5MB) | 0x100000 (1MB) | sbrk領域=1MB |
| `MEM_EXEC_STACK_TOP` | 0x57F000 | **(削除 → 動的計算)** | Phase 4で動的化 |
| `MEM_EXEC_STACK_SIZE` | 0x20000 | 維持 | スタックサイズは128KB据え置き |
| `MEM_EXEC_HEAP_BASE` | 0x580000 | **(削除 → 動的計算)** | Phase 4で動的化 |

### 追加する定数

| 定数 | 値 | 用途 |
|------|-----|------|
| `MEM_KDATA_BASE` | 0x100000 | カーネルデータ域の開始 |
| `MEM_KAPI_ADDR` | 0x189000 | KernelAPIテーブルの新アドレス |
| `MEM_KERNEL_RESV_START` | 0x200000 | カーネル予約域開始 |
| `MEM_KERNEL_RESV_END` | 0x3FFFFF | カーネル予約域終端 |
| `MEM_EXEC_SBRK_GUARD` | (exec_heap_base - PAGE_SIZE) | GUARD Aアドレス (動的) |

### 変更する定数 (os32_kapi_shared.h)

| 定数 | 旧値 | 新値 |
|------|------|------|
| `KAPI_ADDR` | 0x3F0000 | 0x189000 |

## 新 memmap.h 構造案

```c
/* === コンベンショナルメモリ (0x00000 - 0xFFFFF) === */
#define MEM_1MB               0x100000UL
#define KERNEL_LOAD_ADDR      0x9000UL

/* カーネルヒープ */
#define KHEAP_BASE            0x40000UL
#define KHEAP_SIZE            0x4F000UL   /* 316KB (0x40000-0x8EFFF) */

/* ページング保護 */
#define MEM_IVT_PROT_START    0x01000UL
#define MEM_IVT_PROT_END      0x05FFFUL
#define MEM_LOADER_START      0x08000UL
#define MEM_LOADER_END        0x08FFFUL
#define MEM_STACK_GUARD       0x8F000UL
#define MEM_STACK_GUARD_END   0x8FFFFUL
#define MEM_BIOS_ROM_START    0xF0000UL
#define MEM_BIOS_ROM_END      0xFFFFFUL

/* === カーネルデータ (0x100000 - 0x1FFFFF) === */
#define MEM_KDATA_BASE        0x100000UL
#define MEM_FONT_CACHE_BASE   0x100000UL  /* ~292KB フォントキャッシュ */
#define MEM_UNICODE_TABLE_BASE 0x149000UL /* 128KB Unicode-JIS変換 */
#define MEM_GFX_BB_BASE       0x169000UL  /* 128KB GFXバックバッファ */
/* KAPIアドレスは os32_kapi_shared.h で定義 */

/* カーネル予約域 (将来拡張用, NOT PRESENT) */
#define MEM_KERNEL_RESV_START 0x200000UL
#define MEM_KERNEL_RESV_END   0x3FFFFFULL

/* === プログラム空間 (0x400000 - メモリ上限) === */
#define MEM_EXEC_LOAD_ADDR    0x400000UL
#define MEM_EXEC_MAX_SIZE     0x100000UL  /* コード+sbrk 最大1MB */
#define MEM_EXEC_STACK_SIZE   0x20000UL   /* スタック 128KB */

/* PIT */
#define PIT_HZ                100
```

## コミット方針

```
refactor(memmap): Phase 1 — 定数再定義とゴースト領域削除
```

## 検証

- `make all` でビルドが通ること (定数変更のみ、この段階ではビルド不可の場合は Phase 2-4 と同時コミット)
- **注意**: KAPI_ADDR 変更は全プログラム再ビルドが必要

## 依存関係

- なし (最初に着手するフェーズ)
- Phase 2-4 は全てこのフェーズの定数を前提とする
