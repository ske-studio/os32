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
| [POLICY_DEV.md](POLICY_DEV.md) | **開発ポリシー** — コーディング規約、ビルド/デプロイ、Gitコミット、テスト、リリース |
| [POLICY_DEBUG.md](POLICY_DEBUG.md) | **デバッグポリシー** — 仮説駆動デバッグ、バイナリ反映確認、教訓集、AI協調ルール |
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v28 仕様書 — 142エントリテーブル (関数140 + データフィールド2) |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 技術仕様ガイド — メモリマップ、アーキテクチャ制約、KernelAPI拡張手順 |
| [ROADMAP.md](ROADMAP.md) | リリースロードマップ (v1.0以降および履歴) |
| [NHD_FORMAT.md](NHD_FORMAT.md) | NHD r0形式ファイル構造仕様 |
| [BENCHMARK.md](BENCHMARK.md) | ベンチマークプログラム(bench.bin) の仕様とテスト内容 |

## プロジェクトルート

| ファイル | 内容 |
|---------|------|
| [LICENSE](../LICENSE) | MIT License (著作者: すけさん) |
| [README.md](../README.md) | プロジェクト概要・機能一覧・クイックスタート |
| [INSTALL.md](../INSTALL.md) | インストール・ビルド手順 |
| [CHANGELOG.md](../CHANGELOG.md) | リリース変更履歴 |

## KAPI_SPEC.md 目次

| 節 | 内容 |
|----|------|
| §1 | 概要（アドレス配置、マジックナンバー） |
| §2 | 呼び出し規約（System V ABI, cdecl等） |
| §3 | 外部プログラムのビルド手順（main配置ルール） |
| §4 | KernelAPI 構造体レイアウト（関数140 + データ2） |
| §4-1 | グラフィクスAPI補足（libos32gfx移行について） |
| §4-2 | ラスタパレット (gfx_present_raster) |
| §4-3 | FDリダイレクト・パイプAPI |
| §4-4 | ページング問い合わせAPI |
| §4-5 | キー押下状態ポーリングAPI |
| §4-6 | FM/SSG個別チャンネル制御API |
| §4-7 | マウスAPI |
| §4-8 | TVRAM読取・反転API |
| §4-9 | マウスカーソル制御API |

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

## タスク

| ドキュメント | 内容 |
|-------------|------|
| [tasks/fep/FEP_STATUS.md](tasks/fep/FEP_STATUS.md) | FEP (日本語入力) — 実装状態スナップショット |
| [tasks/fep/FEP_FUTURE.md](tasks/fep/FEP_FUTURE.md) | FEP — 今後の改善・拡張タスク |
| [tasks/sqlite/00_INDEX.md](tasks/sqlite/00_INDEX.md) | SQLite カーネル統合 — 設計・実装ドキュメント (全7部) |
| [tasks/libmath/LIBMATH_DESIGN.md](tasks/libmath/LIBMATH_DESIGN.md) | libos32math — 整数数学ライブラリ設計書 |

## ソースツリー概要

```
src/os32/
├── boot/             — ブートローダ (NASM)
├── kernel/           — カーネルコア (メイン処理、ページング、IDT)
├── exec/             — プログラムローダー / KernelAPI
├── fs/               — ファイルシステム (VFS, ext2, fat12, iso9660, hostdrvfs)
├── drivers/          — 各種ドライバ (IDE, ATAPI, FDC, KBD, Mouse, Serial, KCG, NP2SysPなど)
├── gfx/              — グラフィックス (CPU描画用バックバッファ層)
├── kapi/             — KernelAPI ラッパー実装 (自動生成分含む)
├── lib/              — 汎用ライブラリ (UTF-8, UTF-16, Path, kprintf等)
├── include/          — 共通ヘッダ群
├── programs/         — 外部プログラム
│   ├── shell/        — システム標準シェル (モジュール構造)
│   ├── apps/         — アプリケーション (edit, filer, vdpview, mdview等)
│   ├── cmds/         — コマンドラインツール (grep, less, sort等)
│   ├── system/       — システムユーティリティ (hsync, install, cdinst等)
│   ├── tests/        — テスト・デモプログラム
│   ├── libos32/      — newlib-nano ブリッジ
│   ├── libos32math/  — 整数数学ライブラリ (固定小数点, LUT, ベクトル)
│   ├── libos32gfx/   — グラフィックスライブラリ
│   ├── libos32snd/   — サウンドライブラリ
│   └── libpyxel/     — Pyxel互換ゲームエンジン
├── tools/            — ホスト側ツール (Pythonスクリプト、KAPI自動生成用JSON)
├── Makefile          — 自動ビルドスクリプト
└── docs/             — 仕様書ドキュメント群 (本ファイル含む)
```

---

*Last Updated: 2026-04-27*
