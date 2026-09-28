# TASK CORE — P4 / P5a / P5b 本線

## 目的

v3 のクリティカルパスを担当する。C11/ABI固定、VM/VFS/資源管理/モジュール基盤、PE32/i386 frontend を順に成立させる。

## 必読

- ../PLAN.md §4, §4.1, §4.1.1, §5, §7
- ../MIGRATION_AUDIT.md
- ../DEBUG_AND_MODULES.md
- ../../KAPI_SPEC.md
- ../../02_memory.md
- ../../06_filesystem.md
- ../../08_build.md

## 開始条件

- v3 の分岐 SHA と作業場所が固定済み。
- P3 の結果チャネル/guest test が、異常終了・timeout・未実行を成功扱いしない。
- 対象旧バイナリの保存物とハッシュがある。

## Phase CORE-1: P4 C11/ABI固定

実施:
1. GNU89→GNU11 の差分監査。
2. 言語規格切替だけのコミットを作る。機能追加・ISA変更を混ぜない。
3. shared struct / enum / integer width / calling convention を `_Static_assert` 等で検証。
4. 486 基本経路に上位 CPU 命令が混入しないことを確認。
5. 旧 CUI バイナリを再ビルドせず起動して回帰。

完了条件:
- clean build / host tests / guest tests が通る。
- ABIレイアウトが固定される。
- 旧バイナリの出力・終了値・副作用に意図しない差がない。

## Phase CORE-2: P5a 基盤

実施対象:
- VM: reserve/commit相当の低水準確保、解放、保護属性、照会。Win32名は使わない。
- VFS: open/read/write/seek/stat/error の安定契約確認。
- resource ownership: child exit/fault/abort 時の FD/DB/SHM/module 等の回収。
- module contract: 配置、依存、ABI version、初期化、失敗時rollback、認可台帳。
- loader共通基盤: executable frontend が OS32X/PE32 を共通の配置・資源管理へ接続できる構造。

非対象:
- Win32 API実装。
- NT Object Manager。
- プリエンプション/SMP。
- 実行中コードの強制アンロード。

完了条件:
- 未認可/ABI不一致/配置衝突/依存循環をロード前に拒否。
- init失敗で資源リークしない。
- DX4・16MB目標の予算を測定。
- PE32 frontend追加のために基盤を作り直す必要がない。

## Phase CORE-3: P5b PE32/i386

必須:
- PE32/i386
- section mapping
- image base / base relocation
- import table
- export lookup
- DLL用module基本機構
- entry point
- zero-fill
- section protection

非対象:
- kernel32.dll互換
- SEH完全互換
- delay import
- SxS
- Win98 DLL群

受入:
外部 Win32 DLL を要求しない自己完結テスト PE を用意し、
1. relocation
2. import/export
3. 複数section
4. BSS/zero-fill
5. R/W/X保護
6. entry→正常終了
を検証する。

## 他担当への公開境界

CORE担当だけが共通 ABI/KAPI の凍結変更を統合する。
Audio/Host/GUI/Compat から追加要求が来た場合、Win32/OSS/provider固有型を漏らさず原語化できるか確認する。

## 完了報告

- commit SHA
- 変更したABI/KAPI
- 互換試験一覧
- 486 ISA確認方法
- DX4/16MB計測
- PE32 fixture一覧
- 残課題
