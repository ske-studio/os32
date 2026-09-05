/* ======================================================================== */
/*  MEMMAP.H — OS32 システムメモリマップ定数                                 */
/*                                                                          */
/*  カーネル・ページング・プログラムローダーが参照する物理/仮想アドレスを     */
/*  一元管理する。変更時はpaging.c, kernel.c, exec.hとの整合性を確認。       */
/*                                                                          */
/*  メモリレイアウト概要 (2026-04 ブートアーキテクチャ改善):                  */
/*                                                                          */
/*    [コンベンショナルメモリ 0x00000-0xFFFFF]                               */
/*      0x00000-0x00FFF  NOT PRESENT (NULLポインタ検出)                      */
/*      0x01000-0x49FFF  フォントキャッシュ (292KB) ※ブート後に配置          */
/*      0x4A000-0x69FFF  Unicode-JIS変換テーブル (128KB) ※ブート後に配置     */
/*      0x6A000-0x89FFF  GFXバックバッファ (128KB) ※ブート後に配置          */
/*      0x8A000-0x9FFFF  空き (88KB, 将来用)                                 */
/*      0xA0000-0xEFFFF  VRAM (テキスト+グラフィック)                         */
/*      0xF0000-0xFFFFF  BIOS ROM (R/O)                                      */
/*                                                                          */
/*      ※ 0x00000-0x9FFFF は V86 セッション中まるごとゲストに明け渡す。      */
/*         V86 のリニアアドレスは CPU が (seg<<4)+off で作るので低位 1MB に   */
/*         固定されており、この窓を空けられるかどうかがゲストに 640KB を      */
/*         渡せるかどうかを決める (docs/tasks/v86v2/09_memmap.md)。           */
/*                                                                          */
/*    [カーネル帯域 0x100000-0x1FFFFF, 1MB]                                  */
/*      0x100000          カーネルバイナリ (.text+.data+.bss, ~220KB)         */
/*      __bss_end (align) カーネルヒープ (320KB)                             */
/*      +320KB            KAPIテーブル (4KB)                                  */
/*      +4KB              SHM前方ガード (NP, 4KB)                            */
/*      +4KB              共有メモリ本体 (256KB)                              */
/*      +256KB            SHM後方ガード (NP, 4KB)                            */
/*      〜0x1FAFFF        空き/予約 (NP)                                      */
/*      0x1FB000-0x1FBFFF カーネルスタックガード (NOT PRESENT)                 */
/*      0x1FC000-0x1FFFFF カーネルスタック (16KB)                             */
/*                                                                          */
/*    [SQLite帯域 0x200000-0x2FFFFF, 1MB]                                    */
/*      0x200000          SQLite code+BSS (~579KB)                            */
/*      +code末尾         SQLite代替スタック (128KB)                          */
/*      残り              空き/予約                                           */
/*                                                                          */
/*    [シェル常駐 0x300000-0x3FFFFF, 1MB]                                    */
/*      0x300000          シェル .text+.data+.bss (~113KB)                    */
/*      ガード            スタックガード (NP)                                 */
/*      ~0x3FFFFF         スタック (下向き成長)                               */
/*                                                                          */
/*    [共有ライブラリ帯域 0x400000-0x4FFFFF, 1MB]  (GUI v1.1 K3)             */
/*      0x400000          libos32gui.shlib の先頭ページ = ジャンプ表          */
/*      +text_pages       .text/.rodata (read-only + USER、全 PD 共有)        */
/*      data_vaddr        .data/.bss (アプリごとの物理ページを同じ仮想番地に) */
/*      帯域末尾          .data/.bss の原本 (ロード時に退避、複製元)          */
/*                                                                          */
/*    [プログラム空間 0x500000-メモリ上限]  ※2026-09-05 に 1MB 上へ移動      */
/*      0x500000          外部プログラム code+bss → sbrk (固定上限なし)      */
/*      ガード A          sbrk 上限ガード (exec_heap の直下、位置は動的)       */
/*      ~ヒープ上端       exec_heap (KAPI mem_alloc、スタックガード直下)     */
/*      ~mem_end          プログラムスタック (256KB)                          */
/* ======================================================================== */

#ifndef MEMMAP_H
#define MEMMAP_H

#include "types.h"

/* ====================================================================== */
/*  カーネル配置                                                            */
/* ====================================================================== */
#define MEM_1MB               0x100000UL  /* 1MB */
#define KERNEL_LOAD_ADDR      0x100000UL  /* カーネルロードアドレス (1MB) */

/* ====================================================================== */
/*  カーネルヒープ (kmalloc)                                                */
/*  __bss_end から動的算出 (4KBアライン)                                    */
/* ====================================================================== */
extern u32 __bss_end;
#define KHEAP_BASE            ((((u32)&__bss_end) + 0xFFF) & ~0xFFFUL)
#define KHEAP_SIZE            0x050000UL  /* カーネルヒープサイズ (320KB) */

/* ====================================================================== */
/*  ページング保護範囲                                                      */
/* ====================================================================== */

/* IVT/BIOSデータ領域: Read-Only (ブート後に再利用されるまで) */
/* ページ0 (0x0-0xFFF) はブート後に NOT PRESENT (NULLポインタ検出) */
#define MEM_IVT_PROT_START    0x01000UL
#define MEM_IVT_PROT_END      0x05FFFUL

/* loader.bin (使用済み): Read-Only → ブート後に再利用 
 * (注: ブートローダーが 0x804E に作成した GDT は
 *  カーネル初期化時 (gdt_init) に安全な上位メモリへ退避されます) */
#define MEM_LOADER_START      0x08000UL
#define MEM_LOADER_END        0x08FFFUL

/* ====================================================================== */
/*  カーネルスタック (カーネル帯域の末尾, 下向き成長)                        */
/*                                                                          */
/*  **低位に置いてはいけない。** V86 のゲストが見るリニアアドレスは CPU が   */
/*  (seg<<4)+off で作るので 0x00000-0x10FFEF に固定されている。カーネル      */
/*  スタックが低位のリニアを占有していると、その分だけゲストに渡せる         */
/*  コンベンショナルメモリが減る。しかも PC-98 は 128KB 単位でしか申告       */
/*  できないので、中途半端に空けても切り捨てで 512KB のままになる。          */
/*  0x00000-0x9FFFF を丸ごと空けて初めて 640KB を渡せる。                    */
/*                                                                          */
/*  サイズは実測にもとづく。ブート直後からシェル起動までの最深到達点は       */
/*  3,068 バイト (0x9F400) で、その後は誰もこのスタックを使わない            */
/*  (GDT はリング 0 のみ = 割り込みで特権遷移が起きない / exec_run() は      */
/*   子ごとに ESP を張り替える / V86 の #GP フレームは v86_enter が          */
/*   TSS.ESP0 を呼び出し元の ESP にするので v86 プログラムのスタックに載る)。 */
/*  16KB は実測の 5 倍強。**リング 3 のユーザプログラムを導入したら          */
/*  ここが全プログラムの ISR ネストを受けることになるので、その時は          */
/*  measure し直すこと。**                                                  */
/*  → docs/tasks/v86v2/09_memmap.md                                         */
/* ====================================================================== */
#define MEM_STACK_GUARD       0x1FB000UL
#define MEM_STACK_GUARD_END   0x1FBFFFUL
#define MEM_KSTACK_BASE       0x1FC000UL
#define MEM_KSTACK_TOP        0x1FFFFCUL

/* ブート時スタック (ローダー専用)。
 *
 * **ここは低位のままでなければならない。** ローダーは PM に入った後も
 * ディスク BIOS (INT 1Bh) を呼ぶためにリアルモードへ戻るので、SS:SP で
 * 届く 1MB 未満にスタックが要る。3 つのローダーの `mov esp, 0009FFFCh` が
 * この値。カーネルは kentry.asm で MEM_KSTACK_TOP に張り替えるため、
 * この領域はカーネルが動き出した時点で用済みになる
 * (V86 セッションはブートの遥か後なので競合しない)。 */
#define MEM_BOOT_STACK_TOP    0x9FFFCUL

/* BIOS ROM: Read-Only */
#define MEM_BIOS_ROM_START    0xF0000UL
#define MEM_BIOS_ROM_END      0xFFFFFUL

/* ====================================================================== */
/*  コンベンショナルメモリ再利用域 (ブート後に配置)                          */
/*  Step 04 で最終アドレスに更新される。暫定的にカーネル帯域後方にも配置。   */
/* ====================================================================== */
#define MEM_CONV_RECLAIM_START  0x01000UL
#define MEM_CONV_RECLAIM_END    0x9FFFFUL

/* コンベンショナルメモリの終端 (VRAM の直前)。
 * V86 ゲストに渡せるリニアの上限でもある。 */
#define MEM_CONV_END            0xA0000UL

/* NULLポインタ検出ガード */
#define MEM_NULL_GUARD_END      0x00FFFUL

/* フォントキャッシュ (コンベンショナル, ~292KB) */
#define MEM_FONT_CACHE_BASE     0x01000UL

/* Unicode-JIS変換テーブル (コンベンショナル, 128KB) */
#define MEM_UNICODE_TABLE_BASE  0x4A000UL
#define MEM_UNICODE_TABLE_SIZE  0x20000UL  /* 128KB */

/* GFXバックバッファ (コンベンショナル, 128KB = 32000B × 4プレーン) */
#define MEM_GFX_BB_BASE         0x6A000UL
#define MEM_GFX_BB_SIZE         0x20000UL  /* 128KB (32000B×4プレーン, 0x6A000-0x89FFF) */

/* KernelAPIテーブルアドレスは os32_kapi_shared.h で定義 (KAPI_ADDR) */

/* ====================================================================== */
/*  カーネル帯域内の動的配置 (0x100000-0x1FFFFF)                             */
/*  KAPI + SHM はヒープの直後に動的配置される                               */
/* ====================================================================== */

/* KAPIテーブル: KHEAP末尾直後 */
#define MEM_KAPI_OFFSET       (KHEAP_SIZE)  /* ヒープ末尾からのオフセット */
#define MEM_KAPI_SIZE         0x1000UL      /* 4KB */

/* 共有メモリ: KAPIテーブルの直後 */
#define MEM_SHM_GUARD_LO      (KHEAP_BASE + KHEAP_SIZE + MEM_KAPI_SIZE)
#define MEM_SHM_BASE          (MEM_SHM_GUARD_LO + 0x1000UL)
#define MEM_SHM_SIZE          0x040000UL  /* 256KB */
#define MEM_SHM_END           (MEM_SHM_BASE + MEM_SHM_SIZE - 1)
#define MEM_SHM_GUARD_HI      (MEM_SHM_BASE + MEM_SHM_SIZE)

/* GUI 予約 SHM (契約 T2 / docs/tasks/gui/API_CONTRACTS.md)。
 * SHM 帯 (MEM_SHM_BASE, 16KB×16 ブロック) の末尾側ブロック 12〜15
 * (先頭 +192KB から 64KB) を GUI 用に固定予約する。kernel/shm.c の初期化で
 * この 4 ブロックを SHM_RESERVED にし、shm_alloc が配らないようにする。
 * 1 スロット (16KB) = アプリ 1 本、最大 4 アプリ (v1 はスロット 0 のみ)。
 * PTE は SHM 帯として既に RW+USER (v2 C2)。 */
#define MEM_SHM_GUI_BASE      (MEM_SHM_BASE + 0x30000UL)  /* +192KB = ブロック 12 */
#define MEM_SHM_GUI_SIZE      0x10000UL                    /* 64KB = 4 ブロック */
#define GUI_SLOT_SIZE         0x4000UL                     /* 16KB = 1 スロット */
#define GUI_SLOT_MAX          4                            /* スロット 0〜3 */

/* カーネル帯域終端 */
#define MEM_KERNEL_BAND_END   0x1FFFFFUL

/* SHM後方予約域 (NOT PRESENT): SHMガード後 〜 カーネルスタックガードの直前。
 * 末尾 20KB はカーネルスタックとそのガードが使う。 */
#define MEM_SHM_RESV_START    (MEM_SHM_GUARD_HI + 0x1000UL)
#define MEM_SHM_RESV_END      (MEM_STACK_GUARD - 1)

/* ====================================================================== */
/*  SQLite帯域 (0x200000-0x2FFFFF, 1MB)                                     */
/*  SQLite code+BSS + 代替スタック (128KB)                                  */
/* ====================================================================== */
extern u32 __sqlite_end;
#define MEM_SQLITE_STACK_BASE  ((((u32)&__sqlite_end) + 0xFFF) & ~0xFFFUL)
#define MEM_SQLITE_STACK_SIZE  0x020000UL  /* 128KB */
#define MEM_SQLITE_STACK_TOP   (MEM_SQLITE_STACK_BASE + MEM_SQLITE_STACK_SIZE - 16)

/* SQLite帯域後の予約: NOT PRESENT */
#define MEM_KERNEL_RESV_START  ((MEM_SQLITE_STACK_BASE + MEM_SQLITE_STACK_SIZE + 0xFFF) & ~0xFFFUL)
#define MEM_KERNEL_RESV_END    0x2FFFFFUL  /* シェル帯域の直前まで */

/* ====================================================================== */
/*  シェル常駐帯域 (0x300000-0x3FFFFF, 1MB)                                 */
/*  シェルはここに常駐し、子プロセスは一切触れない。PD切り替え不要。         */
/*                                                                          */
/*  レイアウト:                                                             */
/*    0x300000-           .text + .data + .bss (~466KB)                     */
/*    (〜0x372000)        BSS終端 → ここから newlib の sbrk ヒープ (malloc) */
/*    0x375000            ガードページ (Not-Present, 4KB) = sbrk 上限        */
/*    0x376000-0x37FFFF   スタック (40KB, ESP初期値=0x380000)               */
/*    0x380000-0x3FFFFF   exec_heap (KAPI mem_alloc/mem_free 用, 512KB)      */
/*                                                                          */
/*  sbrk ヒープと exec_heap は **必ず別領域** にすること。かつて両方が BSS   */
/*  終端から始まっていたため、newlib の stdio バッファと mem_alloc のブロック */
/*  ヘッダが互いを上書きし、`ls > file` の化け・`pipe: out of memory`・     */
/*  double free 警告として現れていた (2026-09-03 実測)。                     */
/* ====================================================================== */
#define MEM_SHELL_LOAD_ADDR   0x300000UL  /* シェルロードアドレス */
#define MEM_SHELL_MAX_SIZE    0x075000UL  /* シェルcode+bss最大 (468KB) */
#define MEM_SHELL_GUARD       0x375000UL  /* シェルスタックガード (= sbrk 上限) */
#define MEM_SHELL_STACK_TOP   0x380000UL  /* シェルスタック先頭 (下向き成長) */
#define MEM_SHELL_STACK_SIZE  0x00A000UL  /* シェルスタックサイズ (40KB) */
#define MEM_SHELL_HEAP_BASE   0x380000UL  /* シェル exec_heap 先頭 (mem_alloc) */
#define MEM_SHELL_HEAP_SIZE   0x080000UL  /* シェル exec_heap サイズ (512KB) */
#define MEM_SHELL_BAND_END    0x3FFFFFUL  /* シェル帯域終端 */

/* ====================================================================== */
/*  アプリ帯域 (APP_BAND_PDE = PDE 1, 0x400000-0x7FFFFF)                    */
/*                                                                          */
/*  この 4MB だけが「PD ごと」の帯域 (kernel/paging.h の CONTRACTS C2)。     */
/*  先頭 1MB を共有ライブラリに、残りを外部プログラム本体とユーザスタックに   */
/*  割り当てる。PDE 単位で切り替わるので、境界を PDE をまたぐ位置へ動かして   */
/*  はならない (paging.c の STATIC_ASSERT が検査する)。                      */
/* ====================================================================== */
#define MEM_APP_BAND_BASE     0x400000UL  /* PDE 1 の先頭 */
#define MEM_APP_BAND_TOP      0x800000UL  /* PDE 1 の上端 (exclusive) */

/* ====================================================================== */
/*  共有ライブラリ帯域 (0x400000-0x4FFFFF, 1MB)  — GUI v1.1 K3              */
/*                                                                          */
/*  固定アドレス常駐の位置依存ライブラリ (再配置なし、ロードアドレスは 1 つ)。 */
/*  カーネル (kernel/shlib.c) が /sys/lib/libos32gui.shlib を起動時にここへ  */
/*  読み、レイアウトは先頭ページの OS32ShlibHeader が決める:                 */
/*                                                                          */
/*    0x400000                          ジャンプ表 (OS32_SHLIB_HDR_SIZE=4KB) */
/*    +.. text_pages ページ             .text/.rodata — read-only + USER。   */
/*                                      master PD に張るので全 PD で共有。   */
/*    data_vaddr .. +data_pages ページ  .data/.bss — 同じ仮想番地に          */
/*                                      **アプリごとの物理ページ**を張る。    */
/*    帯域末尾 data_pages ページ         .data/.bss の原本 (複製元)。          */
/*                                      pgalloc の管理外に置くため帯域内。    */
/*                                                                          */
/*  帯域全体はロード成功時に pgalloc_mark_used() で予約する (子プロセスの     */
/*  claim は MEM_EXEC_LOAD_ADDR からなので、ここは別に押さえないと            */
/*  PD/PT や V86 バッキングに持っていかれる)。未ロードなら予約しない。        */
/* ====================================================================== */
#define MEM_SHLIB_BASE        MEM_APP_BAND_BASE   /* 0x400000 */
#define MEM_SHLIB_SIZE        0x100000UL          /* 1MB */
#define MEM_SHLIB_END         (MEM_SHLIB_BASE + MEM_SHLIB_SIZE)  /* 0x500000 */

/* ====================================================================== */
/*  外部プログラムロード関連 (子プロセス用: 0x500000〜)                      */
/*  シェル常駐帯域とは完全に分離。アイデンティティマッピング。               */
/*  スタック/ヒープは exec_run() にてシステムメモリ量から動的に計算される      */
/*                                                                          */
/*  2026-09-05 (K3): 共有ライブラリ帯域を下に挿し込んだので 0x400000 →       */
/*  0x500000 へ 1MB 上がった。sdk/link/app.ld と mkos32x の load_addr、      */
/*  および exec の旧バイナリ判定がこの値に追従する。                          */
/* ====================================================================== */
#define MEM_EXEC_LOAD_ADDR    MEM_SHLIB_END
/* 子プロセス帯のレイアウト (2026-09-04 に固定 1MB 上限を撤廃):
 *   [load .. code_end)            code + data + bss
 *   [code_end .. guard_a)         newlib sbrk (少なくとも MEM_EXEC_SBRK_MIN)
 *   [guard_a]                     ガードページ (非present)
 *   [exec_heap_base .. heap_top)  KAPI mem_alloc (exec_heap)。ヘッダ heap_size
 *                                 指定があればその大きさ、0 なら空きを折半
 *   heap_top = スタックガード直下 (CPL=3: RING3_HEAP_TOP / CPL=0: guard_b
 *              から動的確保リザーブを引いた位置)
 * 本体の上限は heap_top - MEM_EXEC_SBRK_MIN - ガード - MEM_EXEC_HEAP_MIN で決まる。 */
#define MEM_EXEC_SBRK_MIN     0x40000UL          /* sbrk に最低限残す 256KB */
#define MEM_EXEC_HEAP_MIN     0x10000UL          /* exec_heap の最小 64KB */
#define MEM_EXEC_STACK_SIZE   0x40000UL          /* スタックサイズ 256KB (-O0 SQLite対応) */

/* ====================================================================== */
/*  ホットデプロイ用ステージング領域 (物理末尾から予約)                      */
/*                                                                          */
/*  ホストが NP21/W 内蔵 aidebug の /api/deploy で直接書き込む窓。          */
/*  カーネル帯域にもシェル帯域にも入れず、物理メモリの末尾を削って確保する。 */
/*  子プロセスのスタックは mem_end から下へ伸びるので、exec と pgalloc は   */
/*  sys_usable_mem_end() を使ってこの分を避けること。                       */
/*  設計: docs/tasks/hotdeploy/DESIGN.md                                    */
/* ====================================================================== */
#define MEM_HOTDEPLOY_SIZE    0x040000UL  /* 256KB (最大バイナリ game.bin 118KB) */

/* 制御ブロック: GFXバックバッファ末尾より後、自動プレイ用メールボックス
 * (0x90000) より前の空き。カーネルがブート時に magic と番地を書く。 */
#define MEM_HOTDEPLOY_DESC    0x8C000UL
#define MEM_HOTDEPLOY_DESC_SZ 0x001000UL  /* 4KB */

/* ====================================================================== */
/*  タイマー設定                                                            */
/* ====================================================================== */
#define PIT_HZ                100   /* タイマー割り込み周波数 (Hz) */

#endif /* MEMMAP_H */
