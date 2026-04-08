/* ======================================================================== */
/*  SHM.H — 共有メモリ管理 (IPC用)                                          */
/*                                                                          */
/*  プロセス切り替え下でも全プログラムから参照可能な静的共有メモリ帯域。       */
/*  カーネル予約域 0x200000 付近に 256KB を配置する。                         */
/*  16KBブロック × 16個の固定長ブロック分割管理。                            */
/*  ページ保護によるロック (R/O) / アンロック (R/W) をサポート。              */
/* ======================================================================== */

#ifndef __SHM_H
#define __SHM_H

#include "types.h"
#include "memmap.h"

/* ブロック設定 */
#define SHM_BLOCK_SIZE   (16 * 1024)   /* 16KB */
#define SHM_BLOCK_COUNT  16            /* 16ブロック */
#define SHM_TOTAL_SIZE   (SHM_BLOCK_SIZE * SHM_BLOCK_COUNT) /* 256KB */

/* ブロック状態 */
#define SHM_FREE    0   /* 未使用 */
#define SHM_USED    1   /* 確保済み (R/W) */
#define SHM_LOCKED  2   /* ロック済み (R/O) */

/* ======== API ======== */

/* 初期化 (paging_init の後に呼ぶ) */
void shm_init(void);

/* ブロック確保 (連続 block_count ブロック)
 * 戻り値: 先頭アドレス, NULL=空き不足 */
void *shm_alloc(int block_count);

/* ブロックをRead-Onlyに保護 (ロック)
 * 戻り値: 0=成功, -1=不正なポインタ */
int shm_lock(void *ptr);

/* ブロックを解放 (R/Wに戻す)
 * 戻り値: 0=成功, -1=不正なポインタ */
int shm_free(void *ptr);

#endif /* __SHM_H */
