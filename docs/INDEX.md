# OS32 GCC ドキュメント索引

PC-9801シリーズ向け 32ビット ベアメタルOS (GCC移植版)

---

## ドキュメント一覧

| ドキュメント | 内容 |
|-------------|------|
| [OS32_SPEC.md](OS32_SPEC.md) | カーネル技術仕様書（アーキテクチャ、メモリマップ、デバイスドライバ、ファイルシステム、シェル、ビルドシステム） |
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI 仕様書（外部プログラム実行基盤、呼び出し規約、APIレイアウト） |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 開発ガイドライン・ロードマップ |
| [PM_PIO_TEST.md](PM_PIO_TEST.md) | プロテクトモード IDE PIO 読み込み実証実験記録 |
| [HDD_BIOS_DEBUG.md](HDD_BIOS_DEBUG.md) | HDD ブート開発・デバッグログ（INT 1Bh / ディスクレイアウト） |
| [BENCHMARK.md](BENCHMARK.md) | ベンチマークプログラム(bench.bin) の仕様とテスト内容 |

---

## OS32_SPEC.md 目次

| 部 | 内容 |
|----|------|
| 第1部 | システム概要（アーキテクチャ、ブートシーケンス） |
| 第2部 | メモリマップ（物理メモリ配置、DMA境界制約、セグメント方式） |
| 第3部 | ディスクレイアウト（FDD仕様、セクタ配置、INT 1Bh） |
| 第4部 | 割り込みシステム（IDT、PIC、PIT） |
| 第5部 | デバイスドライバ（キーボード、FDD、FM音源、RS-232C、グラフィック等） |
| 第6部 | ファイルシステム（VFS、ext2、IDE、FAT12） |
| 第7部 | シェル（コマンド一覧、入力機能） |
| 第8部 | ビルドシステム（Makeパイプライン、ソースファイル一覧） |
| 第9部 | 外部プログラム実行 → [KAPI_SPEC.md](KAPI_SPEC.md) |
| 第10部| 既知の制約と注意事項 |

## KAPI_SPEC.md 目次

| 節 | 内容 |
|----|------|
| §1 | 概要（アドレス配置、マジックナンバー） |
| §2 | 呼び出し規約（System V ABI, cdecl等） |
| §3 | 外部プログラムのビルド手順（main配置ルール） |
| §4 | KernelAPI 構造体レイアウト |
| §5 | フォルト復帰機構 |
| §6 | 実行方式 |

---

## ソースツリー概要

```
src/os32/
├── boot/             — ブートローダ (NASM)
├── kernel/           — カーネルコア (メイン処理、ページング、IDT)
├── exec/             — プログラムローダー / KernelAPI
├── fs/               — ファイルシステム (VFS, ext2, fat12, serialfs)
├── drivers/          — 各種ドライバ (IDE, FDC, KBD, Serial, KCG, NP2SysPなど)
├── gfx/              — グラフィックス (CPU描画用バックバッファ層)
├── kapi/             — KernelAPI ラッパー実装 (自動生成分含む)
├── lib/              — 汎用ライブラリ (UTF-8, Path, kprintf等)
├── include/          — 共通ヘッダ群
├── programs/         — 外部プログラム (外部シェル、ライブラリ、各種コマンド群)
├── tools/            — ホスト側ツール (Pythonスクリプト、KAPI自動生成用JSON)
├── Makefile          — 自動ビルドスクリプト
├── docs/             — 仕様書ドキュメント群 (本ファイル含む)
└── _backup/          — 退避場所
```

---

*OS32 Documentation Index*
