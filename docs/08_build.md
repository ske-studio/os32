## 第8部 ビルドシステム (GCC/NASM/Make)

### §8-1 ビルド手順

```bash
# 全体ビルド (カーネルおよびプログラム, 統合環境イメージ os.d88/os.img を生成)
make all

# 掃除
make clean
```

### §8-2 ビルドパイプライン

### §8-2 ビルドパイプライン

```
boot/*.asm      →  nasm (-f bin)           →  boot/*.bin
kernel/*.asm    →  nasm (-f elf32)         →  kernel/*.o
*/*.c           →  gcc (-m32 -ffreestanding) →  */*.o
                                              ↓
*.o + kernel/*.o → ld (-T os32.ld)         → kernel.elf
kernel.elf      →  objcopy (-O binary)     → kernel.bin
                                              ↓
boot_fat.bin + loader_fat.bin + kernel.bin    → raw image
                                              ↓
mkfat12.py                                    →  os.d88 または os.img

外部プログラム (crt0.asm および libos32 連携):
programs/*.c    → gcc -m32                 → programs/*.o
                → ld -T app.ld             → programs/*.elf (newlib-nano -lc -lgcc リンク)
                → objcopy                  → programs/*.raw
                → mkos32x.py               → programs/*.bin
```

インクルードパスは `Makefile` で細かく制御されており、基本的にソースファイルから他のヘッダディレクトリは `-I` によって自動解決できるため `#include "file.h"` で問題なく参照可能。

### §8-3 ディレクトリ構造

```
os32_gcc/
├── boot/           ブートローダー (16bit/32bit ASM)
├── kernel/         カーネルコア・メモリ等・割り込みルーチン群
├── drivers/        ハードウェアドライバ (kbd, rtc, fm, fdc, disk, ide, kcg, np2sysp 等)
├── gfx/            グラフィック描画機能
├── fs/             ファイルシステム (vfs, ext2, fat12 等)
├── exec/           OS32X(外部プログラム) のロードと環境設定
├── kapi/           外部プログラム向け KernelAPI リダイレクタ
├── lib/            汎用ライブラリ (utf8, path 等)
├── include/        システム統合用共通ヘッダ群
├── programs/       外部プログラム実装ソース群
├── tools/          ホスト上でのイメージ生成ツール
├── Makefile        GCC/NASM用 マスタービルドスクリプト
└── _backup/        制作途中のソースや旧Watcom用スクリプト等の退避場所
```

### §8-4 ホスト側イメージ生成ツール

#### `tools/install_hdd.py`
ホスト側で直接 NHD HDDイメージ を構築する Python スクリプトです。カーネルやブートセクタ、ext2ファイルシステムを一括で書き込みます。

```bash
python3 tools/install_hdd.py /tmp/test_new.nhd
```
- NHDヘッダ・ジオメトリの自動生成
- IPL、パーテーションテーブル、第2ステージローダー(LBA 2-5)、カーネル(LBA 6-) の自動バイナリ配置
- シリンダ2からの ext2 ファイルシステム構築と、`programs/*.bin` 等のファイル収集・書き込み

#### `tools/write_ipl.py`
既存NHDイメージの特定LBAにバイナリを上書きするセクタ単位の書き込みツールです。カーネル部だけの高速な書き換えに使用します。

```bash
python3 tools/write_ipl.py kernel.bin --sector 6
```
**注意**: 対象サイズが指定領域に収まるか注意深く確認してください（例: LBA 272のext2スーパーブロックを破壊しないこと）。
