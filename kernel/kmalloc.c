/* ======================================================================== */
/*  KMALLOC.C — カーネルメモリアロケータ                                    */
/*                                                                          */
/*  ファーストフィット方式の簡易アロケータ                                   */
/*  連結リストでフリーブロックを管理、隣接ブロック結合付き                    */
/*                                                                          */
/*  ブロックヘッダ構造 (8バイト):                                            */
/*    u32 size    — データ部サイズ (ヘッダ含まず)                            */
/*    u32 magic   — 確保済み=0xA110CA7E, 解放済み=0xFEEEFEEE                */
/*    [data...]   — ユーザデータ (4バイトアライン)                           */
/* ======================================================================== */

#include "kmalloc.h"

/* ======== ブロックヘッダ ======== */
#define BLK_MAGIC_USED  0xA110CA7EUL  /* "ALLOCATE" */
#define BLK_MAGIC_FREE  0xFEEEFEEEUL
#define BLK_HDR_SIZE    8             /* sizeof(BlkHdr) */
#define BLK_ALIGN       4             /* 4バイトアライメント */

typedef struct BlkHdr {
    u32 size;      /* データ部サイズ */
    u32 magic;     /* マジックナンバー */
} BlkHdr;

/* ======== ヒープ管理 ======== */
static u8  *heap_base = (u8 *)0;
static u32  heap_sz   = 0;
static u32  heap_used = 0;

/* ======================================================================== */
/*  初期化                                                                  */
/* ======================================================================== */
void kmalloc_init(void *heap_start, u32 heap_size)
{
    BlkHdr *first;

    heap_base = (u8 *)heap_start;
    heap_sz   = heap_size;
    heap_used = 0;

    /* ヒープ全体を1つのフリーブロックにする */
    first = (BlkHdr *)heap_base;
    first->size  = heap_size - BLK_HDR_SIZE;
    first->magic = BLK_MAGIC_FREE;
}

/* ======================================================================== */
/*  kmalloc — メモリ確保                                                    */
/* ======================================================================== */
void *kmalloc(u32 size)
{
    u8 *p;
    BlkHdr *blk;
    BlkHdr *best_blk = (BlkHdr *)0;
    u8 *best_p = (u8 *)0;
    u32 best_size = 0xFFFFFFFF;

    if (size == 0) return (void *)0;

    /* 4バイトアライメント */
    size = (size + BLK_ALIGN - 1) & ~(BLK_ALIGN - 1);

    /* Best-fit 探索 */
    p = heap_base;
    while (p < heap_base + heap_sz) {
        blk = (BlkHdr *)p;

        /* 壊れたブロック検出 */
        if (blk->magic != BLK_MAGIC_FREE && blk->magic != BLK_MAGIC_USED) {
            return (void *)0;   /* ヒープ破損 */
        }

        if (blk->magic == BLK_MAGIC_FREE && blk->size >= size) {
            /* 要求を満たす最小のブロックを探す */
            if (blk->size < best_size) {
                best_size = blk->size;
                best_blk = blk;
                best_p = p;
                if (best_size == size) {
                    break; /* 完全一致 (無駄ゼロ) なら即探索終了 */
                }
            }
        }

        /* 次のブロックへ */
        p += BLK_HDR_SIZE + blk->size;
    }

    if (best_blk != (BlkHdr *)0) {
        blk = best_blk;
        p = best_p;

        /* 分割可能か？ (残り >= ヘッダ+最小データ8バイト) */
        if (blk->size >= size + BLK_HDR_SIZE + 8) {
            BlkHdr *next;
            u32 remain = blk->size - size - BLK_HDR_SIZE;

            /* 後半を新しいフリーブロックに */
            next = (BlkHdr *)(p + BLK_HDR_SIZE + size);
            next->size  = remain;
            next->magic = BLK_MAGIC_FREE;

            blk->size = size;
        }

        blk->magic = BLK_MAGIC_USED;
        heap_used += blk->size + BLK_HDR_SIZE;
        return (void *)(p + BLK_HDR_SIZE);
    }

    return (void *)0;   /* メモリ不足 */
}

/* ======================================================================== */
/*  kfree — メモリ解放 + 隣接フリーブロック結合                              */
/* ======================================================================== */
void kfree(void *ptr)
{
    BlkHdr *blk;
    u8 *p;

    if (ptr == (void *)0) return;

    blk = (BlkHdr *)((u8 *)ptr - BLK_HDR_SIZE);

    /* マジック検証 */
    if (blk->magic != BLK_MAGIC_USED) return;

    blk->magic = BLK_MAGIC_FREE;
    heap_used -= blk->size + BLK_HDR_SIZE;

    /* 前方結合: ヒープを先頭から走査して隣接フリーブロックを結合 */
    p = heap_base;
    while (p < heap_base + heap_sz) {
        BlkHdr *cur = (BlkHdr *)p;
        u8 *next_p;

        if (cur->magic != BLK_MAGIC_FREE && cur->magic != BLK_MAGIC_USED) {
            break;  /* ヒープ破損 */
        }

        next_p = p + BLK_HDR_SIZE + cur->size;

        /* 隣接する2つのフリーブロックを結合 */
        if (cur->magic == BLK_MAGIC_FREE && next_p < heap_base + heap_sz) {
            BlkHdr *next = (BlkHdr *)next_p;
            if (next->magic == BLK_MAGIC_FREE) {
                cur->size += BLK_HDR_SIZE + next->size;
                continue;  /* 結合後、同じ位置から再チェック */
            }
        }

        p = next_p;
    }
}

/* ======================================================================== */
/*  ヒープ情報                                                              */
/* ======================================================================== */
u32 kmalloc_total(void) { return heap_sz; }
u32 kmalloc_used(void)  { return heap_used; }
u32 kmalloc_free(void)  { return heap_sz - heap_used; }
