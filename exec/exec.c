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
#include "fat12.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
static KernelAPI *kapi = (KernelAPI *)KAPI_ADDR;
void exec_init(void) {
#include "exec_kapi_init.inc"
}

#define MAX_EXEC_NEST 4

/* スタックを4バイト境界に揃えるためのマスク */
#define STACK_ALIGN_MASK 3

volatile int is_exec_running = 0;
volatile int exec_exit_status = EXEC_SUCCESS;
static u32 exec_kernel_jmpbuf[6];

extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

void exec_exit(int status)
{
    if (is_exec_running) {
        exec_exit_status = status;
        is_exec_running = 0;
        exec_longjmp(exec_kernel_jmpbuf);
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

int exec_run(const char *cmdline)
{
    u32 load_base = EXEC_LOAD_ADDR;
    u32 max_size  = EXEC_MAX_SIZE;
    u32 stack_top = EXEC_STACK_TOP;
    u8 *file_buf = (u8 *)load_base;
    u8 *load_addr = (u8 *)load_base;
    OS32Header *hdr;
    int sz;
    u32 code_off, text_sz, bss_sz, heap_sz, entry_off;
    ExecEntry entry;
    
    char path[VFS_MAX_PATH];
    const char *p = cmdline;
    int i = 0;


    
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
        if (is_exec_running) exec_exit(EXEC_ERR_NOT_FOUND);
        return EXEC_ERR_NOT_FOUND;
    }

    hdr = (OS32Header *)file_buf;
    if (hdr->magic != OS32X_MAGIC || hdr->header_size < OS32X_HDR_V1_SIZE || hdr->min_api_ver > KAPI_VERSION) {
        shell_print("Error: invalid OS32X binary\n", ATTR_RED);
        if (is_exec_running) exec_exit(EXEC_ERR_INVALID);
        return EXEC_ERR_INVALID;
    }

    code_off  = hdr->header_size;
    text_sz   = hdr->text_size;
    bss_sz    = hdr->bss_size;
    heap_sz   = hdr->heap_size;
    entry_off = hdr->entry_offset;

    if (text_sz + bss_sz > max_size) {
        if (is_exec_running) exec_exit(EXEC_ERR_NOMEM);
        return EXEC_ERR_NOMEM;
    }

    {
        kmemcpy(load_addr, load_addr + code_off, text_sz);
        kmemset(load_addr + text_sz, 0, bss_sz);
    }

    exec_heap_init(heap_sz);
    kapi->sbrk_heap_limit = EXEC_HEAP_BASE + heap_sz;

    if (!is_exec_running) {
        if (exec_setjmp(exec_kernel_jmpbuf) != 0) {
            exec_heap_reset();
            return exec_exit_status;
        }
    }

    entry = (ExecEntry)(load_addr + entry_off);
    
    is_exec_running = 1;

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
        /* User program shouldn't return directly from call; it should use sys_exit.
           If it does return, we treat it as success and manually call exec_exit. */
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS; // Should technically never be reached
}
