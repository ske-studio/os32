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
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v38 仕様書 — 概要・呼び出し規約・変更履歴 |
| [KAPI_TABLE.md](KAPI_TABLE.md) | KernelAPI v38 関数テーブル — 全162エントリ |
| [KAPI_NOTES.md](KAPI_NOTES.md) | KernelAPI v38 補足ノート — 機能グループ別詳細説明 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 技術仕様ガイド — メモリマップ、アーキテクチャ制約、KernelAPI拡張手順 |
| [BOOT_ARCHITECTURE.md](BOOT_ARCHITECTURE.md) | ブートアーキテクチャ — VK32フォーマット、メモリマップ、HDD/FDDローダー、デプロイ |
| [V86_USERLAND_MIGRATION.md](V86_USERLAND_MIGRATION.md) | V86 ユーザー空間化 移行プラン — カーネル/ユーザー切り分け思想・段階的移行ロードマップ |
| [SQLITE_INTEGRATION.md](SQLITE_INTEGRATION.md) | SQLite カーネル統合 — アーキテクチャ、ビルド設定、VFS、IPC、障害サマリ |
| [CROSS_COMPILER_REBUILD.md](CROSS_COMPILER_REBUILD.md) | i386-elf クロスコンパイラ soft-float 再構築手順 (見送り・参照資料) |
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

## KAPI 仕様書 構成

| ファイル | 内容 |
|----|------|
| [KAPI_SPEC.md](KAPI_SPEC.md) | §1 概要、§2 呼び出し規約、§3 ビルド手順、§4 ヘッダ、§5 変更履歴 |
| [KAPI_TABLE.md](KAPI_TABLE.md) | 全162 API関数のオフセット・プロトタイプ (機能グループ別) |
| [KAPI_NOTES.md](KAPI_NOTES.md) | §5-1〜§5-12 機能グループ別補足ノート (GFX/パイプ/マウス/V86/DB/Loop等) |

## ハードウェア技術資料 (外部リファレンス)

| ドキュメント | 内容 |
|-------------|------|
| [UNDOCUMENTED 9801/9821 Vol.2](../../../../docs/undocumented/INDEX.md) | **非公開メモリ・I/Oポート資料集 (独自調査基盤、より正確)** |
| [PC-9800 テクニカルマニュアル](../../../../docs/PC9800Bible/INDEX.md) | PC-9800シリーズ テクニカルデータブック (公式資料ベース) |

> **注意:** PC9800Bible と UNDOCUMENTED の記述が矛盾する場合は、UNDOCUMENTED の方を優先してください。

## 参考ソースコード (DOS/BIOS リファレンス)

V86 サブシステムの DOS 互換性実装で参照するソースコード:

| ソース | パス | 内容 |
|--------|------|------|
| **NP21/W** | [`src/np21w-src-main/`](../../../../src/np21w-src-main/) | PC-9801 エミュレータ。BIOS ROM、I/O ポートエミュレーション |
| **MS-DOS 4.0** | [`src/MS-DOS/v4.0/`](../../../../src/MS-DOS/v4.0/) | Microsoft 公式 MS-DOS 4.0 ソース。IO.SYS/MSDOS.SYS 初期化 |
| **FreeDOS(98) カーネル** | [`src/fdkernel/`](../../../../src/fdkernel/) | FreeDOS PC-98 対応。NEC98.txt に固有変更点 |
| **FreeDOS(98) COMMAND.COM** | [`src/freecom_dbcs2/`](../../../../src/freecom_dbcs2/) | DBCS 対応コマンドインタプリタ |

> 📖 各ソースのディレクトリ構造・モジュール概要・逆引きトピックマップは
> [リファレンスソース概要索引](../../../../docs/reference_sources/INDEX.md) を参照。

## ログ (歴史的記録)

| ドキュメント | 内容 |
|-------------|------|
| [PM_PIO_TEST.md](logs/PM_PIO_TEST.md) | プロテクトモード IDE PIO 読み込み実証実験記録 |
| [HDD_BIOS_DEBUG.md](logs/HDD_BIOS_DEBUG.md) | HDD ブート開発・デバッグログ（INT 1Bh / ディスクレイアウト） |

## タスク・設計書

### アクティブ開発

| ドキュメント | 内容 |
|-------------|------|
| [tasks/v86_integration/README.md](tasks/v86_integration/README.md) | **V86統合** — V86サブシステム統合リファクタリング |
| [tasks/v86/INDEX.md](tasks/v86/INDEX.md) | **V86** — V86サブシステム 設計・実装ドキュメント (旧VDOS) |
| [tasks/fep/FEP_STATUS.md](tasks/fep/FEP_STATUS.md) | FEP (日本語入力) — 実装状態スナップショット |
| [tasks/fep/FEP_FUTURE.md](tasks/fep/FEP_FUTURE.md) | FEP — 今後の改善・拡張タスク |

### ユーザー空間ライブラリ設計書

| ドキュメント | 内容 |
|-------------|------|
| [tasks/libs/INDEX.md](tasks/libs/INDEX.md) | **ライブラリ設計書索引** — 全13ライブラリの設計書一覧 |

### アーカイブ (完了済み実装の元ドキュメント)

| ドキュメント | 集約先 |
|-------------|-------|
| [tasks/_archived/boot_reform/](tasks/_archived/boot_reform/) | → [BOOT_ARCHITECTURE.md](BOOT_ARCHITECTURE.md) |
| [tasks/_archived/sqlite/](tasks/_archived/sqlite/) | → [SQLITE_INTEGRATION.md](SQLITE_INTEGRATION.md) |

## ソースツリー概要

```
src/os32/
├── boot/             — ブートローダ (NASM)
├── kernel/           — カーネルコア (メイン処理、ページング、IDT、V86)
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
│   ├── libos32chem/  — 化学エンジンライブラリ (SQLite連携)
│   ├── libos32map/   — マップ管理ライブラリ (SQLite連携)
│   ├── libos32input/ — 入力抽象化ライブラリ (アクションバインディング)
│   ├── libos32db/    — SQLiteデータベースアクセスライブラリ
│   ├── libtilemap/   — タイルマップ描画エンジン
│   └── libpyxel/     — Pyxel互換ゲームエンジン (廃止方向・参考実装)
├── tools/            — ホスト側ツール (Pythonスクリプト、KAPI自動生成用JSON)
├── Makefile          — 自動ビルドスクリプト
└── docs/             — 仕様書ドキュメント群 (本ファイル含む)
```

---

*Last Updated: 2026-05-11*
