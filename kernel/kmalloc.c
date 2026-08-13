/* ======================================================================== */
/*  KMALLOC.C — ヒープアロケータ共通実装 + カーネルヒープ                    */
/*                                                                          */
/*  Best-fit 方式の簡易アロケータ                                            */
/*  連結リストでフリーブロックを管理、隣接ブロック結合付き                    */
/*                                                                          */
/*  実装はヒープ記述子 (KHeap) をパラメータに取る kheap_* 共通関数群で、      */
/*  カーネルヒープ (kmalloc) と exec ヒープ (exec/exec_heap.c) は同じ         */
/*  実装の 2 インスタンス。かつては exec_heap.c がこのファイルのクローンで、  */
/*  バグ修正が二重管理になっていた (R1)。                                    */
/*                                                                          */
/*  ブロックヘッダ構造 (8バイト):                                            */
/*    u32 size    — データ部サイズ (ヘッダ含まず)                            */
/*    u32 magic   — 確保済み=0xA110CA7E, 解放済み=0xFEEEFEEE                */
/*    [data...]   — ユーザデータ (4バイトアライン)                           */
/* ======================================================================== */

#include "kmalloc.h"
#include "kstring.h"
#include "kprintf.h"

/* ======== ブロックヘッダ ======== */
#define BLK_MAGIC_USED  0xA110CA7EUL  /* "ALLOCATE" */
#define BLK_MAGIC_FREE  0xFEEEFEEEUL
#define BLK_HDR_SIZE    8             /* sizeof(BlkHdr) */
#define BLK_ALIGN       4             /* 4バイトアライメント */

typedef struct BlkHdr {
    u32 size;      /* データ部サイズ */
    u32 magic;     /* マジックナンバー */
} BlkHdr;

/* ======================================================================== */
/*  共通実装 (kheap_*)                                                      */
/* ======================================================================== */

void kheap_init(KHeap *h, void *base, u32 size, const char *name)
{
    BlkHdr *first;

    h->name = name;

    /* ヘッダすら入らないサイズなら空ヒープとして初期化する
     * (first->size = size - BLK_HDR_SIZE のアンダーフロー防止) */
    if (size < BLK_HDR_SIZE + BLK_ALIGN) {
        h->base = (u8 *)0;
        h->size = 0;
        h->used = 0;
        return;
    }

    h->base = (u8 *)base;
    h->size = size;
    h->used = 0;

    /* ヒープ全体を1つのフリーブロックにする */
    first = (BlkHdr *)h->base;
    first->size  = size - BLK_HDR_SIZE;
    first->magic = BLK_MAGIC_FREE;
}

void *kheap_alloc(KHeap *h, u32 size)
{
    u8 *p;
    BlkHdr *blk;
    BlkHdr *best_blk = (BlkHdr *)0;
    u8 *best_p = (u8 *)0;
    u32 best_size = 0xFFFFFFFF;

    /* size > h->size の除外はアライン時の整数オーバーフロー
     * (0xFFFFFFFD〜 が 0 に化ける) も同時に防ぐ */
    if (size == 0 || size > h->size) return (void *)0;

    /* 4バイトアライメント */
    size = (size + BLK_ALIGN - 1) & ~(BLK_ALIGN - 1);

    /* Best-fit 探索 */
    p = h->base;
    while (p + BLK_HDR_SIZE <= h->base + h->size) {
        blk = (BlkHdr *)p;

        /* 壊れたブロック検出 (マジック不正、またはサイズがヒープ外を指す) */
        if (blk->magic != BLK_MAGIC_FREE && blk->magic != BLK_MAGIC_USED) {
            return (void *)0;   /* ヒープ破損 */
        }
        if (blk->size > (u32)(h->base + h->size - p) - BLK_HDR_SIZE) {
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
        h->used += blk->size + BLK_HDR_SIZE;
        return (void *)(p + BLK_HDR_SIZE);
    }

    return (void *)0;   /* メモリ不足 */
}

void kheap_free(KHeap *h, void *ptr)
{
    BlkHdr *blk;
    u8 *p;

    if (ptr == (void *)0) return;

    /* ポインタ検証: ヒープ範囲外・アライメント不正はヘッダを読む前に弾く。
     * 検証せずに ptr-8 を読むと、任意アドレスの free でヒープ管理が壊れる */
    if ((u8 *)ptr < h->base + BLK_HDR_SIZE ||
        (u8 *)ptr >= h->base + h->size ||
        (((u32)ptr) & (BLK_ALIGN - 1)) != 0) {
        kprintf(0xC1, "[%s] invalid free %x\n", h->name, (u32)ptr);
        return;
    }

    blk = (BlkHdr *)((u8 *)ptr - BLK_HDR_SIZE);

    /* マジック検証 (double free / ヘッダ破壊の検出) */
    if (blk->magic != BLK_MAGIC_USED) {
        kprintf(0xC1, "[%s] bad magic %x at %x (double free?)\n",
                h->name, blk->magic, (u32)ptr);
        return;
    }

    blk->magic = BLK_MAGIC_FREE;
    h->used -= blk->size + BLK_HDR_SIZE;

    /* 前方結合: ヒープを先頭から走査して隣接フリーブロックを結合 */
    p = h->base;
    while (p + BLK_HDR_SIZE <= h->base + h->size) {
        BlkHdr *cur = (BlkHdr *)p;
        u8 *next_p;

        if (cur->magic != BLK_MAGIC_FREE && cur->magic != BLK_MAGIC_USED) {
            break;  /* ヒープ破損 */
        }
        if (cur->size > (u32)(h->base + h->size - p) - BLK_HDR_SIZE) {
            break;  /* ヒープ破損 */
        }

        next_p = p + BLK_HDR_SIZE + cur->size;

        /* 隣接する2つのフリーブロックを結合 */
        if (cur->magic == BLK_MAGIC_FREE &&
            next_p + BLK_HDR_SIZE <= h->base + h->size) {
            BlkHdr *next = (BlkHdr *)next_p;
            if (next->magic == BLK_MAGIC_FREE) {
                cur->size += BLK_HDR_SIZE + next->size;
                continue;  /* 結合後、同じ位置から再チェック */
            }
        }

        p = next_p;
    }
}

void kheap_reset(KHeap *h)
{
    if (h->size > 0) {
        BlkHdr *first = (BlkHdr *)h->base;
        first->size  = h->size - BLK_HDR_SIZE;
        first->magic = BLK_MAGIC_FREE;
    }
    h->used = 0;
}

u32 kheap_block_size(KHeap *h, void *ptr)
{
    BlkHdr *blk;
    if (ptr == (void *)0) return 0;
    if ((u8 *)ptr < h->base + BLK_HDR_SIZE ||
        (u8 *)ptr >= h->base + h->size ||
        (((u32)ptr) & (BLK_ALIGN - 1)) != 0) return 0;
    blk = (BlkHdr *)((u8 *)ptr - BLK_HDR_SIZE);
    if (blk->magic != BLK_MAGIC_USED) return 0;
    return blk->size;
}

/* ======================================================================== */
/*  カーネルヒープ インスタンス                                              */
/* ======================================================================== */
static KHeap kernel_heap;

void kmalloc_init(void *heap_start, u32 heap_size)
{
    kheap_init(&kernel_heap, heap_start, heap_size, "kheap");
}

void *kmalloc(u32 size)
{
    return kheap_alloc(&kernel_heap, size);
}

/* ======================================================================== */
/*  kzalloc — ゼロクリア付き確保                                            */
/*  「kmalloc して kmemset」の手書きは未ゼロ化の漏れを生むのでこれを使う。   */
/* ======================================================================== */
void *kzalloc(u32 size)
{
    void *p = kmalloc(size);
    if (p) {
        kmemset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr)
{
    kheap_free(&kernel_heap, ptr);
}

u32 kmalloc_block_size(void *ptr)
{
    return kheap_block_size(&kernel_heap, ptr);
}

/* ======================================================================== */
/*  krealloc — メモリ再確保                                                  */
/*  新しいブロックを確保し、既存データをコピーして旧ブロックを解放する。       */
/*  ptr==NULL の場合は kmalloc(new_size) と同等。                             */
/*  new_size==0 の場合は kfree(ptr) と同等で NULL を返す。                    */
/* ======================================================================== */
void *krealloc(void *ptr, u32 new_size)
{
    void *new_ptr;
    u32 old_size;
    u32 copy_size;

    if (ptr == (void *)0) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return (void *)0; }

    old_size = kmalloc_block_size(ptr);
    if (old_size == 0) return (void *)0; /* 無効なポインタ */

    /* 既に十分なサイズがある場合はそのまま返す */
    if (old_size >= new_size) return ptr;

    new_ptr = kmalloc(new_size);
    if (new_ptr == (void *)0) return (void *)0;

    copy_size = (old_size < new_size) ? old_size : new_size;
    kmemcpy(new_ptr, ptr, copy_size);
    kfree(ptr);

    return new_ptr;
}

/* ======================================================================== */
/*  ヒープ情報                                                              */
/* ======================================================================== */
u32 kmalloc_total(void) { return kernel_heap.size; }
u32 kmalloc_used(void)  { return kernel_heap.used; }
u32 kmalloc_free(void)  { return kernel_heap.size - kernel_heap.used; }

/* ======================================================================== */
/*  libc互換ラッパー                                                         */
/*  カーネル空間でのみリンクされる。SQLite等の外部コードが暗黙に使用する       */
/*  malloc/free/realloc を kmalloc ベースで提供する。                         */
/*  外部プログラムの newlib-nano malloc とは完全に独立。                      */
/* ======================================================================== */
void *malloc(u32 size)               { return kmalloc(size); }
void  free(void *ptr)                { kfree(ptr); }
void *realloc(void *ptr, u32 size)   { return krealloc(ptr, size); }
