#include "types.h"
static u32 host_cr3;
#include "paging_host_source.c"
__asm__(".globl __sqlite_start\n.set __sqlite_start, 0x200000\n"
        ".globl __sqlite_end\n.set __sqlite_end, 0x240000\n"
        ".globl __bss_end\n.set __bss_end, 0x180000\n");
static u32 limit = 16, calls;
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
#define pgalloc_alloc_n_pfn actual_alloc_n_pfn
#include "pgalloc_host_source.c"
#undef pgalloc_alloc_n_pfn
void __cdecl kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }
int pgalloc_alloc_n_pfn(int n, u32 first, u32 end, u32 *pfn)
{
    calls++;
    if (used_pages >= limit) return 0;
    return actual_alloc_n_pfn(n, first, end, pfn);
}
#define used used_pages
void _start(void)
{
    u32 args[6] = {0x400000, 0xC00000, 3, 0x32, 0xFFFFFFFF, 0};
    u32 result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(90), "b"(args) : "memory");
    CHECK(result == 0x400000);
    paging_init(16384);
    /* K6-RAM: paging_init が張るのは「ブート窓の内側 x 検出量」だけで、
     * RAM の上限ではない。16MiB 報告ならそこまで (従来と同じ)。 */
    CHECK(paging_boot_identity_end() == 16384UL * 1024 / PAGE_SIZE);
    CHECK(paging_is_present(PAGING_BOOT_MAP_SIZE / 2 - PAGE_SIZE));
    CHECK(!paging_is_present(PAGING_BOOT_MAP_SIZE / 2));
    CHECK(!paging_is_present(0xFFFFFFFFUL));
    CHECK(paging_set_page(0xFFFFF000UL, 0, PAGE_RW) == -1);
    CHECK(used == 0);
    CHECK(sizeof(pt_raw) == 8 * PAGE_SIZE + PAGE_SIZE - 1);
    pgalloc_init(16384);
    CHECK(paging_map_phys(0xFFFFF000UL, 0xFFFFF000UL, 1, PAGE_RW | PTE_PCD) == 0);
    CHECK(paging_is_present(0xFFFFFFFFUL));
    CHECK(paging_map_phys(0xFFFFF000UL, 0, 2, PAGE_RW) == -1);
    CHECK(paging_map_phys(0, 0xFFFFF000UL, 2, PAGE_RW) == -1);
    CHECK(paging_map_phys(0, 0, 0x100000UL + 1, PAGE_RW) == -1);
    CHECK(paging_map_range(0x2000, 0x4000, 0xFFFFF000UL, PAGE_RW) == -1);
    CHECK(paging_map_range(0x4000, 0x2000, 0, PAGE_RW) == -1);
    CHECK(paging_map_range(0xFFFFF000UL, 0xFFFFFFFFUL, 0xFFFFF000UL, PAGE_RW | PTE_PCD) == 0);
    CHECK(paging_set_readonly(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
    CHECK(page_tables[1023][1023] == (0xFFFFF000UL | PAGE_RO | PTE_PCD));
    CHECK(paging_set_not_present(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
    CHECK(!paging_is_present(0xFFFFFFFFUL));
    CHECK(paging_set_readonly(0x2000000, 0x2000FFF) == 0);
    CHECK(!paging_is_present(0x2000000));
    CHECK(paging_set_readonly(0x4000, 0x2000) == -1);
    CHECK(paging_set_not_present(0x4000, 0x2000) == -1);
    CHECK(paging_pde_clear_user(0x4000, 0x2000) == -1);
    {
        struct addrspace as;
        u32 before = used, before_calls;
        CHECK(paging_addrspace_create(&as) == 0);
        before_calls = calls;
        CHECK(paging_map_phys(0x2000000, 0, 1, PAGE_RW) == -1);
        CHECK(calls == before_calls);
        CHECK(paging_set_page(0xFFFFF000UL, 0, PAGE_RW | PTE_PCD) == 0);
        paging_addrspace_destroy(&as);
        CHECK(used == before);
        CHECK(paging_map_phys(0x2000000, 0, 1, PAGE_RW) == 0);
        CHECK(used == before + 1);
        before = used;
        limit = used + 1;
        CHECK(paging_map_phys(0x27FF000, 0, 2, PAGE_RW) == -1);
        CHECK(used == before);
        CHECK(!page_tables[9] && !page_tables[10]);
        CHECK(!page_directory[9] && !page_directory[10]);
        limit = 16;
    }
    {
        struct addrspace as;
        u32 saved;
        CHECK(paging_addrspace_create(&as) == 0);
        CHECK(paging_addrspace_map_user_range(&as, 0x4000, 0x2000, PAGE_RW | PTE_USER) == -1);
        saved = page_tables[8][1023];
        CHECK(paging_addrspace_map_user_range(&as, 0x23FF000, 0x2401000, PAGE_RW | PTE_USER) == -1);
        CHECK(page_tables[8][1023] == saved);
        CHECK(paging_addrspace_map_user(&as, 0x2400000, 0, PAGE_RW | PTE_USER) == -1);
        CHECK(paging_addrspace_map_user_keep(&as, 0xFFFFF000UL, 0xFFFFFFFFUL, PAGE_RW | PTE_USER) == 0);
        CHECK((page_tables[1023][1023] & (PTE_USER | PTE_PCD)) == (PTE_USER | PTE_PCD));
        CHECK(!(page_directory[1023] & PTE_USER));
        paging_addrspace_destroy(&as);
        CHECK(paging_map_user_keep_selftest() == 0);
    }
    {
        u32 d2 = page_directory[2], d3 = page_directory[3];
        u32 before = used;
        page_directory[2] = 0;
        page_directory[3] = 0;
        CHECK(paging_set_page(0x2400000, 0, PAGE_RW) == -1);
        CHECK(used == before);
        page_directory[2] = d2;
        page_directory[3] = d3;
        host_cr3 = 0x123000;
        CHECK(paging_set_page(0x2400000, 0, PAGE_RW) == -1);
        CHECK(used == before);
        host_cr3 = paging_kernel_pd_phys();
        CHECK(paging_set_page(0x2400000, 0, PAGE_RW) == 0);
        CHECK((u32)page_tables[9] >= MEM_APP_BAND_TOP);
    }
    {
        struct addrspace a, b;
        u32 before = used, c = calls;
        CHECK(paging_addrspace_create(&a) == 0);
        CHECK(paging_addrspace_create(&b) == 0);
        paging_addrspace_destroy(&a);
        CHECK(paging_set_page(0x2800000, 0, PAGE_RW) == -1);
        CHECK(calls == c);
        host_cr3 = b.pd_phys;
        paging_addrspace_destroy(&b);
        CHECK(b.pd_phys != 0);
        host_cr3 = paging_kernel_pd_phys();
        CHECK(paging_set_page(0x2800000, 0, PAGE_RW) == -1);
        paging_addrspace_destroy(&b);
        paging_addrspace_destroy(&b);
        CHECK(used == before);
        CHECK(paging_set_page(0x2800000, 0, PAGE_RW) == 0);
        CHECK(used == before + 1);
    }
    {
        u32 old = page_tables[10][1023], before = used;
        limit = used + 1;
        CHECK(paging_map_phys(0x2BFF000, 0, 1026, PAGE_RW | PTE_USER) == -1);
        CHECK(used == before);
        CHECK(page_tables[10][1023] == old);
        CHECK(!page_tables[11] && !page_tables[12]);
        limit = 16;
        old = page_tables[0][2];
        CHECK(paging_map_range(0x2000, 0x4000, 0xFFFFF000UL, PAGE_RW) == -1);
        CHECK(page_tables[0][2] == old);
        CHECK(paging_set_readonly(0x1FFF000, 0x1FFFFFF) == 0);
        CHECK(!paging_is_present(0x1FFF000));
        CHECK(paging_map_phys(0x1000000, 0x1000000, 512, PAGE_RW | PTE_PCD) == 0);
        CHECK((page_tables[4][511] & (PAGE_RW | PTE_PCD | PTE_USER)) == (PAGE_RW | PTE_PCD));
    }
    {
        u32 flags = PAGE_RW | PTE_USER | PTE_PCD | PTE_PWT | PTE_ACCESSED;
        CHECK(paging_set_page(0xFFFFF000UL, 0x12345000, flags) == 0);
        CHECK(paging_set_readonly(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
        CHECK(page_tables[1023][1023] == (0x12345000 | (flags & ~(u32)PTE_RW)));
        CHECK(paging_set_not_present(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
        CHECK(paging_set_readonly(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
        CHECK(!paging_is_present(0xFFFFFFFFUL));
        CHECK(paging_pde_clear_user(0xFFFFF000UL, 0xFFFFFFFFUL) == 0);
        CHECK(!(page_directory[1023] & PTE_USER));
    }
    {
        struct addrspace as;
        u32 *pt = page_tables[1023];
        u32 pte = pt[1023], pde = page_directory[1023];
        u32 before, before_calls, cr3, live;
        CHECK(paging_addrspace_create(&as) == 0);
        host_cr3 = as.pd_phys;
        before = used;
        before_calls = calls;
        cr3 = host_cr3;
        live = live_addrspaces;
        paging_init(1024);
        CHECK(page_tables[1023] == pt);
        CHECK(pt[1023] == pte && page_directory[1023] == pde);
        CHECK(live_addrspaces == live && used == before);
        CHECK(calls == before_calls && host_cr3 == cr3);
        CHECK(paging_enabled());
        CHECK(((u32 *)as.pd_phys)[1023] == pde);
        host_cr3 = paging_kernel_pd_phys();
        paging_addrspace_destroy(&as);
        CHECK(live_addrspaces == 0 && used == before - 2);
    }
    {
        /* 票 S0-K / 実機 K2 (2026-09-13): KAPI のポインタ検証が見る 2 ビット。
         * **exec が実機で作るのと同じ順序** で AS を組み、CR3 に載せてから
         * `paging_current_pte_flags` で歩く:
         *   create_n → clear_app_band → app_map_region 相当 (per-app 物理を
         *   USER で 3 領域) → shlib_addrspace_attach 相当 (帯の下側を RO+USER)
         * 以前ここは `paging_addrspace_pte_flags(&as, v)` を呼んでいて、
         * **控え (as->app_pt_phys[]) から PT を選んでいた**。控えと実配置が
         * ずれると健全なページを非 present と誤判定する — 実機はまさにそれで、
         * .rodata の 0x501000 が「非 present」と出た。MMU と同じ辿り方
         * (CR3 → PDE → PDE が指す PT) なら控えが何であれ答は一致する。 */
        struct addrspace as;
        u32 code = 0x500000, sbrk_end = 0x520000;
        u32 heap = 0x600000, heap_end = 0x610000;
        u32 stack = 0x7C0000, stack_top = 0x800000;
        u32 saved_cr3 = host_cr3;
        u32 flags, other_pt;

        CHECK(paging_addrspace_create_n(&as, 1) == 0);
        CHECK(as.app_pde == APP_BAND_PDE && as.app_pde_count == 1);
        CHECK(paging_addrspace_clear_app_band(&as) == 0);
        CHECK(paging_addrspace_map_user_range_phys(&as, code, sbrk_end,
                                                   0x900000, PAGE_RW | PTE_USER) == 0);
        CHECK(paging_addrspace_map_user_range_phys(&as, heap, heap_end,
                                                   0x980000, PAGE_RW | PTE_USER) == 0);
        CHECK(paging_addrspace_map_user_range_phys(&as, stack, stack_top,
                                                   0x9A0000, PAGE_RW | PTE_USER) == 0);
        /* shlib 相当: 帯の下側 (0x400000-) を RO + USER で張り直す */
        CHECK(paging_addrspace_map_user_range(&as, MEM_SHLIB_BASE,
                                              MEM_SHLIB_BASE + 0x2000,
                                              PAGE_RO | PTE_USER) == 0);

        /* ---- ここから「いま効いている表」を歩く (syscall 中と同じ状態) ---- */
        host_cr3 = as.pd_phys;
        /* 実機で落ちた番地と同じ形 = ロード先の **次のページ** の .rodata */
        flags = paging_current_pte_flags(code + 0x140E);
        CHECK((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        flags = paging_current_pte_flags(code);
        CHECK((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        flags = paging_current_pte_flags(sbrk_end - 1);
        CHECK((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        flags = paging_current_pte_flags(stack_top - 1);
        CHECK((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        flags = paging_current_pte_flags(MEM_SHLIB_BASE);
        CHECK((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        CHECK(!(flags & PTE_RW));                 /* shlib text は RO */
        /* 張っていない隙間 (sbrk 上限〜heap、= guard) は非 present */
        CHECK(paging_current_pte_flags(sbrk_end) == 0);
        CHECK(paging_current_pte_flags(heap_end) == 0);

        /* **控えではなく PDE を辿っている** ことの証拠: PDE の指す PT だけを
         * 別の (全部 0 の) PT に差し替えると答が変わる。控えから選ぶ実装は
         * ここで古い PT を読み続けてしまう (実機 K2 の壊れ方)。 */
        other_pt = pgalloc_alloc_page();
        CHECK(other_pt != 0);
        {
            u32 *zero = (u32 *)other_pt;
            int z;
            for (z = 0; z < PTE_COUNT; z++) zero[z] = 0;
        }
        {
            u32 *pd = (u32 *)as.pd_phys;
            u32 saved_pde = pd[APP_BAND_PDE];
            pd[APP_BAND_PDE] = (other_pt & 0xFFFFF000UL) |
                               (saved_pde & 0xFFFu);
            CHECK(paging_current_pte_flags(code + 0x140E) == 0);
            pd[APP_BAND_PDE] = saved_pde;
            CHECK((paging_current_pte_flags(code + 0x140E) &
                   (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        }
        /* PDE.PS は明示的に拒否 (この OS は 4MB ページを張らない) */
        {
            u32 *pd = (u32 *)as.pd_phys;
            u32 saved_pde = pd[APP_BAND_PDE];
            pd[APP_BAND_PDE] = saved_pde | PTE_PS;
            CHECK(paging_current_pte_flags(code) == 0);
            pd[APP_BAND_PDE] = saved_pde;
        }
        pgalloc_free_page(other_pt);

        host_cr3 = saved_cr3;
        /* master に戻すと同じ番地はアプリ帯の USER 写像を持たない */
        CHECK(!(paging_current_pte_flags(code + 0x140E) & PTE_USER));
        paging_addrspace_destroy(&as);
    }
    SAY("PASS: one-shot init preserves dynamic PT, live AS, CR3, allocator");
    SAY("PASS: final-page, virtual/physical overflow, range preflight");
    SAY("PASS: sparse NP, attribute flags, USER/PCD, user-range preflight");
    SAY("PASS: real allocator rollback, live-AS lifecycle, master-only backing");
    die(0);
}
