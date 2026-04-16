# OS32 インストールガイド

## 1. 動作環境

### エミュレータ (推奨)

- **NP21/W** (NekoProject 21/W) — PC-9801エミュレータ
  - Windows版を推奨
  - HDD (NHD) イメージからのブートに対応

### 実機

- NEC PC-9801/9821 シリーズ (i386以上のCPU搭載機)
- IDE HDD または FDD

## 2. ビルド環境の準備

WSL (Ubuntu) または Linux 環境で以下のツールが必要です。

### 必須ツール

| ツール | バージョン | 用途 |
|-------|-----------|------|
| `i386-elf-gcc` | 13.x 推奨 | i386クロスコンパイラ |
| `nasm` | 2.15+ | アセンブラ |
| `make` | GNU Make | ビルドシステム |
| `python3` | 3.8+ | ビルドツール・スクリプト |

### クロスコンパイラのインストール

```bash
# ソースからビルドする場合 (推奨)
# https://wiki.osdev.org/GCC_Cross-Compiler を参照

# またはパッケージマネージャから (ディストリビューションによる)
# Ubuntu/Debian では以下が利用可能な場合がある:
sudo apt install nasm make python3
```

※ i386-elf-gcc は通常パッケージマネージャにないため、
[osdev.org の手順](https://wiki.osdev.org/GCC_Cross-Compiler) に従ってビルドしてください。

## 3. ビルド

```bash
cd src/os32
make clean
make all
```

成功すると以下が生成されます:

| ファイル | 説明 |
|---------|------|
| `kernel.bin` | OS32カーネルバイナリ |
| `programs/*.bin` | 外部プログラム (OS32X形式) |
| `os.d88` | FDDブート用D88ディスクイメージ |
| `os.img` | FDDブート用RAWイメージ |

## 4. NP21/Wでの起動 (NHDイメージ)

### 4-1. NHDイメージの準備

```bash
# 初回セットアップ: NHDイメージの作成・フォーマット・マウント
make nhd-init

# カーネル・プログラム・データをNHDに書き込み
make deploy
```

### 4-2. NP21/Wの設定

1. NP21/Wを起動
2. メニューから **HardDisk** → **IDE #0** に生成されたNHDファイルを設定
3. **Emulate** → **Reset** でリセット
4. OS32が起動し、シェルプロンプトが表示される

### 4-3. 基本操作

```
A:> help              # コマンド一覧を表示
A:> ver               # バージョン情報を表示
A:> ls                # ファイル一覧
A:> edit memo.txt     # テキストエディタを起動
A:> cal               # カレンダーを表示
A:> man grep          # grepのマニュアルを表示
```

## 5. FDDブート (D88イメージ)

1. `make all` で生成された `os.d88` を使用
2. NP21/Wの **FDD1** に `os.d88` をセット
3. FDDからブート

※ FDDイメージは容量制約 (1,232KB) のため、
NHDに比べて搭載できるプログラムが限られます。

## 6. トラブルシューティング

### ブートしない

- NP21/Wの設定で IDE #0 に正しいNHDファイルが設定されているか確認
- `make deploy` が正常に完了しているか確認

### プログラムが動かない

- `ver` コマンドの Build タイムスタンプが最新か確認
- `make clean` → `make all` → `make deploy` で再ビルド・再デプロイ

### 文字化け

- NP21/Wのフォント設定でPC-98用フォントが選択されているか確認
