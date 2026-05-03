# VDM (Virtual DOS Machine) ドキュメント

OS32上でDOSアプリケーションをV86モードで実行するサブシステムの設計・実装ドキュメント。

## ドキュメント一覧

| ファイル | 内容 |
|----------|------|
| [implementation_plan.md](implementation_plan.md) | 実装計画書 v5 — メモリ/IO/INT/BDA/ロードマップ |
| [v86_poc_report.md](v86_poc_report.md) | Phase 0 完了レポート — PoC + カーネル統合 |
| [v86_phase1_2_report.md](v86_phase1_2_report.md) | Phase 1 & 2 中間レポート — メモリ空間, BIOS実装とバグ対応録 |
| [phase2_3_research.md](phase2_3_research.md) | Phase 2残り / Phase 3 技術調査レポート — INT 1Bh, タイマ, KB, PIT, 画面リストア |
| [phase2_freedos_boot.md](phase2_freedos_boot.md) | Phase 2 FreeDOS(98) ブート進捗 — IPL到達/ハング原因/デバッグ戦略 |
| [fdkernel_build.md](fdkernel_build.md) | FreeDOS(98) カーネル ビルド手順書 — WSL+OpenWatcomハイブリッド環境 |

## 関連ドキュメント

| ファイル | 内容 |
|----------|------|
| [PC9800Bible §4-3](../../../../docs/PC9800Bible/4-3_I_Oマップ.md) | I/Oポートマップ |
| [PC9800Bible §4-5](../../../../docs/PC9800Bible/4-5_割り込みベクタ.md) | 割り込みベクタ一覧 |
| [PC9800Bible §2-6](../../../../docs/PC9800Bible/2-6_テキスト.md) | テキストVRAM / GDC / BIOS |
| [PC9800Bible §2-7](../../../../docs/PC9800Bible/2-7_グラフィック.md) | グラフィックVRAM / パレット |
| [UNDOCUMENTED INDEX](../../../../docs/undocumented/INDEX.md) | 非公開I/O・メモリ仕様 |
