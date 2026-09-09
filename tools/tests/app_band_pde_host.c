/* ========================================================================
 *  app_band_pde_host.c — アプリ帯の可変 PDE 化 (docs/tasks/memory/APP_BAND_PDE.md)
 *
 *  実物の kernel/paging.c + kernel/pgalloc.c + kernel/physmem.c を ILP32 で
 *  そのままコンパイルし、特権 asm だけホスト用に差し替えて動かす
 *  (tools/tests/paging_bounds_host.c と同じ作り)。
 *
 *  見るもの:
 *    A. 枚数計算 paging_app_band_pdes() が票 §4-1 の規則どおりか
 *    B. 枚数 1 のとき現行と完全に同じレイアウトか (回帰ゼロ = 最重要)
 *    C. 枚数 2 のとき 2 枚目の PDE がアプリ固有 PT に差し替わるか
 *    D. USER が当該アプリの PD にだけ伝播し master へ漏れないか (票 §2)
 *    E. 引数不正・物理ページ不足で master も pgalloc も汚さずに失敗するか
 *    F. destroy が枚数分の PT + PD をきっちり返すか
 *    G. paging_app_band_selftest() (ブート時 kselftest に載せるもの)
 * ======================================================================== */
#include "types.h"
static u32 host_cr3;
#include "paging_host_source.c"
__asm__(".globl __sqlite_start\n.set __sqlite_start, 0x200000\n"
        ".globl __sqlite_end\n.set __sqlite_end, 0x240000\n"
        ".globl __bss_end\n.set __bss_end, 0x180000\n");
static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}
static void report(const char *text, u32 len)
{
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(text), "d"(len) : "memory");
}
#define SAY(s) report(s "\n", sizeof(s "\n") - 1)
#define CHECK(x) do { if (!(x)) { SAY("FAIL: " #x); die(1); } } while (0)
#include "pgalloc_host_source.c"
void __cdecl kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }
#define used used_pages

/* 帯の 2 枚目に当たる PDE / 仮想番地 (最大枚数が 2 未満なら試験は縮む) */
#define PDE2      (APP_BAND_PDE + 1)
#define BAND2_VA  (MEM_APP_BAND_BASE + MEM_APP_BAND_PDE_SIZE)

void _start(void)
{
    /* 恒等で読み書きできる実メモリを 0x400000 から 12MB 張る (paging_bounds と同じ) */
    u32 args[6] = {0x400000, 0xC00000, 3, 0x32, 0xFFFFFFFF, 0};
    u32 result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(90), "b"(args) : "memory");
    CHECK(result == 0x400000);

    paging_init(16384);
    pgalloc_init(16384);

    /* ---- A. 枚数計算 (純関数) ---------------------------------------- */
    /* 帯を伸ばせる上限 (子の claim 範囲 A の末尾) を 14MB として与える。 */
    {
        u32 top = 0xE00000UL;
        u32 code_end = MEM_EXEC_LOAD_ADDR;   /* 本体ゼロサイズ相当 */

        /* heap_size 無指定 (0) は従来どおり必ず 1 枚 — 回帰ゼロの要 */
        CHECK(paging_app_band_pdes(code_end, 0, top) == 1);
        CHECK(paging_app_band_pdes(0x7B0000UL, 0, top) == 1);

        /* 1 枚に収まる要求は 1 枚のまま */
        CHECK(paging_app_band_pdes(code_end, MEM_EXEC_HEAP_MIN, top) == 1);

        /* 1 枚の残りちょうど: heap_top(0x7BF000) - code_end - sbrk - guard */
        {
            u32 fit = (MEM_APP_BAND_TOP - MEM_EXEC_STACK_SIZE - PAGE_SIZE)
                      - code_end - MEM_EXEC_SBRK_MIN - PAGE_SIZE;
            CHECK(paging_app_band_pdes(code_end, fit, top) == 1);
            CHECK(paging_app_band_pdes(code_end, fit + PAGE_SIZE, top) == 2);
        }

        /* 4MB 要求は 2 枚 */
        CHECK(paging_app_band_pdes(code_end, 0x400000UL, top) == 2);

        /* 上限で頭打ち (票 §4-1: 最大 2 枚) */
        CHECK(paging_app_band_pdes(code_end, 0xF0000000UL, top)
              == MEM_APP_BAND_MAX_PDES);

        /* 空き RAM が足りなければ伸ばさない (8MB 構成) */
        CHECK(paging_app_band_pdes(code_end, 0x400000UL, 0x6BF000UL) == 1);
        /* 上限が帯の底より下でも 1 枚は返す (従来の挙動を壊さない) */
        CHECK(paging_app_band_pdes(code_end, 0x400000UL, 0) == 1);
        CHECK(paging_app_band_pdes(code_end, 0x400000UL, MEM_APP_BAND_BASE) == 1);

        /* 桁あふれで大きい枚数を返さない */
        CHECK(paging_app_band_pdes(0xFFFFF000UL, 0x400000UL, top) >= 1);
        CHECK(paging_app_band_pdes(0xFFFFF000UL, 0x400000UL, top)
              <= MEM_APP_BAND_MAX_PDES);
    }

    /* ---- B. 枚数 1: 現行と完全に同じレイアウト ------------------------ */
    {
        struct addrspace as;
        u32 before = used;
        u32 master_pde2 = page_directory[PDE2];
        u32 *pd;

        CHECK(paging_addrspace_create(&as) == 0);
        pd = (u32 *)as.pd_phys;
        CHECK(as.app_pde == APP_BAND_PDE);
        CHECK(as.app_pde_count == 1);
        CHECK(as.app_pt_phys[0] != 0);
        /* 先頭 PDE だけがアプリ PT に差し替わる (PRESENT|RW, USER はまだ) */
        CHECK(pd[APP_BAND_PDE] == (as.app_pt_phys[0] | PAGE_RW));
        /* 2 枚目は master と同じ = 共有のまま */
        CHECK(pd[PDE2] == master_pde2);
        CHECK(page_directory[PDE2] == master_pde2);
        /* アプリ PT は master と同一 identity で初期化される */
        CHECK(((u32 *)as.app_pt_phys[0])[0] == page_tables[APP_BAND_PDE][0]);
        /* 2 枚目の帯は「共有 PT」扱い = master の PT へ書かれる (従来動作) */
        CHECK(paging_addrspace_map_user(&as, BAND2_VA, BAND2_VA,
                                        PAGE_RW | PTE_USER) == 0);
        CHECK(page_tables[PDE2][0] == (BAND2_VA | PAGE_RW | PTE_USER));
        CHECK(!(page_directory[PDE2] & PTE_USER));   /* master PDE へは漏れない */
        CHECK(pd[PDE2] & PTE_USER);
        /* 後始末 (master PT を元に戻す) */
        page_tables[PDE2][0] = BAND2_VA | PAGE_RW;
        page_directory[PDE2] = master_pde2;
        paging_addrspace_destroy(&as);
        CHECK(as.pd_phys == 0 && as.app_pde_count == 0);
        CHECK(used == before);          /* PD 1 + PT 1 をきっちり返す */
    }

#if MEM_APP_BAND_MAX_PDES >= 2
    /* ---- C/D. 枚数 2 ------------------------------------------------- */
    {
        struct addrspace as;
        u32 before = used;
        u32 master_pde1 = page_directory[APP_BAND_PDE];
        u32 master_pde2 = page_directory[PDE2];
        u32 master_pt2_0 = page_tables[PDE2][0];
        u32 *pd, *pt1, *pt2;

        CHECK(paging_addrspace_create_n(&as, 2) == 0);
        CHECK(used == before + 3);      /* PD + PT 2 枚 */
        pd = (u32 *)as.pd_phys;
        pt1 = (u32 *)as.app_pt_phys[0];
        pt2 = (u32 *)as.app_pt_phys[1];
        CHECK(as.app_pde == APP_BAND_PDE && as.app_pde_count == 2);
        CHECK(pt1 != pt2);
        CHECK(pd[APP_BAND_PDE] == ((u32)pt1 | PAGE_RW));
        CHECK(pd[PDE2] == ((u32)pt2 | PAGE_RW));
        /* master は無傷 */
        CHECK(page_directory[APP_BAND_PDE] == master_pde1);
        CHECK(page_directory[PDE2] == master_pde2);
        /* 2 枚目も master と同一 identity から始まる (CPL=0 で CR3 を載せても
         * カーネルから見た番地が変わらないこと = V1) */
        CHECK(pt2[0] == page_tables[PDE2][0]);
        CHECK(pt2[1023] == page_tables[PDE2][1023]);

        /* 2 枚目に USER を張る → アプリ固有 PT に書かれ、master PT は無傷 */
        CHECK(paging_addrspace_map_user(&as, BAND2_VA, BAND2_VA,
                                        PAGE_RW | PTE_USER) == 0);
        CHECK(pt2[0] == (BAND2_VA | PAGE_RW | PTE_USER));
        CHECK(page_tables[PDE2][0] == master_pt2_0);
        CHECK(pd[PDE2] & PTE_USER);
        CHECK(!(page_directory[PDE2] & PTE_USER));

        /* PDE をまたぐ range も全部アプリ固有 PT へ落ちる */
        CHECK(paging_addrspace_map_user_range(&as, BAND2_VA - PAGE_SIZE,
                                              BAND2_VA + PAGE_SIZE,
                                              PAGE_RW | PTE_USER) == 0);
        CHECK(pt1[1023] == ((BAND2_VA - PAGE_SIZE) | PAGE_RW | PTE_USER));
        CHECK(pt2[0] == (BAND2_VA | PAGE_RW | PTE_USER));
        CHECK(page_tables[APP_BAND_PDE][1023] != pt1[1023]);
        CHECK(page_tables[PDE2][0] == master_pt2_0);

        /* 帯の外 (共有帯) は従来どおり master の PT を共有する */
        CHECK(paging_addrspace_map_user(&as, 0xA8000UL, 0xA8000UL,
                                        PAGE_RW | PTE_USER) == 0);
        CHECK(page_tables[0][0xA8] == (0xA8000UL | PAGE_RW | PTE_USER));
        CHECK(!(page_directory[0] & PTE_USER));

        paging_addrspace_destroy(&as);
        CHECK(used == before);
        CHECK(page_directory[APP_BAND_PDE] == master_pde1);
        CHECK(page_directory[PDE2] == master_pde2);
        CHECK(page_tables[PDE2][0] == master_pt2_0);
    }

    /* ---- E. 引数不正 / 物理ページ不足 -------------------------------- */
    {
        struct addrspace as;
        u32 before = used;

        CHECK(paging_addrspace_create_n(&as, 0) == -1);
        CHECK(paging_addrspace_create_n(&as, MEM_APP_BAND_MAX_PDES + 1) == -1);
        CHECK(paging_addrspace_create_n((struct addrspace *)0, 1) == -1);
        CHECK(used == before);
        CHECK(live_addrspaces == 0);

        /* PD と 1 枚目は取れるが 2 枚目で尽きる → 全部返して失敗。
         * 空きをちょうど 2 ページまで吸い出して作る。 */
        {
            u32 n = pgalloc_free_pages();
            u32 blob;
            CHECK(n > 2);
            blob = pgalloc_alloc_n((int)(n - 2));
            CHECK(blob != 0);
            CHECK(pgalloc_free_pages() == 2);
            CHECK(paging_addrspace_create_n(&as, 2) == -1);
            CHECK(pgalloc_free_pages() == 2);
            CHECK(as.pd_phys == 0 && as.app_pde_count == 0);
            CHECK(live_addrspaces == 0);
            /* 1 枚なら通る (PD + PT の 2 ページちょうど) */
            CHECK(paging_addrspace_create_n(&as, 1) == 0);
            CHECK(pgalloc_free_pages() == 0);
            paging_addrspace_destroy(&as);
            pgalloc_free_n(blob, (int)(n - 2));
            CHECK(used == before);
        }
    }
#endif

    /* ---- F/G. 自己診断 ------------------------------------------------ */
    {
        u32 before = used;
        CHECK(paging_app_band_selftest() == 0);
        CHECK(used == before);
        CHECK(live_addrspaces == 0);
        /* 既存の 2 本も引き続き通ること (回帰ゼロ) */
        CHECK(paging_map_user_keep_selftest() == 0);
        CHECK(paging_pd_clone_selftest() == 0);
        CHECK(used == before);
    }

    SAY("PASS: pde count rule (heap_size 0 = 1 pde, ram-capped, max clamped)");
    SAY("PASS: 1 pde layout identical to the current one");
    SAY("PASS: 2 pdes get private PTs, USER never reaches the master PDE/PT");
    SAY("PASS: bad count / out of pages roll back leaving master untouched");
    SAY("PASS: app band selftest, keep/clone selftests still green");
    die(0);
}
