# OS32 v3 実装タスク分割

> 状態: 計画用。v3 本体の開発開始を意味しない。
> 正典: ../PLAN.md / ../MIGRATION_AUDIT.md / ../DEBUG_AND_MODULES.md

## 目的

v3 の作業を、担当者がこのディレクトリの担当票と参照先だけで着手できる単位に分割する。
各票は並列実装可能範囲と統合ゲートを明示し、ABI/KAPI を担当ごとに独断で増殖させない。

## 共通ルール

- 現行 main の GNU89 規約を変更しない。v3 側のみで C11/GNU11 を扱う。
- PC-98/x86 v3 の最低目標は DX4・16MB、カーネル基本 ISA は 486。
- 旧 CUI バイナリ互換、既存 KAPI 番号・意味を破壊しない。
- Win32 固有型を KAPI に持ち込まない。
- TCP/IP/DNS/HTTP(S)/TLS は OS32 に実装せず Host Services に委譲する。
- 実装前提が未確定なら、独自に契約を確定せず阻害点を報告する。
- 各担当は変更ファイル、追加 ABI/KAPI 要求、試験結果、未解決事項を完了報告に含める。

## 統合順序

```text
P3 検証基盤
  ↓
P4 C11 / ABI / 旧互換固定
  ↓
P5a VM / VFS / resource / module
  ↓
P5b PE32
  ↓
P6 GUI

並列:
Audio / Host Services / OSS backend / Compatibility research / Hardware QA
```

## タスク

| ID | 文書 | 並列性 | 主な統合点 |
|---|---|---|---|
| CORE | TASK_CORE_P4_P5.md | 直列本線 | P4→P5a→P5b |
| AUDIO | TASK_AUDIO.md | 並列 | 音声 native API |
| HOST | TASK_HOST_SERVICES.md | 並列 | provider共通契約 |
| OSS | TASK_OSS_BACKENDS.md | 並列 | native abstraction |
| COMPAT | TASK_COMPATIBILITY.md | 並列 | P4/P5 APIレビュー |
| QA | TASK_QA_HARDWARE.md | 並列 | P3/P5/P6受入 |
| GUI | TASK_GUI.md | 準備並列・統合直列 | PE32受入後 P6 |
