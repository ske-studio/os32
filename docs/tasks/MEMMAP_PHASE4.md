# Phase 4: プログラム空間再設計

## 概要

プログラムメモリ空間を「動的計算 + ガードページ完備」に再設計する。
このフェーズが **最もリスクが高く、最も多くのバグを修正する**。

## 修正するバグ

1. **sbrk_heap_limit バグ** — クリップ前の `heap_sz` で設定される問題
2. **EXEC_HEAP_MAX 固定上限** — 1MB固定を撤廃し、メモリ量に自動適応
3. **ガードページ不在** — sbrk/exec_heap/スタック間に保護なし

## 新レイアウト (14MB システムの例)

```
  0x400000  ┌─────────────────────────────────┐
            │ .text + .data + .bss             │ app.ld: . = 0x400000 (変更なし)
            │ _end                             │
            │ newlib sbrk ヒープ →             │ (上方成長)
            │                                  │
  0x500000  │ ★ GUARD A (4KB NOT PRESENT)      │ ← sbrk上限バリア
  0x501000  │                                  │
            │ exec_heap                        │ mem_alloc / mem_free
            │ (動的サイズ: ~8.9MB)             │
            │                                  │
  0xDDF000  │ ★ GUARD B (4KB NOT PRESENT)      │ ← スタックオーバーフロー検出
  0xDE0000  │                                  │
            │ プログラムスタック (128KB)        │ (下方成長)
            │                                  │
  0xDFFFFC  │ ← ESP 初期値                     │
  0xE00000  ████ NOT PRESENT (未実装メモリ) ████│ ← 自然なスタック上界ガード
```

## 動的パラメータ計算式

```c
/* memmap.h の固定定数 */
#define MEM_EXEC_LOAD_ADDR    0x400000UL
#define MEM_EXEC_MAX_SIZE     0x100000UL  /* コード+sbrk最大1MB */
#define MEM_EXEC_STACK_SIZE   0x20000UL   /* 128KB */
#define PAGE_SIZE             4096

/* exec_run() 内で動的に計算 */
extern u32 sys_mem_kb;

u32 mem_end        = sys_mem_kb * 1024;
u32 stack_top      = mem_end;                            /* ESP = mem_end - 4 */
u32 stack_bottom   = stack_top - MEM_EXEC_STACK_SIZE;    /* スタック底 */
u32 guard_b        = stack_bottom - PAGE_SIZE;           /* GUARD B */
u32 exec_heap_end  = guard_b;                            /* exec_heap 上限 */
u32 guard_a        = MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE; /* 0x500000 */
u32 exec_heap_base = guard_a + PAGE_SIZE;                /* 0x501000 */
u32 exec_heap_size = exec_heap_end - exec_heap_base;     /* 動的サイズ */

/* sbrk上限 = GUARD A の位置 (= exec_heap_base - PAGE_SIZE) */
kapi->sbrk_heap_limit = guard_a;  /* 0x500000 */
```

### メモリ量別の結果

| 項目 | 8MB | 14MB | 16MB |
|------|-----|------|------|
| mem_end | 0x800000 | 0xE00000 | 0x1000000 |
| sbrk上限 (GUARD A) | 0x500000 | 0x500000 | 0x500000 |
| exec_heap 開始 | 0x501000 | 0x501000 | 0x501000 |
| exec_heap サイズ | **2.9MB** | **8.9MB** | **10.9MB** |
| GUARD B | 0x7DF000 | 0xDDF000 | 0xFDF000 |
| Stack 範囲 | 0x7E0000-0x7FFFFF | 0xDE0000-0xDFFFFF | 0xFE0000-0xFFFFFF |
| ESP 初期値 | 0x7FFFFC | 0xDFFFFC | 0xFFFFFC |

## 作業対象ファイル

### 1. `exec/exec.c` — 動的レイアウト計算 + sbrk修正 + ガードページ

```diff
 int exec_run(const char *cmdline)
 {
     u32 load_base = EXEC_LOAD_ADDR;
     u32 max_size  = EXEC_MAX_SIZE;
-    u32 stack_top = EXEC_STACK_TOP;
+    /* --- 動的レイアウト計算 --- */
+    u32 mem_end = sys_mem_kb * 1024;
+    u32 stack_top = mem_end;
+    u32 stack_bottom = stack_top - MEM_EXEC_STACK_SIZE;
+    u32 guard_b = stack_bottom - PAGE_SIZE;
+    u32 guard_a = MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE;
+    u32 exec_heap_base = guard_a + PAGE_SIZE;
+    u32 exec_heap_size = guard_b - exec_heap_base;
     ...

     /* ヒープ初期化 (動的サイズ) */
-    exec_heap_init(heap_sz);
-    kapi->sbrk_heap_limit = EXEC_HEAP_BASE + heap_sz;  /* ★バグ */
+    exec_heap_init_at(exec_heap_base, exec_heap_size);
+    kapi->sbrk_heap_limit = guard_a;  /* ★修正: 固定上限 */

+    /* ガードページ設定 */
+    paging_set_not_present(guard_a, guard_a + PAGE_SIZE - 1);
+    paging_set_not_present(guard_b, guard_b + PAGE_SIZE - 1);
     ...
```

### 2. `exec/exec_heap.h` — EXEC_HEAP_MAX 廃止

```diff
-#define EXEC_HEAP_BASE    MEM_EXEC_HEAP_BASE
-#define EXEC_HEAP_MAX     (1024UL * 1024UL)  /* 最大1MB */
-#define EXEC_HEAP_DEFAULT (1024UL * 1024UL)  /* デフォルト1MB */
+/* exec_heap のベースアドレスとサイズは exec_run() 内で動的に計算される */

-void exec_heap_init(u32 size);
+void exec_heap_init_at(u32 base, u32 size);
```

### 3. `exec/exec_heap.c` — 動的ベース対応

```diff
-static u8  *heap_base = (u8 *)EXEC_HEAP_BASE;
+static u8  *heap_base = (u8 *)0;

-void exec_heap_init(u32 size)
+void exec_heap_init_at(u32 base, u32 size)
 {
     BlkHdr *first;

-    if (size > EXEC_HEAP_MAX) size = EXEC_HEAP_MAX;
-    if (size == 0) size = EXEC_HEAP_DEFAULT;
-
-    heap_base = (u8 *)EXEC_HEAP_BASE;
+    heap_base = (u8 *)base;
     heap_size = size;
     heap_used = 0;
```

### 4. `exec/exec.h` — 不要な定数削除

```diff
-/* プログラムのロード先アドレス */
-#define EXEC_LOAD_ADDR    MEM_EXEC_LOAD_ADDR
-#define EXEC_MAX_SIZE     MEM_EXEC_MAX_SIZE
-
-/* プログラム専用スタック (カーネルスタックと分離) */
-#define EXEC_STACK_TOP    MEM_EXEC_STACK_TOP
-#define EXEC_STACK_SIZE   MEM_EXEC_STACK_SIZE
+/* プログラムのロード先 (固定) */
+#define EXEC_LOAD_ADDR    MEM_EXEC_LOAD_ADDR
+#define EXEC_MAX_SIZE     MEM_EXEC_MAX_SIZE
```

### 5. `exec/exec.c` — ガードページ解除 (exec_run 終了時)

```c
    /* exec_longjmp 帰還後、ガードページを解除して元の状態に戻す */
    if (exec_setjmp(exec_kernel_jmpbuf) != 0) {
        /* ガードページ解除 */
        paging_set_page(guard_a, guard_a, PAGE_RW);
        paging_set_page(guard_b, guard_b, PAGE_RW);
        exec_heap_reset();
        return exec_exit_status;
    }
```

### 6. `Makefile` — demo1 ヒープサイズ修正

```diff
     elif [ "$*" = "demo1" ]; then \
-        python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 2097152; \
+        python3 tools/mkos32x.py $< $@ --elf programs/$*.elf --api 19 --heap 1048576; \
```

**注意**: OS32Xヘッダの `heap_size` フィールドは exec_heap_init_at に渡されるサイズの「ヒント」として使用していたが、
新設計ではメモリ量に基づく動的計算に置換されるため、ヘッダの `heap_size` は無視される（互換性のため残すが使用しない）。

## インラインASMのスタックポインタ変更

exec.c L159-171 のインラインASMでスタック切り替えを行う部分:

```c
    new_esp = stack_top;  /* 動的計算された値を使用 */
```

ここは現行コードの `stack_top` 変数を参照しており、その値が動的計算に変わるだけなので ASM自体の変更は不要。

## コミット方針

```
fix(exec): Phase 4 — sbrk_heap_limitバグ修正 + 動的プログラム空間 + ガードページ
```

## 検証

### ビルド検証

```bash
make clean && make all
```

### 実行検証 (NP21/W)

1. **shell 起動テスト** — 正常に起動し、コマンドが実行できること
2. **demo1 実行** — Page Fault が発生しないこと (★メインの修正目標)
3. **gfx_demo 実行** — スプライト描画が正常であること
4. **heap コマンド** — exec_heap の統計情報が新しい範囲を反映すること

### ガードページ検証

Page Faultハンドラに以下のログを追加して、ガードページが正しく機能することを確認:

```c
/* isr_handlers.c */
if (fault_addr >= guard_a && fault_addr < guard_a + PAGE_SIZE) {
    kprintf(ATTR_RED, "SBRK OVERFLOW detected at 0x%08lX\n", fault_addr);
}
if (fault_addr >= guard_b && fault_addr < guard_b + PAGE_SIZE) {
    kprintf(ATTR_RED, "STACK OVERFLOW detected at 0x%08lX\n", fault_addr);
}
```

## 依存関係

- Phase 1 (定数), Phase 2 (paging.c), Phase 3 (KAPI_ADDR) が全て完了していること
- **このフェーズは Phase 1-3 と同時に適用しないとビルドが通らない**
  → 実装上は Phase 1-4 を1つのブランチで作業し、Phase ごとにコミットを分割する
