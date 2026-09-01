# OS32 標準アプリケーション

PC-9801 向け 32bit OS [OS32](https://github.com/ske-studio/os32) の標準アプリ集。

OS のソースツリーには依存しない。ビルドに必要なのは OS32 SDK だけで、
サードパーティが書くアプリとまったく同じ形になっている。

## ビルド

OS32 リポジトリ側で SDK を作る:

```
cd /path/to/os32
make sdk            # build/sdk/ に staging される
```

こちら側でビルドする:

```
make OS32_SDK=/path/to/os32/build/sdk        # 全 12 本
make OS32_SDK=/path/to/os32/build/sdk edit   # 個別
```

配布 tarball (`os32-sdk-<版>.tar.gz`) を展開したものを指してもよい。
`CROSS_DIR` はクロスコンパイラの位置 (既定 `/usr/local/cross`)。

## 収録アプリ

| | |
|---|---|
| `edit/` | VZ 風テキストエディタ (最大構成、21 ソース) |
| `mdview/` | Markdown ビューア (libos32md + ファイラ) |
| `mgxview/` | MGX 漫画ビューア (可変 bpp・先読みキャッシュ) |
| `vbzview/` | VBZ ベクタ画像ビューア |
| `vdpview/` | VDP 画像ビューア |
| `ekakiuta/` | えかきうたアニメーション |
| `raster/` | ラスタパレットによる擬似多色表示 |
| `gfx_demo/` | スプライトと背景退避のデモ |
| `demo1/` | VDP/スプライト表示デモ |
| `spr_test/` | スプライト描画テスト |
| `ui_demo/` | microUI (イミディエイトモード GUI) デモ |
| `hello32/` | 最小デモ (16 色の帯を 5 秒表示)。配置経路の疎通確認にも使う |

## アプリを追加する

1. ディレクトリを掘って `.c` を置く (ディレクトリ名 = バイナリ名)
2. `app.conf` に 1 行足す (最低 API バージョンとヒープサイズ)
3. 既定 (`-los32gfx -los32math`) 以外の SDK ライブラリが要るなら
   `Makefile` の `LIBS_<名前>` を書く
4. 配備するなら `deploy.yaml` / `package_defs.yaml` にも足す

`main()` はソース中の最初の関数にすること。crt0 がファイル先頭へ
ジャンプするため、補助関数は `main()` の後ろに前方宣言付きで置く。

## 配備

`deploy.yaml` と `package_defs.yaml` がこの層の配備定義。OS32 側の
`build/core.yaml` / `userland/deploy.yaml` / `game/deploy.yaml` と
マージされて 1 つのディスクイメージになる。
