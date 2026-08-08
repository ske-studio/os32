/* ======================================================================== */
/*  V86_MEM.C — V86 ゲスト用アドレス空間                                    */
/* ======================================================================== */

#include "v86_mem.h"
#include "paging.h"
#include "pgalloc.h"

static u32 backing_phys = 0;

u32 v86_mem_backing_phys(void) { return backing_phys; }

/* 指定範囲を「実物理そのまま」で属性だけ張り替える */
static void map_identity(u32 start, u32 end, u32 flags)
{
    u32 a;
    for (a = start; a < end; a += PAGE_SIZE) {
        paging_set_page(a, a, flags);
    }
}

int v86_mem_setup(void)
{
    u32 a;

    if (backing_phys) {
        return -1;              /* 二重セットアップ */
    }

    backing_phys = pgalloc_alloc_n(V86_BACKING_PAGES);
    if (!backing_phys) {
        return -2;              /* 連続領域が取れない */
    }

    /* 低位 RAM をバッキングへ差し替える。
     * ゲストは CPL=3 なので USER を付ける。paging_set_page() が PDE 側にも
     * USER を伝播させる (実効権限は PDE と PTE の論理積のため)。 */
    for (a = V86_REMAP_START; a < V86_REMAP_END; a += PAGE_SIZE) {
        paging_set_page(a, backing_phys + (a - V86_REMAP_START),
                        PAGE_RW | PTE_USER);
    }

    /* ゲストに渡す前にゼロクリアしておく。IVT/BDA の中身を作るのは
     * BIOS エミュレーションの仕事なので Phase 3 で載せる。 */
    {
        volatile u32 *p = (volatile u32 *)V86_REMAP_START;
        u32 n = (V86_REMAP_END - V86_REMAP_START) / 4;
        u32 i;
        for (i = 0; i < n; i++) {
            p[i] = 0;
        }
    }

    /* VRAM は実物理を直マップ。ここが #GP しないことが性能の前提。 */
    map_identity(V86_VRAM_START,     V86_VRAM_END,     PAGE_RW | PTE_USER);
    map_identity(V86_GVRAM_E_START,  V86_GVRAM_E_END,  PAGE_RW | PTE_USER);

    /* ROM 帯は読み取り専用で見せる。
     * BIOS ROM も含めて実行可能にしておく — 前回プロジェクトは ROM 実行を
     * 排除する「方法C」を実装した翌日に、NP21/W のソースを読んで
     * 「ROM は CPU が直接実行するのが正しい」と判明して全撤回している
     * (docs/tasks/v86v2/01_prior_session_analysis.md)。同じ回り道はしない。 */
    map_identity(V86_EXTROM_START,   V86_EXTROM_END,   PAGE_RO | PTE_USER);
    map_identity(V86_SOUNDROM_START, V86_SOUNDROM_END, PAGE_RO | PTE_USER);
    map_identity(MEM_BIOS_ROM_START, MEM_BIOS_ROM_END + 1, PAGE_RO | PTE_USER);

    return 0;
}

void v86_mem_teardown(void)
{
    u32 a;

    if (!backing_phys) {
        return;
    }

    /* 低位 RAM をアイデンティティマッピングに戻す。
     * paging_init() 当時の属性を再現する: ページ0 は NULL ポインタ検出用に
     * Not-Present、それ以外は R/W。 */
    paging_set_page(0, 0, PAGE_NOT_PRESENT);
    for (a = V86_REMAP_START + PAGE_SIZE; a < V86_REMAP_END; a += PAGE_SIZE) {
        paging_set_page(a, a, PAGE_RW);
    }

    /* VRAM / ROM から USER を剥がす */
    map_identity(V86_VRAM_START,     V86_VRAM_END,     PAGE_RW);
    map_identity(V86_GVRAM_E_START,  V86_GVRAM_E_END,  PAGE_RW);
    map_identity(V86_EXTROM_START,   V86_EXTROM_END,   PAGE_RO);
    map_identity(V86_SOUNDROM_START, V86_SOUNDROM_END, PAGE_RO);
    map_identity(MEM_BIOS_ROM_START, MEM_BIOS_ROM_END + 1, PAGE_RO);

    /* PDE 側の USER も落とす。paging_set_page() は USER を立てる方向にしか
     * 伝播させないので、ここで明示的に戻す。 */
    paging_pde_clear_user(V86_REMAP_START, MEM_BIOS_ROM_END);

    pgalloc_free_n(backing_phys, V86_BACKING_PAGES);
    backing_phys = 0;
}
