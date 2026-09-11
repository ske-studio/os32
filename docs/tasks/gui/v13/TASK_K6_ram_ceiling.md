# K6-RAM: 物理 RAM の上限を 16MB から引き上げる (32MB 以上、目標 128MB)

> 発行: PM (2026-09-11) / レーン: K (C89、カーネル背骨 + ブートローダ) / 前提: ユーザー決裁 (2026-09-11)
> 「RAM 上限を上げる K 票を先に (32MB 以上)」。K5b の受入 G3/G6 (4 本同時、5 本目拒否) がこれに依存する。
> 排他: `boot/loader_hdd.asm` `boot/loader_fat.asm` `kernel/physmem.{c,h}` `kernel/pgalloc.{c,h}` `kernel/paging.{c,h}`
> `kernel/memory_boot.c` `include/memmap.h` `tools/tests/` の該当ホスト試験。`exec/**` `kapi/**` は触らない (K5b の領分)。

## ゴール

プロジェクト目標「P100 / 32MB で超快適」(memory `os32-project-purpose-ceiling`) に向け、**16MB 超の物理 RAM を
検出して pgalloc の池に入れる**。まず 32MB で受入、NP21/W で 128MB まで動くことを確かめる。
アプリ帯の仮想レイアウト (`app.ld` / `MEM_*`) は動かさない — 増えるのは per-app 物理 (K5b-K) の供給源だけ。

## 現状の事実 (2026-09-11、PM が静的確認)

| # | 事実 | 根拠 |
|---|---|---|
| R1 | ローダは 512KB 刻みの書き込みプローブで `mem_kb` を数え `kernel_main(mem_kb, …)` へ渡す。上限の扱いはローダ側で確認する (16MB で止めているなら 16MB 超は永遠に見えない) | `boot/loader_fat.asm:252-290`、`boot/loader_hdd.asm:286-289` |
| R2 | `physmem_bootstrap_legacy()` は `mem_kb` を **16MB で clamp** し、注記に「16MiB 超の報告は取り込まない」 | `kernel/physmem.c:94-95`、`kernel/physmem.h:62-69` (`PHYSMEM_LEGACY_MAX_PFN = 4096`) |
| R3 | `pgalloc` の bitmap は `LEGACY_WORDS = (PHYSMEM_LEGACY_MAX_PFN+31)/32` で 16MB 分。範囲・admit も同じ定数で弾く | `kernel/pgalloc.c:11,58,71,106` |
| R4 | `paging_init()` は `PAGING_RAM_LIMIT` (= 32MB、注記「守備範囲を 32MB に広げた」) で頭打ちにして identity を張る | `kernel/paging.c:141-158` |
| R5 | 15〜16MB (`0xF00000〜0xFFFFFF`) は 9821 の PEGC リニア窓 = **RAM として使ってはいけない穴** | `include/memmap.h:280-284` (`MEM_APP_BAND_DEVICE_FLOOR = 0x00F00000`)、`include/pegc.h` |
| R6 | K5a D5: 15MB 構成の 0xC00000 以上 ≈3MB が per-app 物理で初めて使える。上限を上げれば 16MB 以上が丸ごと供給源になる | `TASK_K5_multiapp.md` §設計 D5/D10 |
| R7 | 実測 (2026-09-11): 15MB・`heap_size = 0` の既定で GUI アプリは 3 本、4 本目が `Launch failed` | `TASK_K5B_gshell.md` 受入 G3 前半 |

## 作業

1. **検出**: PC-98 の BIOS ワークエリアで 16MB 超の容量を読む (`0x594` = 16MB 超の MB 数、`0x401` = 15MB までの 128KB 単位。
   正典は `docs/hw/` の UNDOCUMENTED、矛盾は UNDOCUMENTED を採る) か、ローダのプローブを 16MB 超へ伸ばす。
   どちらを採るかはソースと `docs/hw/` で根拠を出す。NP21/W の `ExMemory` の意味 (`/home/hight/np21w-src/src/mem.c` /
   `pccore.c`) も確認し、ini の値 → ゲストが見る MB 数の対応表を票に書く。
2. **physmem**: 16MB clamp を撤廃し、RAM の範囲を **[1MB, 15MB) + [16MB, N)** として登録 (15〜16MB は `PHYSMEM_RESERVED` /
   デバイス)。`PHYSMEM_LEGACY_MAX_PFN` の意味を「legacy 経路の上限」から切り離すか、新しい上限定数 (例 `PHYSMEM_RAM_MAX_PFN`
   = 128MB) を足す。`physmem_bootstrap_legacy` / モデル経路 (`memory_boot.c`) の両方。
3. **pgalloc**: bitmap と範囲検査を新上限に。128MB = 32768 ページ = 4KB の bitmap (現 512B)。
4. **paging**: `PAGING_RAM_LIMIT` を新上限に。identity の PT 枚数 (128MB = 32 枚 = 128KB) の置き場と、
   `paging_addrspace_create_n` が PDE をコピーする範囲を確認。カーネルの識別 PDE 0 (全 PD 共有) の前提を壊さない。
5. **memmap / 予約**: `sys_usable_mem_end()` / `sys_reserve_top()` (PEGC バックバッファ、メモリ末尾の 1MB 予約は
   ユーザー方針で可) が 16MB 超でも整合すること。`v86` のバッキング (`EXEC_DYN_RESERVE` の穴、低位メモリ) は不変。
6. **ホスト試験**: `test_physmem.py` / `test_pgalloc_model.py` / `test_paging_bounds.py` / `test_memory_boot.py` に
   「32MB / 128MB の入力で範囲が正しく登録され 15〜16MB が除外される」「上限超えは切り詰めではなく拒否/切り捨てを明示」を
   RED→GREEN で。記録は `tools/tests/k6_ram_tdd.md`。`build/sdk.mk` への登録は PM。

## 受入 (ゲスト、PM/テスター)

| ID | 試験 | 合格条件 |
|---|---|---|
| M1 | NP21/W `ExMemory` を 32MB 相当にして起動 | `ver`/kselftest 通過、`sys_mem_kb` (= `kernel.map` の番地) が 32MB 相当、pgalloc の空きページが増えている |
| M2 | 同 128MB | 同上 (上限定数どおり)。それ以上は切り捨てて起動する |
| M3 | 15MB 構成の回帰 | regress 6 本、v86 -t、GUI アプリ 1 本 (K5b の G8 と同じ) が従来どおり |
| M4 | K5b の G3 | 32MB 以上で GUI アプリ 4 本が立ち、5 本目が `ERR_FULL` で拒否される (`heap_size = 0` の既定のまま) |
| M5 | 15〜16MB の穴 | 128MB でも `0xF00000〜0xFFFFFF` が RAM として配られない (pgalloc の範囲検査で証明) |

ini の変更 (`ExMemory`) は [D2]。PM がスキル `os32-emu-config` の手順で行い、値の許容表 (`np21w_ini_live.py` の
`EXMEMORY`) を 32 / 128 相当へ拡張する (別コミット)。

## この票に含めないもの

アプリ帯の仮想レイアウト変更、KAPI の追加、K5b の gshell 側。
