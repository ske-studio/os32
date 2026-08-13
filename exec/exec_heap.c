/* ======================================================================== */
/*  EXEC_HEAP.C — プログラム専用 動的メモリアロケータ                       */
/*                                                                          */
/*  カーネルヒープ(kmalloc)とは完全に独立した領域。                          */
/*  バグプログラムがバッファオーバーランしてもカーネルヒープを破壊しない。    */
/*                                                                          */
/*  ファーストフィット方式の動的アロケータ:                                  */
/*    - alloc: 空きブロックを検索し、必要に応じて分割して割り当て            */
/*    - free:  ブロックを解放し、前後の空きブロックと結合（マージ）          */
/*    - reset: 全域を1つのフリーブロックに戻す（exec_run終了時）             */
/*                                                                          */
/*  ブロックヘッダ構造 (8バイト):                                            */
/*    u32 size    — データ部サイズ (ヘッダ含まず)                            */
/*    u32 magic   — 確保済み=0xA110CA7E, 解放済み=0xFEEEFEEE                */
/*    [data...]   — ユーザデータ (4バイトアライン)                           */
/* ======================================================================== */

#include "exec_heap.h"
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

/* ======== ヒープ管理 ======== */
static u8  *heap_base = (u8 *)0;
static u32  heap_size = 0;
static u32  heap_used = 0;

/* ======================================================================== */
/*  初期化                                                                  */
/* ======================================================================== */
void exec_heap_init_at(u32 base, u32 size)
{
    BlkHdr *first;

    /* ヘッダすら入らないサイズなら空ヒープとして初期化する
     * (first->size = size - BLK_HDR_SIZE のアンダーフロー防止) */
    if (size < BLK_HDR_SIZE + BLK_ALIGN) {
        heap_base = (u8 *)0;
        heap_size = 0;
        heap_used = 0;
        return;
    }

    heap_base = (u8 *)base;
    heap_size = size;
    heap_used = 0;

    /* ヒープ全体を1つのフリーブロックにする */
    first = (BlkHdr *)heap_base;
    first->size  = size - BLK_HDR_SIZE;
    first->magic = BLK_MAGIC_FREE;
}

/* ======================================================================== */
/*  exec_heap_alloc — メモリ確保 (ファーストフィット)                        */
/* ======================================================================== */
void *exec_heap_alloc(u32 size)
{
    u8 *p;
    BlkHdr *blk;

    /* size > heap_size の除外はアライン時の整数オーバーフロー
     * (0xFFFFFFFD〜 が 0 に化ける) も同時に防ぐ */
    if (size == 0 || size > heap_size) return (void *)0;

    /* 4バイトアライメント */
    size = (size + BLK_ALIGN - 1) & ~(BLK_ALIGN - 1);

    /* ファーストフィット探索 */
    p = heap_base;
    while (p + BLK_HDR_SIZE <= heap_base + heap_size) {
        blk = (BlkHdr *)p;

        /* 壊れたブロック検出 (マジック不正、またはサイズがヒープ外を指す) */
        if (blk->magic != BLK_MAGIC_FREE && blk->magic != BLK_MAGIC_USED) {
            return (void *)0;   /* ヒープ破損 */
        }
        if (blk->size > (u32)(heap_base + heap_size - p) - BLK_HDR_SIZE) {
            return (void *)0;   /* ヒープ破損 */
        }

        if (blk->magic == BLK_MAGIC_FREE && blk->size >= size) {
            /* 十分なサイズのフリーブロック発見 */

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

        /* 次のブロックへ */
        p += BLK_HDR_SIZE + blk->size;
    }

    return (void *)0;   /* メモリ不足 */
}

/* ======================================================================== */
/*  exec_heap_free — メモリ解放 + 隣接フリーブロック結合                     */
/* ======================================================================== */
void exec_heap_free(void *ptr)
{
    BlkHdr *blk;
    u8 *p;

    if (ptr == (void *)0) return;

    /* ポインタ検証: ヒープ範囲外・アライメント不正はヘッダを読む前に弾く */
    if ((u8 *)ptr < heap_base + BLK_HDR_SIZE ||
        (u8 *)ptr >= heap_base + heap_size ||
        (((u32)ptr) & (BLK_ALIGN - 1)) != 0) {
        kprintf(0xC1, "[exec_heap_free] invalid ptr %x\n", (u32)ptr);
        return;
    }

    blk = (BlkHdr *)((u8 *)ptr - BLK_HDR_SIZE);

    /* マジック検証 (double free / ヘッダ破壊の検出) */
    if (blk->magic != BLK_MAGIC_USED) {
        kprintf(0xC1, "[exec_heap_free] bad magic %x at %x (double free?)\n",
                blk->magic, (u32)ptr);
        return;
    }

    blk->magic = BLK_MAGIC_FREE;
    heap_used -= blk->size + BLK_HDR_SIZE;

    /* 前方結合: ヒープを先頭から走査して隣接フリーブロックを結合 */
    p = heap_base;
    while (p + BLK_HDR_SIZE <= heap_base + heap_size) {
        BlkHdr *cur = (BlkHdr *)p;
        u8 *next_p;

        if (cur->magic != BLK_MAGIC_FREE && cur->magic != BLK_MAGIC_USED) {
            break;  /* ヒープ破損 */
        }
        if (cur->size > (u32)(heap_base + heap_size - p) - BLK_HDR_SIZE) {
            break;  /* ヒープ破損 */
        }

        next_p = p + BLK_HDR_SIZE + cur->size;

        /* 隣接する2つのフリーブロックを結合 */
        if (cur->magic == BLK_MAGIC_FREE &&
            next_p + BLK_HDR_SIZE <= heap_base + heap_size) {
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
/*  exec_heap_reset — 全域破棄 (exec_run終了時)                             */
/*  全体を1つのフリーブロックに戻す。                                       */
/* ======================================================================== */
void exec_heap_reset(void)
{
    if (heap_size > 0) {
        BlkHdr *first = (BlkHdr *)heap_base;
        first->size  = heap_size - BLK_HDR_SIZE;
        first->magic = BLK_MAGIC_FREE;
    }
    heap_used = 0;
}

/* ======================================================================== */
/*  exec_heap_save_state — 管理変数のスナップショット保存                    */
/*  ヒープのメタデータ (BlkHdr) はメモリ上にそのまま残す。                   */
/* ======================================================================== */
void exec_heap_save_state(u32 *out_used)
{
    if (out_used) {
        *out_used = heap_used;
    }
}

/* ======================================================================== */
/*  exec_heap_restore_state — 管理変数のスナップショット復元                 */
/*                                                                          */
/*  exec_heap_init_at() と異なり、ヒープメモリの内容(BlkHdr)を破壊しない。   */
/*  管理変数 (base, size, used) だけを親プロセスの値に戻す。                 */
/*  凍結モデル前提: 親のヒープ領域は子プロセスが書き換えない。               */
/* ======================================================================== */
void exec_heap_restore_state(u32 base, u32 size, u32 used)
{
    heap_base = (u8 *)base;
    heap_size = size;
    heap_used = used;
}

/* ======================================================================== */
/*  統計情報                                                                */
/* ======================================================================== */
u32 exec_heap_total(void) { return heap_size; }
u32 exec_heap_used(void)  { return heap_used; }
