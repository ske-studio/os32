# フォルダ・ファイル構造リファクタリング計画

策定日: 2026-05-23

---

## 概要

OS32 プロジェクトのフォルダ・ファイル構造を調査した結果、以下の問題を特定した。
Makefile 変更を伴う大規模リファクタリングは本ディレクトリに個別ドキュメントとして記録し、
安全な変更（.gitignore修正、ファイル削除等）とは分離して管理する。

## 問題一覧

| # | 問題 | 重要度 | ビルド影響 | ドキュメント |
|---|------|--------|-----------|------------|
| 1 | ビルド成果物のルート直下出力 | 🔴 高 | Makefile全体 | [01_BUILD_OUTPUT.md](01_BUILD_OUTPUT.md) |
| 2 | kernel/ V86ファイル肥大化 (36ファイル) | 🔴 高 | kernel.mk | [02_KERNEL_V86_SPLIT.md](02_KERNEL_V86_SPLIT.md) |
| 3 | ライブラリ命名不統一 | 🟡 中 | libs.mk + 全プログラム | [03_LIB_RENAME.md](03_LIB_RENAME.md) |
| 4 | programs/ 配下の構造整理 | 🟡 中 | programs.mk | [04_PROGRAMS_CLEANUP.md](04_PROGRAMS_CLEANUP.md) |

## 既に完了した整理

- **2026-05-23**: 実装済みドキュメント22件をアーカイブ化 (docs/_archived/, docs/tasks/_archived/)
- **2026-05-23**: GEMINI.md 新設、CLAUDE.md をポインタ化
- **2026-05-23**: docs/INDEX.md デッドリンク修正、09_exec.md APIバージョン修正
