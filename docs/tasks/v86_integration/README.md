# v86 サブシステム統合・整理

## 概要

vdos (Virtual DOS Machine) として開発を始めた V86 サブシステムを、
実態に合わせて整理・統合するリファクタリングプロジェクト。

### 現状の問題点

1. **機能重複**: `v86_disk.c` (983行) が独自に FDI/D88/物理FDD の3経路を
   分岐管理しているのに対し、`loop_dev.c` (726行) も同等の CHS コア API を
   持ち、両者で機能が重複している。

2. **名前と実態の乖離**: VDOS という名前は「DOS エミュレーション」を示唆するが、
   実体は V86 のランチャーに過ぎない。DOS API はゲスト FreeDOS が処理する
   設計であり、OS32 側に DOS API エミュレータは存在しない。

3. **フォーマット未対応**: HDI (HDD イメージ) は完全未対応、
   2DD (640/720KB) FDI も不安定。

4. **コード重複**: `v86_boot_freedos()` と `v86_boot_native()` の
   イメージ判定ロジックが約200行にわたり複製されている。
   さらに freedos 側の D88 パスにレガシーコードが残存。

### 意図する成果

1. `v86_disk` を `loop_dev` の薄いブリッジに変換
   (独自ジオメトリ/状態管理を撤去)
2. `loop_dev` に HDI/RAW フォーマットを追加 (HDD イメージ起動を実現)
3. コマンド名 `vdos` → `v86`、ネイティブ PC-98 モードに一本化
4. **DOS モードを完全廃止** — `v86_boot_freedos` 撤去、KAPI 削除
5. FreeDOS 関連 (`tools/fdkernel/nec98`, `vdosquit.asm`) を `_archive` に移動

## フェーズ構成

| Phase | 内容 | ドキュメント |
|-------|------|-------------|
| 前段  | kapi.json version 同期 | [PHASE_0_PREREQ.md](PHASE_0_PREREQ.md) |
| A     | loop_dev フォーマット拡張 | [PHASE_A_LOOP_DEV.md](PHASE_A_LOOP_DEV.md) |
| B     | v86_disk の薄いブリッジ化 | [PHASE_B_V86_DISK.md](PHASE_B_V86_DISK.md) |
| C     | セッション起動 API 統合 | [PHASE_C_SESSION.md](PHASE_C_SESSION.md) |
| D     | コマンド改名 & DOS モード廃止 | [PHASE_D_RENAME.md](PHASE_D_RENAME.md) |
| E     | ドキュメント整理 & アーカイブ | [PHASE_E_DOCS.md](PHASE_E_DOCS.md) |
| F     | DOS モード完全撤去 (カーネル) | [PHASE_F_DOS_REMOVAL.md](PHASE_F_DOS_REMOVAL.md) |

## 推奨実施順序とコミット粒度

各段階で `kernel.bin` がブート可能・既存 D88 ゲームが動作することを必須条件:

```
 1. Phase 0   : kapi.json version 同期 (35 → 37)
 2. Phase A-1 : HDI サポート追加 → 既存 D88/FDI 回帰
 3. Phase A-2 : RAW フォーマット追加
 4. Phase A-3 : WRITE 経路追加
 5. Phase A-4 : 新規公開 API
 6. Phase B-1 : v86_disk 公開 API 再設計
 7. Phase B-2 : 内部状態の単純化
 8. Phase B-3 : INT 1Bh ハンドラ簡素化
 9. Phase B-4 : v86_fdc.c 改修
10. Phase B-5 : v86_boot_freedos D88 パス修正 (native と同一化)
11. Phase C-1 : v86_open_image_to_loop ヘルパ新設
12. Phase C-2 : boot_native に統合 (freedos パスは削除対象として温存)
13. Phase C-3 : KAPI 拡張 (KAPI_VERSION 37 → 38)
14. Phase D   : コマンド改名 + DOS モード廃止 (v86.c)
15. Phase F   : v86_boot_freedos 削除 + KAPI 撤去 (make clean 必須)
16. Phase E   : ドキュメント整理 + fdkernel/vdosquit アーカイブ
17. 最後      : B-1 で残した互換ラッパー削除
```

## 変更対象ファイル

### 変更

| ファイル | Phase |
|---------|-------|
| `drivers/loop_dev.c` (726行) | A |
| `drivers/loop_dev.h` | A |
| `kernel/v86_disk.c` (984行) | B |
| `kernel/v86_disk.h` | B |
| `kernel/v86_session.c` (1100行) | B-5, C |
| `kernel/v86_fdc.c` (690行) | B-4 |
| `programs/cmds/vdos.c` → `v86.c` | D |
| `tools/kapi.json` | 0, C-3, F |
| `include/os32_kapi_shared.h` | C-3, F |
| `build/app.conf` | D |

### 移動 (アーカイブ)

| 移動元 | 移動先 | Phase |
|--------|--------|-------|
| `tools/fdkernel/nec98/` | `tools/_archive/fdkernel/nec98/` | E |
| `programs/system/vdosquit.asm` | `tools/_archive/vdosquit.asm` | F |

### 削除

| ファイル | Phase |
|---------|-------|
| `kernel/v86_session.c` 内 `v86_boot_freedos()` (~245行) | F |

## 技術制約

- **C89 (GNU89) 必須**: `//` コメント禁止、変数宣言はブロック先頭のみ
- **呼び出し規約**: System V i386 ABI (KAPI 外部公開関数は `__cdecl`)
- **KAPI 構造体変更時は `make clean` → `make all`**
- **loop_dev の fd 所有権**: `v86_session_end` での close 順序は
  「**detach → close**」に統一
