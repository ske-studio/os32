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
#include "gdt.h"
#include "tss.h"

extern void shell_print(const char *s, u8 attr);
extern void shell_print_dec(u32 val, u8 color);
extern u32 sys_mem_kb;
/* kernel/tss.c の TSS 実体。CPL=3 遷移で TSS.ESP0 を現在のカーネル ESP に
 * 合わせる (割り込み/int 0x80 のフレームが exec_run の frame を踏まないよう)。*/
extern struct tss_entry kernel_tss;
static KernelAPI *kapi;

/* ======================================================================== */
/*  KAPI トランポリン (v2 M2)                                                */
/*                                                                          */
/*  CPL=3 アプリは本物の KAPI 表 (カーネルコードポインタ) を読めない/呼べない */
/*  ので、全 PD 共有の USER ページ 1 枚に「本物と同一レイアウトのユーザ可視表 */
/*  + スタブ列」を置き、exec はアプリにこのページのアドレスを渡す。アプリの   */
/*  api->kprintf(...) は表のスタブを呼び、スタブが int 0x80 でカーネルに入る。 */
/*  カーネル band (.bss, PDE0 共有) に置き PTE を RO+USER にする              */
/*  (CR0.WP=0 なのでカーネルは RO でも書ける = per-launch のデータ更新可)。   */
/*  レイアウト (CONTRACTS C3, KernelAPI と同一オフセット):                    */
/*    0x00 magic / 0x04 version / 0x08+ 表[i]=STUB_BASE+i*8 /                */
/*    データフィールド (値) / STUB_BASE: 各 8B スタブ B8<slot>CD80C3         */
/* ======================================================================== */
static u8  ring3_tramp_raw[PAGE_SIZE * 2];   /* 4KB アライン用に 2 ページ分 */
static u32 ring3_tramp_page = 0;             /* 4KB 境界に揃えた実アドレス (=物理) */
static void ring3_trampoline_init(void);

/* int 0x80 引数コピー+呼び出しの ASM ヘルパ (kernel/ring3_entry.asm)。
 * args_src から nbytes をスタックへコピーして wrapfn を cdecl 呼び出し、
 * 戻り値 (eax) を返す。 */
extern u32 kapi_invoke(void *wrapfn, const void *args_src, u32 nbytes);

/* CPL=3 由来のフォールト/不正 slot でアプリを kill (定義は下方, v2 M1e/M2d) */
void ring3_fault_kill(void);

void exec_init(void) {
    kapi = (KernelAPI *)KAPI_ADDR;
#include "exec_kapi_init.inc"
    /* 共有メモリ先頭アドレスを公開する。
     * MEM_SHM_BASE はカーネルの __bss_end 由来で可変のため、
     * ユーザ空間側がアドレスをハードコードしてはならない。 */
    kapi->shm_base = (u32)MEM_SHM_BASE;

    /* CPL=3 用 KAPI トランポリンページを構築 (paging_init 済みが前提) */
    ring3_trampoline_init();
}

/* ======================================================================== */
/*  ring3_trampoline_init — トランポリンページの構築 (v2 M2b)               */
/* ======================================================================== */
static void ring3_trampoline_init(void)
{
    u32 page = ((u32)ring3_tramp_raw + PAGE_SIZE - 1) & ~(u32)(PAGE_SIZE - 1);
    u32 *tbl = (u32 *)page;
    u32 stub_base = (page + sizeof(KernelAPI) + 3u) & ~3u; /* 全 struct の後ろ */
    u32 i;

    ring3_tramp_page = page;

    /* magic / version は本物と同じ値 */
    tbl[0] = kapi->magic;
    tbl[1] = kapi->version;

    for (i = 0; i < KAPI_FUNC_COUNT; i++) {
        u8 *st = (u8 *)(stub_base + i * 8u);
        /* ユーザ可視表: entry[i] = スタブ i の番地 (KernelAPI fn[i] と同一 offset) */
        tbl[2 + i] = stub_base + i * 8u;
        /* スタブ: B8 <slot:imm32> CD 80 C3  (mov eax,slot; int 0x80; ret) */
        st[0] = 0xB8;
        st[1] = (u8)(i & 0xFF);
        st[2] = (u8)((i >> 8) & 0xFF);
        st[3] = (u8)((i >> 16) & 0xFF);
        st[4] = (u8)((i >> 24) & 0xFF);
        st[5] = 0xCD;   /* int */
        st[6] = 0x80;   /* 0x80 */
        st[7] = 0xC3;   /* ret */
    }

    /* データフィールド (値): KernelAPI 表と同一オフセット (fn 表の直後)。
     * index 2+KAPI_FUNC_COUNT = sbrk_heap_limit, +1 = shm_base。
     * sbrk_heap_limit は exec_run が launch 時に上書きする。 */
    tbl[2 + KAPI_FUNC_COUNT + 0] = 0;
    tbl[2 + KAPI_FUNC_COUNT + 1] = (u32)MEM_SHM_BASE;

    /* 全 PD 共有で RO+USER マップ (kernel band PDE0)。i386 は NX なしなので
     * RO でも実行可能 (スタブ実行 OK)。ユーザは書けない = スタブ改竄不可。
     * CR0.WP=0 によりカーネルは RO でも書ける (per-launch のデータ更新)。 */
    paging_set_page(page, page, PAGE_RO | PTE_USER);
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

/* ======================================================================== */
/*  リング3 (CPL=3) 実行状態 (v2 M1)                                        */
/*                                                                          */
/*  M1 は単一アプリのみ (リング3 のネストは後続)。CPL=3 で走るアプリの      */
/*  アドレス空間を 1 つだけ保持する。int 0x80 (sys_exit) の C 側ディスパッチ */
/*  がここを参照して master PD へ戻し AS を破棄する。                        */
/* ======================================================================== */

/* リング3 ユーザスタック: 0x400000 帯 (APP_BAND_PDE) 上端に置く (M1_RING3 §5)。
 * プログラムは 0x400000 から最大 1MB。スタックはその十分上、帯の最上位 64KB。 */
#define RING3_USTACK_TOP     (MEM_EXEC_LOAD_ADDR + 0x400000UL)  /* 0x800000 (帯上端, exclusive) */
#define RING3_USTACK_SIZE    0x010000UL                          /* 64KB */

static struct addrspace g_ring3_as;
static volatile int g_ring3_active = 0;

/* CPL=3 アプリをフォールト (#PF/#GP) で kill した回数 (CONTRACTS C6, v2 M1e)。
 * static にせずカーネルシンボルとして公開する (kselftest_pass 等と同じ形)。
 * PM の V4 検証が emu_read_mem で読む。 */
volatile u32 fault_kill_count = 0;

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
    u32 mem_end = sys_usable_mem_end();  /* 末尾はホットデプロイ用に予約 */
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
    /* CPL=3 (リング3) アプリからの正常終了 (トランポリン経由, v2 M2)。
     * exec_exit の後始末とシェル復帰は master PD 上で行うので、先に master
     * CR3 へ戻し AS を破棄する。CPL=0 プログラム (シェル等) は g_ring3_active
     * が偽なので従来どおり。二重破棄は g_ring3_active と destroy 側で防ぐ。 */
    if (g_ring3_active) {
        paging_load_cr3(paging_kernel_pd_phys());
        paging_addrspace_destroy(&g_ring3_as);
        g_ring3_active = 0;
    }
    exec_exit(status);
}

/* ======================================================================== */
/*  ring3_syscall_dispatch — CPL=3 からの int 0x80 ディスパッチャ (v2 M2d)   */
/*                                                                          */
/*  kernel/ring3_entry.asm の int80_stub が pushad 後のフレーム先頭を渡す。  */
/*  フレーム (u32 配列, pushad + CPU が積んだ例外フレーム):                   */
/*    [0..7]=pushad (EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX)  → EAX=[7]             */
/*    [8]=EIP [9]=CS [10]=EFLAGS [11]=userESP [12]=userSS                    */
/*                                                                          */
/*  eax(=[7]) がスタブの積んだ slot。範囲外は即 kill (CONTRACTS C4)。        */
/*  本物の KAPI 表 (KAPI_ADDR: [magic][version][fn0..]) から wrap を引き、    */
/*  ユーザスタック (userESP+4, スタブの ret アドレス分を飛ばす) から引数を    */
/*  コピーして呼ぶ。戻り値は eax スロット([7])へ書く → popad で復元される。   */
/*  現 CR3 はアプリ PD のまま呼ぶ (ユーザポインタ引数がアプリ帯で解決される)。*/
/*  sys_exit は wrap → kapi_sys_exit が teardown+longjmp するのでここへ戻らない。*/
/*  ※ ポインタ/ESP の厳密な範囲検証は M2e (ここでは上端クランプのみ)。       */
/* ======================================================================== */

/* 可変長引数 (kprintf) を拾うためのコピー窓 (固定分より広めに取る)。 */
#define RING3_ARG_WINDOW  64u

void __cdecl ring3_syscall_dispatch(u32 *frame)
{
    u32 slot     = frame[7];         /* スタブが積んだ slot (eax) */
    u32 user_esp = frame[11];        /* CPL=3 の ESP (int が積んだ) */
    const void *args_src;
    u32 nbytes;
    u32 window;
    u32 wrapptr;

    /* 範囲外 slot はワイルド呼び出し → アプリだけ kill (カーネルを飛ばさない)。
     * ring3_fault_kill は fault_kill_count++ / teardown / longjmp で戻らない。 */
    if (slot >= (u32)KAPI_FUNC_COUNT) {
        ring3_fault_kill();
    }

    /* 本物の表から wrap_<slot> を取得 ([magic][version] の後が fn 表)。 */
    wrapptr = ((const u32 *)KAPI_ADDR)[2 + slot];

    /* ユーザスタックから引数をコピー。スタブの ret アドレス分 (+4) を飛ばす。
     * 固定分 = kapi_argsize[slot]。kprintf 等の可変長のため広めの窓を取り、
     * ユーザスタック上端でクランプして over-read #PF を避ける (厳密検証は M2e)。 */
    args_src = (const void *)(user_esp + 4u);
    nbytes = (u32)kapi_argsize[slot];
    window = (nbytes < RING3_ARG_WINDOW) ? RING3_ARG_WINDOW : nbytes;
    if ((u32)args_src < RING3_USTACK_TOP &&
        (u32)args_src + window > RING3_USTACK_TOP) {
        window = RING3_USTACK_TOP - (u32)args_src;
    }

    /* 本物の wrap を呼び、戻り値を eax スロットへ (popad で復元 → スタブの
     * ret でアプリへ戻り、cdecl 呼び出し側が引数を掃除する)。 */
    frame[7] = kapi_invoke((void *)wrapptr, args_src, window);
}

/* ======================================================================== */
/*  ring3_fault_kill — CPL=3 由来の #PF/#GP でアプリを kill (v2 M1e)        */
/*                                                                          */
/*  #PF/#GP ハンドラ (kernel/isr_handlers.c) がフォールトフレームの         */
/*  CS.RPL=3 (= CPL=3 由来) を検出したときに呼ぶ。カーネルを巻き込まず       */
/*  アプリだけを畳んでシェルに戻す ([ABI4] 解消の実装点, V4)。              */
/*                                                                          */
/*  後始末は ring3_syscall_dispatch (正常終了) と同一: master CR3 復帰 →     */
/*  AS 破棄 → exec_fault_recover (= exec_exit(EXEC_ERR_FAULT) → longjmp)。   */
/*  二重破棄は g_ring3_active で防ぐ (destroy 側もアクティブ CR3 を弾く)。    */
/*  この関数は longjmp するので戻らない。                                    */
/* ======================================================================== */
void ring3_fault_kill(void)
{
    fault_kill_count++;
    if (g_ring3_active) {
        paging_load_cr3(paging_kernel_pd_phys());
        paging_addrspace_destroy(&g_ring3_as);
        g_ring3_active = 0;
    }
    exec_fault_recover();   /* longjmp するので戻らない */
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

    u32 mem_end = sys_usable_mem_end();  /* 末尾はホットデプロイ用に予約 */
    u8 *file_buf;
    u8 *load_addr;
    OS32Header *hdr;
    int sz;
    u32 code_off, text_sz, bss_sz, heap_sz, entry_off;
    ExecEntry entry;
    ExecContext *ctx;
    int want_ring3 = 0;      /* CPL=3 で走らせるか (OS32X_FLAG_RING3, v2 M1) */

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

    /* リング3 実行の意思表示 (v2 M1)。シェル (Level 0) は常駐帯域で
     * CPL=0 のまま。子プログラムだけが OS32X_FLAG_RING3 で CPL=3 に降りる。
     * M1 の対象は KAPI を使わない自己完結プログラム (トランポリンは M2)。 */
    want_ring3 = (!is_shell) && ((hdr->flags & OS32X_FLAG_RING3) != 0);
    if (want_ring3) {
        /* ユーザスタックを 0x400000 帯 (PD ごと) の上端へ移す。argv は
         * この後この stack_top を使って積まれるので、ここで差し替える。 */
        stack_top = RING3_USTACK_TOP;
    }

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
        u32 u_esp;   /* ring3: iret に渡すユーザ ESP (ダミー retaddr 込み) */
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

        if (want_ring3) {
            /* ================= CPL=3 への遷移 (v2 M1c/M1d) ================= */
            if (paging_addrspace_create(&g_ring3_as) != 0) {
                shell_print("Error: ring3 addrspace create failed\n", ATTR_RED);
                exec_nest_level--;
                return EXEC_ERR_NOMEM;
            }

            /* --- M1c: 0x400000 帯・ユーザスタック・VRAM・SHM を RW+USER に ---
             * 共有 PDE (カーネル帯域) には USER を立てない = CPL=3 から不可のまま。 */
            /* プログラム帯 (code/data/bss + 予備) */
            paging_addrspace_map_user_range(&g_ring3_as,
                MEM_EXEC_LOAD_ADDR, MEM_EXEC_LOAD_ADDR + MEM_EXEC_MAX_SIZE,
                PAGE_RW | PTE_USER);
            /* ユーザスタック帯 (0x400000 帯の上端, PD ごと) */
            paging_addrspace_map_user_range(&g_ring3_as,
                RING3_USTACK_TOP - RING3_USTACK_SIZE, RING3_USTACK_TOP,
                PAGE_RW | PTE_USER);
            /* VRAM (テキスト 0xA0000 + グラフィック 0xA8000) — C2: 全PD共有+USER */
            paging_addrspace_map_user_range(&g_ring3_as,
                0xA0000UL, 0xC0000UL, PAGE_RW | PTE_USER);
            /* SHM (アプリ間データ受け渡し) — C2: 全PD共有+USER */
            paging_addrspace_map_user_range(&g_ring3_as,
                (u32)MEM_SHM_BASE, (u32)MEM_SHM_BASE + (u32)MEM_SHM_SIZE,
                PAGE_RW | PTE_USER);
            /* KAPI トランポリンページ (RO+USER, 全PD共有)。この app PD の PDE0 に
             * USER を伝播させる (VRAM/SHM で既に立つが明示・冪等)。 */
            paging_addrspace_map_user(&g_ring3_as, ring3_tramp_page,
                ring3_tramp_page, PAGE_RO | PTE_USER);

            /* --- M2c: CPL=3 アプリには本物の表でなくトランポリン表を渡す ---
             * crt0/プログラムは実行時スタック渡しの api ポインタを使うだけなので
             * 無変更。データフィールド (sbrk_heap_limit/shm_base) を本物の表から
             * トランポリンへ反映してから渡す (CR0.WP=0 で RO ページへ書ける)。 */
            ((u32 *)ring3_tramp_page)[2 + KAPI_FUNC_COUNT + 0] =
                kapi->sbrk_heap_limit;
            ((u32 *)ring3_tramp_page)[2 + KAPI_FUNC_COUNT + 1] =
                kapi->shm_base;
            ((u32 *)new_esp)[2] = ring3_tramp_page;   /* api = トランポリン */

            /* --- crt0 スタック規約合わせ (v2 M2, retaddr ズレ修正) ---
             * crt0.asm/_start_c は CPL=0 の `call *entry` を前提にし、
             * [esp]=retaddr, [esp+4]=argc, [esp+8]=argv, [esp+12]=api を読む。
             * だが ring3 は iret でエントリへ飛ぶため call が無く retaddr が
             * 積まれず、スタックが 1 スロットずれて argc↔argv↔api が食い違う
             * (実測: argv[1] に version=0x27 が入り #PF)。iret に渡す ESP を
             * argc の 1 スロット下にし、そこにダミー retaddr を置いて
             * call 経路と同一レイアウトに揃える。crt0 は main 後 sys_exit する
             * ので retaddr へは戻らない (0 でよい)。ダミーは USER 済みの
             * ユーザスタック帯 (0x7F0000-0x800000) 内。 */
            u_esp = new_esp - sizeof(u32);
            ((u32 *)u_esp)[0] = 0;   /* ダミー retaddr */

            g_ring3_active = 1;

            /* --- M1d: iret で CPL=3 に降りる ---
             * v86_entry.asm の iretd フレーム構築 (SS/ESP/EFLAGS/CS/EIP) を流用。
             * CS=USER_CS(0x23) / SS=USER_DS(0x2B)。EFLAGS=0x202 (IF=1, IOPL=0)
             * なので CPL=3 は cli/sti/in/out で #GP (M0b で特権命令は除去済み)。
             * TSS.ESP0 を現在の ESP に設定 (v86_enter と同じ手法): CPL=3 実行中の
             * 割り込み / int 0x80 のフレームがこの直下に積まれ、exec_run の
             * setjmp フレームを踏まない。ここから通常 return しない —
             * 終了は int 0x80 → ring3_syscall_dispatch → longjmp。 */
            __asm__ volatile(
                "cli\n\t"
                "movl %%esp, %[e0]\n\t"     /* TSS.ESP0 = 現在のカーネル ESP */
                "movl %[pd], %%cr3\n\t"     /* アプリ PD へ切替 */
                "movl %[uds], %%eax\n\t"
                "movw %%ax, %%ds\n\t"
                "movw %%ax, %%es\n\t"
                "movw %%ax, %%fs\n\t"
                "movw %%ax, %%gs\n\t"
                "pushl %[uds]\n\t"          /* SS = USER_DS */
                "pushl %[uesp]\n\t"         /* ESP = ユーザスタック */
                "pushl $0x202\n\t"          /* EFLAGS: IF=1, IOPL=0 */
                "pushl %[ucs]\n\t"          /* CS = USER_CS */
                "pushl %[eip]\n\t"          /* EIP = エントリポイント */
                "iret\n\t"
                : [e0] "=m"(kernel_tss.esp0)
                : [pd]  "r"(g_ring3_as.pd_phys),
                  [uesp]"r"(u_esp),
                  [eip] "r"((u32)entry),
                  [uds] "i"(USER_DS),
                  [ucs] "i"(USER_CS)
                : "eax", "memory"
            );
            /* iret 後はここへ戻らない */
        } else {
            __asm__ volatile(
                "movl %%esp, %0\n\t"
                "movl %1, %%esp\n\t"
                "call *%2\n\t"
                "movl %0, %%esp"
                : "=m"(saved_esp_stack[exec_nest_level - 1])
                : "r"(new_esp), "r"(entry)
                : "eax", "ecx", "edx", "cc", "memory"
            );
        }
        exec_exit(EXEC_SUCCESS);
    }

    return EXEC_SUCCESS;
}
