## 第8部 ビルドシステム (GCC/NASM/Make)

### §8-1 ビルド手順

```bash
# 全体ビルド (カーネルおよびプログラム, 統合環境イメージ images/os32_boot.d88 等を生成)
make all

# パッケージ生成 + ISOイメージ生成 (CDインストーラ用)
make iso

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
mkfat12.py                                    →  images/os32_boot.d88 または .img

外部プログラム (crt0.asm および libos32 連携):
programs/*.c    → gcc -m32                 → programs/*.o
                → ld -T app.ld             → programs/*.elf (newlib-nano -lc -lgcc リンク)
                → objcopy                  → programs/*.raw
                → mkos32x.py               → programs/*.bin
```

インクルードパスは `Makefile` で細かく制御されており、基本的にソースファイルから他のヘッダディレクトリは `-I` によって自動解決できるため `#include "file.h"` で問題なく参照可能。

### §8-3 ディレクトリ構造

```
os32/
├── boot/           ブートローダー (16bit/32bit ASM)
├── kernel/         カーネルコア・メモリ等・割り込みルーチン群
├── drivers/        ハードウェアドライバ (kbd, rtc, fm, fdc, disk, ide, atapi, kcg, np2sysp 等)
├── gfx/            グラフィック描画機能
├── fs/             ファイルシステム (vfs, ext2, fat12, iso9660, serialfs 等)
├── exec/           OS32X(外部プログラム) のロードと環境設定
├── kapi/           外部プログラム向け KernelAPI リダイレクタ
├── lib/            汎用ライブラリ (utf8, path 等)
├── include/        システム統合用共通ヘッダ群
├── programs/       外部プログラム実装ソース群
├── tests/          テストスクリプト
├── tools/          ホスト上でのイメージ生成ツール
├── Makefile        GCC/NASM用 マスタービルドスクリプト
└── docs/           仕様書ドキュメント群
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

#### `tools/nhd_deploy.py`
NHD HDDイメージの管理をホスト側で一元化するデプロイメントツール。`deploy.yaml` に基づき、カーネル・ローダー・全ファイルを一括デプロイする。

```bash
python3 tools/nhd_deploy.py init   # NP21/WからNHDをコピーし初期化
python3 tools/nhd_deploy.py sync   # deploy.yaml に基づくフルデプロイ
python3 tools/nhd_deploy.py deploy # NHDイメージをNP21/Wにコピー
python3 tools/nhd_deploy.py copy programs/shell.bin  # 個別ファイルのデプロイ
```
- `tools/deploy.yaml` でデプロイ対象・ゲストパス・タグを定義
- ext2ファイルシステムへの書き込みはLinux loopデバイス経由
- `config.h` の `SYS_*` 定数と `deploy.yaml` のパスは必ず整合させること

#### `tools/mkpkg.py`
OS32パッケージ (.PKG) を生成するビルダー。複数ファイルをLZSS圧縮し、hash-chainで連結したパッケージを生成する。CDインストーラ (`cdinst.bin`) と連携し、ISOイメージ経由でのプログラム配布に使用される。

```bash
python3 tools/mkpkg.py -o packages/ -c packages.conf
```
- `make packages` ターゲットで自動実行
- `make iso` で `genisoimage` を使用しISOイメージを生成

### §8-5 開発環境の構築 (クロスコンパイラ)

OS32 の外部プログラムをビルドするためには、標準Cライブラリ (`newlib` - `libc.a`) と GCCライブラリ (`libgcc.a`) を含んだ `i386-elf` クロスコンパイラ環境が必要です。

#### Ubuntu / Debian系での簡易構築
もっとも手軽な方法は、ディストリビューション標準のパッケージを使用することです。
```bash
sudo apt update
sudo apt install gcc-i686-elf binutils-i686-elf
```
※ ディストリビューションによってはパッケージとして用意されていない場合があります。その場合はソースからビルドします。

#### ソースからの構築 (推奨手順)
特定のバージョン（例: GCC 13.2.0）を利用したい場合や、確実なCライブラリ環境を構築する場合は、以下の手順でクロスツールチェーンを構築します。
1. `binutils` を取得し `--target=i386-elf --disable-nls --with-sysroot` 等でビルド・インストール
2. `gcc` を取得し `--target=i386-elf --disable-nls --enable-languages=c --without-headers` でビルド (libgccの生成)
3. `newlib` を取得し `--target=i386-elf` でビルド・インストール
4. 再度 `gcc` を `--with-newlib` などを含めて完全ビルド・インストール

#### Makefile へのパス設定
環境が構築できたら、OS32のソースツリー最上位の `.env` ファイルに以下のようにクロスコンパイラのインストールパスを設定してください。
```env
# インストールしたクロスコンパイラの親ディレクトリを指定
CROSS_DIR=/home/user/opt/cross
```
OS32の `Makefile` は、ここで指定された `$CROSS_DIR/i386-elf/include` や `$CROSS_DIR/i386-elf/lib` を参照してビルドを行います。

> [!NOTE]
> **コンパイラのバージョンについて**
> PC-98ターゲットでは新しいコンパイラの最適化やABI変更による非互換リスク（およびバグ）のほうが大きいため、一度安定動作したGCCバージョンで**完全に固定化**して開発を継続するのがセオリーです。OS32では当面GCC 13.x系の利用を推奨しています。
