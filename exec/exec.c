#include "exec.h"
#include "exec_heap.h"
#include "console.h"
#include "kstring.h"
#include "vfs.h"
#include "gfx.h"
#include "kbd.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "paging.h"
#include "pgalloc.h"
#include "fat12.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
extern u32 sys_mem_kb;
static KernelAPI *kapi = (KernelAPI *)KAPI_ADDR;
void exec_init(void) {
#include "exec_kapi_init.inc"
}

/* スタックを4バイト境界に揃えるためのマスク */
#define STACK_ALIGN_MASK 3

/* プログラム空間のページ数計算マクロ */
/* コード+BSS+sbrk (1MB) + ガードA (4KB) + ヒープ + ガードB (4KB) + スタック (128KB) */
#define EXEC_TOTAL_PAGES(mem_end) \
    (((mem_end) - MEM_EXEC_LOAD_ADDR) / PAGE_SIZE)

/* ======================================================================== */
/*  ExecContext — ネスト階層ごとのコンテキスト保存構造体                     */
/* ======================================================================== */
typedef struct {
    u32  jmpbuf[6];           /* setjmp/longjmp用バッファ */
    u32  guard_a;             /* sbrkガードページアドレス */
    u32  guard_b;             /* スタックガードページアドレス */
    u32  sbrk_heap_limit;     /* sbrk上限 */
    u32  exec_heap_base;      /* ヒープベースアドレス */
    u32  exec_heap_size;      /* ヒープサイズ */
    /* Phase 2: ページディレクトリ切り替え用 */
    u32 *page_dir;            /* このレベルのPD (NULL=マスターPD) */
    u8  *pd_raw;              /* PD用kmalloc生ポインタ (解放用) */
    u8  *pt_raw;              /* PT用kmalloc生ポインタ (解放用) */
    u32  phys_base;           /* 確保した物理ページ群の先頭 */
    int  phys_page_count;     /* 確保した物理ページ数 */
} ExecContext;

/* ======================================================================== */
/*  グローバル状態                                                          */
/* ======================================================================== */
volatile int exec_nest_level = 0;
volatile int exec_exit_status = EXEC_SUCCESS;
static ExecContext exec_ctx_stack[MAX_EXEC_NEST];

extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* ======================================================================== */
/*  exec_exit — 現在の実行階層を終了し、親のsetjmp復帰ポイントへ戻る        */
/* ======================================================================== */
void exec_exit(int status)
{
    if (exec_nest_level > 0) {
        exec_exit_status = status;
        exec_nest_level--;
        exec_longjmp(exec_ctx_stack[exec_nest_level].jmpbuf);
    }
}

void exec_fault_recover(void)
{
    exec_exit(EXEC_ERR_FAULT);
}

void __cdecl kapi_sys_exit(int status)
{
    exec_exit(status);
}

/* ======================================================================== */
/*  exec_run — 外部プログラムのロードと実行 (ネスト対応 + PD切り替え)       */
/* ======================================================================== */
int exec_run(const char *cmdline)
{
    u32 load_base = EXEC_LOAD_ADDR;
    u32 max_size  = EXEC_MAX_SIZE;
    /* --- 動的レイアウト計算 --- */
    u32 mem_end = sys_mem_kb * 1024;
    u32 stack_top = mem_end;
    u32 stack_bottom = stack_top - MEM_EXEC_STACK_SIZE;
    u32 guard_b = stack_bottom - PAGE_SIZE;
    u32 guard_a = MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE;
    u32 exec_heap_base = guard_a + PAGE_SIZE;
    u32 exec_heap_size = guard_b - exec_heap_base;
    u8 *file_buf = (u8 *)load_base;
    u8 *load_addr = (u8 *)load_base;
    OS32Header *hdr;
    int sz;
    u32 code_off, text_sz, bss_sz, heap_sz, entry_off;
    ExecEntry entry;
    ExecContext *ctx;
    
    char path[VFS_MAX_PATH];
    const char *p = cmdline;
    int i = 0;

    /* ネスト上限チェック */
    if (exec_nest_level >= MAX_EXEC_NEST) {
        shell_print("Error: exec nest limit reached\n", ATTR_RED);
        if (exec_nest_level > 0) exec_exit(EXEC_ERR_NOMEM);
        return EXEC_ERR_NOMEM;
    }

    
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < sizeof(path) - 1) {
        path[i++] = *p++;
    }
    path[i] = '\0';

    sz = vfs_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
    if (sz <= 0 && fat12_is_mounted()) {
        sz = fat12_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
    }
    if (sz <= 0) {
        if (exec_nest_level > 0) exec_exit(EXEC_ERR_NOT_FOUND);
        return EXEC_ERR_NOT_FOUND;
    }

    hdr = (OS32Header *)file_buf;
    if (hdr->magic != OS32X_MAGIC || hdr->header_size < OS32X_HDR_V1_SIZE || hdr->min_api_ver > KAPI_VERSION) {
        shell_print("Error: invalid OS32X binary\n", ATTR_RED);
        if (exec_nest_level > 0) exec_exit(EXEC_ERR_INVALID);
        return EXEC_ERR_INVALID;
    }

    code_off  = hdr->header_size;
    text_sz   = hdr->text_size;
    bss_sz    = hdr->bss_size;
    heap_sz   = hdr->heap_size;
    entry_off = hdr->entry_offset;

    if (text_sz + bss_sz > max_size) {
        if (exec_nest_level > 0) exec_exit(EXEC_ERR_NOMEM);
        return EXEC_ERR_NOMEM;
    }

    /* ======== Phase 2: ページディレクトリ切り替え ======== */
    ctx = &exec_ctx_stack[exec_nest_level];
    ctx->page_dir = (u32 *)0;
    ctx->pd_raw = (u8 *)0;
    ctx->pt_raw = (u8 *)0;
    ctx->phys_base = 0;
    ctx->phys_page_count = 0;

    if (exec_nest_level > 0) {
        /* Level 1以上: 物理ページを確保し、新しいPDを構築 */
        int total_pg = (int)EXEC_TOTAL_PAGES(mem_end);
        u32 phys_base;
        u32 *phys_pages;
        int j;
        u32 *new_pd;
        u8 *pd_raw_buf;
        u8 *pt_raw_buf;
        u32 *new_pt;
        u32 pdi_start;

        phys_base = pgalloc_alloc_n(total_pg);
        if (phys_base == 0) {
            shell_print("Error: no physical pages\n", ATTR_RED);
            exec_exit(EXEC_ERR_NOMEM);
            return EXEC_ERR_NOMEM;
        }

        ctx->phys_base = phys_base;
        ctx->phys_page_count = total_pg;

        /* 物理ページアドレス配列を一時的に構築 (スタック上) */
        /* 注意: total_pg は最大 3072 だが実際は数百程度 */
        /* kmalloc で一時バッファを確保 */
        phys_pages = (u32 *)kmalloc(total_pg * sizeof(u32));
        if (!phys_pages) {
            pgalloc_free_n(phys_base, total_pg);
            shell_print("Error: kmalloc for phys_pages\n", ATTR_RED);
            exec_exit(EXEC_ERR_NOMEM);
            return EXEC_ERR_NOMEM;
        }
        for (j = 0; j < total_pg; j++) {
            phys_pages[j] = phys_base + (u32)j * PAGE_SIZE;
        }

        /* PD + PT をkmalloc確保 (4096アラインのためパディング付き) */
        pd_raw_buf = (u8 *)kmalloc(4096 + 4095);
        pt_raw_buf = (u8 *)kmalloc(4096 + 4095);
        if (!pd_raw_buf || !pt_raw_buf) {
            if (pd_raw_buf) kfree(pd_raw_buf);
            if (pt_raw_buf) kfree(pt_raw_buf);
            kfree(phys_pages);
            pgalloc_free_n(phys_base, total_pg);
            shell_print("Error: kmalloc for PD/PT\n", ATTR_RED);
            exec_exit(EXEC_ERR_NOMEM);
            return EXEC_ERR_NOMEM;
        }

        ctx->pd_raw = pd_raw_buf;
        ctx->pt_raw = pt_raw_buf;

        {
            /* 4096バイト境界にアライン (paging.cのalign4096と同じ) */
            u32 pd_addr = (u32)pd_raw_buf;
            u32 pt_addr = (u32)pt_raw_buf;
            pd_addr = (pd_addr + 4095) & ~4095UL;
            pt_addr = (pt_addr + 4095) & ~4095UL;
            new_pd = (u32 *)pd_addr;
            new_pt = (u32 *)pt_addr;
        }

        /* マスターPDの全エントリをコピー (カーネル空間を共有) */
        {
            u32 *master = paging_get_master_pd();
            for (j = 0; j < PDE_COUNT; j++) {
                new_pd[j] = master[j];
            }
        }

        /* プログラム空間のページテーブルを構築 */
        pdi_start = MEM_EXEC_LOAD_ADDR >> 22; /* = 1 */
        for (j = 0; j < PTE_COUNT; j++) {
            new_pt[j] = PAGE_NOT_PRESENT;
        }
        for (j = 0; j < total_pg && j < PTE_COUNT; j++) {
            u32 virt = MEM_EXEC_LOAD_ADDR + (u32)j * PAGE_SIZE;
            u32 pti = (virt >> 12) & 0x3FF;
            new_pt[pti] = (phys_pages[j] & 0xFFFFF000UL) | PAGE_RW;
        }
        new_pd[pdi_start] = (u32)new_pt | PAGE_RW;

        kfree(phys_pages);

        ctx->page_dir = new_pd;

        /* CR3をそう新PDに切り替え */
        paging_switch_pd(new_pd);

        /* ★ ここからfile_buf (0x400000) は新しい物理ページを指す */
        /* バイナリを再読み込み (親のデータはもう見えない) */
        sz = vfs_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
        if (sz <= 0 && fat12_is_mounted()) {
            sz = fat12_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
        }
        if (sz <= 0) {
            /* PD を元に戻して失敗 */
            paging_switch_pd(paging_get_master_pd());
            kfree(ctx->pd_raw);
            kfree(ctx->pt_raw);
            pgalloc_free_n(ctx->phys_base, ctx->phys_page_count);
            exec_exit(EXEC_ERR_NOT_FOUND);
            return EXEC_ERR_NOT_FOUND;
        }

        /* ヘッダを再パース */
        hdr = (OS32Header *)file_buf;
        code_off  = hdr->header_size;
        text_sz   = hdr->text_size;
        bss_sz    = hdr->bss_size;
        entry_off = hdr->entry_offset;
    } else {
        /* Level 0: アイデンティティマッピング。初回起動時にページ範囲を予約 */
        int total_pg = (int)EXEC_TOTAL_PAGES(mem_end);
        pgalloc_mark_used(MEM_EXEC_LOAD_ADDR, total_pg);
    }

    {
        kmemcpy(load_addr, load_addr + code_off, text_sz);
        kmemset(load_addr + text_sz, 0, bss_sz);
    }

    /* ヒープ初期化 (動的サイズ) */
    exec_heap_init_at(exec_heap_base, exec_heap_size);
    kapi->sbrk_heap_limit = guard_a;  /* sbrk上限 */

    /* ガードページ設定 */
    paging_set_not_present(guard_a, guard_a + PAGE_SIZE - 1);
    paging_set_not_present(guard_b, guard_b + PAGE_SIZE - 1);

    /* 現在の階層にコンテキストを保存 */
    ctx->guard_a = guard_a;
    ctx->guard_b = guard_b;
    ctx->sbrk_heap_limit = kapi->sbrk_heap_limit;
    ctx->exec_heap_base = exec_heap_base;
    ctx->exec_heap_size = exec_heap_size;

    /* setjmp — 毎回実行 (ネスト対応) */
    if (exec_setjmp(ctx->jmpbuf) != 0) {
        /* ======== longjmp復帰ポイント ======== */
        ctx = &exec_ctx_stack[exec_nest_level];

        /* Phase 2: 子プロセスのPDから親のPDに切り替え */
        if (ctx->page_dir) {
            /* マスターPDまたは親のPDに戻す */
            if (exec_nest_level > 0) {
                ExecContext *parent = &exec_ctx_stack[exec_nest_level - 1];
                if (parent->page_dir) {
                    paging_switch_pd(parent->page_dir);
                } else {
                    paging_switch_pd(paging_get_master_pd());
                }
            } else {
                paging_switch_pd(paging_get_master_pd());
            }
            /* PD/PT メモリと物理ページを解放 */
            kfree(ctx->pd_raw);
            kfree(ctx->pt_raw);
            pgalloc_free_n(ctx->phys_base, ctx->phys_page_count);
            ctx->page_dir = (u32 *)0;
        }

        /* ガードページ解除 */
        paging_set_page(ctx->guard_a, ctx->guard_a, PAGE_RW);
        paging_set_page(ctx->guard_b, ctx->guard_b, PAGE_RW);
        exec_heap_reset();

        /* 親が存在する場合、親のヒープ/sbrk状態を復元 */
        if (exec_nest_level > 0) {
            ExecContext *parent = &exec_ctx_stack[exec_nest_level - 1];
            exec_heap_init_at(parent->exec_heap_base, parent->exec_heap_size);
            kapi->sbrk_heap_limit = parent->sbrk_heap_limit;
            /* 親のガードページも再設定 */
            paging_set_not_present(parent->guard_a,
                                   parent->guard_a + PAGE_SIZE - 1);
            paging_set_not_present(parent->guard_b,
                                   parent->guard_b + PAGE_SIZE - 1);
        }
        return exec_exit_status;
    }

    entry = (ExecEntry)(load_addr + entry_off);
    
    exec_nest_level++;

    {
        char *str_area;
        char **argv_area;
        int argc = 0;
        int cmd_len = kstrlen(cmdline);
        const char *s = cmdline;
        char *d;
        int in_arg = 0;
        u32 new_esp;
        static u32 saved_esp;
        
        stack_top -= (cmd_len + 1);
        stack_top &= ~((u32)STACK_ALIGN_MASK);
        str_area = (char *)stack_top;
        
        stack_top -= sizeof(char *) * OS32_MAX_ARGS;
        argv_area = (char **)stack_top;
        
        s = cmdline;
        d = str_area;
        while (*s) {
            if (*s == ' ') {
                *d++ = '\0';
                in_arg = 0;
            } else {
                if (!in_arg) {
                    if (argc < OS32_MAX_ARGS - 1) argv_area[argc++] = d;
                    in_arg = 1;
                }
                *d++ = *s;
            }
            s++;
        }
        *d = '\0';
        argv_area[argc] = NULL;
        
        new_esp = stack_top;
        
        __asm__ volatile(
            "mov %%esp, %0\n\t"
            "mov %1, %%esp\n\t"
            "push %4\n\t"
            "push %3\n\t"
            "push %2\n\t"
            "call *%5\n\t"
            "add $12, %%esp\n\t"
            "mov %0, %%esp"
            : "=m"(saved_esp)
            : "r"(new_esp), "g"(argc), "g"(argv_area), "g"(kapi), "r"(entry)
            : "eax", "ecx", "edx", "memory"
        );
        /* プログラムがsys_exitを使わずreturnした場合、成功として処理 */
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS; /* 到達しない */
}
