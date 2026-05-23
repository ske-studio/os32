# kernel/ V86 サブディレクトリ分離

策定日: 2026-05-23  
ステータス: 未着手

---

## 現状の問題

`kernel/` ディレクトリは **110ファイルのフラット構造** で、うち V86 関連が **36ファイル (33%)**
を占める。可読性・ナビゲーション性が著しく低い。

### V86 関連ファイル一覧 (19ソース + 17ヘッダ)

**Cソース (18ファイル)**:
`v86.c`, `v86_bios.c`, `v86_debug.c`, `v86_disasm.c`, `v86_disk.c`, `v86_dma.c`,
`v86_dos_ref.c`, `v86_event.c`, `v86_fdc.c`, `v86_iocore.c`, `v86_mem.c`, `v86_pic.c`,
`v86_pit.c`, `v86_session.c`, `v86_sstep.c`, `v86_test.c`, `v86_vsync.c`, `v86_watch.c`

**ASMソース (1ファイル)**: `v86_entry.asm`

**主要ヘッダ**: `v86.h`, `v86_event.h`, `v86_debug.h` 等

**巨大ファイル**:
- `v86.c` — 87KB
- `v86_disk.c` — 56KB
- `v86_debug.c` — 56KB

## 変更方針

### ディレクトリ構造

```
kernel/
├── v86/                    ← 新設
│   ├── v86.c
│   ├── v86.h
│   ├── v86_bios.c
│   ├── v86_debug.c / .h
│   ├── v86_disasm.c
│   ├── v86_disk.c
│   ├── v86_dma.c
│   ├── v86_dos_ref.c
│   ├── v86_entry.asm
│   ├── v86_event.c / .h
│   ├── v86_fdc.c
│   ├── v86_iocore.c
│   ├── v86_mem.c
│   ├── v86_pic.c
│   ├── v86_pit.c
│   ├── v86_session.c
│   ├── v86_sstep.c
│   ├── v86_test.c
│   ├── v86_vsync.c
│   └── v86_watch.c
├── kernel.c
├── gdt.c
├── ... (残りのカーネルファイル)
```

### 変更対象

| ファイル | 変更内容 |
|---------|---------|
| `build/kernel.mk` | C_KERNEL の v86 エントリを `kernel/v86/*.c` に変更。ASM_KERNEL の `kernel/v86_entry.asm` → `kernel/v86/v86_entry.asm`。`-I kernel/v86` のインクルードパス追加検討 |
| `kernel/*.c` | `#include "v86.h"` → `#include "v86/v86.h"` (V86外のカーネルファイルから参照がある場合) |
| `kernel/v86/*.c` | 内部の `#include` パスを相対パスに調整 |

### kernel.mk の具体的変更

現状 (L10-22、v86部分):
```makefile
C_KERNEL = ... \
    kernel/v86.c kernel/v86_mem.c kernel/v86_bios.c kernel/v86_pic.c \
    kernel/v86_pit.c kernel/v86_vsync.c kernel/v86_fdc.c kernel/v86_dma.c \
    kernel/v86_disk.c kernel/v86_test.c kernel/v86_session.c \
    kernel/v86_debug.c kernel/v86_event.c kernel/v86_disasm.c \
    kernel/v86_dos_ref.c kernel/v86_sstep.c kernel/v86_watch.c \
    kernel/v86_iocore.c \
    ...
```

変更後:
```makefile
C_V86 = $(wildcard kernel/v86/*.c)
ASM_V86 = kernel/v86/v86_entry.asm

C_KERNEL = ... \
    $(C_V86) \
    ...

ASM_KERNEL = kernel/kentry.asm kernel/setjmp.asm $(ASM_V86)
```

## リスク

- **影響範囲**: kernel.mk + 全 `#include` パス
- **V86開発中**: feat/vdm ブランチで活発に開発中。ブランチマージ時のコンフリクトリスク大
- **インクルードパス**: V86ファイル間の `#include` は相対パスのため、同一ディレクトリ内移動なら変更不要。ただし `kernel/kernel.c` 等からの参照は要確認

## 実行タイミング

**V86 開発のマイルストーン完了後** に実施することを強く推奨。
feat/vdm ブランチがマージされたタイミングが最適。
