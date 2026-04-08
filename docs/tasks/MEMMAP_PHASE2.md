# Phase 2: コンベンショナルメモリ修正

## 概要

カーネルヒープ(kmalloc)を拡張し、paging.cの保護マップコメントを実態に合わせて全面修正する。

## 作業対象ファイル

| ファイル | 変更内容 |
|---------|---------|
| `kernel/kernel.c` | kmalloc_init の引数更新 (表示文字列も変更) |
| `kernel/paging.c` | ファイル先頭の保護マップコメントを全面書き直し |

## 変更1: カーネルヒープ拡張

### kernel.c

```diff
-    kmalloc_init((void *)KHEAP_BASE, KHEAP_SIZE);
-    tvram_print(31, 2, "192K", TATTR_WHITE);
+    kmalloc_init((void *)KHEAP_BASE, KHEAP_SIZE);
+    tvram_print(31, 2, "316K", TATTR_WHITE);
```

memmap.h (Phase 1) で `KHEAP_SIZE = 0x4F000` に変更済みのため、コード変更は表示文字列のみ。

### メモリ範囲の意味

```
旧: 0x40000 - 0x6FFFF  (192KB)  kmalloc使用
    0x70000 - 0x8EFFF  (124KB)  ★デッドゾーン (旧VRAMバックバッファ)

新: 0x40000 - 0x8EFFF  (316KB)  kmalloc使用 (デッドゾーン回収)
```

## 変更2: paging.c 保護マップコメント修正

ファイル先頭のコメントブロックを現実に合わせて全面書き直す。

### 旧コメント (現在の paging.c L14-L27)

```c
/*  保護マップ:                                                             */
/*    0x00000 - 0x00FFF : Read-Only   (IVT + BIOSデータ)                   */
/*    0x01000 - 0x06FFF : Read-Only   (BIOS周辺)                           */
/*    0x07000 - 0x07FFF : Read-Write  (BIOSトランポリン)                    */
/*    0x08000 - 0x08FFF : Read-Only   (loader+pm32, 使用済み)              */
/*    0x09000 - 0x3FFFF : Read-Write  (カーネル)                           */
/*    0x40000 - 0x6FFFF : Read-Write  (kmallocヒープ)                      */
/*    0x70000 - 0x8EFFF : Read-Write  (VRAMバックバッファ)       ← 嘘      */
/*    0x8F000 - 0x8FFFF : Not-Present (★スタックガードページ)              */
/*    ...                                                                   */
```

### 新コメント

```c
/*  保護マップ (実態に基づく, 2026-04 再構築):                              */
/*                                                                          */
/*  [コンベンショナルメモリ]                                                */
/*    0x00000 - 0x00FFF : R/W  (IVT + BDA, FDDトランポリンの書き込み有)      */
/*    0x01000 - 0x05FFF : R/O  (BIOS周辺)                                   */
/*    0x06000 - 0x07FFF : R/W  (BIOSトランポリン)                            */
/*    0x08000 - 0x08FFF : R/O  (loader.bin, 使用済み)                        */
/*    0x09000 - 0x3FFFF : R/W  (カーネル .text+.data+.bss + マージン)        */
/*    0x40000 - 0x8EFFF : R/W  (kmallocヒープ, 316KB)                        */
/*    0x8F000 - 0x8FFFF : NP   (★カーネルスタックガード)                     */
/*    0x90000 - 0x9FFFF : R/W  (カーネルスタック, ESP=0x9FFFC)               */
/*    0xA0000 - 0xEFFFF : R/W  (テキスト/グラフィックVRAM)                   */
/*    0xF0000 - 0xFFFFF : R/O  (BIOS ROM)                                   */
/*                                                                          */
/*  [拡張メモリ]                                                            */
/*    0x100000-0x1FFFFF : R/W  (カーネルデータ: フォント/Unicode/BB/KAPI)    */
/*    0x200000-0x3FFFFF : NP   (カーネル予約, 将来拡張用)                    */
/*    0x400000-mem_end  : R/W  (プログラム空間, ガードページ付き)            */
/*    mem_end -0xFFFFFF : NP   (未実装メモリ)                               */
```

### paging.c L121 の旧コメント削除

```diff
-    /* ====== 外部プログラム保護 (VMMにより不要となったため削除) ====== */
+    /* ====== 外部プログラム保護 (Phase 4 でガードページ設定に置換) ====== */
```

## コミット方針

```
refactor(kernel): Phase 2 — カーネルヒープ拡張 (192→316KB) + paging.cコメント修正
```

## 検証

```bash
make clean && make all
# BSS終端が KHEAP_BASE (0x40000) より十分下にあることを確認
i386-elf-nm kernel.elf | grep __bss_end
# → 0x2725C (< 0x40000) であればOK
```

## 依存関係

- Phase 1 (memmap.h 定数) が先に完了していること
