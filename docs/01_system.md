## 第1部 システム概要

### §1-1 アーキテクチャ

OS32は、PC-9801シリーズ上で動作する32ビットプロテクトモードOSである。  
GCC および NASM を用いてクロスコンパイルし、D88フロッピーディスクイメージとしてNP21/Wエミュレータ上で実行する。

| 項目 | 仕様 |
|------|------|
| ターゲットCPU | i386互換 (32ビットプロテクトモード) |
| ブートメディア | PC-98 2HD 1MBフロッピー (D88形式) |
| コンパイラ | GCC (i686-elf) / NASM |
| メモリモデル | フラットモデル (32ビット) |
| 動作環境 | NP21/W (Neko Project 21/W x64) |

### §1-2 ブートシーケンス

```
電源ON
  ↓
BIOS POST → FDD1からIPL読込 (C0/H0/S1 → 0000:7C00h)
  ↓
boot.asm (16bit リアルモード) — IPL (第1段階)
  ├── テキストVRAMにブートメッセージ表示
  ├── INT 1Bh × 4回: C0-C1 → 0x8000-0xFBFF (31セクタ)
  └── far jmp 0000:8000h
  ↓
loader.asm (16bit リアルモード) — 第2段階
  ├── INT 1Bh ループ: C2-C15 → 0x10000-0x47FFF (DMA境界対応)
  ├── A20ゲート有効化 (ポート0xF2)
  ├── GDT設定 (コード/データ/フラット)
  ├── CR0.PE = 1 → プロテクトモード遷移
  └── far jmp 0x08:pm32_entry
  ↓
pm32.asm (32bit プロテクトモード)
  ├── DS/ES/SS = 0x10 (データセグメント)
  ├── ESP = 0x9FFFC (スタック)
  └── call _kernel_main
  ↓
kernel.c :: kernel_main(u32 mem_kb, u32 boot_drive)
  ├── tvram_clear()
  ├── idt_init() → pic_init() → pit_init(100Hz)
  ├── kbd_init()
  ├── fdc_init()
  ├── dev_init() → path_init() → FS初期化 (fat12/ext2/serialfs)
  ├── ide_init() → auto mount (ルート・サブマウント)
  ├── kmalloc_init() / paging_init() / shm_init()
  ├── fd_redirect_init() / pipe_buffer_init()
  ├── exec_init() (KernelAPIテーブル構築)
  ├── Unicodeテーブルロード / ime_init()
  ├── boot_splash() (ブートスプラッシュ)
  └── exec_run("shell.bin") (シェル起動、終了/クラッシュ時自動再起動)
      └── シェル内で /etc/profile を自動実行 (環境変数・パス初期化)
```

---

### §1-3 レイヤー構造と垂直依存性

上位レイヤーは下位レイヤーの機能を利用するが、下位レイヤーが上位レイヤーの具体的な実装に依存すること（逆参照）は原則として禁止する。

```text
[ アプリケーション層 ] (programs/)
        |
        v
[ API・システムコール層 ] (kapi/) <--- (kapi.json から自動生成)
        |
        +-----------------------+-----------------------+
        |                       |                       |
[ ファイルシステム層 ] (fs/)  [ グラフィックス層 ] (gfx/)  [ 実行制御 ] (exec/)
        |                       |                       |
        +-----------+-----------+-----------+-----------+
                    |           |           |
              [ カーネルコア層 ] (kernel/)
                    |           |
              [ デバイスドライバ層 ] (drivers/)
                    |           |
              [ 共通定義・I/O層 ] (include/, lib/)
```

### §1-4 モジュール別詳細依存関係

#### 共通基盤 (include/, lib/)
*   **依存先:** なし
*   **役割:** 基本型定義 (`types.h`)、I/Oポート操作マクロ (`io.h`)、定数定義。

#### デバイスドライバ (drivers/)
*   **依存先:** `include/`, `kernel/` (kprintf/kmalloc)
*   **役割:** ハードウェア（KBD, FDC, IDE, RTC, Serial, KCG）の直接制御。
*   **特記事項:** `kcg.c` は `gfx/` の描画機能を利用せず、自前でピクセル描画（またはバックバッファ操作）を行う。

#### カーネルコア (kernel/)
*   **依存先:** `include/`, `drivers/` (serial, console用)
*   **役割:** `idt.c`/`isr_handlers.c` (中断処理), `paging.c` (メモリ管理), `kmalloc.c`, `console.c`

#### ファイルシステム (fs/)
*   **依存先:** `include/`, `drivers/disk.h`, `kernel/`
*   **役割:** VFS（仮想ファイルシステム）による抽象化と、FAT12/ext2/serialfsの実装。

#### グラフィックス (gfx/)
*   **依存先:** `include/`, `drivers/kcg.h`, `kernel/`
*   **役割:** バックバッファ管理、VRAM転送、スプライト・図形描画。高速化のためアセンブラルーチンを利用。

#### プログラムローダー (exec/)
*   **依存先:** `fs/`, `kernel/paging.h`, `kapi/`
*   **役割:** 外部実行ファイル（.EXE/OS32X）のロード、仮想メモリ展開、実行。ヒープ管理（`exec_heap.c`）。

#### KernelAPI (kapi/)
*   **依存先:** `kernel/`, `fs/`, `gfx/`, `exec/`, `drivers/`
*   **役割:** カーネル機能を外部プログラムに公開するためのゲートウェイ。`kapi_generated.c` を自動生成。

---
