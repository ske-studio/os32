# OS32 ドキュメント索引

PC-9801シリーズ向け 32ビット ベアメタルOS

---

## 情報単位ごとの正典 (更新先は 1 つ)

同じ事実を 2 か所で独立に更新する構造は必ず食い違う (2026-09-05 の診断で 6 件)。
**変わりやすい数値・手順・進捗は下表の正典だけを更新し、他の文書は要約と参照に留める。**

| 情報単位 | 正典 (ここだけ更新) | 参照側 (要約 + リンクのみ) |
|---|---|---|
| 制約規則 [C/HW/ABI/V/D] | [CONSTRAINTS.md](CONSTRAINTS.md) | CLAUDE.md / SOUL.md (ID 参照、`make check` が照合) |
| 番地・帯域 | `include/memmap.h` (定義) → [02_memory.md §2-1](02_memory.md) (説明) | CLAUDE.md「Memory Layout」(要約図) |
| KAPI の一覧・オフセット・版 | `sdk/kapi.json` → [KAPI_SPEC.md §4](KAPI_SPEC.md) | README / CLAUDE.md の版番号 (`make check` が照合) |
| KAPI 追加手順 | [KAPI_SPEC.md §3-1](KAPI_SPEC.md) | CLAUDE.md (AI 入口の写し。両方直す) |
| KAPI 版番号・エラー番号の予約 (未実装の先取り調停) | [KAPI_SPEC.md §3-2](KAPI_SPEC.md) | 各計画 (GUI TASK_K1、network LINK_PLAN) は参照 |
| エラーコード | `os32_kapi_shared.h` の `OS32_ERR_*` | 各 FS は境界で翻訳 |
| ビルドターゲット・ツール | [08_build.md](08_build.md) | CLAUDE.md「Build Commands」(日常分のみ) |
| 配備 3 経路の使い分け | CLAUDE.md「Deploy Workflow」 | [POLICY_DEV.md §4](POLICY_DEV.md) (表のみ)、[08 §8-4](08_build.md) (ツール) |
| ディレクトリ木 | [08_build.md §8-3](08_build.md) | CLAUDE.md / INDEX は参照のみ |
| ファイル → 役割 → 仕様の対応 | [DEVELOPMENT.md §2](DEVELOPMENT.md) | — |
| 作業別の参照先 | [DEVELOPMENT.md §1](DEVELOPMENT.md) | — |
| 実行モデル (ローダ、ネスト、リング3、資源回収、exec_run の分割壁) | [09_exec.md](09_exec.md) | [10 §10-9](10_notes.md)、`tasks/v2/` (設計経緯) |
| 描画方式 (ページフリップ、200 ライン) | [05_drivers.md §5-5](05_drivers.md) | CLAUDE.md「Graphics」(1 行) |
| 落とし穴の経緯・検証記録 | [POLICY_DEBUG.md §4](POLICY_DEBUG.md) | CLAUDE.md「Known Gotchas」(2〜3 行の注意 + §番号) |
| コーディング規約 (C89、kstring、三層定数、asm) | [POLICY_DEV.md §2](POLICY_DEV.md) | CONSTRAINTS [C1]〜[C4] (規則行) |
| 進捗 | 領域別索引 ([tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) の表、[tasks/v86v2/04](tasks/v86v2/04_implementation_status.md)、[tasks/gui/TASKS.md](tasks/gui/TASKS.md) のゲート) | [ROADMAP.md](ROADMAP.md) (計画)、[CHANGELOG.md](../CHANGELOG.md) (履歴) |
| プログラムの一覧 | 各層の `deploy.yaml` (機械可読の正典)、コマンドは [07_shell.md §7-1](07_shell.md) | 09_exec / INDEX に表を持たない |
| LAN の設計・進捗 | ドライバ = [tasks/network/PLAN.md](tasks/network/PLAN.md)、リンク層と Host Services = [tasks/network/LINK_PLAN.md](tasks/network/LINK_PLAN.md) | 05_drivers / DEVELOPMENT は要約 + リンク |
| 現行 / 未実装 / 過去 の区別 | 各文書の冒頭に「現行仕様」「計画」「YYYY-MM-DD 時点のスナップショット」を明記 | — |

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
| [CONSTRAINTS.md](CONSTRAINTS.md) | **プロジェクト制約の正典** — C/ABI・ハードウェア・KernelAPI・検証・破壊的操作。CLAUDE.md と SOUL.md は ここの規則行を ID で参照する (`make check` が照合) |
| [POLICY_DEV.md](POLICY_DEV.md) | **開発ポリシー** — コーディング規約、ビルド/デプロイ、Gitコミット、テスト、リリース |
| [POLICY_DEBUG.md](POLICY_DEBUG.md) | **デバッグポリシー** — 仮説駆動デバッグ、バイナリ反映確認、教訓集、AI協調ルール |
| [KAPI_SPEC.md](KAPI_SPEC.md) | KernelAPI v40 仕様書 — 175エントリテーブル (ヘッダ2 + 関数171 + データフィールド2) + API追加手順 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | **開発案内** — 作業別の参照先 (読む / 触る / 検証) と、ファイル → 役割 → 仕様のファイル地図。仕様本文は持たない |
| [ROADMAP.md](ROADMAP.md) | リリースロードマップ (v1.0以降および履歴) |
| [archive/](archive/) | 完了済みの計画書 (ROADMAP_v1.0, REFACTORING_PLAN) — 当時の記録 |
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

## ハードウェア技術資料 (外部リファレンス)

ハードウェアリファレンスはリポジトリ外の `C:\WATCOM\docs` (WSL: `/mnt/c/WATCOM/docs`) に配置されている:

| ドキュメント | 内容 |
|-------------|------|
| `C:\WATCOM\docs\undocumented\` | **非公開メモリ・I/Oポート資料集 (独自調査基盤、より正確)** |
| `C:\WATCOM\docs\PC9800Bible\` | PC-9800シリーズ テクニカルデータブック (公式資料ベース) |
| `docs/hw/` (git 管理外) | 上記と UNDOCUMENTED の Markdown をローカルにミラーしたもの。`tools/sync_hwdocs.sh` で更新。著作権物なので git に入れない |
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
| [tasks/network/PLAN.md](tasks/network/PLAN.md) | LGY-98 / NE2000 **ドライバ**計画 — NASM PIO、OS32 IRQ 統合、リング管理・復旧、段階別検証（M1〜M3 はエミュレータ合格、進捗 §9） |
| [tasks/network/LINK_PLAN.md](tasks/network/LINK_PLAN.md) | OS32 **リンクプロトコル / Host Services** 計画 — 独自 raw Ethernet、絶対値 WINDOW フロー制御、DATA ストリーミング、HTTP/File/RPC を KAPI で公開（未実装） |
| [tasks/boot_reform/00_OVERVIEW.md](tasks/boot_reform/00_OVERVIEW.md) | ブート刷新 (vmkernel.lz4 / ext2ローダー) — 設計 (全8部) |
| [tasks/v2/PLAN.md](tasks/v2/PLAN.md) | **v2 カーネルアーキテクチャ計画** — リング3 / Rust 適用範囲 / KAPI 呼び出し実測 / get_tick 根絶。GUI は別トラック |
| [tasks/v2/CONTRACTS.md](tasks/v2/CONTRACTS.md) | v2 実装の凍結インターフェース契約 (2 コーダー体制、C1-C7) |
| [tasks/v2/TASK_coder1_M0b_privileged.md](tasks/v2/TASK_coder1_M0b_privileged.md) | コーダー1 タスク — ユーザランドの特権命令除去 (M1 前提) |
| [tasks/v2/TASK_coder1_M1_ring3.md](tasks/v2/TASK_coder1_M1_ring3.md) | コーダー1 タスク — M1 リング3 土台の実装 (M1a-M1e) |
| [tasks/v2/TASK_coder2_libos32gui.md](tasks/v2/TASK_coder2_libos32gui.md) | コーダー2 タスク — libos32gui (Rust GUI) 新規開発 (試作。v1.1 では tasks/gui/ の票で WM と共有ライブラリに分割) |
| [tasks/gui/DESIGN.md](tasks/gui/DESIGN.md) | **GUI シェル v1.x 設計記録** (2026-09-04) — 再描画モデル、HAL/バックエンド表、9821 PEGC / Cirrus、ボトルネック、API 様式、共有ライブラリ帯域 |
| [tasks/gui/API_CONTRACTS.md](tasks/gui/API_CONTRACTS.md) | libos32gui 凍結インターフェース契約 (2026-09-04 凍結) — G 描画 / T 経路 / U 窓とイベント / P 性能規約 |
| [tasks/gui/TASKS.md](tasks/gui/TASKS.md) | GUI v1.1 作業分担票 (2026-09-05) — レーン H/K/W/C、依存順、排他、共有定数、ゲート G1〜G5。各票 `TASK_{H1..H3,K1..K4,W1..W2,C1..C3}.md` |
| [tasks/v2/M1_RING3.md](tasks/v2/M1_RING3.md) | v2 M1 設計 — リング3 土台 (GDT/PD 切替/CPL=3 遷移/検証項目) |
| [tasks/v2/M2_KAPI_TRAMPOLINE.md](tasks/v2/M2_KAPI_TRAMPOLINE.md) | v2 M2 設計 — KAPI トランポリン (CPL=3 から int 0x80 経由、アプリ無変更) |
| [tasks/v2/M3_VERIFY.md](tasks/v2/M3_VERIFY.md) | v2 M3 設計 — 検証 (フォールト注入/特権命令の静的検査/性能再測) |
| [tasks/fep/00_INDEX.md](tasks/fep/00_INDEX.md) | FEP (日本語入力) 拡張 — 詳細設計 P1〜P7 の索引 (実装状況付き) |
| [tasks/fep/FEP_STATUS.md](tasks/fep/FEP_STATUS.md) | FEP — アーキテクチャ説明 (2026-04-27 時点のスナップショット。進捗は 00_INDEX の表) |
| [tasks/fep/FEP_FUTURE.md](tasks/fep/FEP_FUTURE.md) | FEP — 今後の改善・拡張タスク |
| `os32-game:docs/game/GAME_PORT_PLAN.md` | 対戦スゴロクRPG 移植計画 (別リポジトリ ske-studio/os32-game) |
| `os32-game:docs/game/ENGINE_EXTENSION_PLAN.md` | エンジン拡張計画 (別リポジトリ)。userland/lib/save は本体に残る |
| [tasks/wintree_port/PORT_PLAN.md](tasks/wintree_port/PORT_PLAN.md) | feat/vdm 系作業ツリーの移植計画と実施結果 |
| [tasks/v86v2/README.md](tasks/v86v2/README.md) | **V86 サブシステム (再挑戦)** — 16bit ゲスト実行。方式決定・実測・実装状況の索引 |
| [tasks/sqlite/00_INDEX.md](tasks/sqlite/00_INDEX.md) | SQLite カーネル統合 — 設計・実装ドキュメント (全7部) |
| [tasks/libmath/LIBMATH_DESIGN.md](tasks/libmath/LIBMATH_DESIGN.md) | libos32math — 整数数学ライブラリ設計書 |
| `os32-game:docs/libchem/LIBCHEM_DESIGN.md` | 化学エンジンライブラリ設計 (別リポジトリ) |
| [tasks/libinput/LIBINPUT_DESIGN.md](tasks/libinput/LIBINPUT_DESIGN.md) | libos32input — 入力抽象化ライブラリ設計書 |
| [tasks/libasset/LIBASSET_DESIGN.md](tasks/libasset/LIBASSET_DESIGN.md) | libos32asset — アセット・リソース管理ライブラリ設計書 |
| `tasks/libai/` `libbattle/` `libboard/` `libecon/` `libecs/` `libevent/` `libinv/` `libtext/` `tilemap/` | 各ゲームライブラリの設計書群 |
| [tasks/cross_compiler_rebuild.md](tasks/cross_compiler_rebuild.md) / [tasks/ext2_dind_debug.md](tasks/ext2_dind_debug.md) | 単発タスク記録 |

## man ページ

`docs/manpages/*.1` — ゲスト内 `man` コマンド用マニュアル (約60ページ)。
`make packages` で `/usr/man/` に配置される (`tools/package_defs.yaml`)。

## ソースツリー概要

[08_build.md §8-3](08_build.md) を参照 (複製しない。CLAUDE.md「Source Tree」も同じ表)。
