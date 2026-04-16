# OS32 ドキュメント索引

PC-9801シリーズ向け 32ビット ベアメタルOS

---

## カーネル技術仕様書 (§1-§10)

| ファイル | 内容 |
|---------|------|
| [01_system.md](01_system.md) | **§1** システム概要 — アーキテクチャ、ブートシーケンス、レイヤー構造 |
| [02_memory.md](02_memory.md) | **§2** メモリマップ — 物理メモリ配置、DMA制約、ガードページ |
| [03_disk.md](03_disk.md) | **§3** ディスクレイアウト — FDD/HDD仕様、セクタ配置、INT 1Bh |
| [04_interrupts.md](04_interrupts.md) | **§4** 割り込みシステム — IDT/PIC/PIT |
| [05_drivers.md](05_drivers.md) | **§5** デバイスドライバ — KBD/Serial/FM/FDD/GFX/libos32gfx |
| [06_filesystem.md](06_filesystem.md) | **§6** ファイルシステム — VFS/ext2/IDE/FDリダイレクト/パイプ |
| [07_shell.md](07_shell.md) | **§7** シェル — コマンド一覧、入力機能、スクリプトエンジン |
| [08_build.md](08_build.md) | **§8** ビルドシステム — パイプライン、ディレクトリ構造、デプロイツール |
| [09_exec.md](09_exec.md) | **§9** 外部プログラム実行 — OS32X/exec、ネスト実行、ステータスコード |
| [10_notes.md](10_notes.md) | **§10** 既知の制約と注意事項 |

## API・ガイド・ポリシー

| ファイル | 内容 |
|---------|------|
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v26 仕様書 — 118エントリテーブル (関数117 + データフィールド1) |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 開発ガイドライン — コーディングルール、リファクタリング制約、ロードマップ |
| [GIT_POLICY.md](GIT_POLICY.md) | AIとの協調開発における公式Git運用ポリシー |
| [ROADMAP.md](ROADMAP.md) | リリースロードマップ — v0.1 Alpha 〜 v1.0 Stable |
| [NHD_FORMAT.md](NHD_FORMAT.md) | NHD r0形式ファイル構造仕様 |
| [BENCHMARK.md](BENCHMARK.md) | ベンチマークプログラム(bench.bin) の仕様とテスト内容 |

## KAPI_SPEC.md 目次

| 節 | 内容 |
|----|------|
| §1 | 概要（アドレス配置、マジックナンバー） |
| §2 | 呼び出し規約（System V ABI, cdecl等） |
| §3 | 外部プログラムのビルド手順（main配置ルール） |
| §4 | KernelAPI 構造体レイアウト（データフィールド + 117関数） |
| §4-1 | グラフィクスAPI補足（libos32gfx移行について） |
| §4-2 | ラスタパレット (gfx_present_raster) |
| §4-3 | FDリダイレクト・パイプAPI |
| §4-4 | ページング問い合わせAPI |

## ハードウェア技術資料 (外部リファレンス)

| ドキュメント | 内容 |
|-------------|------|
| [UNDOCUMENTED 9801/9821 Vol.2](../../../../docs/undocumented/INDEX.md) | **非公開メモリ・I/Oポート資料集 (独自調査基盤、より正確)** |
| [PC-9800 テクニカルマニュアル](../../../../docs/PC9800Bible/INDEX.md) | PC-9800シリーズ テクニカルデータブック (公式資料ベース) |

> **注意:** PC9800Bible と UNDOCUMENTED の記述が矛盾する場合は、UNDOCUMENTED の方を優先してください。

## ログ (歴史的記録)

| ドキュメント | 内容 |
|-------------|------|
| [PM_PIO_TEST.md](logs/PM_PIO_TEST.md) | プロテクトモード IDE PIO 読み込み実証実験記録 |
| [HDD_BIOS_DEBUG.md](logs/HDD_BIOS_DEBUG.md) | HDD ブート開発・デバッグログ（INT 1Bh / ディスクレイアウト） |

## タスク (進行中)

| ドキュメント | 内容 |
|-------------|------|
| [tasks/fep/](tasks/fep/) | FEP (日本語入力) 実装タスク |

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
├── tests/            — テストスクリプト
├── tools/            — ホスト側ツール (Pythonスクリプト、KAPI自動生成用JSON)
├── Makefile          — 自動ビルドスクリプト
└── docs/             — 仕様書ドキュメント群 (本ファイル含む)
```

---

*Last Updated: 2026-04-16*
