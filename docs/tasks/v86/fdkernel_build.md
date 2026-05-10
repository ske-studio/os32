# FreeDOS(98) カーネル ビルド手順書

> **注記 (2026-05)**: VDOS (Virtual DOS Machine) 方針は撤回されました。
> OS32 の V86 サブシステムは DOS API エミュレータを持たず、
> ゲスト OS (FreeDOS 等) が DOS API を処理する設計です。
> 名称は `v86` に統一されました。
> 詳細は `docs/tasks/v86_integration/README.md` を参照してください。

OS32 V86サブシステムで使用する FreeDOS(98) カーネルのビルド環境と手順。

---

## 1. 概要

FreeDOS(98) は FreeDOS の PC-9801 移植版。
lpproj/fdkernel リポジトリの `nec98test` ブランチをベースとし、
デバッグ用のシリアルログ出力等を追加してカスタムビルドする。

| 項目 | 値 |
|------|-----|
| リポジトリ | `https://github.com/lpproj/fdkernel` |
| ブランチ | `nec98test` |
| ローカルパス | `tools/fdkernel/` |
| ビルド対象 | `tools/fdkernel/nec98/` (NEC98固有ソース) |
| 出力 | `tools/fdkernel/nec98/bin/kernel.sys` (≒90KB) |
| コンパイラ | OpenWatcom V2 (wcc 16bit) — Windows版 `binnt64` |
| アセンブラ | NASM |
| ホストツール | Linux GCC (exeflat, patchobj) |

---

## 2. 前提条件

- WSL2 環境
- OpenWatcom V2 が `C:\WATCOM` にインストール済み (`binnt64/wcc.exe` 等)
- NASM がWSLにインストール済み
- Linux GCC がWSLにインストール済み

---

## 3. ビルド方法

### 3.1 ワンコマンドビルド

```bash
bash tools/fdkernel/nec98/build98.sh debug
```

`debug` 引数を付けると `-DDEBUG` フラグが有効になり、カーネル内部の
デバッグprintf出力が有効になる。

### 3.2 ビルドの仕組み (ハイブリッド方式)

64bit Windows 上の WSL では 16bit DOS EXE を実行できないため、
ビルドを3段階に分割するハイブリッド方式を採用した。

```
Step 1: ホストツール (Linux GCC)
  └── exeflat, patchobj をLinuxネイティブでコンパイル

Step 2: カーネル本体 (cmd.exe + OpenWatcom)
  ├── DRIVERS: floppy.obj, rw98clk.obj → device.lib
  └── KERNEL: *.c/*.asm → wcc/nasm → wlink → kernel.exe

Step 3: 後処理 (Linux exeflat)
  └── exeflat kernel.exe kernel.sys 0x60 → フラットバイナリ
```

### 3.3 個別リビルド

カーネルソースのみ修正した場合、kernel ディレクトリだけリビルドできる:

```bash
# kernel.exe をクリーンして再ビルド
cmd.exe /C "set WATCOM=C:\WATCOM& set PATH=C:\WATCOM\binnt64;%PATH%& \
  set INCLUDE=C:\WATCOM\h& set COMPILER=owwin& set XCPU=86& set XFAT=16& \
  set XNASM=nasm& set XLINK=wlink& set ALLCFLAGS=-DDEBUG -wcd303& \
  cd /D C:\WATCOM\src\os32\tools\fdkernel\nec98\kernel& \
  wmake -ms -h -e -f makefile.wc kernel.exe"

# exeflat で変換
cd tools/fdkernel/nec98/kernel
../utils/exeflat kernel.exe kernel.sys 0x60 -S0x10 -S0x78 -S0x79
cp kernel.sys ../bin/
```

---

## 4. ディレクトリ構成

```
tools/fdkernel/              ← lpproj/fdkernel クローン (nec98test ブランチ)
├── hdr/                     共通ヘッダ (portab.h, exe.h 等)
├── kernel/                  共通カーネルソース (IBM-PC)
├── utils/                   共通ツールソース (exeflat.c 等)
├── mkfiles/                 共通ビルド設定
├── nec98/                   ★ NEC98固有 (ビルドはここで行う)
│   ├── build98.sh           ハイブリッドビルドスクリプト
│   ├── build98.bat          cmd.exe用バッチ (参考用)
│   ├── config.mak           wmake用設定
│   ├── platform.mak         NEC98プラットフォーム定義
│   ├── makefile              トップレベルmakefile
│   ├── mkfiles/             NEC98用 .mak (owwin.mak 等)
│   ├── kernel/              NEC98固有カーネルソース
│   │   ├── main.c           カーネルエントリポイント
│   │   ├── config.c         CONFIG.SYS処理
│   │   ├── dsk.c            ディスクドライバ
│   │   ├── chario.c         文字デバイス
│   │   ├── initdisk.c       ディスク初期化
│   │   ├── initclk.c        クロック初期化
│   │   ├── prf.c            printf実装
│   │   ├── entry.asm        エントリアセンブリ
│   │   ├── kernel.asm       カーネルアセンブリ
│   │   ├── console.asm      コンソールドライバ
│   │   ├── io.asm           I/Oルーチン
│   │   └── makefile.wc      カーネルmakefile
│   ├── drivers/             FDDドライバ (floppy.asm)
│   ├── boot/                ブートセクタ
│   ├── sys/                 SYSコマンド (スキップ)
│   ├── utils/               ビルドツール (exeflat等)
│   ├── lib/                 ライブラリ
│   └── bin/                 ★ ビルド成果物
│       ├── kernel.sys       カーネルバイナリ
│       ├── KWC8616.sys      カーネル (名称付き)
│       ├── KWC8616.map      リンカマップ
│       ├── config.sys       サンプルCONFIG.SYS
│       └── autoexec.bat     サンプルAUTOEXEC.BAT
└── docs/                    ドキュメント
```

---

## 5. ビルド環境の制約と対処

| 制約 | 原因 | 対処 |
|------|------|------|
| DOS 16bit EXE の実行不可 | 64bit Windows (WSL) | exeflat をLinux GCCでリビルド |
| `sys.com` ビルドスキップ | bin2c.exe が16bit DOS EXE | SYSステップをスキップ (カーネルビルドには不要) |
| W303 警告がエラー扱い | `-we` フラグ + nec98条件コンパイル | `-wcd303` で個別抑制 |
| wmake 環境変数伝搬 | GNU make構文との混在 | 環境変数を `set` で直接設定 |
| owcc が Windows GUI ライブラリを要求 | owwin.mak の CLT 定義 | CLT を `wcl` (DOS CLI) に変更 |

---

## 6. カスタマイズ: デバッグコード挿入

FreeDOS(98) カーネルに独自のデバッグログを追加するには、
以下のファイルを編集する:

### 6.1 シリアル出力によるトレース

PC-98のCOMポート (INT 19h / ポート30h) 経由でデバッグ文字列を出力する。
NP21/W のシリアルウィンドウで確認可能。

```c
/* nec98/kernel/main.c や initoem.c に追加する例 */
static void dbg_serial_char(char c)
{
    /* PC-98 COM1 (8251A): データポート=30h, ステータス=32h */
    while (!(inportb(0x32) & 0x01))  /* TxRDY待ち */
        ;
    outportb(0x30, c);
}

static void dbg_serial_str(const char FAR *s)
{
    while (*s)
        dbg_serial_char(*s++);
}
```

### 6.2 INT 29h 経由出力

FreeDOS の `printf()` は内部で INT 29h (fast console output) を使用する。
`-DDEBUG` ビルドではカーネル内部の `printf()` が有効になり、
V86環境ではOS32の INT 29h ハンドラが TVRAM に出力する。

### 6.3 推奨トレースポイント

| ファイル | 関数 | 用途 |
|----------|------|------|
| `nec98/kernel/main.c` | `init_kernel()` | カーネル初期化の最初期 |
| `nec98/kernel/initoem.c` | `init_oem()` | OEM固有初期化 |
| `nec98/kernel/initdisk.c` | `InitDisk()` | ディスクドライバ初期化 |
| `nec98/kernel/config.c` | `DoConfig()` | CONFIG.SYS 処理 |
| `kernel/dosfns.c` | 各INT 21hハンドラ | DOSファンクション呼び出し |

---

## 7. V86環境への組み込み

ビルドした `kernel.sys` をV86環境で使用するには:

1. `kernel.sys` を OS32 の FDD イメージまたは HostDrv 経由で配置
2. `v86_boot_freedos()` で FDD イメージ内の kernel.sys をロード
3. IPL が kernel.sys を読み込んで実行

### FDDイメージへの埋め込み

既存の `tools/freedos98/fd98_2hd.img` (1.2MB 2HD) のkernel.sysを
カスタムビルド版で置換する:

```bash
# FDDイメージのFAT12構造を解析してkernel.sysのオフセットを特定
# (要: mtools や自作ツール)
```

> [!NOTE]
> 現時点では FDD イメージ内の kernel.sys 置換ツールは未実装。
> 次のステップとして開発予定。

---

## 8. 参考情報

- [lpproj/fdkernel README](https://github.com/lpproj/fdkernel)
- [FreeDOS(98) 配布ページ](https://github.com/lpproj/fdkernel/tree/nec98test)
- OpenWatcom V2: `C:\WATCOM\binnt64\wcc.exe` (Version 2.0 beta May 17 2025)
