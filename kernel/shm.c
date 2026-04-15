/* ======================================================================== */
/*  SHM.C — 共有メモリ管理 (IPC用)                                          */
/*                                                                          */
/*  カーネル予約域 0x201000-0x240FFF に 256KB の共有メモリ空間を配置。        */
/*  16KB × 16ブロックの固定長ブロック分割管理。                              */
/*                                                                          */
/*  ガードページ:                                                           */
/*    0x200000 (前方, NOT PRESENT) — オーバーフロー保護                      */
/*    0x241000 (後方, NOT PRESENT) — オーバーフロー保護                      */
/*                                                                          */
/*  ページ保護:                                                             */
/*    shm_lock()  — ブロックの全ページを Read-Only に変更                   */
/*    shm_free()  — ブロックの全ページを Read-Write に戻す                  */
/* ======================================================================== */

#include "shm.h"
#include "paging.h"

/* ブロック管理テーブル */
static u8 shm_state[SHM_BLOCK_COUNT]; /* 各ブロックの状態 */
static int shm_block_span[SHM_BLOCK_COUNT]; /* 各確保の先頭ブロックが持つスパン数 */

/* ブロックインデックス → 物理/仮想アドレス変換 */
static u32 block_to_addr(int idx)
{
    return MEM_SHM_BASE + (u32)idx * SHM_BLOCK_SIZE;
}

/* アドレス → ブロックインデックス変換 (-1=無効) */
static int addr_to_block(void *ptr)
{
    u32 addr = (u32)ptr;
    int idx;
    if (addr < MEM_SHM_BASE || addr >= MEM_SHM_BASE + SHM_TOTAL_SIZE) {
        return -1;
    }
    idx = (int)((addr - MEM_SHM_BASE) / SHM_BLOCK_SIZE);
    /* ブロック境界チェック */
    if (addr != block_to_addr(idx)) {
        return -1;
    }
    return idx;
}

/* ======================================================================== */
/*  shm_init — 共有メモリ初期化                                             */
/*  ガードページ設定 + 共有メモリ領域をR/Wに設定                            */
/* ======================================================================== */
void shm_init(void)
{
    int i;
    u32 addr;

    /* 全ブロックを未使用に初期化 */
    for (i = 0; i < SHM_BLOCK_COUNT; i++) {
        shm_state[i] = SHM_FREE;
        shm_block_span[i] = 0;
    }

    /* ガードページ設定 */
    paging_set_not_present(MEM_SHM_GUARD_LO,
                           MEM_SHM_GUARD_LO + PAGE_SIZE - 1);
    paging_set_not_present(MEM_SHM_GUARD_HI,
                           MEM_SHM_GUARD_HI + PAGE_SIZE - 1);

    /* 共有メモリ領域を R/W に設定 (アイデンティティマッピング) */
    for (addr = MEM_SHM_BASE; addr < MEM_SHM_BASE + SHM_TOTAL_SIZE;
         addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PAGE_RW);
    }
}

/* ======================================================================== */
/*  shm_alloc — 連続ブロック確保 (ファーストフィット)                        */
/*  戻り値: 先頭アドレス, NULL=空き不足                                     */
/* ======================================================================== */
void *shm_alloc(int block_count)
{
    int start, i, found;

    if (block_count <= 0 || block_count > SHM_BLOCK_COUNT) {
        return (void *)0;
    }

    /* ファーストフィット探索 */
    for (start = 0; start <= SHM_BLOCK_COUNT - block_count; start++) {
        found = 1;
        for (i = 0; i < block_count; i++) {
            if (shm_state[start + i] != SHM_FREE) {
                found = 0;
                start += i; /* 次の候補へスキップ */
                break;
            }
        }
        if (found) {
            /* 確保 */
            for (i = 0; i < block_count; i++) {
                shm_state[start + i] = SHM_USED;
            }
            shm_block_span[start] = block_count;
            return (void *)block_to_addr(start);
        }
    }

    return (void *)0; /* 空き不足 */
}

/* ======================================================================== */
/*  shm_lock — ブロックを Read-Only に保護                                  */
/*  戻り値: 0=成功, -1=不正なポインタ                                       */
/* ======================================================================== */
int shm_lock(void *ptr)
{
    int idx, span, i;
    u32 addr;

    idx = addr_to_block(ptr);
    if (idx < 0) return -1;

    span = shm_block_span[idx];
    if (span <= 0) return -1;

    for (i = 0; i < span; i++) {
        if (shm_state[idx + i] != SHM_USED) return -1;
    }

    /* 全ブロックの全ページを Read-Only に */
    for (i = 0; i < span; i++) {
        u32 blk_start = block_to_addr(idx + i);
        for (addr = blk_start; addr < blk_start + SHM_BLOCK_SIZE;
             addr += PAGE_SIZE) {
            paging_set_page(addr, addr, PAGE_RO);
        }
        shm_state[idx + i] = SHM_LOCKED;
    }

    return 0;
}

/* ======================================================================== */
/*  shm_free — ブロックを解放 (R/W に戻す)                                  */
/*  戻り値: 0=成功, -1=不正なポインタ                                       */
/* ======================================================================== */
int shm_free(void *ptr)
{
    int idx, span, i;
    u32 addr;

    idx = addr_to_block(ptr);
    if (idx < 0) return -1;

    span = shm_block_span[idx];
    if (span <= 0) return -1;

    /* 全ブロックの全ページを R/W に戻して解放 */
    for (i = 0; i < span; i++) {
        u32 blk_start = block_to_addr(idx + i);
        for (addr = blk_start; addr < blk_start + SHM_BLOCK_SIZE;
             addr += PAGE_SIZE) {
            paging_set_page(addr, addr, PAGE_RW);
        }
        shm_state[idx + i] = SHM_FREE;
    }
    shm_block_span[idx] = 0;

    return 0;
}

/* ======================================================================== */
/*  shm_cleanup_all — 全ブロックを強制解放 (プログラム終了時の安全網)        */
/*  プログラムがshm_free()を呼び忘れても、exec_exit()で自動回収する。        */
/* ======================================================================== */
void shm_cleanup_all(void)
{
    int i;
    u32 addr;
    u32 blk_start;

    for (i = 0; i < SHM_BLOCK_COUNT; i++) {
        if (shm_state[i] != SHM_FREE) {
            /* ページ属性をR/Wに戻す */
            blk_start = block_to_addr(i);
            for (addr = blk_start; addr < blk_start + SHM_BLOCK_SIZE;
                 addr += PAGE_SIZE) {
                paging_set_page(addr, addr, PAGE_RW);
            }
            shm_state[i] = SHM_FREE;
        }
        shm_block_span[i] = 0;
    }
}
