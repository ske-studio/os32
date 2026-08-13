#include "exec.h"
#include "exec_heap.h"
#include "io.h"
#include "console.h"
#include "kstring.h"
#include "vfs.h"
#include "gfx.h"
#include "kbd.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "paging.h"
#include "pgalloc.h"
#include "fd_redirect.h"
#include "pipe_buffer.h"
#include "shm.h"
#include "snd_engine.h"
#include "kapi_db.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
extern u32 sys_mem_kb;
static KernelAPI *kapi;
void exec_init(void) {
    kapi = (KernelAPI *)KAPI_ADDR;
#include "exec_kapi_init.inc"
    /* 共有メモリ先頭アドレスを公開する。
     * MEM_SHM_BASE はカーネルの __bss_end 由来で可変のため、
     * ユーザ空間側がアドレスをハードコードしてはならない。 */
    kapi->shm_base = (u32)MEM_SHM_BASE;
}

/* スタックを4バイト境界に揃えるためのマスク */
#define STACK_ALIGN_MASK 3

/* コード領域のページ数 (1MB / 4KB = 256ページ) — pgalloc_mark_used 用 */
#define EXEC_CODE_PAGES  ((MEM_EXEC_MAX_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)

/* 動的確保リザーブ (1MB)。
 *
 * 子プロセスの空間はコード+ヒープ+スタックで pgalloc の管理域
 * (0x400000〜mem_end) をほぼ使い切る。かつてはコード 1MB しか
 * pgalloc_mark_used していなかったため、子の実行中に v86_mem_setup() の
 * pgalloc_alloc_n(160) が「空いている」ヒープ領域 0x500000〜 を確保して
 * 636KB を memset(0) し、起動元プログラムのヒープを破壊していた。
 *
 * 対策: 子の exec ヒープをこのぶんだけ縮め、ヒープ末尾とスタックガードの
 * 間に pgalloc 専用の穴を残す。V86 バッキング RAM (160 ページ = 640KB
 * 連続) はここから取れる。 */
#define EXEC_DYN_RESERVE  (256UL * PAGE_SIZE)

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

#include "ksetjmp.h"

/* ======================================================================== */
/*  exec_child_claim — 子プロセスが占有する物理ページ範囲を求める            */
/*                                                                          */
/*  範囲 A: コード + guard_a + exec ヒープ (動的確保リザーブの手前まで)      */
/*  範囲 B: guard_b + スタック (mem_end まで)                               */
/*  A と B の間の穴 (EXEC_DYN_RESERVE) は pgalloc の動的確保用に残す。       */
/*                                                                          */
/*  mark (exec_run) と free (setjmp 復帰) の双方から同じ式で計算する。       */
/*  グローバル (sys_mem_kb) と定数だけから求めるのは、setjmp 後に書き換わる  */
/*  ローカル変数を longjmp 復帰側で参照しないため。                          */
/* ======================================================================== */
static void exec_child_claim(u32 *a_start, int *a_pages,
                             u32 *b_start, int *b_pages)
{
    u32 mem_end = sys_mem_kb * 1024;
    u32 heap_base = MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE + PAGE_SIZE;
    u32 guard_b = mem_end - MEM_EXEC_STACK_SIZE - PAGE_SIZE;
    u32 a_end = guard_b;

    if (guard_b > heap_base && (guard_b - heap_base) > EXEC_DYN_RESERVE * 2) {
        a_end = guard_b - EXEC_DYN_RESERVE;
    }
    *a_start = MEM_EXEC_LOAD_ADDR;
    *a_pages = (int)((a_end - MEM_EXEC_LOAD_ADDR) / PAGE_SIZE);
    *b_start = guard_b;
    *b_pages = (int)((mem_end - guard_b) / PAGE_SIZE);
}

/* ======================================================================== */
/*  exec_exit — 現在の実行階層を終了し、親のsetjmp復帰ポイントへ戻る        */
/* ======================================================================== */
void exec_exit(int status)
{
    ExecContext *ctx;

    if (exec_nest_level > 0) {
        exec_exit_status = status;

        /* 現在のレベルのクリーンアップ (ガードページ解除 + ヒープリセット)。
         *
         * ネストレベル N で走っているプログラムのコンテキストは、親の
         * exec_run がインデックス N-1 に書いたもの。[exec_nest_level] を
         * 読むと 1 つ先 (未初期化/過去のゴミ、N==MAX_EXEC_NEST なら配列外)
         * を参照し、ゴミの guard_a に対して paging_set_page してしまう。 */
        ctx = &exec_ctx_stack[exec_nest_level - 1];
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

        /* (2) FD自動クローズ (FD 3以上の全オープンファイル)
         * ただしカーネル常駐FD (FEP辞書のSQLite接続など、
         * vfs_fd_set_protect で保護されたFD) は回収しない。 */
        {
            int fd;
            for (fd = 3; fd < VFS_MAX_OPEN_FILES; fd++) {
                if (vfs_fd_is_protected(fd)) continue;
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

        /* (5) サウンドエンジンクリーンアップ (bgm_persistでなければBGM停止) */
        snd_cleanup();

        /* (6) SQLite DB リソースクリーンアップ (未closeのDB接続を解放) */
        db_cleanup_all();

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
        /* ヒープ末尾に動的確保リザーブの穴を空ける (上のコメント参照) */
        if (exec_heap_size > EXEC_DYN_RESERVE * 2) {
            exec_heap_size -= EXEC_DYN_RESERVE;
        }
    }

    file_buf  = (u8 *)load_base;
    load_addr = (u8 *)load_base;


    /* ====== ファイル読み込み ====== */
    sz = vfs_read(path, file_buf, max_size + OS32X_HDR_V1_SIZE);

    /* フォールバック: パスにスラッシュがない場合、標準ディレクトリを順に検索 */
    /* 注意: SYS_DEFAULT_PATH (config.h) と整合させること */
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
                kstrncpy(try_path, search_dirs[di], VFS_MAX_PATH);
                kstrncat(try_path, path, VFS_MAX_PATH);
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
        /* 子プロセスが使う全域 (コード+ヒープ+スタック) を予約する。
         * コード 1MB しか予約しないと、実行中の動的確保 (V86 バッキング等)
         * がヒープ領域を「空き」と誤認して確保・ゼロクリアしてしまう。
         * マークは冪等なのでネスト exec でもそのまま呼んでよい。 */
        u32 ca_start, cb_start;
        int ca_pages, cb_pages;
        exec_child_claim(&ca_start, &ca_pages, &cb_start, &cb_pages);
        pgalloc_mark_used(ca_start, ca_pages);
        pgalloc_mark_used(cb_start, cb_pages);
    }

    {
        /* ヘッダ分だけ前方へ詰めるオーバーラップコピー。kmemcpy は
         * オーバーラップ時の動作を保証しない (rep movsd 実装の内部詳細に
         * 依存していた) ので memmove を使う。 */
        memmove(load_addr, load_addr + code_off, text_sz);
        kmemset(load_addr + text_sz, 0, bss_sz);
    }


    /* ヒープ・ガードページ設定 */
    if (is_shell) {
        /* シェルも BSS 終端からスタックガードの直前までをヒープとして使用可能にする */
        exec_heap_base = (u32)load_addr + text_sz + bss_sz;
        exec_heap_base = (exec_heap_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (guard_b > exec_heap_base) {
            exec_heap_size = guard_b - exec_heap_base;
        } else {
            exec_heap_size = 0;
        }
        ctx->exec_heap_base = exec_heap_base;
        ctx->exec_heap_size = exec_heap_size;
        kapi->sbrk_heap_limit = guard_b;
    }

    if (exec_heap_size > 0) {
        /* OS32X ヘッダの heap_size 指定があればサイズを制限 */
        if (heap_sz > 0 && heap_sz < exec_heap_size) {
            exec_heap_size = heap_sz;
            ctx->exec_heap_size = exec_heap_size;
        }
        exec_heap_init_at(exec_heap_base, exec_heap_size);
    }

    if (!is_shell) {
        kapi->sbrk_heap_limit = guard_a;
        ctx->sbrk_heap_limit = guard_a;

        /* ガードページ設定。失敗＝保護なしで走ることを意味するので必ず検知する */
        if (paging_set_not_present(guard_a, guard_a + PAGE_SIZE - 1) != 0 ||
            paging_set_not_present(guard_b, guard_b + PAGE_SIZE - 1) != 0) {
            kprintf(0xC1, "[exec] guard page setup failed (a=%x b=%x)\n",
                    guard_a, guard_b);
        }
    }

    /* setjmp — 毎回実行 (ネスト対応) */
    if (exec_setjmp(ctx->jmpbuf) != 0) {
        /* ======== longjmp復帰ポイント ======== */

        /* フォルト経由の復帰では例外ゲートが IF をクリアしたまま
         * longjmp してくる (exec_longjmp は EFLAGS を復元しない)。
         * exec_run の呼び出し元は常に割り込み有効で動いているので、
         * ここで無条件に開けてよい。 */
        _enable();

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

        /* 子プロセス空間の物理ページ予約を解放する。
         * ネスト exec (親も子プロセス) の場合は領域がまだ使用中なので、
         * シェル (Level 1) まで戻ったときだけ解放する。
         * 動的確保リザーブの穴は最初から予約していないので、そこに
         * 生きている確保 (V86 バッキング等) を巻き込むことはない。 */
        if (!is_shell && exec_nest_level == 1) {
            u32 ca_start, cb_start;
            int ca_pages, cb_pages;
            exec_child_claim(&ca_start, &ca_pages, &cb_start, &cb_pages);
            pgalloc_free_n(ca_start, ca_pages);
            pgalloc_free_n(cb_start, cb_pages);
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
        u32 new_esp;
        /* 呼び出し元 ESP の退避先。
         *
         * ローカル変数にしないのは、子プログラムのスタックへ切り替えた後の
         * 復帰ムーブが %esp/%ebp 相対アドレスでは読めないため (static なら
         * 絶対アドレスでアクセスされる)。
         *
         * 単一の static だとネスト exec で上書きされる: 子 A の実行中に
         * 孫 B を exec すると B の退避値が A のものを潰し、A の main が
         * 通常 return したときに壊れた ESP を復元していた。レベル別に持つ。 */
        static u32 saved_esp_stack[MAX_EXEC_NEST];

        stack_top -= (cmd_len + 1);
        stack_top &= ~((u32)STACK_ALIGN_MASK);
        str_area = (char *)stack_top;

        stack_top -= sizeof(char *) * OS32_MAX_ARGS;
        argv_area = (char **)stack_top;

        s = cmdline;
        d = str_area;
        while (*s) {
            char quote;

            /* 引数間の空白をスキップ */
            while (*s == ' ') s++;
            if (!*s) break;

            /* 新しい引数を開始 */
            if (argc < OS32_MAX_ARGS - 1) argv_area[argc++] = d;

            /* クォート対応トークナイザ */
            while (*s && *s != ' ') {
                if (*s == '"' || *s == '\'') {
                    /* クォート開始 — 対応する閉じクォートまで取り込む */
                    quote = *s++;
                    while (*s && *s != quote) {
                        if (*s == '\\' && quote == '"' && *(s + 1)) {
                            /* ダブルクォート内のバックスラッシュエスケープ */
                            s++;
                        }
                        *d++ = *s++;
                    }
                    if (*s == quote) s++;  /* 閉じクォートをスキップ */
                } else if (*s == '\\' && *(s + 1)) {
                    /* バックスラッシュエスケープ */
                    s++;
                    *d++ = *s++;
                } else {
                    *d++ = *s++;
                }
            }
            *d++ = '\0';
        }
        argv_area[argc] = NULL;

        /* ---- 呼び出しフレームを子スタック上に自分で組む ----
         *
         * 以前は引数 3 つを asm 内で push しており、しかも "g" 制約で
         * 渡していた。"g" はメモリオペランドを許すので GCC が
         * 「-4(%esp)」のような **ESP 相対アドレス**を選ぶことがあり、
         * その場合 `mov %1, %%esp` でスタックを切り替えた後の push が
         * 子スタック上の無関係な場所を読む。実際に壊れていなかったのは
         * レジスタが選ばれていた偶然でしかない。
         *
         * 引数を C 側で書き込んでおけば asm は「ESP を差し替えて call」
         * だけになり、入力は new_esp と entry の 2 本 (どちらも "r") で済む。
         *
         * ExecEntry は __cdecl (int argc, char **argv, KernelAPI *api) なので
         * 低位から argc, argv, kapi の順に並べる。
         * call 時点で ESP を 16 バイト境界に揃えるのは SysV i386 ABI の
         * 要求 (GCC は SSE スピルでこれを前提にする)。 */
        new_esp = (stack_top - 3 * sizeof(u32)) & ~(u32)15;
        ((u32 *)new_esp)[0] = (u32)argc;
        ((u32 *)new_esp)[1] = (u32)argv_area;
        ((u32 *)new_esp)[2] = (u32)kapi;

        __asm__ volatile(
            "movl %%esp, %0\n\t"
            "movl %1, %%esp\n\t"
            "call *%2\n\t"
            "movl %0, %%esp"
            : "=m"(saved_esp_stack[exec_nest_level - 1])
            : "r"(new_esp), "r"(entry)
            : "eax", "ecx", "edx", "cc", "memory"
        );
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS;
}
