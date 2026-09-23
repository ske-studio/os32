# TASK_MEMMAP_V3 — カーネル帯の切り直し (v3 のメモリマップ見直し)

> 発行: PM (Claude Code `claude-fable-5-1`、2026-09-23) / 状態: **設計 v1 (レビュー前)**。
> 出所: ユーザー指示 2026-09-23「カーネル予算はシュリンクではなく考え直す。順に実行」。1 (KHEAP 320 → 192KB) は着地済み (aa536e9)、
> ここは 2 (ページ表を画像の外へ) と 4 (帯の切り直し) をまとめた票。3 (動的読み込み) は [`PLAN.md`](PLAN.md) §3。
> 正典の関係: 番地の正典は `include/memmap.h`、地図は `docs/02_memory.md` §2-1 (生成)、経緯は [`../memory/TASK_KSTACK_USER.md`](../memory/TASK_KSTACK_USER.md)。

## 0. いまの数字 (2026-09-23)

| 帯 | 範囲 | 中身 | 余り |
|---|---|---|---|
| カーネル帯 | 0x100000〜0x1FFFFF (1MB) | 本体 461.8KB (予算 596KB) → KHEAP 192KB → KAPI 4KB → SHM 224KB (末尾 64KB は GUI 予約) → 予約 | **134KB** (本体の伸び代) |
| SQLite 帯 | 0x200000〜0x2FFFFF (1MB) | SQLite 752KB → 代替スタック 128KB → 予約 (0x2DD000〜) → DMA プール 64KB (0x2E8000) → 予約 12KB → ガード → カーネルスタック 16KB (0x2FC000) | SQLite の伸び代 44KB |
| シェル帯 / shlib 帯 | 0x300000〜 / 0x400000〜 | 常駐シェル (2 ヒープ) / 共有ライブラリ | — |

本体 461.8KB の内訳 (オブジェクト合計): kernel/ 193KB (うち **`paging.o` の静的ページ表 48KB** = pd_raw 8KB + pt_raw 36KB + page_tables 4KB、
alignment の捨て 8KB を含む)、fs/ 94KB (FAT12 が `fat12.o` 18KB + `fatfs/ff.o` 13KB の 2 系統)、drivers/ 60KB、exec/ 35KB、net/ 31KB、kapi/ 28KB、gfx/ 13KB。

## 1. 目的

- 本体の伸び代を「削って作る」のではなく「配置で作る」。v3 で載る予定のもの: PCM ドライバ (TASK_PCM_CS4231、数 KB + プール 32KB)、
  82557 (L-B、数 KB + プール 16KB)、§5-5 P2 の合成器 (**動的読み込みの前提**、本体には入れない)。
- 浮動番地 (`__bss_end` 由来の KHEAP_BASE 以降の連鎖) を減らし、固定番地で STATIC_ASSERT できる範囲を広げる。
- DMA に使うメモリ (ページ表・DMA プール・PCM のリング) は**固定番地・全 PD 共有** (PDE 0〜) に置く。

## 2. 案 (レビューで決める)

### 2-1. ページ表を画像の外の固定番地へ (+48KB)

`paging_init` が使う PD 1 枚 + ブート PT 8 枚 (= 36KB) を `.bss` から出し、**固定番地の帯** `MEM_PT_BASE` (4KB 整列) に置く。
候補: (a) SQLite 帯の予約域 0x2DF000〜0x2E7FFF (36KB。SQLite の伸び代が 8KB になる) / (b) 2-2 でカーネル帯を広げた後の末尾。
**(a) は 2-2 と一緒でなければ採らない** (SQLite の伸び代 8KB は薄すぎる)。`page_tables[]` の 4KB は sparse なポインタ配列なので画像に残す。
ページングを張る前 (PG=0) に物理番地でゼロ埋めして使う。`tools/gen_memmap.py` の MIRRORS と `os32.ld` の ASSERT を足す。

### 2-2. カーネル帯を 2MB に (SQLite を上へ)

| 帯 | 新しい範囲 | 中身 |
|---|---|---|
| カーネル帯 | 0x100000〜0x2FFFFF (2MB) | 本体 (予算 ≒ 1.5MB) → KHEAP 192KB → KAPI → SHM 224KB → 予約 → **ページ表 36KB (固定)** → **DMA プール 64KB (固定)** → ガード → カーネルスタック 16KB (固定、末尾) |
| SQLite 帯 | 0x300000〜0x3FFFFF | SQLite 752KB → 代替スタック 128KB → 予約 |
| シェル帯 | 0x400000〜0x4FFFFF | 常駐シェル |
| shlib 帯 | 0x500000〜0x5FFFFF | 共有ライブラリ |
| プログラム空間 | 0x600000〜 | 外部プログラム (`sdk/link/app.ld`、`crt0`、`RING3_*` の定数が動く) |

**影響**: `include/memmap.h` の全帯、`build/os32.ld` / `sdk/link/app.ld` / `sdk/link/shell.ld` の番地、`sdk/crt/crt0.asm`、`kernel/paging.h` の PDE
割り当て (アプリ帯の PDE、共有 PDE の表)、`exec/exec.c` の `RING3_*`、`docs/02_memory.md` の生成器、`tools/tests/test_memmap_*`、
**SDK の定数 (`GUI_SHM_OFFSET` は不変、`MEM_SHLIB_BASE` 相当は変わる) → `make external` で全アプリの再ビルド**、ホットデプロイの窓、
V86 の `v86_mem.c` (低位だけなので影響小)、ブートローダの kernel 配置 (0x100000 のまま)。**大きい**ので、2-1 と分けて段階を切る。

### 2-3. 小さい整理 (どちらの案でも)

- `mem` / `heap` コマンドの地図の文言 (kstack 0x1FC000 表記、DMA プール無し) を `memmap.h` の定数から出す。
- FAT12 の 2 系統 (`fs/fat12.c` と `fs/fatfs/`) のどちらかを外す (31KB のうち 13〜18KB)。**別票** (ブート FS の互換確認が要る)。
- PCM のステージング 16KB を DMA プールから KHEAP へ (KHEAP の実測 peak 64KB に対して 192KB あるので余裕)。TASK_PCM_CS4231 §2-1 の暫定を解く。

## 3. 段取り

1. **この票のレビュー** (Codex、案の選択: 2-1 だけ先に (a) で行くか、2-2 と一緒か)。
2. 2-1 (+48KB) を先に、2-2 は v3 の §1 (C11 移行) の前に。どちらも `make check` の地図 + NP21/W の起動 + kselftest の MM 検査 + 実機の FD 起動が受入。

## 4. しないこと

本体のコードを削ること (診断文字列・kselftest の圧縮は今回の対象外)、SHM の GUI 予約の移動 (SDK 定数)、物理メモリ 16MB 超の扱い。
