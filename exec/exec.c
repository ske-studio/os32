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
#include "fd_redirect.h"
#include "pipe_buffer.h"
#include "shm.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
extern u32 sys_mem_kb;
static KernelAPI *kapi = (KernelAPI *)KAPI_ADDR;
void exec_init(void) {
#include "exec_kapi_init.inc"
}

/* スタックを4バイト境界に揃えるためのマスク */
#define STACK_ALIGN_MASK 3

/* コード領域のページ数 (1MB / 4KB = 256ページ) — pgalloc_mark_used 用 */
#define EXEC_CODE_PAGES  ((MEM_EXEC_MAX_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)

/* ======================================================================== */
/*  ExecContext — ネスト階層ごとのコンテキスト保存構造体                     */
/*                                                                          */
/*  シェル常駐モデル:                                                       */
/*    Level 0 (シェル): 0x300000 に常駐。ヒープ/スタック不要 (静的バッファ)  */
/*    Level 1+ (子):    0x400000 にロード。アイデンティティマッピング。       */
/*    PD切り替え不要。物理ページ確保不要。                                   */
/* ======================================================================== */
typedef struct {
    u32  jmpbuf[6];           /* setjmp/longjmp用バッファ */
    u32  guard_a;             /* sbrkガードページアドレス */
    u32  guard_b;             /* スタックガードページアドレス */
    u32  sbrk_heap_limit;     /* sbrk上限 */
    u32  exec_heap_base;      /* ヒープベースアドレス */
    u32  exec_heap_size;      /* ヒープサイズ */
    u32  load_addr;           /* このレベルのロードアドレス */
    u32  stack_top;           /* このレベルのスタック先頭 */
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
    ExecContext *ctx;

    if (exec_nest_level > 0) {
        exec_exit_status = status;

        /* 現在のレベルのクリーンアップ (ガードページ解除 + ヒープリセット) */
        ctx = &exec_ctx_stack[exec_nest_level];
        if (ctx->guard_a != 0) {
            paging_set_page(ctx->guard_a, ctx->guard_a, PAGE_RW);
            paging_set_page(ctx->guard_b, ctx->guard_b, PAGE_RW);
        }
        if (ctx->exec_heap_base != 0) {
            exec_heap_reset();
        }

        /* ============================================================ */
        /*  リソース自動クリーンアップ (プログラム終了時の安全網)        */
        /*  プログラムがclose/reset忘れてもカーネルが回収する            */
        /* ============================================================ */

        /* (1) 標準FDのリダイレクト解除 (ファイルFDも自動クローズ) */
        fd_redirect_reset(0);
        fd_redirect_reset(1);
        fd_redirect_reset(2);

        /* (2) FD自動クローズ (FD 3以上の全オープンファイル) */
        {
            int fd;
            for (fd = 3; fd < VFS_MAX_OPEN_FILES; fd++) {
                vfs_close(fd);
            }
        }

        /* (3) パイプバッファ自動解放 (全バッファを解放) */
        {
            int pi;
            for (pi = 0; pi < PIPE_BUF_COUNT; pi++) {
                pipe_free(pi);
            }
        }

        /* (4) 共有メモリ自動解放 (全ブロックの使用中フラグを解除) */
        shm_cleanup_all();

        /* 親レベルへ復帰 */
        exec_nest_level--;
        ctx = &exec_ctx_stack[exec_nest_level];
        exec_longjmp(ctx->jmpbuf);
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
/*  exec_run — 外部プログラムのロードと実行 (シェル常駐モデル)              */
/*                                                                          */
/*  Level 0 (シェル): 0x300000 にロード。スタック=0x380000。                 */
/*  Level 1+  (子)  : 0x400000 にロード。スタック=mem_end。                  */
/*  PD切り替え不要。メモリは完全に分離されている。                           */
/* ======================================================================== */
int exec_run(const char *cmdline)
{
    /* --- Level に応じたロードアドレスとレイアウトを決定 --- */
    u32 load_base;
    u32 max_size;
    u32 stack_top;
    u32 guard_a, guard_b;
    u32 exec_heap_base, exec_heap_size;
    int is_shell;

    u32 mem_end = sys_mem_kb * 1024;
    u8 *file_buf;
    u8 *load_addr;
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
        return EXEC_ERR_NOMEM;
    }

    /* コマンドラインからパスを抽出 */
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < (int)sizeof(path) - 1) {
        path[i++] = *p++;
    }
    path[i] = '\0';

    /* ====== Level に応じたメモリレイアウト決定 ====== */
    is_shell = (exec_nest_level == 0);

    if (is_shell) {
        /* Level 0 (シェル): 常駐帯域 0x300000-0x37FFFF */
        load_base = MEM_SHELL_LOAD_ADDR;
        max_size  = MEM_SHELL_MAX_SIZE;
        stack_top = MEM_SHELL_STACK_TOP;
        guard_a   = 0; /* シェルは sbrk/exec_heap 未使用 */
        guard_b   = MEM_SHELL_GUARD;
        exec_heap_base = 0;
        exec_heap_size = 0;
    } else {
        /* Level 1+ (子プロセス): 0x400000〜 アイデンティティマッピング */
        u32 child_stack_bottom;
        load_base = MEM_EXEC_LOAD_ADDR;
        max_size  = MEM_EXEC_MAX_SIZE;
        stack_top = mem_end;
        child_stack_bottom = stack_top - MEM_EXEC_STACK_SIZE;
        guard_b   = child_stack_bottom - PAGE_SIZE;
        guard_a   = MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE;
        exec_heap_base = guard_a + PAGE_SIZE;
        exec_heap_size = guard_b - exec_heap_base;
    }

    file_buf  = (u8 *)load_base;
    load_addr = (u8 *)load_base;


    /* ====== ファイル読み込み ====== */
    sz = vfs_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
    if (sz <= 0 && fat12_is_mounted()) {
        sz = fat12_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);
    }

    /* フォールバック: パスにスラッシュがない場合、標準ディレクトリを順に検索 */
    if (sz <= 0) {
        int has_slash = 0;
        int pi;
        for (pi = 0; path[pi]; pi++) {
            if (path[pi] == '/') { has_slash = 1; break; }
        }
        if (!has_slash) {
            static const char *search_dirs[] = {
                "/bin/", "/sbin/", "/usr/bin/", (const char *)0
            };
            int di;
            for (di = 0; search_dirs[di]; di++) {
                char try_path[VFS_MAX_PATH];
                int tp = 0, dp = 0;
                while (search_dirs[di][dp] && tp < VFS_MAX_PATH - 2)
                    try_path[tp++] = search_dirs[di][dp++];
                dp = 0;
                while (path[dp] && tp < VFS_MAX_PATH - 1)
                    try_path[tp++] = path[dp++];
                try_path[tp] = '\0';
                sz = vfs_read(try_path, file_buf, max_size + OS32X_HDR_V1_SIZE);
                if (sz > 0) break;
            }
        }
    }

    if (sz <= 0) {
        return EXEC_ERR_NOT_FOUND;
    }

    hdr = (OS32Header *)file_buf;

    if (hdr->magic != OS32X_MAGIC || hdr->header_size < OS32X_HDR_V1_SIZE || hdr->min_api_ver > KAPI_VERSION) {
        shell_print("Error: invalid OS32X binary\n", ATTR_RED);
        return EXEC_ERR_INVALID;
    }

    code_off  = hdr->header_size;
    text_sz   = hdr->text_size;
    bss_sz    = hdr->bss_size;
    heap_sz   = hdr->heap_size;
    entry_off = hdr->entry_offset;

    if (text_sz + bss_sz > max_size) {
        shell_print("[DBG] NOMEM: text=", 0xE1);
        shell_print_dec(text_sz, 0xE1);
        shell_print(" bss=", 0xE1);
        shell_print_dec(bss_sz, 0xE1);
        shell_print(" max=", 0xE1);
        shell_print_dec(max_size, 0xE1);
        shell_print("\n", 0xE1);
        return EXEC_ERR_NOMEM;
    }

    /* ======== コンテキスト設定 ======== */
    ctx = &exec_ctx_stack[exec_nest_level];
    ctx->load_addr = load_base;
    ctx->stack_top = stack_top;
    ctx->guard_a = guard_a;
    ctx->guard_b = guard_b;
    ctx->exec_heap_base = exec_heap_base;
    ctx->exec_heap_size = exec_heap_size;

    /* コードセクションの配置 + BSS ゼロクリア */
    if (!is_shell) {
        /* 子プロセスの物理ページを予約 (アイデンティティマッピング) */
        pgalloc_mark_used(MEM_EXEC_LOAD_ADDR, EXEC_CODE_PAGES);
    }

    {
        kmemcpy(load_addr, load_addr + code_off, text_sz);
        kmemset(load_addr + text_sz, 0, bss_sz);
    }


    /* ヒープ・ガードページ設定 (子プロセスのみ) */
    if (!is_shell) {
        /* OS32X ヘッダの heap_size 指定があればサイズを制限 */
        if (heap_sz > 0 && heap_sz < exec_heap_size) {
            exec_heap_size = heap_sz;
            ctx->exec_heap_size = exec_heap_size;
        }
        exec_heap_init_at(exec_heap_base, exec_heap_size);
        kapi->sbrk_heap_limit = guard_a;
        ctx->sbrk_heap_limit = guard_a;

        /* ガードページ設定 */
        paging_set_not_present(guard_a, guard_a + PAGE_SIZE - 1);
        paging_set_not_present(guard_b, guard_b + PAGE_SIZE - 1);
    }

    /* setjmp — 毎回実行 (ネスト対応) */
    if (exec_setjmp(ctx->jmpbuf) != 0) {
        /* ======== longjmp復帰ポイント ======== */

        ctx = &exec_ctx_stack[exec_nest_level];

        /* ガードページ解除 (子プロセスのガードのみ) */
        if (ctx->guard_a != 0) {

            paging_set_page(ctx->guard_a, ctx->guard_a, PAGE_RW);
            paging_set_page(ctx->guard_b, ctx->guard_b, PAGE_RW);
        }

        /* 子プロセスのヒープリセット */
        if (ctx->exec_heap_base != 0) {
            exec_heap_reset();
        }

        /* 親のヒープ/sbrk状態を復元 */
        if (exec_nest_level > 0) {
            ExecContext *parent = &exec_ctx_stack[exec_nest_level - 1];
            /* 親が子プロセス (Level 1+) の場合のみ復元 */
            if (parent->exec_heap_base != 0) {

                exec_heap_init_at(parent->exec_heap_base, parent->exec_heap_size);
                kapi->sbrk_heap_limit = parent->sbrk_heap_limit;
                paging_set_not_present(parent->guard_a,
                                       parent->guard_a + PAGE_SIZE - 1);
                paging_set_not_present(parent->guard_b,
                                       parent->guard_b + PAGE_SIZE - 1);
            }
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
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS;
}
