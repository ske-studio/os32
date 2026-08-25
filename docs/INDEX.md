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
| [05_drivers.md](05_drivers.md) | **§5** デバイスドライバ — KBD/Serial/FM/FDD/GFX/RTC/KCG/NP2SysP/libos32gfx |
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
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v39 仕様書 — 172エントリテーブル (ヘッダ2 + 関数168 + データフィールド2) + API追加手順 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 技術仕様ガイド — メモリマップ、アーキテクチャ制約、KernelAPI拡張手順 |
| [ROADMAP.md](ROADMAP.md) | リリースロードマップ (v1.0以降および履歴) |
| [NHD_FORMAT.md](NHD_FORMAT.md) | NHD r0形式ファイル構造仕様 |
| [MGX_FORMAT.md](MGX_FORMAT.md) | MGX 漫画専用グレースケール画像形式 仕様 (48Bヘッダ + パレット表 + deflate、4bpp 16階調、ホスト側エンコード専用) |
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
| §4 | KernelAPI 構造体レイアウト（ヘッダ2 + 関数150 + データ1） |
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

ハードウェアリファレンスはリポジトリ外の `C:\WATCOM\docs` (WSL: `/mnt/c/WATCOM/docs`) に配置されている:

| ドキュメント | 内容 |
|-------------|------|
| `C:\WATCOM\docs\undocumented\` | **非公開メモリ・I/Oポート資料集 (独自調査基盤、より正確)** |
| `C:\WATCOM\docs\PC9800Bible\` | PC-9800シリーズ テクニカルデータブック (公式資料ベース) |
| `C:\WATCOM\docs\D88_FORMAT_SPEC.md` | D88 フロッピーイメージ形式仕様 |
| `C:\WATCOM\docs\NP21W_DEBUG_PORT.md` | NP21/W デバッグポート資料 |

> **注意:** PC9800Bible と UNDOCUMENTED の記述が矛盾する場合は、UNDOCUMENTED の方を優先してください。

## ログ (歴史的記録)

| ドキュメント | 内容 |
|-------------|------|
| [PM_PIO_TEST.md](logs/PM_PIO_TEST.md) | プロテクトモード IDE PIO 読み込み実証実験記録 |
| [HDD_BIOS_DEBUG.md](logs/HDD_BIOS_DEBUG.md) | HDD ブート開発・デバッグログ（INT 1Bh / ディスクレイアウト） |

## タスク

| ドキュメント | 内容 |
|-------------|------|
| [tasks/boot_reform/00_OVERVIEW.md](tasks/boot_reform/00_OVERVIEW.md) | ブート刷新 (vmkernel.lz4 / ext2ローダー) — 設計 (全8部) |
| [tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) | FEP (日本語入力) 拡張 — 詳細設計 P1〜P7 の索引 (実装状況付き) |
| [tasks/fep/FEP_STATUS.md](tasks/fep/FEP_STATUS.md) | FEP — 実装状態スナップショット |
| [tasks/fep/FEP_FUTURE.md](tasks/fep/FEP_FUTURE.md) | FEP — 今後の改善・拡張タスク |
| [tasks/game/GAME_PORT_PLAN.md](tasks/game/GAME_PORT_PLAN.md) | 対戦スゴロクRPG 移植計画 (ステージA/B、Phase 0〜9) |
| [tasks/game/ENGINE_EXTENSION_PLAN.md](tasks/game/ENGINE_EXTENSION_PLAN.md) | エンジン拡張実装計画 — libos32turn / libos32rpg / libos32save 設計 |
| [tasks/wintree_port/PORT_PLAN.md](tasks/wintree_port/PORT_PLAN.md) | feat/vdm 系作業ツリーの移植計画と実施結果 |
| [tasks/v86v2/README.md](tasks/v86v2/README.md) | **V86 サブシステム (再挑戦)** — 16bit ゲスト実行。方式決定・実測・実装状況の索引 |
| [tasks/sqlite/00_INDEX.md](tasks/sqlite/00_INDEX.md) | SQLite カーネル統合 — 設計・実装ドキュメント (全7部) |
| [tasks/libmath/LIBMATH_DESIGN.md](tasks/libmath/LIBMATH_DESIGN.md) | libos32math — 整数数学ライブラリ設計書 |
| [tasks/libchem/LIBCHEM_DESIGN.md](tasks/libchem/LIBCHEM_DESIGN.md) | libos32chem — 化学エンジンライブラリ設計書 |
| [tasks/libinput/LIBINPUT_DESIGN.md](tasks/libinput/LIBINPUT_DESIGN.md) | libos32input — 入力抽象化ライブラリ設計書 |
| [tasks/libasset/LIBASSET_DESIGN.md](tasks/libasset/LIBASSET_DESIGN.md) | libos32asset — アセット・リソース管理ライブラリ設計書 |
| `tasks/libai/` `libbattle/` `libboard/` `libecon/` `libecs/` `libevent/` `libinv/` `libtext/` `tilemap/` | 各ゲームライブラリの設計書群 |
| [tasks/cross_compiler_rebuild.md](tasks/cross_compiler_rebuild.md) / [tasks/ext2_dind_debug.md](tasks/ext2_dind_debug.md) | 単発タスク記録 |

## man ページ

`docs/manpages/*.1` — ゲスト内 `man` コマンド用マニュアル (約60ページ)。
`make packages` で `/usr/man/` に配置される (`tools/package_defs.yaml`)。

## ソースツリー概要

```
os32/
├── boot/             — ブートローダ (NASM + C: boot_main.c, ext2_mini.c, lz4_mini.c)
├── kernel/           — カーネルコア (メイン処理、ページング、IDT)
├── exec/             — プログラムローダー (OS32X)
├── fs/               — ファイルシステム (VFS, ext2, FatFs, iso9660, hostdrvfs, fd_redirect, pipe_buffer)
├── drivers/          — 各種ドライバ (IDE, ATAPI, FDC, KBD, Mouse, Serial, RTC, FM, KCG, NP2SysP, loop_dev, dev など)
├── gfx/              — グラフィックス (CPU描画用バックバッファ層)
├── kapi/             — KernelAPI ラッパー実装 (自動生成分含む)
├── lib/              — 汎用ライブラリ (UTF-8, UTF-16, Path, LZ4, SQLite等)
├── include/          — 共通ヘッダ群
├── programs/         — 外部プログラム
│   ├── shell/        — システム標準シェル (モジュール構造、ファイラ・スクリプトエンジン内蔵)
│   ├── apps/         — アプリケーション (edit/, game/, ui_demo/, vdpview, mdview, vbzview, gfx_demo等)
│   ├── cmds/         — コマンドラインツール (grep, less, sort, ime等 17種)
│   ├── system/       — システムユーティリティ (hsync, install, cdinst, sndctl等)
│   ├── tests/        — テスト・デモプログラム (約45種)
│   ├── rust/         — Rust プログラム (hello_gfx, alloc_demo, math_test_rs) + os32api/os32_math クレート
│   ├── libos32/      — newlib-nano ブリッジ
│   ├── libos32math/  — 整数数学ライブラリ (固定小数点, LUT, ベクトル)
│   ├── libos32gfx/   — グラフィックスライブラリ
│   ├── libos32snd/   — サウンドライブラリ
│   ├── libos32chem/  — 化学エンジンライブラリ (SQLite連携)
│   ├── libos32map/   — マップ管理ライブラリ (SQLite連携)
│   ├── libos32input/ — 入力抽象化ライブラリ (アクションバインディング)
│   ├── libos32db/    — SQLiteデータベースアクセスライブラリ
│   ├── libos32text/  — テキスト/メッセージ管理ライブラリ
│   ├── libos32asset/ — アセット管理ライブラリ
│   ├── libos32ecs/   — ECSライブラリ
│   ├── libos32event/ — イベントシステムライブラリ
│   ├── libos32ai/ libos32battle/ libos32board/ libos32econ/ libos32inv/ — ゲームシステム各種
│   ├── libos32turn/ libos32rpg/ libos32save/ — 手番進行・キャラ育成・セーブ
│   ├── libos32tilemap/ — タイルマップ描画エンジン
│   ├── libos32filer/ — ファイラ共通ライブラリ
│   └── libos32md/    — Markdown パーサーライブラリ
├── build/            — モジュール化 Makefile 群 (config/kernel/programs/libs/deploy/image.mk) + リンカスクリプト
│   └── out/          — ビルド成果物 (kernel.bin, sqlite.bin, vmkernel.lz4, unicode.bin, kernel.elf/.map) ※gitignore
├── tools/            — ホスト側ツール (デプロイ・イメージ生成・KAPI自動生成)
├── assets/           — データアセット (各種DB, FEP辞書, profile等)
├── packages/         — 生成された .PKG
├── images/           — 生成されたブートイメージ
├── tests/            — ホスト側テストスクリプト
├── Makefile          — マスタービルドスクリプト (build/*.mk を include)
└── docs/             — 仕様書ドキュメント群 (本ファイル含む)
```

---

*Last Updated: 2026-08-06*
