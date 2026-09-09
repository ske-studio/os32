# アプリ帯の可変 PDE 化 — 実装票

状態: **設計提案・実装前**。この票の作成は実装・試験合格を意味しない。
発行: PM (2026-09-10) / 担当: コーダー (Opus 5、worktree 隔離) / 検証: PM + ローカル AI

## 1. 何を解くのか

**1 アプリに渡せるメモリが、搭載 RAM に依らず約 2.5MB で頭打ち**になっている。
原因は構造であって空き RAM 不足ではない。

x86 32bit ページングでは 1 つの PDE が 4MB を覆う (PDE 番号 = 仮想アドレス >> 22)。
OS32 は **PDE 1 (`0x400000`-`0x7FFFFF`) の 1 枚だけ**をアプリごとに差し替え、
残り 1023 本は master の写しを共有している (`kernel/paging.c:561-566`)。
その 1 枚に共有ライブラリ帯 (1MB)・プログラム本体・sbrk・exec_heap・
CPL=3 スタック (256KB) が全部載る。

実測 (2026-09-10、15MB 構成、`install.bin` 相当):
`RING3_HEAP_TOP` 0x7BF000 − 本体末尾 − `MEM_EXEC_SBRK_MIN` 256KB − ガード
= **avail 2,605,056 バイト**。8MB 構成でも 15MB 構成でも同じ値になる。

**目標**: 要求量に応じてアプリ固有 PDE を 4MB 単位で増やし、空き RAM がある限り
渡す。足りなければ拒否する (`a6b2461` で入れた振る舞いを維持)。

## 2. 現行の不変条件 (壊してはいけない)

`kernel/paging.c:63-67` の 5 本の `STATIC_ASSERT` が現在の前提を固定している。

```c
STATIC_ASSERT(APP_BAND_PDE == (MEM_APP_BAND_BASE >> 22), app_band_pde_matches);
STATIC_ASSERT(APP_BAND_PDE < PAGING_PT_COUNT, app_band_pde_in_range);
STATIC_ASSERT((MEM_SHLIB_BASE >> 22) == APP_BAND_PDE, shlib_base_in_app_band);
STATIC_ASSERT((MEM_EXEC_LOAD_ADDR >> 22) == APP_BAND_PDE, exec_load_in_app_band);
STATIC_ASSERT(((MEM_APP_BAND_TOP - 1) >> 22) == APP_BAND_PDE, app_band_top_in_pde);
```

- **共有ライブラリ帯 `0x400000`-`0x4FFFFF` は先頭 PDE に居続ける**。
  `.text` は全 PD 共有・read-only、`.data`/`.bss` はアプリごと (K3/C3)。
- **`paging_map_phys()` は master の `page_tables[]` に書く**。アプリ固有 PDE の
  範囲にこれを使っても走行中のアプリからは見えない。アプリ AS への写像は
  `paging_addrspace_map_user*()` を通すこと。
- **USER は当該アプリの PD の PDE にだけ伝播**させる。master 側の PDE には
  立てない (実効権限は PDE ∧ PTE、`addrspace_map_user_page`)。
- PD / アプリ PT のバッキングは identity で読める領域から取る
  (`kernel/paging.c:488-496` の前提)。

## 3. 変更の範囲

| ファイル | 変更 |
|---|---|
| `kernel/paging.h` | `struct addrspace` に PDE 枚数と PT 物理配列を持たせる。`APP_BAND_PDE` は「先頭」の意味に変える |
| `kernel/paging.c` | `paging_addrspace_create` に枚数を渡す。`destroy` は全 PT を解放。`addrspace_map_user_page` / `_range` の判定を「先頭 ≤ pdi < 先頭+枚数」に。`STATIC_ASSERT` を新しい不変条件に書き換え |
| `exec/exec.c` | ヘッダの `heap_size` + 本体 + sbrk + スタックから必要枚数を算出し、AS 作成時に渡す。`RING3_USTACK_TOP` / `RING3_HEAP_TOP` を枚数から導く |
| `include/memmap.h` | `MEM_APP_BAND_TOP` を「既定の」上端に格下げし、最大枚数の定数を足す |
| `kernel/kselftest.c` | 複数 PDE の AS で USER 写像と権限が正しいかの項目を追加 |

## 4. 決めること (実装者が票に書き戻す)

1. **最大枚数**。上の共有帯とぶつからないこと。デバイス窓は Cirrus リニア窓
   `0x1000000` (PDE 4)、PEGC リニア窓 `0xF00000` (PDE 3 の中) にある。
   したがってアプリ帯を伸ばせるのは **PDE 1〜2 (`0x400000`-`0xBFFFFF`) が安全**で、
   PDE 3 まで伸ばすと PEGC の窓と衝突する。**まず最大 2 枚**を推奨する。
2. **PT のバッキングをどこから取るか**。枚数分の PT (4KB × 枚数) がアドレス空間ごとに
   要る。identity で読める領域から取る前提を保つこと。
3. **枚数が減る方向の再利用**。`exec_heap_restore_state()` で親へ戻るときの扱い。
4. **CPL=0 の子** (`--cpl0`) は master AS のままなので影響を受けないことの確認。

## 5. 受入条件

- ホスト TDD を先に書く (RED → GREEN)。`tools/tests/` に `*_host.c` + `test_*.py` +
  `*_tdd.md` の 3 点セット。`make check` に登録する
- 枚数 1 のときに**現行と完全に同じレイアウト**になること (回帰ゼロ)
- 枚数 2 で `heap_size` 4MB 超の要求が通ること。空き RAM が足りなければ
  `EXEC_ERR_NOMEM` で拒否されること (`a6b2461` の振る舞いを維持)
- アプリ固有 PDE の外 (共有帯) に USER が漏れないこと。master から見た実効権限が
  supervisor のまま保たれること
- ゲスト検証は PM が行う: kselftest、`emu_agent` 回帰 6/6、`ring3_guard` の
  否定試験 (guard / shlib / pegc / cirrus)、`heap_test` の overlap check、
  8MB / 15MB の両構成

## 6. やらないこと

- スワップは採らない (速度面の不利が大きすぎる。2026-09-10 ユーザー判断)
- 共有ライブラリ帯の位置は動かさない
- CPL=0 の子のレイアウトは変えない
