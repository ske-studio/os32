/* ======================================================================== */
/*  MEMMAP.H — OS32 システムメモリマップ定数                                 */
/*                                                                          */
/*  カーネル・ページング・プログラムローダーが参照する物理/仮想アドレスを     */
/*  一元管理する。変更時はpaging.c, kernel.c, exec.hとの整合性を確認。       */
/*                                                                          */
/*  メモリレイアウト概要 (2026-04 再構築):                                   */
/*                                                                          */
/*    [コンベンショナルメモリ 0x00000-0xFFFFF]                               */
/*      0x09000-          カーネルバイナリ (.text+.data+.bss)                */
/*      0x40000-0x8EFFF   カーネルヒープ (kmalloc, 316KB)                    */
/*      0x8F000-0x8FFFF   スタックガード (NOT PRESENT)                       */
/*      0x90000-0x9FFFF   カーネルスタック (64KB)                            */
/*      0xA0000-0xEFFFF   VRAM (テキスト+グラフィック)                       */
/*      0xF0000-0xFFFFF   BIOS ROM (R/O)                                    */
/*                                                                          */
/*    [カーネルデータ 0x100000-0x1FFFFF]                                     */
/*      0x100000-0x148FFF フォントキャッシュ (~292KB)                        */
/*      0x149000-0x168FFF Unicode-JIS変換テーブル (128KB)                    */
/*      0x169000-0x188FFF GFXバックバッファ (128KB, 4プレーン)               */
/*      0x189000-0x189FFF KernelAPIテーブル (4KB)                            */
/*      0x18A000-0x1FFFFF [予約: カーネル拡張用]                             */
/*                                                                          */
/*    [カーネル予約 0x200000-0x3FFFFF]                                       */
/*      将来拡張用 (NOT PRESENT)                                             */
/*                                                                          */
/*    [プログラム空間 0x400000-メモリ上限]                                   */
/*      Phase 4 で動的レイアウトに移行予定                                   */
/*      コード+sbrk / GUARD A / exec_heap / GUARD B / スタック              */
/* ======================================================================== */

#ifndef MEMMAP_H
#define MEMMAP_H

/* ====================================================================== */
/*  カーネル配置                                                            */
/* ====================================================================== */
#define MEM_1MB               0x100000UL  /* 1MB */
#define KERNEL_LOAD_ADDR      0x9000UL    /* カーネルロードアドレス */

/* ====================================================================== */
/*  カーネルヒープ (kmalloc)                                                */
/* ====================================================================== */
/* BSS終端(~0x27260)→0x40000開始, 旧BBデッドゾーン(0x70000-0x8EFFF)を回収  */
#define KHEAP_BASE            0x40000UL   /* カーネルヒープ開始 */
#define KHEAP_SIZE            0x4F000UL   /* カーネルヒープサイズ (316KB) */

/* ====================================================================== */
/*  ページング保護範囲                                                      */
/* ====================================================================== */

/* IVT/BIOSデータ領域: Read-Only */
/* ページ0 (0x0-0xFFF) はBIOSトランポリン用にR/W維持 */
#define MEM_IVT_PROT_START    0x01000UL
#define MEM_IVT_PROT_END      0x05FFFUL

/* loader.bin (使用済み): Read-Only */
#define MEM_LOADER_START      0x08000UL
#define MEM_LOADER_END        0x08FFFUL

/* カーネルスタックガードページ: Not-Present */
#define MEM_STACK_GUARD       0x8F000UL
#define MEM_STACK_GUARD_END   0x8FFFFUL

/* BIOS ROM: Read-Only */
#define MEM_BIOS_ROM_START    0xF0000UL
#define MEM_BIOS_ROM_END      0xFFFFFUL

/* ====================================================================== */
/*  カーネルデータ域 (0x100000 - 0x1FFFFF)                                  */
/*  フォントキャッシュ → Unicode → バックバッファ → KAPI の順に先頭詰め   */
/* ====================================================================== */

/* フォントキャッシュ (~292KB, kcg.c) */
#define MEM_FONT_CACHE_BASE   0x100000UL

/* Unicode-JIS変換テーブル (128KB, utf8.c / kernel.c) */
#define MEM_UNICODE_TABLE_BASE 0x149000UL

/* GFXバックバッファ (128KB = 32000B × 4プレーン, gfx_core.c) */
#define MEM_GFX_BB_BASE       0x169000UL

/* KernelAPIテーブルアドレスは os32_kapi_shared.h で定義 (KAPI_ADDR) */

/* カーネル予約域 (将来拡張用, NOT PRESENTに設定) */
#define MEM_KERNEL_RESV_START 0x200000UL
#define MEM_KERNEL_RESV_END   0x3FFFFFUL

/* ====================================================================== */
/*  外部プログラムロード関連                                                 */
/*  ※ Phase 4 でスタック/ヒープを動的計算に移行予定                         */
/* ====================================================================== */
#define MEM_EXEC_LOAD_ADDR    0x400000UL
#define MEM_EXEC_MAX_SIZE     (0x100000UL)       /* コード+sbrk 最大1MB */
#define MEM_EXEC_STACK_TOP    0x57F000UL         /* [暫定] Phase 4で動的化 */
#define MEM_EXEC_STACK_SIZE   0x20000UL          /* スタックサイズ 128KB */

#define MEM_EXEC_HEAP_BASE    0x580000UL         /* [暫定] Phase 4で動的化 */

/* ====================================================================== */
/*  タイマー設定                                                            */
/* ====================================================================== */
#define PIT_HZ                100   /* タイマー割り込み周波数 (Hz) */

#endif /* MEMMAP_H */
