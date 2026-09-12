# v1.3 メモリ予算 — ホスト側測定

状態: ホストビルドの静的測定。ゲスト空き容量・ピーク使用量・断片化は未測定。
親: [PLAN.md](PLAN.md)、判定条件: [REVIEW_T0_T1.md](REVIEW_T0_T1.md) R3。

## 測定対象

`make gshell` は成功。現行ソースからRust releaseビルドと再リンクを実行した。
clean/full buildは行っていない。Cライブラリは既存のMake依存関係で使用した。
配備・エミュレータ操作は未実施。

- ELF: `userland/gshell.elf`
- ELF SHA-256: `8d247c9e3916f33bae6cebadf6181de66dd3e2579622a3eb122d65c452853a69`
- BIN SHA-256: `0c447320650e208d06c86f900764bb1f82c8793c83f13d3b56105d1d69a89409`
- linker警告: libc_a-sbrkr.oのGNU-stack note欠如、およびLOAD segmentのRWX属性。
  ビルドはexit 0だが、警告なしとは扱わない。

## 結果

`i386-elf-size` と `i386-elf-nm -n` で測定した。

| 項目 | 値 |
|---|---:|
| ELF text | 103206 B |
| ELF data | 49796 B |
| ELF bss | 17924 B |
| __bss_start | 0x3255e0 |
| __bss_end / _end | 0x329be4 |
| MEM_SHELL_GUARD | 0x375000 |
| _endからguardまでの静的差 | 308252 B |
| shell exec_heapの定義容量 | 524288 B |

帯域定義は `include/memmap.h:242-248`。リンカ配置は `sdk/link/app_sys.ld`。
308252 Bはnewlib sbrk等と競合する空間の上限差であり、端末専用の利用可能量ではない。
exec_heapは別帯域だが、524288 Bは総容量であり実行時の空きではない。

## T1案の算術評価 (採用未決)

| 項目 | 計算結果 |
|---|---:|
| stdin 4KiB + stdout/stderr各16KiB | 36864 B |
| 各8192セル × 8B × 2ストリーム | 131072 B |
| 上記合計 | 167936 B |

この合計には管理情報、アロケータヘッダ、追加コード、行索引、番兵等を含まない。
ホスト上の計算が帯域内でも、同時稼働時の確保成功は保証できない。

T4のCellはhost試験でsize 8B / align 4Bと確認（x86_64-unknown-linux-gnu）。
`userland/libos32term/tests/model.rs` の `cell_host_layout_measurement` が根拠。
上の8B仮定とは一致するが、guest実寸・Rust ABI保証・ゲスト確保成功を意味しない。

## カーネル帯の静的計上 (K6C、2026-09-12)

`kernel/con_sink.c` の console シンク。`kmalloc` せず **カーネル .bss の静的配列**なので、
シェル帯・アプリ帯・exec_heap のどれも減らさない (上の T1 案の算術とは別勘定)。

| 項目 | 値 | 出所 |
|---|---:|---|
| リング `g_ring[]` | 8192 B | `CON_SINK_RING_SIZE` (`include/con_sink.h`、票 §2-1 の決定) |
| 自己診断の作業域 `g_self_buf` + `g_self_src` | 403 B | `CON_SINK_REC_MAX` (203) + `CON_SINK_PRINT_MAX` (200) |
| head / tail / count / enabled / reader / drop_count | 24 B | `u32` × 4 + `int` × 2 |
| **合計** | **8619 B** | |

`i386-elf-gcc -O2 -c kernel/con_sink.c` の実測は text 2937 B / data 4 B / **bss 8672 B**
(整列込み。`con_sink_drop_count` は `.data`)。カーネル全体を
リンクしての実測とゲストの空き容量は未測定 (`make` は未実施)。端末アプリ側の受け皿
(K6C-A) はここには含まない。

## カーネル帯の静的計上 (K7-K、2026-09-12)

`kernel/kbd_inject.c` の注入リングと、`AppSlot` に増えた印。どちらも `kmalloc` せず
**カーネル .bss の静的領域**なので、シェル帯・アプリ帯・exec_heap のどれも減らさない。
測定は `i386-elf-gcc -O2 -c` + `i386-elf-size` / `i386-elf-nm -S` (カーネル全体のリンクと
ゲストの空き容量は未測定 — `make` は未実施)。

| 項目 | 値 | 出所 |
|---|---:|---|
| 注入リング `g_inj_ring[]` | 256 B | `KBD_INJECT_RING_SIZE` (`include/kbd_inject.h`、票 §1 メモリ) |
| head / tail / count / drop_count | 16 B | `u32` × 4 |
| 整列込みの実測 `kbd_inject.o` .bss | **288 B** | text 1489 B / data 0 B |
| 印 `parked_from_kbd` (AppSlot 1 本 4 B × 6 スロット) | 24 B | `g_slot` が 0x408 → 0x420 |
| カウンタ `ring3_kbd_park_count` | 0 B (実質) | 既存の整列の隙間に入り、`appslot.o` の .bss は 1064 → 1088 B = **+24 B** |
| **合計** | **312 B** | 288 + 24 |

票 §1 の見積り (「注入リング 256B + AppSlot の印 1 語 × 5」= 276 B) との差は 36 B:
リングの head/tail/count/drop_count の 16 B (整列で +16 B) と、印がシェル帯を含む
6 スロット分 (ID 0 と 1 も表にある) であることによる 4 B。

`kbd_inject_selftest()` は受け皿の静的配列を持たない (`inj_push` を 1 バイトずつ呼ぶ) —
`con_sink_selftest` が 403 B の作業域を持つのと違い、256 B に収めた意味を消さないため。

## カーネル帯の静的計上 (T8-K、2026-09-12)

画面の所有者 (票 T8 D1) と、`AppSlot` に増えたヘッダ flags の欄 (D1a)。どれも `kmalloc` せず
**カーネルの .data / .bss の静的領域**なので、シェル帯・アプリ帯・exec_heap のどれも減らさない。
測定は `i386-elf-gcc -O2 -c` + `i386-elf-size` / `i386-elf-nm -S` を HEAD 版と並べたもの
(カーネル全体のリンクとゲストの空き容量は未測定 — `make` は未実施)。

| 項目 | 値 | 出所 |
|---|---:|---|
| 所有者 `g_gfx_owner` | 4 B (.data) | `exec/appslot.c`。初期値 `GFX_OWNER_WM` = 1 なので .bss ではなく .data |
| カウンタ `gfx_init_reject_count` | 4 B (.bss) | 同上。KAPI にはしない (kernel.map から読む) |
| 欄 `AppSlot.hdr_flags` (4 B × 6 スロット) | 24 B | `g_slot` が 0x420 → 0x438 |
| 整列で増えた分 | 28 B | `g_slot` の先頭が 0x20 → 0x40 へ寄った (`appslot.o` .bss 1088 → **1144** = +56 B) |
| **合計 (静的)** | **60 B** | .data +4 B / .bss +56 B |

`appslot.o` の実測は text 2617 → **3355 B** (+738 B: 判定 3 本 + 回収 + 自己診断
`appslot_gfx_owner_selftest`)、data 4 → 8 B、bss 1088 → 1144 B。
`gfx/gfx_core.c` 側は門 2 本と `gfx_screen_owner` (どれも 3 行以下) で、静的領域は増えない。
KAPI スロットが 1 本増えた分 `KernelAPI` 構造体が 4 B 伸びる (192 → 193 関数)。

票 §2 の見積り (「所有者 1 語 + カウンタ 1 語」= 8 B) との差 52 B は、宣言ビットを起動時に
控える欄 (`hdr_flags` 24 B) と整列 (28 B) — 決裁 D1a を入れた分。

## カーネル帯の静的計上 (T8-3 K、2026-09-12)

ポーリング型の協調 yield (票 T8 §7 D8)。増えたのは `AppSlot` の印 1 語と、間引きの控え
1 語、カウンタ 1 語だけ。どれも `kmalloc` せず **カーネル .bss の静的領域**なので、
シェル帯・アプリ帯・exec_heap のどれも減らさない。測定は `i386-elf-gcc -O2 -c` +
`i386-elf-size` / `i386-elf-nm -S` を `feat/gui` cfd2768 版と並べたもの
(カーネル全体のリンクとゲストの空き容量は未測定 — `make` は未実施)。

| 項目 | 値 | 出所 |
|---|---:|---|
| 印 `AppSlot.parked_from_poll` (4 B × 6 スロット) | 24 B | `g_slot` が 0x438 → **0x450** |
| 間引きの控え `g_poll_last_tick` | 0 B (実質) | `static u32`。`g_slot` 手前の整列の隙間に入る |
| カウンタ `ring3_poll_yield_count` | 0 B (実質) | 同上。KAPI にはしない (kernel.map から `emu_read_mem` で読む) |
| **合計 (静的)** | **24 B** | `appslot.o` の .bss 1144 → **1168 B** |

`appslot.o` の実測は text 3474 → **4131 B** (+657 B: `appslot_park_poll_check/commit` +
`appslot_poll_yield_reset` + `resume_check` / `kill_check` / `appslot_state` の枝 +
自己診断 2 項)、data 8 B で不変。`exec/exec.c` (`exec_park_poll` + `exec_resume` の poll 分岐) と
`drivers/kbd.c` (`kbd_trygetchar` / `kbd_trygetkey` / `kbd_has_key` の GUI 分岐) は
**静的領域を 1 バイトも増やさない** — cfd2768 版と並べた実測で
`exec.o` は text 11037 → 11325 B (+288)・data 12 B・**bss 8708 B で不変**、
`kbd.o` は text 2530 → 2650 B (+120)・data 0 B・**bss 208 B で不変**。

票 §7 の見積り (「印 1 語 + tick 1 語」) との差は、印がシェル帯と ID 0 を含む 6 スロット分
あることと、控え・カウンタが既存の整列の隙間に収まったこと。KAPI は 1 本も増えていない
(v48 のまま — `kbd_trygetchar` / `kbd_trygetkey` の中身だけが変わる)。

## PM判断

- pipe案は使用時にkernel kmallocを消費する (`fs/pipe_buffer.c:30-46`) ため、無償の予約領域として採らない。
- 初期候補は親exec_heapから実行前に確保し、子実行中には確保も解放もしない方式。
  ただし動的空き・cleanup・失敗経路を検証するまで確定しない。
- セルモデルの単独ホスト試験は実行統合と分離する。モデルの必要容量を計算可能にし、
  提供容量不足をエラーで返す設計なら、ゲストの容量決定前に境界条件を検証できる。
- 起動前のバッファ確保が途中で失敗した場合は全体を巻き戻し、子を起動しない。
- ゲストで親ヒープ空き・ピーク・連続実行後の回収を測る試験は未実施。
