# TASK GUI — P6準備 / GUI発展

## 目的

PE32受入後にP6を開始できるよう、GUI public API と CPU別backend の境界を先に整理する。
統合実装はP5b PE32受入後。

## 必読

- ../PLAN.md V3-05 / Q04 / Q08 / P6
- ../PLAN.md §4.1 の GUI/input境界
- 既存 libos32gui / gfx / WM 文書
- QA票のvideo regression

## 事前に並列可能な作業

- GUI public API棚卸し
- window/event/drawing contract
- bitmap/pixel format contract
- input event contract
- CPU feature detection案
- blit/convert/text/composite backend interface
- SIMD state ownership/FPU保存復元の設計
- benchmark fixture

## P6開始条件

- CORE P5b PE32受入済み。
- ABI/KAPIがP4契約を満たす。
- CPU feature detectionが486基本経路を壊さない。
- QAのvideo mode regressionが試験項目化済み。

## 実装

1. 共通GUI APIをCPU命令から独立。
2. scalar/reference backendを保持。
3. MMX/SSE/SSE2等は別ビルド単位/dispatch。
4. 非対応CPUでillegal instructionを実行しない。
5. FPU/SIMD state ownershipを明示。
6. 画面mode切替とrendererを分離。

## 非対象

- GUI都合で486 kernel基本経路のISAを引き上げる。
- PE32 loader未受入のままP6統合を開始。
- HWND/HDC等Win32型をnative APIに導入。

## 受入

- scalar/referenceとの画像差分テスト。
- feature有/無CPUで同一API。
- unsupported CPUで安全にfallback/拒否。
- Ra266 video regressionで表示崩れを検出。
- blit/text/compositeの性能計測。

## 完了報告

- public API差分
- backend構成
- CPU dispatch
- state save/restore
- benchmark
- real hardware結果
