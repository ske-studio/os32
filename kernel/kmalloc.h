/* ======================================================================== */
/*  KMALLOC.H — カーネルメモリアロケータ                                    */
/*                                                                          */
/*  シンプルなファーストフィット方式の動的メモリ管理                          */
/*  ヒープ領域はBSS末尾からスタック手前まで                                  */
/* ======================================================================== */

#ifndef __KMALLOC_H
#define __KMALLOC_H

#include "types.h"

/* ======== API ======== */

/* ヒープ初期化 (起動時に1回だけ呼ぶ) */
/* heap_start: ヒープ開始アドレス, heap_size: バイト数 */
void kmalloc_init(void *heap_start, u32 heap_size);

/* メモリ確保 (アライメント: 4バイト) */
void *kmalloc(u32 size);

/* メモリ解放 */
void kfree(void *ptr);

/* メモリ再確保 (kmalloc + memcpy + kfree) */
void *krealloc(void *ptr, u32 new_size);

/* ヒープ情報 */
u32 kmalloc_total(void);  /* ヒープ総サイズ */
u32 kmalloc_used(void);   /* 使用中サイズ */
u32 kmalloc_free(void);   /* 空き容量 */

/* ブロックのデータ部サイズを取得 (テスト用) */
u32 kmalloc_block_size(void *ptr);

/* ======== libc互換ラッパー ======== */
/* カーネル空間でのみリンクされる。外部プログラムの newlib malloc には影響しない。 */
void *malloc(u32 size);
void  free(void *ptr);
void *realloc(void *ptr, u32 new_size);

#endif /* __KMALLOC_H */
