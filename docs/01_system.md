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
  ├── ESP = 0x9FFFF (スタック)
  └── call _kernel_main
  ↓
kernel.c :: kernel_main(u32 mem_kb, u32 boot_drive)
  ├── tvram_clear()
  ├── idt_init() → pic_init() → pit_init(100Hz)
  ├── kbd_init()
  ├── fdc_init() 
  ├── dev_init() → path_init() → FS初期化 (fat12/ext2/serialfs)
  ├── ide_init() → auto mount (ルート・サブマウント)
  ├── kmalloc_init() / paging_init() / exec_init()
  ├── exec_run("autoexec.bin") (存在する場合)
  └── exec_run("shell.bin") (外部シェルの起動、無限ループ)
```

---

