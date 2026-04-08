# Page Fault (0x004053E4) 調査指針

## 🎯 結論・ステータス (Closed)
- **ステータス**: `Closed` (Phase 4 で修正完了)
- **根本原因**: `exec.c` 内の `sbrk_heap_limit` 設定において、クリップ前の上限値 (`heap_sz`) でガードを設定していたバグ。これにより1MBのハードリミットが超過し、後続するメモリ領域 (スタック等) への不正な領域侵食と上書きを引き起こしていた。
- **対策**: `MEMMAP_PHASE4` 実装により、システム実メモリ量ベースでのプログラム空間の動的生成と、安全なハードガード (`paging_set_not_present()`) によるページング保護を導入。


| 項目 | 値 |
|------|-----|
| Fault Address | 0x004053E4 (プログラム空間内、BSS領域) |
| Fault EIP | 0x0040046D |
| Error Code | 0x00000000 (Not-Present, Read, Supervisor) |
| エミュレータ メモリ | 約14MB |
| プログラムロードアドレス | 0x400000 |
| ページテーブルカバー範囲 | 16MB (PAGING_PT_COUNT=4) |
| 再現性 | 同一アドレスで100%再現 |

## 2. 矛盾点

- 14MBのメモリがある → identity mappingで0x400000〜0x405000は**全てPresent+R/W**のはず
- `gfx_demo`（BSS=252B、BSSの終端は0x40495C）は問題なく動作
- `demo1`（BSS=2076B、BSSの終端は0x4053FC）は常にクラッシュ
- **しかし**、0x4053E4は14MBの範囲内なのでページは存在するはず

→ identity mapping以外のメカニズムでページが**Not-Presentに書き換えられている**可能性

## 3. 仮説

### 仮説A: exec loader内でプログラム領域のページ属性が変更されている
- `exec.c` L121のコメント `外部プログラム保護 (VMMにより不要となったため削除)` は、過去にVMMが存在し、exec時にページ属性を操作していた痕跡
- 削除されたつもりが一部残っている、または別の場所でNot-Presentが設定されている可能性

### 仮説B: newlib sbrkがページ属性を破壊している
- demo1は `<stdio.h>` の `sprintf` を使用しており、newlibのinitがsbrk経由でメモリを触る
- sbrk実装が `sbrk_heap_limit` を超えて拡張しようとし、別セグメントのページを破壊する可能性

### 仮説C: exec_heap_initのBSSゼロクリアが隣接ページに影響
- `kmemset(load_addr + text_sz, 0, bss_sz)` でBSS初期化時に、text_szの計算がELF上のオフセットと不整合を起こし、想定外の範囲をゼロクリアしている可能性

### 仮説D: スタックオーバーフローによるページテーブル破壊
- プログラムスタック（0x57F000からの128KB）がlibos32gfxの大量のスタックフレームで溢れ、ページテーブル領域を破壊

### 仮説E: SprDataPool確保によるヒープ枯渇 → NULLポインタ経由のアクセス
- `exec_heap_alloc` が `sizeof(SprDataPool)` (~1.1MB) を確保しようとして失敗（ヒープ上限1MB）
- NULLが返され、NULLポインタ + オフセットでアクセスした結果 0x004053E4 に到達
- **しかし**: これだとFault Addressが0x000053E4になるはずで、0x004053E4にはならない

## 4. 調査手順

### Step 1: Page Fault時のCR3を確認
ISRハンドラでCR3の値をダンプし、**exec実行時のページディレクトリが`paging_init`で作成したものと同一か**を確認する。

```c
/* isr_handlers.c の page fault handler に追加 */
u32 cr3_val;
__asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
kprintf(ATTR_RED, "CR3: 0x%08lX\n", cr3_val);
```

### Step 2: 0x400000〜0x406000のPTE値をダンプ
Page Fault発生前にプログラム領域のページテーブルエントリを読み出し、Present/Not-Presentを確認する。

```c
/* exec.c のentry呼び出し直前に追加 */
extern u32 *page_tables[];
{
    int pdi = 0x400000 >> 22;  /* = 1 */
    int i;
    for (i = 0; i < 8; i++) {
        int pti = ((0x400000 >> 12) & 0x3FF) + i;
        kprintf(ATTR_WHITE, "PTE[0x%06lX] = 0x%08lX\n",
                (u32)(pdi * 1024 + pti) * 4096, page_tables[pdi][pti]);
    }
}
```

### Step 3: mem_alloc (exec_heap_alloc) の戻り値を検証
SprDataPool確保が成功しているかログ出力。

```c
/* demo1.c の libos32gfx_init後に追加 */
api->kprintf(0xE1, "ents=%lX results=%lX\n", (u32)ents, (u32)results);
```

### Step 4: newlib初期化 (_sbrk) のアクセス範囲を確認
`programs/libos32/syscalls.c` の `_sbrk` 実装を確認し、ヒープ拡張時にページ属性を変更していないか調査。

### Step 5: gfx_demo と demo1 の差分を最小化して再現
- demo1から`<stdio.h>`を除去し、自前の数値→文字列変換に変更
- libos32gfxの初期化のみで（スプライト追加前に）クラッシュするか確認
- → libos32gfx_init自体の問題か、スプライト追加の問題かを切り分け

## 5. 推奨する最初の手順

**Step 2 (PTE値ダンプ)** が最も直接的。Fault Addressに対応するPTEがNot-Presentであることを確認した上で、**いつ、誰がNot-Presentに設定したか**を追跡する。
