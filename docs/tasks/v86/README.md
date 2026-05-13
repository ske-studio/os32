# V86サブシステム — 詳細設計ドキュメント

[`V86_STATUS.md`](../../V86_STATUS.md) の各章を掘り下げた個別ドキュメント群。

## ファイル一覧

| # | ファイル | 内容 |
|---|---------|------|
| 1 | [01_architecture.md](01_architecture.md) | アーキテクチャ・モジュール依存関係 |
| 2 | [02_cpu_emulation.md](02_cpu_emulation.md) | CPUエミュレーション (#GPハンドラ詳細) |
| 3 | [03_io_port_map.md](03_io_port_map.md) | I/Oポート完全マップ (パススルー/仮想化/保護) |
| 4 | [04_memory.md](04_memory.md) | メモリ管理 (バッキングRAM・ページング・IVT/BDA) |
| 5 | [05_irq_injection.md](05_irq_injection.md) | 割り込み仮想化・IRQ注入メカニズム |
| 6 | [06_bios_emulation.md](06_bios_emulation.md) | BIOSエミュレーション (INT 18h/1Bh/1Ch等) |
| 7 | [07_fdc_virtualization.md](07_fdc_virtualization.md) | FDC/DMAポートレベル仮想化 |
| 8 | [08_session_lifecycle.md](08_session_lifecycle.md) | セッション管理・ブートシーケンス |
| 9 | [09_msdos_roadmap.md](09_msdos_roadmap.md) | MS-DOS起動ロードマップ |
| 10 | [10_test_matrix.md](10_test_matrix.md) | テストマトリクス・動作確認記録 |
| 11 | [11_gaps_and_verification.md](11_gaps_and_verification.md) | 未実装・要検証項目 一覧 (MS-DOSブート観点 / A〜E カテゴリ31項目) |
| 12 | [12_debug_tools_inventory.md](12_debug_tools_inventory.md) | デバッグツール現状棚卸し (既存機能 + 弱点 W1〜W10) |
| 13 | [13_debug_tools_design.md](13_debug_tools_design.md) | デバッグツール機能設計 (Tier 1〜4 / 18機能) |
| 14 | [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) | デバッグツール実装ロードマップ (Phase 1〜4 + KAPI + 依存図) |

---

## 目的別の読み順

### 🐛 MS-DOS のハングを調査したい

1. [09_msdos_roadmap.md §9.2](09_msdos_roadmap.md) — DOS5 IPLハングの仮説と Step1/2/3
2. [11_gaps_and_verification.md A節](11_gaps_and_verification.md) — 高優先度の検証項目
3. [12_debug_tools_inventory.md](12_debug_tools_inventory.md) — 現状デバッグ機能の何が足りないか
4. [13_debug_tools_design.md Tier 1](13_debug_tools_design.md) — ハング解析に必要なツール設計
5. [14_debug_tools_roadmap.md Phase 1](14_debug_tools_roadmap.md) — 実装順序と受入条件

### 🔌 新しい I/O ポートを追加したい

1. [03_io_port_map.md](03_io_port_map.md) — 既存マップと分類 (パススルー/仮想化/保護)
2. [06_bios_emulation.md](06_bios_emulation.md) — 既存 BIOS 実装
3. [11_gaps_and_verification.md D1](11_gaps_and_verification.md) — 未分類ポートのトリアージ基準
4. [13_debug_tools_design.md T2.2](13_debug_tools_design.md) — I/O 分類タグ付き統計

### 🎮 ゲーム互換性を検証したい

1. [09_msdos_roadmap.md §9.3 Phase 4](09_msdos_roadmap.md) — ネイティブゲーム要件
2. [10_test_matrix.md](10_test_matrix.md) — 動作実績
3. [07_fdc_virtualization.md](07_fdc_virtualization.md) — コピプロ対応 (Ys等)
4. [05_irq_injection.md §5.4](05_irq_injection.md) — VSYNC 注入

### 📖 V86 サブシステムを初めて読む

1. [01_architecture.md](01_architecture.md) — 全体像
2. [04_memory.md](04_memory.md) — メモリレイアウト
3. [08_session_lifecycle.md](08_session_lifecycle.md) — ブート〜終了の流れ
4. [10_test_matrix.md](10_test_matrix.md) — 何が動いているか
5. [`/mnt/c/WATCOM/src/os32/docs/V86_STATUS.md`](../../V86_STATUS.md) — 上位サマリ

### 🛠️ デバッグツールを実装したい

1. [12_debug_tools_inventory.md](12_debug_tools_inventory.md) — 既存機能を把握
2. [13_debug_tools_design.md](13_debug_tools_design.md) — 各 Tx.x の詳細設計
3. [14_debug_tools_roadmap.md](14_debug_tools_roadmap.md) — Phase 順と依存関係
4. [`/mnt/c/WATCOM/CLAUDE.md`](../../../../CLAUDE.md) — KAPI 追加手順

### 🔍 BIOS 機能の追加 / 修正

1. [06_bios_emulation.md](06_bios_emulation.md) — 既存 INT 実装の一覧
2. [11_gaps_and_verification.md B節](11_gaps_and_verification.md) — INT 1Ah / 1Eh / 10h の評価
3. [04_memory.md §4.4](04_memory.md) — BDA レイアウト
4. [`docs/PC9800Bible/4-7_MS-DOS.md`](../../../../docs/PC9800Bible/4-7_MS-DOS.md) — 仕様参照

### 📊 PC-98 と PC/AT の差分が気になる

1. [11_gaps_and_verification.md §11.6](11_gaps_and_verification.md) — E1〜E7 差分チェックリスト
2. [03_io_port_map.md](03_io_port_map.md) — PC-98 固有 I/O アドレス
3. [04_memory.md §4.4](04_memory.md) — PC-98 固有 BDA
4. [`docs/PC9800Bible/`](../../../../docs/PC9800Bible/) — PC-98ハードウェア技術資料
