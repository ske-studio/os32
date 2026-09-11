#include "types.h"
static u32 host_cr3;
static unsigned int host_if = 0x202U;
static unsigned int host_irq_save(void)
{
    unsigned int f = host_if;
    host_if &= ~0x200U;
    return f;
}
static void host_irq_restore(unsigned int f) { host_if = f; }
#include "paging_host_source.c"
#include "pgalloc_host_source.c"
#include "sys_host_source.c"
static u32 legacy_calls, bootstrap_calls, stage_calls;
static void observed_legacy(u32 kb)
{
    legacy_calls++;
    pgalloc_init(kb);
}
static int observed_bootstrap(struct physmem *m, const struct pgalloc_layout *l,
                              int (*verify)(u32, u32, void *))
{
    bootstrap_calls++;
#ifdef TEST_BOOTSTRAP_FAIL
    (void)m; (void)l; (void)verify;
    return 0;
#else
    return sys_memory_bootstrap_model(m, l, verify);
#endif
}
static int observed_stage(void)
{
    stage_calls++;
#ifdef TEST_STAGE_FAIL
    return 0;
#else
    return sys_memory_stage_online();
#endif
}
#define pgalloc_init observed_legacy
#define sys_memory_bootstrap_model observed_bootstrap
#define sys_memory_stage_online observed_stage
#include "memory_boot_host_source.c"
#undef pgalloc_init
#undef sys_memory_bootstrap_model
#undef sys_memory_stage_online
__asm__(".globl __sqlite_start\n.set __sqlite_start, 0x200000\n"
        ".globl __sqlite_end\n.set __sqlite_end, 0x240000\n"
        ".globl __bss_end\n.set __bss_end, 0x180000\n");
static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}
static void report(const char *s, u32 n)
{
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(s), "d"(n) : "memory");
}
#define CHECK(x) do { if (!(x)) { report("FAIL " #x "\n", sizeof("FAIL " #x "\n") - 1); die(1); } } while (0)
extern int memory_boot_init(u32) __attribute__((weak));
static void host_failstop(void)
{
    CHECK(bootstrap_calls == 1 && !legacy_calls && !pgalloc_alloc_page());
#ifdef TEST_BOOTSTRAP_FAIL
    CHECK(!initialized && !stage_calls && !sys_model_staged);
#else
    CHECK(stage_calls == 1 && pgalloc_model_state() == PGALLOC_BOOTSTRAP);
#endif
    CHECK(host_if == 0x202U);
    die(0);
}
#include "kernel_boot_gate.c"
void _start(void)
{
    u32 args[6] = {0x800000, 0x700000, 3, 0x32, 0xffffffffUL, 0};
    u32 result, count;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(90), "b"(args) : "memory");
    CHECK(result == 0x800000);
    paging_init(TEST_KB);
    sys_mem_kb = TEST_KB;
    CHECK(memory_boot_init != 0);
    /* 巨大なヒント単独では高位 RAM は 1 ページも昇格しない。 */
    CHECK(!boot_high_end);
#ifdef TEST_TABLES
    /* 表 (bitmap 2 面 + 恒等 PT の workspace) の大きさは検出量から出る。
     * 人為的な上限は無く、置ける場所 — アプリ帯の最大上端から legacy
     * アリーナ上端までの低位 RAM — の広さだけが覆える量を決める。 */
    {
        u32 top, room, fit, huge;
        top = MEM_SYSTEM_SPACE_BASE / PAGE_SIZE;      /* 15MiB: アリーナ上端 */
        room = top - MEM_APP_BAND_MAX_TOP / PAGE_SIZE;
        /* 32MiB / 128MiB は丸ごと通る (切り詰め無し)。 */
        CHECK(memory_boot_high_fit(0x2000, top) == 0x2000);
        CHECK(memory_boot_table_pages(0x2000) <= room);
        CHECK(memory_boot_workspace_pages(0x2000) == MEMORY_BOOT_WORKSPACE_PAGES);
        CHECK(memory_boot_high_fit(0x8000, top) == 0x8000);
        CHECK(memory_boot_table_pages(0x8000) <= room);
        CHECK(memory_boot_workspace_pages(0x8000) == MEMORY_BOOT_WORKSPACE_PAGES +
              0x8000 / PTE_COUNT - PAGING_BOOT_PT_COUNT);
        /* 4GiB 相当 (上端は最上位の ROM/MMIO 帯)。 */
        huge = MEM_PHYS_MMIO_TOP / PAGE_SIZE;
        fit = memory_boot_high_fit(huge, top);
        CHECK(fit > MEM_HIGH_RAM_BASE / PAGE_SIZE && fit <= huge);
        /* 表は RAM の外に出ない。かつ置ける限りは目一杯まで取る。 */
        CHECK(memory_boot_table_pages(fit) <= room);
        CHECK(memory_boot_table_pages(fit + PTE_COUNT) > room);
        /* 2GiB 超を覆える = 人為的に小さい上限は残っていない。 */
        CHECK(fit > 0x80000);
        /* 置き場所が無い構成は 0 (legacy へ落ちる)。でっち上げはしない。 */
        CHECK(memory_boot_high_fit(huge, MEM_APP_BAND_MAX_TOP / PAGE_SIZE) == 0);
        CHECK(host_if == 0x202U);
        die(0);
    }
#endif
/* 15MiB clamp なので legacy 上端は 0xF00。窓の撤去 (2026-09-09) で
 * metadata が 0xEFF、workspace が 0xEFE に上がった (旧: 0xEBF / 0xEBE)。 */
#if defined(TEST_METADATA_PTE) || defined(TEST_WORKSPACE_PTE)
    *(u32 *)0xEFF000 = 0xA55AA55A;
#ifdef TEST_METADATA_PTE
    page_tables[3][0xEFF % PTE_COUNT] |= PTE_USER;
#endif
#ifdef TEST_WORKSPACE_PTE
    page_tables[3][0xEFE % PTE_COUNT] &= ~PTE_PRESENT;
#endif
    CHECK(!memory_boot_init(TEST_KB));
    CHECK(bootstrap_calls == 1 && !stage_calls && !legacy_calls);
    CHECK(!initialized && !sys_model_staged && !pgalloc_alloc_page());
    CHECK(*(u32 *)0xEFF000 == 0xA55AA55A);
    die(0);
#endif
#if defined(TEST_BOOTSTRAP_FAIL) || defined(TEST_STAGE_FAIL)
    /* Execute the actual kernel gate: reaching shm is an exit(7) failure. */
    host_kernel_boot(TEST_KB);
    CHECK(0);
#endif
#ifdef TEST_HIGH
    /* The detector is the only source of a high attestation; the host cannot
     * touch the BIOS work area, so stand in for it with the same value. */
    boot_high_end = TEST_KB / (PAGE_SIZE / 1024UL);
#endif
    CHECK(memory_boot_init(TEST_KB));
#ifdef TEST_HIGH
    /* K6-RAM: 16MiB 超を検出量ぶんそのまま池に入れる。人為的な上限は無く、
     * 引かれてよいのは名前の付いたデバイス予約だけ。 */
    {
        u32 p, i, hole, high, arena, limit;
        hole = MEM_SYSTEM_SPACE_BASE / PAGE_SIZE;
        high = MEM_HIGH_RAM_BASE / PAGE_SIZE;
        limit = pgalloc_limit_pfn();
        CHECK(pgalloc_model_state() == PGALLOC_ONLINE);
        CHECK(limit == TEST_KB / (PAGE_SIZE / 1024));
        /* M5: 15-16MiB の穴は RAM として配られない */
        CHECK(physmem_count(&device_boot_map, hole, high, PHYSMEM_RESERVED, &count));
        CHECK(count == high - hole);
        p = 99;
        CHECK(!pgalloc_alloc_n_pfn(1, hole, high, &p) && p == 99);
        /* 最上位の ROM / PCI MMIO 帯も RAM ではない */
        CHECK(physmem_count(&device_boot_map, MEM_PHYS_MMIO_TOP / PAGE_SIZE,
                            PHYSMEM_MAX_PFN, PHYSMEM_MMIO, &count));
        CHECK(count == PHYSMEM_MAX_PFN - MEM_PHYS_MMIO_TOP / PAGE_SIZE);
        /* 検出量の最終ページまで配れる (切り詰めが無いことの証明) */
        CHECK(pgalloc_alloc_n_pfn(1, limit - 1, limit, &p) && p == limit - 1);
        CHECK(pgalloc_free_n_pfn(p, 1));
        arena = workspace_first - MEM_APP_BAND_BASE / PAGE_SIZE;
        CHECK(pgalloc_total_pages() > arena);
        /* 連続アリーナ (exec のレイアウト) と表の置き場所は不変 */
        CHECK(sys_usable_mem_end() == workspace_first * PAGE_SIZE);
        CHECK((u32)eligible == workspace_end * PAGE_SIZE);
        CHECK((u32)eligible >= MEM_APP_BAND_MAX_TOP);
        CHECK((u32)eligible < MEM_SYSTEM_SPACE_BASE);
        CHECK(workspace_first >= MEM_APP_BAND_MAX_TOP / PAGE_SIZE);
        CHECK(workspace_end <= MEM_SYSTEM_SPACE_BASE / PAGE_SIZE);
        /* ブート窓より上の identity PT は workspace (RAM) から動的に取る */
        for (i = PAGING_BOOT_PT_COUNT; i < limit / PTE_COUNT; i++) {
            CHECK((u32)page_tables[i] >= workspace_first * PAGE_SIZE);
            CHECK((u32)page_tables[i] < workspace_end * PAGE_SIZE);
        }
        CHECK(host_if == 0x202U);
        die(0);
    }
#endif
#ifdef TEST_LEGACY
    CHECK(legacy_calls == 1 && !bootstrap_calls && !stage_calls);
    CHECK(initialized && !model_mode && !sys_model_staged);
    CHECK(sys_usable_mem_end() == TEST_KB * 1024);
#else
    CHECK(pgalloc_model_state() == PGALLOC_ONLINE);
    CHECK(sys_usable_mem_end() == 0xEFE000);
    CHECK(workspace_first == 0xEFE && workspace_end == 0xEFF);
    CHECK((u32)eligible == 0xEFF000);
    CHECK(physmem_count(&device_boot_map, 0xF00, 0x100000, PHYSMEM_UNKNOWN, &count));
    CHECK(count == 0x100000 - 0xF00);
    /* 窓が無くなったのでアリーナは 15MiB ちょうどで終わる (排他上限 0xF00)。
     * 未証明の [15,16)MiB へは踏み込まない、という意図は変わらない。 */
    CHECK(pgalloc_limit_pfn() == 0xF00);
#endif
    (void)count;
    CHECK(pgalloc_alloc_page() != 0);
    CHECK(host_if == 0x202U);
    die(0);
}
