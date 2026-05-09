# Device API CHS ネイティブ化リファクタリング

## 概要

PC-98 のブロックデバイスは全て CHS (Cylinder-Head-Sector) でアクセスする。
LBA は PC/AT 以降の抽象化であり、PC-98 のハードウェアには存在しない。

本タスクでは Device API を CHS ベースに再設計し、重複コードを統合する。

## ドキュメント構成

| ファイル | 内容 | 依存 |
|---|---|---|
| [PHASE1_LOOP_CHS.md](./PHASE1_LOOP_CHS.md) | loop_dev CHS コア + v86_disk 統合 | なし |
| [PHASE2_DEVICE_API.md](./PHASE2_DEVICE_API.md) | Device API 2系統化 (CHS/LBA) | Phase 1 |
| [PHASE3_KAPI_SHELL.md](./PHASE3_KAPI_SHELL.md) | KAPI + シェルコマンド更新 | Phase 2 |
| [PHASE4_CLEANUP.md](./PHASE4_CLEANUP.md) | 旧コード削除 + 最終検証 | Phase 3 |

## 設計原則

1. **CHS がコア、LBA はラッパー** — ハードウェア実態に合わせる
2. **CHS / LBA 2系統並立** — ATAPI CD は LBA のまま
3. **loop_dev スロットは共有** — V86 専用予約なし
4. **loop_dev_attach_fd はフルオープン** — 制限なし公開 API

## 現状の問題

```
【全デバイスで LBA→CHS 逆変換が発生】
  dd → dev_blk_read(LBA) → ide_read_sector(LBA) → ide_set_chs(LBA→CHS) → I/O
  dd → dev_blk_read(LBA) → d88_blk_read(LBA)    → LBA→CHS → seek_sector
  dd → dev_blk_read(LBA) → disk_read_lba(LBA)    → disk_lba_to_chs → fdc_read

【v86_disk.c に D88 処理が重複 (~365行)】
  loop_dev.c: d88_blk_read → 内部 seek_sector
  v86_disk.c: d88_seek_sector (同一ロジック、独自実装)
  v86_session.c: フォーマット判定 ×2箇所 (freedos + native)
```

## 変更後のアーキテクチャ

```
【CHS ネイティブ】
  caller → blk_read_chs(C,H,S) → ハードウェア(CHS)    ← ネイティブパス
  caller → dev_blk_read_lba(LBA) → LBA→CHS → blk_read_chs  ← 汎用ラッパー
  caller → blk_read(LBA) → ATAPI CD                     ← LBA ネイティブ

【D88/FDI 統合】
  loop_dev.c: CHS コア (seek_d88 + read_chs)
  v86_disk.c: loop_dev の CHS API を呼ぶだけ
  v86_session.c: loop_dev_attach_fd で統一
```

## リスク管理

| Phase | リスク | 対策 |
|---|---|---|
| 1 | v86 D88 ブート破損 | Ys + FDI 回帰テスト |
| 2 | Device 構造体 ABI 破壊 | `make clean` → `make all` 必須 |
| 3 | KAPI 互換性 | シグネチャ変更なし |
| 4 | 低 | `make all` 確認のみ |
