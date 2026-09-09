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
#if defined(TEST_METADATA_PTE) || defined(TEST_WORKSPACE_PTE) || defined(TEST_HOT_PTE)
    *(u32 *)0xEBF000 = 0xA55AA55A;
#ifdef TEST_METADATA_PTE
    page_tables[3][0xEBF % PTE_COUNT] |= PTE_USER;
#endif
#ifdef TEST_WORKSPACE_PTE
    page_tables[3][0xEBE % PTE_COUNT] &= ~PTE_PRESENT;
#endif
#ifdef TEST_HOT_PTE
    page_tables[3][0xEFF % PTE_COUNT] |= PTE_PCD;
#endif
    CHECK(!memory_boot_init(TEST_KB));
    CHECK(bootstrap_calls == 1 && !stage_calls && !legacy_calls);
    CHECK(!initialized && !sys_model_staged && !pgalloc_alloc_page());
    CHECK(*(u32 *)0xEBF000 == 0xA55AA55A);
    die(0);
#endif
#if defined(TEST_BOOTSTRAP_FAIL) || defined(TEST_STAGE_FAIL)
    /* Execute the actual kernel gate: reaching shm is an exit(7) failure. */
    host_kernel_boot(TEST_KB);
    CHECK(0);
#endif
    CHECK(memory_boot_init(TEST_KB));
#ifdef TEST_LEGACY
    CHECK(legacy_calls == 1 && !bootstrap_calls && !stage_calls);
    CHECK(initialized && !model_mode && !sys_model_staged);
    CHECK(sys_usable_mem_end() == (TEST_KB * 1024 - MEM_HOTDEPLOY_SIZE));
#else
    CHECK(pgalloc_model_state() == PGALLOC_ONLINE);
    CHECK(sys_hotdeploy_base() == 0xEC0000);
    CHECK(sys_usable_mem_end() == 0xEBE000);
    CHECK(workspace_first == 0xEBE && workspace_end == 0xEBF);
    CHECK((u32)eligible == 0xEBF000);
    CHECK(physmem_count(&device_boot_map, 0xF00, 0x100000, PHYSMEM_UNKNOWN, &count));
    CHECK(count == 0x100000 - 0xF00);
    CHECK(pgalloc_limit_pfn() < 0xF00);
#endif
    (void)count;
    CHECK(pgalloc_alloc_page() != 0);
    CHECK(host_if == 0x202U);
    die(0);
}
