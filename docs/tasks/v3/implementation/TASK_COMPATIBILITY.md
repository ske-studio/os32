# TASK COMPAT — VDM / Win98互換準備

## 目的

Win98互換層そのものをv3に実装せず、COREが将来互換層を阻害しないAPI原語を持てているか検証する。
VDM/V86はPE32初期ゲートと分離して後段試験資産を準備する。

## 必読

- ../PLAN.md V3-18 / V3-22 / Q19 / Q23
- ../MIGRATION_AUDIT.md M26 / M30
- 既存 V86/VDM 関連文書とコード

## Subtask C1: Win98 requirement map

PC-98上で動作したWin95/98ユーザーアプリを対象に、API要求を以下へ分類:
- memory
- executable/module
- file/VFS
- time
- GUI/input
- audio
- synchronization/object
- network→Host Services

成果物:
Win32 API名そのものではなく、
`Win32要求 -> OS32原語 -> wrapper側責務`
の対応表。

## Subtask C2: mock compatibility layer

完全なWin32 DLLは作らない。
代表操作を呼ぶ小さなmock wrapperで、COREのAPIだけから要求を表現できるか検証。

代表例:
- fixed/relocatable memory
- file open/read/write/seek
- monotonic/wall time
- GUI event/draw
- PCM playback
- Host Services request

## Subtask C3: VDM/V86 test matrix

アリスソフトDOS世代タイトル群を基準に、タイトルごとに:
- DOS memory
- file I/O
- keyboard
- Japanese display
- FM/PCM
- mouse
- timer
- DOS extender/protected mode (必要時)
を記録する。

単なる起動可否だけにしない。

## 非対象

- Windows 2000/NT互換
- VxD/Windows driver互換
- NT Object Manager
- SMP/preemptive multitasking要求
- GPLコードのMIT本体への直接混入

## 受入

- CORE APIの不足を具体的な原語要求として列挙できる。
- Win32固有型をKAPIへ要求しない。
- VDMとPE32/Win98互換を別ゲートとして評価できる。

## 完了報告

- requirement map
- mock結果
- COREへのAPI不足票
- VDM title matrix
- license/source調査メモ
