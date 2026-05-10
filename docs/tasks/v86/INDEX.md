# V86 サブシステム ドキュメント (旧 VDM)

> **注記 (2026-05)**: VDOS (Virtual DOS Machine) 方針は撤回されました。
> OS32 の V86 サブシステムは DOS API エミュレータを持たず、
> ゲスト OS (FreeDOS 等) が DOS API を処理する設計です。
> 名称は `v86` に統一されました。
> 詳細は `docs/tasks/v86_integration/README.md` を参照してください。

OS32上でリアルモードソフトウェア (PC-98ネイティブゲーム等) をV86モードで実行するサブシステムの設計・実装ドキュメント。

## ドキュメント一覧

| ファイル | 内容 |
|----------|------|
| [implementation_plan.md](implementation_plan.md) | 実装計画書 v5 — メモリ/IO/INT/BDA/ロードマップ |
| [phase2_3_research.md](phase2_3_research.md) | Phase 2残り / Phase 3 技術調査レポート — INT 1Bh, タイマ, KB, PIT, 画面リストア |
| [fdkernel_build.md](fdkernel_build.md) | FreeDOS(98) カーネル ビルド手順書 — WSL+OpenWatcomハイブリッド環境 |
| [freedos98_init_flow.md](freedos98_init_flow.md) | FreeDOS(98) カーネル初期化フローと依存リソース分析 |
| [V86_DISK_ISSUE.MD](V86_DISK_ISSUE.MD) | V86 FDD バグ修正計画書 |

## 関連ドキュメント

| ファイル | 内容 |
|----------|------|
| [v86_integration/README.md](../v86_integration/README.md) | V86統合リファクタリング — フェーズ構成・方針 |
| [PC9800Bible §4-3](../../../../docs/PC9800Bible/4-3_I_Oマップ.md) | I/Oポートマップ |
| [PC9800Bible §4-5](../../../../docs/PC9800Bible/4-5_割り込みベクタ.md) | 割り込みベクタ一覧 |
| [PC9800Bible §2-6](../../../../docs/PC9800Bible/2-6_テキスト.md) | テキストVRAM / GDC / BIOS |
| [PC9800Bible §2-7](../../../../docs/PC9800Bible/2-7_グラフィック.md) | グラフィックVRAM / パレット |
| [UNDOCUMENTED INDEX](../../../../docs/undocumented/INDEX.md) | 非公開I/O・メモリ仕様 |
