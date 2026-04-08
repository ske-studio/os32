/* ======================================================================== */
/*  PGALLOC.C — 物理ページフレームアロケータ                                 */
/*                                                                          */
/*  プログラム空間 (0x400000 〜 mem_end) の物理ページを管理する。            */
/*  ビットマップ方式: 1ビット = 1ページ (4KB)。                              */
/*  最大16MB → 最大3072ページ (0x400000〜0xFFFFFF) → ビットマップ 384バイト  */
/*                                                                          */
/*  Level 0 (シェル) が使用するページは pgalloc_mark_used() で予約する。     */
/*  Level 1+ のネスト呼び出し時に pgalloc_alloc_n() で物理ページを確保し、   */
/*  子プロセス終了時に pgalloc_free_n() で解放する。                         */
/* ======================================================================== */

#include "pgalloc.h"

/* 最大16MBまでの物理ページ管理 */
/* 0x400000〜0xFFFFFF = 12MB = 3072ページ */
#define PGALLOC_MAX_PAGES   3072
#define BITMAP_SIZE         ((PGALLOC_MAX_PAGES + 31) / 32)

/* ビットマップ: 1=使用中, 0=空き */
static u32 bitmap[BITMAP_SIZE];
static u32 total_pages;
static u32 used_pages;

/* ======================================================================== */
/*  pgalloc_init — 初期化                                                   */
/*  mem_kb: システム総メモリ (KB)                                           */
/* ======================================================================== */
void pgalloc_init(u32 mem_kb)
{
    u32 mem_end = mem_kb * 1024;
    u32 avail;
    u32 i;

    /* 管理対象: PGALLOC_BASE 〜 mem_end */
    if (mem_end <= PGALLOC_BASE) {
        total_pages = 0;
        used_pages = 0;
        return;
    }

    avail = mem_end - PGALLOC_BASE;
    total_pages = avail / PAGE_SIZE;
    if (total_pages > PGALLOC_MAX_PAGES) {
        total_pages = PGALLOC_MAX_PAGES;
    }

    /* 全ページを空きに初期化 */
    for (i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0;
    }
    used_pages = 0;
}

/* ======================================================================== */
/*  ビットマップ操作ヘルパー                                                */
/* ======================================================================== */
static void bmp_set(u32 idx)
{
    bitmap[idx / 32] |= (1UL << (idx % 32));
}

static void bmp_clear(u32 idx)
{
    bitmap[idx / 32] &= ~(1UL << (idx % 32));
}

static int bmp_test(u32 idx)
{
    return (bitmap[idx / 32] & (1UL << (idx % 32))) ? 1 : 0;
}

/* ページインデックス ⇔ 物理アドレス変換 */
static u32 idx_to_phys(u32 idx)
{
    return PGALLOC_BASE + idx * PAGE_SIZE;
}

static u32 phys_to_idx(u32 phys)
{
    return (phys - PGALLOC_BASE) / PAGE_SIZE;
}

/* ======================================================================== */
/*  pgalloc_alloc_page — 1ページ確保                                        */
/* ======================================================================== */
u32 pgalloc_alloc_page(void)
{
    u32 i;
    for (i = 0; i < total_pages; i++) {
        if (!bmp_test(i)) {
            bmp_set(i);
            used_pages++;
            return idx_to_phys(i);
        }
    }
    return 0; /* 空きなし */
}

/* ======================================================================== */
/*  pgalloc_free_page — 1ページ解放                                         */
/* ======================================================================== */
void pgalloc_free_page(u32 phys_addr)
{
    u32 idx;
    if (phys_addr < PGALLOC_BASE) return;
    idx = phys_to_idx(phys_addr);
    if (idx >= total_pages) return;
    if (bmp_test(idx)) {
        bmp_clear(idx);
        used_pages--;
    }
}

/* ======================================================================== */
/*  pgalloc_alloc_n — 連続nページ確保                                       */
/*  戻り値: 先頭物理アドレス, 0=失敗                                        */
/* ======================================================================== */
u32 pgalloc_alloc_n(int n)
{
    u32 start, i;
    int found;

    if (n <= 0 || (u32)n > total_pages) return 0;

    for (start = 0; start <= total_pages - (u32)n; start++) {
        found = 1;
        for (i = 0; i < (u32)n; i++) {
            if (bmp_test(start + i)) {
                found = 0;
                start += i; /* 次の候補へスキップ */
                break;
            }
        }
        if (found) {
            for (i = 0; i < (u32)n; i++) {
                bmp_set(start + i);
            }
            used_pages += (u32)n;
            return idx_to_phys(start);
        }
    }
    return 0; /* 連続空き領域なし */
}

/* ======================================================================== */
/*  pgalloc_free_n — 連続nページ解放                                        */
/* ======================================================================== */
void pgalloc_free_n(u32 phys_addr, int n)
{
    u32 idx, i;
    if (phys_addr < PGALLOC_BASE || n <= 0) return;
    idx = phys_to_idx(phys_addr);
    for (i = 0; i < (u32)n && (idx + i) < total_pages; i++) {
        if (bmp_test(idx + i)) {
            bmp_clear(idx + i);
            used_pages--;
        }
    }
}

/* ======================================================================== */
/*  pgalloc_mark_used — 指定範囲を使用中にマーキング                        */
/* ======================================================================== */
void pgalloc_mark_used(u32 phys_start, int page_count)
{
    u32 idx, i;
    if (phys_start < PGALLOC_BASE || page_count <= 0) return;
    idx = phys_to_idx(phys_start);
    for (i = 0; i < (u32)page_count && (idx + i) < total_pages; i++) {
        if (!bmp_test(idx + i)) {
            bmp_set(idx + i);
            used_pages++;
        }
    }
}

/* ======================================================================== */
/*  統計情報                                                                */
/* ======================================================================== */
u32 pgalloc_total_pages(void) { return total_pages; }
u32 pgalloc_free_pages(void)  { return total_pages - used_pages; }
