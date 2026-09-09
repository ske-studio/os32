/* ======================================================================== */
/*  SYS.C — システム制御およびブザー制御                                      */
/* ======================================================================== */

#include "sys.h"
#include "pgalloc.h"
#include "memmap.h"
#include "io.h"
#include "pc98.h"
#include "rtc.h"
#include "os_time.h"

int sys_device_reserve_core(u32 owner, const struct sys_device_span *spans,
                            u32 count, const struct sys_device_capability *cap)
{
    unsigned int flags;
    u32 hot;
    int ok;
    flags = irq_save();
    hot = sys_hotdeploy_base() / PAGE_SIZE;
    ok = 0;
    if (hot <= PHYSMEM_MAX_PFN - MEM_HOTDEPLOY_SIZE / PAGE_SIZE)
        ok = pgalloc_device_reserve(owner, spans, count, cap,
                                    hot + MEM_HOTDEPLOY_SIZE / PAGE_SIZE);
    irq_restore(flags);
    return ok;
}

void sys_reboot(void)
{
    /* PC-98 ハードウェアリセット (FreeBSD実装準拠) */
    outp(SYSPORT_C_BSR, BSR_SHUT0_SET);  /* SHUT0 = 1 */
    outp(SYSPORT_C_BSR, BSR_SHUT1_SET);  /* SHUT1 = 1 */
    outp(CPU_RESET_PORT, 0x00);           /* CPUリセット */
    /* ここには来ない */
    for (;;) { __asm__ volatile("hlt"); }
}

void sys_halt(void)
{
    __asm__ volatile("hlt");
}

void buz_on(void)
{
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);
}

void buz_off(void)
{
    outp(SYSPORT_C_BSR, BSR_BUZ_OFF);
}

u32 sys_mem_kb = 1024; /* 初期値(1MB) */

u32 sys_get_mem_kb(void)
{
    return sys_mem_kb;
}

/* ホットデプロイ窓の直下にさらに固定予約した量 (バイト、ページ境界)。
 * 現状の唯一の利用者は PEGC 8bpp バックバッファ (H2)。9801 では 0 のまま
 * なので sys_usable_mem_end() は従来と同じ値を返す (回帰ゼロ)。 */
static u32 sys_top_reserved = 0;
static u32 sys_frozen_exec, sys_frozen_hot;
static int sys_model_staged;

int sys_memory_bootstrap_model(struct physmem *m, const struct pgalloc_layout *l,
                               int (*verify)(u32, u32, void *))
{
    u32 top, bytes, count;
    struct pgalloc_layout layout;
    unsigned int flags;
    int ok;
    flags = irq_save();
    ok = 0;
    if (sys_frozen_hot || sys_top_reserved || !m || !l || !verify ||
        !paging_boot_context()) goto done;
    /* Backing is about to be zeroed; retain no borrowed layout pointer. */
    layout = *l;
    l = &layout;
    top = physmem_legacy_end(m);
    bytes = pgalloc_metadata_bytes(m);
    if (!bytes || top != m->legacy_ceiling || l->metadata_first >= top ||
        bytes / PAGE_SIZE != top - l->metadata_first ||
        l->workspace_end != l->metadata_first ||
        l->workspace_first < (MEM_EXEC_LOAD_ADDR + MEM_EXEC_STACK_SIZE +
            MEM_EXEC_SBRK_MIN + MEM_EXEC_HEAP_MIN) / PAGE_SIZE ||
        top > PHYSMEM_LEGACY_MAX_PFN - MEM_HOTDEPLOY_SIZE / PAGE_SIZE) goto done;
    if (!physmem_count(m, top, top + MEM_HOTDEPLOY_SIZE / PAGE_SIZE,
                       PHYSMEM_RESERVED, &count) ||
        count != MEM_HOTDEPLOY_SIZE / PAGE_SIZE ||
        !verify(top, count, (void *)(top * PAGE_SIZE))) goto done;
    if (!pgalloc_init_layout(m, l, verify)) goto done;
    sys_frozen_exec = l->workspace_first * PAGE_SIZE;
    sys_frozen_hot = top * PAGE_SIZE;
    sys_model_staged = 1;
    ok = 1;
done:
    irq_restore(flags);
    return ok;
}

int sys_memory_stage_online(void)
{
    return sys_model_staged && pgalloc_stage_online();
}

int sys_memory_init_model(struct physmem *m, void *backing, u32 capacity,
                          u32 first, int (*verify)(u32, u32, void *))
{
    u32 bytes, top, count, minimum;
    unsigned int flags;
    int ok;
    flags = irq_save();
    ok = 0;
    if (sys_frozen_hot || sys_top_reserved || !m || !verify) goto done;
    bytes = pgalloc_metadata_bytes(m);
    if (!bytes) goto done;
    top = physmem_legacy_end(m);
    minimum = (MEM_EXEC_LOAD_ADDR + MEM_EXEC_STACK_SIZE +
               MEM_EXEC_SBRK_MIN + MEM_EXEC_HEAP_MIN) / PAGE_SIZE;
    if (top != m->legacy_ceiling || first < minimum || first >= top ||
        bytes / PAGE_SIZE != top - first ||
        top > PHYSMEM_LEGACY_MAX_PFN - MEM_HOTDEPLOY_SIZE / PAGE_SIZE) goto done;
    if (!physmem_count(m, top, top + MEM_HOTDEPLOY_SIZE / PAGE_SIZE,
                       PHYSMEM_RESERVED, &count) ||
        count != MEM_HOTDEPLOY_SIZE / PAGE_SIZE ||
        !verify(top, count, (void *)(top * PAGE_SIZE))) goto done;
    if (!pgalloc_init_model(m, backing, capacity, first, verify)) goto done;
    sys_frozen_exec = first * PAGE_SIZE;
    sys_frozen_hot = top * PAGE_SIZE;
    ok = 1;
done:
    irq_restore(flags);
    return ok;
}

/* ステージング領域の先頭 (物理)。物理末尾からホットデプロイ窓を引いた位置で、
 * 追加予約 (sys_top_reserved) には影響されない。
 * 設計: docs/tasks/hotdeploy/DESIGN.md */
u32 sys_hotdeploy_base(void)
{
    u32 kb, end;
    if (sys_frozen_hot) return sys_frozen_hot;
    kb = sys_mem_kb;
    if (kb > PHYSMEM_LEGACY_MAX_PFN * (PAGE_SIZE / 1024))
        kb = PHYSMEM_LEGACY_MAX_PFN * (PAGE_SIZE / 1024);
    end = (kb / (PAGE_SIZE / 1024)) * PAGE_SIZE;
    return (end > MEM_HOTDEPLOY_SIZE * 2) ? (end - MEM_HOTDEPLOY_SIZE) : end;
}

/* 物理末尾からホットデプロイ用ステージング領域と固定予約を除いた、
 * 割り当ててよい上限。子プロセスのスタックはここから下へ伸びる。 */
u32 sys_usable_mem_end(void)
{
    if (sys_frozen_hot) return sys_frozen_exec;
    return sys_hotdeploy_base() - sys_top_reserved;
}

/* ======================================================================== */
/*  sys_reserve_top — 物理末尾側に固定領域を切り出す (GUI v1.1 H2)          */
/*                                                                          */
/*  ホットデプロイ窓の直下から bytes バイト (4KB 切り上げ) を予約し、その    */
/*  先頭物理アドレスを返す。以後 sys_usable_mem_end() はその分だけ下がるので */
/*  exec の子プロセス (コード/ヒープ/スタック) はここへ伸びてこない。        */
/*                                                                          */
/*  **exec_run より前 (ブート中) に 1 回だけ呼ぶこと。** 子プロセスが走って  */
/*  いる最中に上限を動かすと、その子のレイアウトと pgalloc の予約範囲が      */
/*  食い違う。同じサイズでの再呼び出しは冪等 (同じ先頭を返す)。             */
/*                                                                          */
/*  戻り値: 予約領域の先頭物理アドレス。0 = 予約できなかった                 */
/*  (メモリ不足、または既に別サイズで予約済み)。                             */
/* ======================================================================== */
u32 sys_reserve_top(u32 bytes)
{
    u32 top, need, result, minimum;
    unsigned int flags;
    flags = irq_save();
    result = 0;
    if (sys_frozen_hot || !bytes || bytes > ~0UL - (PAGE_SIZE - 1)) goto done;
    need = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    top = sys_hotdeploy_base();
    if (sys_top_reserved) {
        if (sys_top_reserved == need) result = top - need;
        goto done;
    }
    minimum = MEM_EXEC_LOAD_ADDR + MEM_EXEC_STACK_SIZE +
              MEM_EXEC_SBRK_MIN + MEM_EXEC_HEAP_MIN;
    if (top < minimum || need > top - minimum) goto done;
    /* Old boot calls after pgalloc_init. Never publish a numeric-only claim. */
    if (!pgalloc_reserve_pfn((top - need) / PAGE_SIZE, top / PAGE_SIZE)) goto done;
    sys_top_reserved = need;
    result = top - need;
done:
    irq_restore(flags);
    return result;
}

os_time_t sys_time(void)
{
    RTC_Time t;
    int y;
    rtc_read(&t);
    y = t.year;
    /* PC-98のRTCは年号下2桁のみ。80以上なら1900年代、未満なら2000年代と仮定 */
    if (y < 80) y += 2000;
    else y += 1900;
    return datetime_to_epoch(y, t.month, t.day, t.hour, t.min, t.sec);
}
