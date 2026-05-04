# ブートアーキテクチャ

OS32 のブートプロセスと関連設計の永続的リファレンス。

> **出典**: このドキュメントは `docs/tasks/boot_reform/` (全7ステップ, 2026年完了済み)
> の設計・実装ドキュメントを集約したものです。
> 元ファイルは `docs/tasks/_archived/boot_reform/` に保存されています。

---

## §1. 概要

raw-sector方式のカーネルロードを廃止し、Linux風の圧縮カーネルイメージ
(`vmkernel.lz4`) を ext2/FAT12 ファイルシステムから読み込むブート方式を採用。

### 主要変更点

1. カーネル配置を `0x9000` → `0x100000` (1MB) に移動
2. カーネル+ヒープ+KAPI+SHMを1MB帯域内に統合
3. SQLite帯域を `0x200000` に配置
4. シェル常駐 (`0x300000`) / 外部プログラム (`0x400000`) は旧アドレス維持
5. コンベンショナルメモリをブート後にフォントキャッシュ/Unicode/GFXバッファに再利用
6. VK32ヘッダ付きLZ4圧縮カーネルイメージ (`vmkernel.lz4`)
7. HDDローダーに ext2 最小リーダー + LZ4デコーダ内蔵
8. FDDローダーも `vmkernel.lz4` 対応
9. カーネル更新を「rawセクタ書き込み」から「通常ファイルコピー」に変更

---

## §2. VK32 イメージフォーマット

`vmkernel.lz4` は VK32 ヘッダ付きの圧縮カーネルイメージ。
`kernel.bin` と `sqlite.bin` を個別にLZ4圧縮し、自己記述的なヘッダで管理する。

### ヘッダ構造 (2エントリ方式)

```
Offset  Size  Field
------  ----  -----
0x00    4     magic: 'VK32' (0x32334B56 LE)
0x04    4     header_size: 共通ヘッダ + エントリ分 = 16 + entry_count * 16
0x08    4     version: 1
0x0C    4     entry_count: 2 (kernel + sqlite)
--- entry[0]: kernel ---
0x10    4     load_addr: 0x100000 (展開先アドレス)
0x14    4     raw_size: 展開後サイズ (バイト)
0x18    4     data_offset: LZ4データ位置 (ファイル先頭からの絶対オフセット)
0x1C    4     compressed_size: LZ4圧縮データサイズ
--- entry[1]: sqlite ---
0x20    4     load_addr: 0x200000
0x24    4     raw_size
0x28    4     data_offset
0x2C    4     compressed_size
--- data ---
0x30    ...   LZ4圧縮データ[0] (kernel.bin)
...     ...   LZ4圧縮データ[1] (sqlite.bin)
```

### C言語ヘッダ定義 (`include/vk32.h`)

```c
#define VK32_MAGIC       0x32334B56UL  /* 'VK32' LE */
#define VK32_VERSION     1
#define VK32_MAX_ENTRIES 4

typedef struct {
    u32 load_addr;
    u32 raw_size;
    u32 data_offset;
    u32 compressed_size;
} VK32Entry;

typedef struct {
    u32 magic;
    u32 header_size;       /* = 16 + entry_count * 16 */
    u32 version;
    u32 entry_count;
    VK32Entry entries[VK32_MAX_ENTRIES];
} VK32Header;
```

### 生成ツール

```bash
python3 tools/mkvmkernel.py \
    --kernel kernel.bin --kernel-addr 0x100000 \
    --sqlite sqlite.bin --sqlite-addr 0x200000 \
    -o vmkernel.lz4
```

---

## §3. メモリマップ

### 全体レイアウト

```
[コンベンショナル 0x0-0xFFFFF]
  0x00000          NOT PRESENT (NULLポインタ検出)
  0x01000-0x49FFF  フォントキャッシュ (292KB) ※ブート後に配置
  0x4A000-0x69FFF  Unicode-JIS変換テーブル (128KB) ※ブート後に配置
  0x6A000-0x89FFF  GFXバックバッファ (128KB) ※ブート後に配置
  0x8A000-0x8EFFF  空き (20KB, 将来用)
  0x8F000          スタックガード (NP)
  0x90000-0x9FFFF  カーネルスタック (64KB)
  0xA0000-0xEFFFF  VRAM
  0xF0000-0xFFFFF  BIOS ROM

[カーネル帯域 0x100000-0x1FFFFF, 1MB]
  0x100000  カーネルバイナリ (.text+.data+.bss, ~200KB)
  __bss_end カーネルヒープ (320KB, __bss_end から動的配置)
  +320KB    KAPIテーブル (4KB)
  +4KB      共有メモリ (ガード付き 264KB)
  残り      空き/予約

[SQLite帯域 0x200000-0x2FFFFF, 1MB]
  0x200000  SQLite code+BSS (~579KB)
  +code末尾 SQLite代替スタック (128KB)
  残り      空き/予約

[シェル常駐 0x300000-0x3FFFFF, 1MB]  ※旧アドレス維持
  0x300000  シェル .text+.data+.bss (~113KB)
  ガード    スタックガード (NP)
  ~0x3FFFFF スタック (下向き成長)

[プログラム空間 0x400000-メモリ上限]  ※旧アドレス維持
  0x400000  外部プログラム (最大1MB)
  ガード    sbrk/スタックガードページ
  ~mem_end  プログラムスタック (256KB)
```

### カーネル帯域詳細 (0x100000-0x1FFFFF)

`__bss_end` (現在 0x131E60) からの動的配置:

```
0x100000-0x131E5F  カーネル code+data+bss (~200KB)
0x132000-0x181FFF  カーネルヒープ (320KB)
0x182000-0x182FFF  KAPIテーブル (4KB)
0x183000-0x183FFF  SHM前方ガード (NP, 4KB)
0x184000-0x1C3FFF  共有メモリ本体 (256KB)
0x1C4000-0x1C4FFF  SHM後方ガード (NP, 4KB)
0x1C5000-0x1FFFFF  空き/予約 (~236KB)
```

### 新旧アドレス対照表

| 領域 | 旧アドレス | 新アドレス | サイズ枠 |
|------|-----------|-----------|---------|
| カーネル本体 | 0x09000 | 0x100000 | 1MB |
| カーネルヒープ | 0x40000 | __bss_end動的 | 320KB |
| KAPIテーブル | 0x189000 | ヒープ直後 | 4KB |
| 共有メモリ | 0x381000 | KAPI直後 | 264KB |
| SQLite拡張域 | 0x18A000 | 0x200000 | 768KB |
| シェル常駐 | 0x300000 | **0x300000** | 1MB (変更なし) |
| プログラム空間 | 0x400000 | **0x400000** | 1MB+ (変更なし) |

---

## §4. コンベンショナルメモリ再利用

ブート完了後、不要になった以下の領域 (0x1000-0x8EFFF, 568KB) を再利用する。

| 旧用途 | アドレス | 再利用理由 |
|--------|---------|------------|
| IVT/BDA | 0x1000-0x5FFF | PM後はIDT使用、IVT不要 |
| BIOSトランポリン | 0x6000-0x7FFF | FDCは直接I/O駆動 |
| ローダー | 0x8000-0xFFFF | ブート完了後不要 |
| LZ4一時バッファ | 0x10000-0x8EFFF | ブート完了後不要 (最大508KB) |

### 初期化順序 (`kernel_main` 内)

1. IDT/PIC/PIT/KBD/FDC/IDE 初期化 (従来通り)
2. ページ0 を NOT PRESENT に設定 (NULLポインタ検出)
3. 0x1000-0x8EFFF を R/W に設定 (旧R/O領域の解除)
4. フォントキャッシュをコンベンショナルにロード
5. Unicodeテーブルをコンベンショナルにロード
6. GFXバックバッファ初期化

> **制約**: LZ4一時バッファは 508KB (0x10000-0x8EFFF)。
> `vmkernel.lz4` の圧縮後サイズはこの上限を超えてはならない。

---

## §5. HDDブートフロー

```
1. IPL (boot_hdd.asm, LBA 0, 512B)
   → ローダーを LBA 2-17 (8KB) からメモリ 0x8000 にロード
   → far jmp 0000:8000

2. Mini-Loader (0x8000, ~6KB)
   a) IPLからのジオメトリ情報保存
   b) PM移行 (A20, GDT, CR0.PE)
   c) IDE SRST + BSY待ち
   d) ext2スーパーブロック読み込み (パーティション開始LBA)
   e) ルートinode → /boot → vmkernel.lz4 のinode特定
   f) データブロックを順次PIOで 0x10000 に読み込み
   g) VK32ヘッダ解析
   h) 各エントリをLZ4展開 → entry[i].load_addr
   i) メモリプローブ
   j) ESP = 0x9FFFC
   k) far jmp 0x100000 (kentry)
```

### HDDレイアウト

```
LBA 0        IPL (512B)
LBA 1        未使用
LBA 2-17     Mini-Loader (8KB)
LBA 18-1631  未使用 (旧カーネル/SQLite領域が不要に)
LBA 1632~    ext2パーティション
               /boot/vmkernel.lz4  ← カーネル圧縮イメージ
               /bin/...
               /sys/...
```

### 新規ファイル

| ファイル | 役割 |
|---------|------|
| `boot/loader_hdd_new.asm` | HDDローダー本体 (ASMエントリ + PM移行) |
| `boot/boot_main.c` | ローダーのCメインロジック (ext2読み込み + LZ4展開) |
| `boot/ext2_mini.c` | ブートローダー用ext2最小リーダー |
| `boot/lz4_mini.c` | ブートローダー用LZ4デコーダ |
| `boot/boot_io.c` | C→ASM ブリッジ (pm_read_sector ラッパー) |
| `boot/loader.ld` | ローダー用リンカスクリプト (0x8000配置) |

---

## §6. FDDブートフロー

```
1. boot_fat.asm (セクタ0, 1024B)
   → FAT12からLOADER.BINを検索・0x8000にロード
   → far jmp 0000:8000

2. loader_fat_new (LOADER.BIN, FAT12ファイル)
   a) FAT12からVMKRNL.LZ4を検索
   b) セグメント切替しながらリアルモードで全データ読み込み
      → 0:C000h (16KB) + 1000:0000h〜 (64KB境界毎にセグメント切替)
   c) PM移行 (A20, GDT, CR0.PE)
   d) PM後: リアルモードで読んだデータを 0x10000 にコピー
   e) VK32ヘッダ解析 + LZ4展開
   f) メモリプローブ
   g) ESP = 0x9FFFC
   h) far jmp 0x100000 (kentry)
```

### FDDイメージレイアウト

```
FAT12 (PC-98 2HD, 1232セクタ × 1024B = 1.2MB)
  /LOADER.BIN      新ローダー (~4KB)
  /VMKRNL.LZ4      圧縮カーネルイメージ (~280KB)
  /sys/shell.bin    シェル
  /sys/unicode.bin  Unicodeテーブル
  /bin/...          コマンド群
  /sbin/install.bin HDDインストーラー
```

---

## §7. デプロイパイプライン

### 設計方針

- **ローダーのみ** rawセクタ書き込み (IPL + Mini-Loader = ~8.5KB)
- **カーネル** (`vmkernel.lz4`) は ext2 パーティションへのファイルコピー
- HostDrvデプロイ経由でも `vmkernel.lz4` を配置可能

### Makefile ターゲット

```makefile
# NHDカーネル書き込み (NP21/W再起動必要)
deploy-kernel: vmkernel.lz4
	$(NHD_DEPLOY) write-boot boot/loader_hdd_new.bin
	$(NHD_DEPLOY) deploy-vmkernel vmkernel.lz4
	$(NHD_DEPLOY) sync-from-hostdrv
	$(NHD_DEPLOY) deploy

# HostDrvデプロイ (再起動不要)
deploy: vmkernel.lz4 programs unicode_bin
	$(HOSTDRV_DEPLOY) sync
```

### deploy.yaml

```yaml
files:
  - src: vmkernel.lz4
    dst: /boot/vmkernel.lz4
  # 旧 kernel.bin / sqlite.bin エントリは削除済み
```

---

## §8. 関連ファイル

| ファイル | 役割 |
|---------|------|
| `include/vk32.h` | VK32ヘッダ構造体定義 |
| `include/memmap.h` | メモリアドレス定数 |
| `build/os32.ld` | カーネルリンカスクリプト (0x100000配置) |
| `build/app.ld` | 外部プログラムリンカスクリプト (0x400000, 変更なし) |
| `tools/mkvmkernel.py` | vmkernel.lz4 生成ツール |
| `tools/nhd_deploy.py` | NHDデプロイツール |
| `tools/deploy.yaml` | デプロイ設定 |

---

*集約元: docs/tasks/boot_reform/ (00_OVERVIEW - 07_DEPLOY_PIPELINE)*
*Last Updated: 2026-05-04*
