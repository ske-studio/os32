## 第1部 システム概要

### §1-1 アーキテクチャ

OS32は、PC-9801シリーズ上で動作する32ビットプロテクトモードOSである。  
GCC および NASM を用いてクロスコンパイルし、D88フロッピーディスクイメージまたはNHD HDDイメージとしてNP21/Wエミュレータ上で実行する。

| 項目 | 仕様 |
|------|------|
| ターゲットCPU | i386互換 (32ビットプロテクトモード) |
| ブートメディア | PC-98 2HD FDD (D88) / IDE HDD (NHD) |
| コンパイラ | GCC (i386-elf) / NASM |
| メモリモデル | フラットモデル (32ビット) |
| 動作環境 | NP21/W (Neko Project 21/W x64) |

### §1-2 ブートシーケンス

#### HDD ブート (プライマリ)

```
電源ON
  ↓
BIOS POST → HDD セクタ0 から IPL 読込 (1FC0:0000)
  ↓
boot_hdd.asm (16bit リアルモード) — IPL (512B)
  ├── テキストVRAMにブートメッセージ表示
  ├── INT 1Bh × 16回: LBA 2-17 → 0x8000 (8KB, ローダー)
  ├── ジオメトリ情報をレジスタに設定 (AL=DA/UA, AH=heads, BL=SPT)
  └── far jmp 0000:8000h
  ↓
loader_hdd.asm (16bit → 32bit) — 第2段階ローダー (ELF + C)
  ├── A20ゲート有効化 (ポート0xF2)
  ├── GDT設定 + CR0.PE=1 → プロテクトモード遷移
  ├── ESP = 0x9FFFC
  ├── BSS ゼロクリア
  └── call boot_main
  ↓
boot_main.c (32bit プロテクトモード) — Cメインロジック
  ├── PC-98パーティションテーブル (LBA 1) からext2パーティション特定
  ├── ext2 ミニドライバで /boot/vmkernel.lz4 を 0x10000 に読み込み
  ├── VK32ヘッダ検証 + LZ4展開 (kernel.bin→0x100000, sqlite.bin→0x200000)
  └── return 0 (ASMに復帰)
  ↓
loader_hdd.asm (32bit PM 復帰)
  ├── メモリプロービング (1MB-16MB, 512KB刻み)
  └── far jmp 0x08:0x100000 (kernel_main へジャンプ)
  ↓
kernel.c :: kernel_main(u32 mem_kb, u32 boot_drive)
  ├── tvram_clear()
  ├── idt_init() → pic_init() → pit_init(100Hz)
  ├── cpu_calibrate() (PITベースCPU速度測定)
  ├── kbd_init() / mouse_init()
  ├── ime_init() (FEP)
  ├── loop_dev_init() (ループバックブロックデバイス)
  ├── fdc_init()
  ├── dev_init() → path_init() → FS初期化 (ext2/fat(FatFs)/iso9660/hostdrv)
  ├── ide_init() → auto mount (ルート・サブマウント・HostDrv)
  ├── kmalloc_init() → paging_init() → paging_reclaim_conventional()
  ├── FPU初期化 → pgalloc_init() → shm_init()
  ├── fd_redirect_init() / pipe_buffer_init()
  ├── exec_init() (KernelAPIテーブル構築)
  ├── Unicodeテーブルロード / ime_init()
  ├── snd_init() / os32_sqlite_init()
  ├── boot_splash() (ブートスプラッシュ)
  └── exec_run(SYS_SHELL_BIN="/sys/shell.bin") (シェル起動、終了/クラッシュ時自動再起動)
      └── シェル内で /etc/profile を自動実行 (環境変数・パス初期化)
```

#### FDD ブート (セカンダリ)

```
電源ON
  ↓
BIOS POST → FDD1からIPL読込 (C0/H0/S1 → 1FC0:0000)
  ↓
boot_fat.asm (16bit リアルモード) — FAT12 IPL (1024B)
  ├── テキストVRAMにブートメッセージ表示
  ├── FAT12ルートDirから /LOADER.BIN を検索・読み込み → 0x8000
  └── far jmp 0000:8000h
  ↓
loader_fat_new.asm (16bit → 32bit) — 第2段階
  ├── FAT12から /VMKRNL.LZ4 を検索・読み込み (DMA境界対応)
  ├── A20ゲート有効化 / GDT設定 / PM遷移
  ├── VK32ヘッダ解析 + LZ4展開 (kernel.bin→0x100000, sqlite.bin→0x200000)
  ├── メモリプロービング
  └── far jmp 0x08:0x100000 (kernel_main へジャンプ)
```

---

### §1-3 レイヤー構造と垂直依存性

上位レイヤーは下位レイヤーの機能を利用するが、下位レイヤーが上位レイヤーの具体的な実装に依存すること（逆参照）は原則として禁止する。

```text
[ アプリケーション層 ] (userland/, apps/, game/)
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
*   **役割:** ハードウェア（KBD, FDC, IDE, ATAPI, RTC, Serial, KCG）の直接制御。
*   **特記事項:** `kcg.c` は `gfx/` の描画機能を利用せず、自前でピクセル描画（またはバックバッファ操作）を行う。

#### カーネルコア (kernel/)
*   **依存先:** `include/`, `drivers/` (serial, console用)
*   **役割:** `idt.c`/`isr_handlers.c` (中断処理), `paging.c` (メモリ管理), `kmalloc.c`, `console.c`

#### ファイルシステム (fs/)
*   **依存先:** `include/`, `drivers/disk.h`, `drivers/atapi.h`, `kernel/`
*   **役割:** VFS（仮想ファイルシステム）による抽象化と、FAT12/ext2/ISO9660/HostDrvFSの実装。

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
