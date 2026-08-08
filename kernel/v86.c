/* ======================================================================== */
/*  V86.C — 仮想8086モード ランタイム (Phase 1 最小実装)                    */
/*                                                                          */
/*  この段階の目的は「V86 に入って、戻って、カーネルが生きている」ことを    */
/*  確認できる最小の骨格を作ること。命令エミュレーションと I/O 仮想化は     */
/*  Phase 2 で載せる。                                                      */
/* ======================================================================== */

#include "v86.h"
#include "tss.h"
#include "paging.h"
#include "kprintf.h"

/* setjmp/longjmp (kernel/setjmp.asm) — セッションからの脱出に使う */
extern int  exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf, int val);

static u32 v86_jmpbuf[6];
static volatile int v86_active = 0;
static enum v86_exit_reason v86_exit_reason = V86_EXIT_NONE;

/* 直近の #GP の観測値 (デバッグ用) */
static u32 v86_gp_cs = 0;
static u32 v86_gp_ip = 0;
static u32 v86_gp_n  = 0;

enum v86_exit_reason v86_get_exit_reason(void) { return v86_exit_reason; }
u32 v86_last_gp_cs(void) { return v86_gp_cs; }
u32 v86_last_gp_ip(void) { return v86_gp_ip; }
u32 v86_gp_count(void)   { return v86_gp_n; }

/* ゲストの seg:off を物理アドレスに直す。
 * V86 のリニアアドレスは (seg << 4) + off で、低位 1MB は
 * アイデンティティマッピングなので物理アドレスと一致する。 */
static u8 *v86_ptr(u32 seg, u32 off)
{
    return (u8 *)(((seg & 0xFFFF) << 4) + (off & 0xFFFF));
}

/* ======================================================================== */
/*  #GP 一次ハンドラ                                                        */
/*                                                                          */
/*  戻り値 0 で V86 に復帰、非 0 でセッション終了。                         */
/*                                                                          */
/*  IOPL=3 で走らせているので INT n / CLI / STI / PUSHF / POPF / IRET は     */
/*  ここに来ない。来るのは I/O 許可ビットマップで塞いだポートへの IN/OUT と、*/
/*  本当に未対応の命令だけ。                                                */
/* ======================================================================== */
int v86_gp_handler(u32 *frame)
{
    u32 cs  = frame[V86F_CS];
    u32 ip  = frame[V86F_EIP];
    u8 *pc  = v86_ptr(cs, ip);

    v86_gp_n++;
    v86_gp_cs = cs;
    v86_gp_ip = ip;

    switch (pc[0]) {
    case 0xF4:  /* HLT — Phase 1 ではこれをセッション終了の合図に使う */
        v86_exit_reason = V86_EXIT_HLT;
        return 1;

    default:
        /* Phase 2 で IN/OUT のデコードと I/O 仮想化を載せる。
         * それまでは未知の命令として終了する。 */
        v86_exit_reason = V86_EXIT_UNKNOWN_OP;
        return 1;
    }
}

/* #GP スタブから呼ばれる脱出口。v86_run() の setjmp 地点へ戻る。 */
void v86_longjmp_out(void)
{
    v86_active = 0;
    exec_longjmp(v86_jmpbuf, 1);
}

/* ======================================================================== */
/*  v86_run — 1 セッションを実行して終了理由を返す                          */
/* ======================================================================== */
int v86_run(const struct v86_context *ctx)
{
    u32 saved_esp0;

    if (v86_active) {
        return -1;      /* 再入禁止 */
    }

    v86_exit_reason = V86_EXIT_NONE;
    v86_gp_n = 0;
    saved_esp0 = tss_get_esp0();

    if (exec_setjmp(v86_jmpbuf) == 0) {
        v86_active = 1;
        v86_enter(ctx);         /* 正常には戻らない */
    }

    /* longjmp で戻ってきた */
    v86_active = 0;
    tss_set_esp0(saved_esp0);
    return (int)v86_exit_reason;
}

/* ======================================================================== */
/*  Phase 1 スモークテスト                                                  */
/*                                                                          */
/*  最小の 16bit コードを V86 で実行し、以下を一度に確認する:               */
/*    - iretd による V86 遷移が成立する                                     */
/*    - ゲストがメモリと TVRAM に書ける (ページの USER 属性が効いている)    */
/*    - HLT が #GP としてカーネルに落ちてくる                               */
/*    - TSS.ESP0 経由のスタック切替が壊れていない                           */
/*    - longjmp でセッションから抜けてカーネルが生き残る                    */
/*                                                                          */
/*  検証が済んだら削除する一時コード。                                      */
/* ======================================================================== */

/* ゲストコード:
 *   MOV AX,8C00 / MOV ES,AX / XOR DI,DI / MOV AX,1234 / MOV ES:[DI],AX
 *   MOV AX,A000 / MOV ES,AX / MOV DI,009E / MOV AX,'V' / MOV ES:[DI],AX
 *   MOV AX,A200 / MOV ES,AX / MOV DI,009E / MOV AL,E1  / MOV ES:[DI],AL
 *   HLT
 * 0x8C000 に 0x1234 を書き、画面右上に白い 'V' を出してから HLT する。 */
static const u8 v86_test_code[] = {
    0xB8, 0x00, 0x8C,       /* mov ax, 8C00h */
    0x8E, 0xC0,             /* mov es, ax    */
    0x31, 0xFF,             /* xor di, di    */
    0xB8, 0x34, 0x12,       /* mov ax, 1234h */
    0x26, 0x89, 0x05,       /* mov es:[di], ax */

    0xB8, 0x00, 0xA0,       /* mov ax, A000h  (TVRAM 文字プレーン) */
    0x8E, 0xC0,             /* mov es, ax    */
    0xBF, 0x9E, 0x00,       /* mov di, 009Eh  (0行79桁 = 79*2) */
    0xB8, 0x56, 0x00,       /* mov ax, 'V'   */
    0x26, 0x89, 0x05,       /* mov es:[di], ax */

    0xB8, 0x00, 0xA2,       /* mov ax, A200h  (TVRAM 属性プレーン) */
    0x8E, 0xC0,             /* mov es, ax    */
    0xBF, 0x9E, 0x00,       /* mov di, 009Eh */
    0xB0, 0xE1,             /* mov al, E1h   */
    0x26, 0x88, 0x05,       /* mov es:[di], al */

    0xF4                    /* hlt → #GP でカーネルに戻る */
};

/* ゲストが触るページを user アクセス可にする。
 *
 * カーネル帯には決して付けないこと。前回の実装はトリプルフォルトの
 * 回避策としてカーネル帯に PTE_USER を付け、その結果 V86 がカーネル
 * メモリを実行できるようになり、それを「エミュレータが U/S を無視して
 * いる」と誤診した (docs/tasks/v86v2/02_np21w_paging_analysis.md)。 */
static void v86_test_map(int user)
{
    u32 flags = PAGE_RW | (user ? PTE_USER : 0);
    u32 a;

    paging_set_page(V86_TEST_CODE_ADDR,  V86_TEST_CODE_ADDR,  flags);
    paging_set_page(V86_TEST_STACK_ADDR, V86_TEST_STACK_ADDR, flags);
    paging_set_page(V86_TEST_MAGIC_ADDR, V86_TEST_MAGIC_ADDR, flags);

    /* TVRAM (文字 0xA0000-0xA1FFF / 属性 0xA2000-0xA3FFF) */
    for (a = 0xA0000UL; a < 0xA4000UL; a += PAGE_SIZE) {
        paging_set_page(a, a, flags);
    }
}

int v86_smoke_test(void)
{
    struct v86_context ctx;
    volatile u16 *magic = (volatile u16 *)V86_TEST_MAGIC_ADDR;
    int reason;
    u32 i;

    /* ゲストコードを配置し、結果格納先をクリア */
    for (i = 0; i < sizeof(v86_test_code); i++) {
        ((volatile u8 *)V86_TEST_CODE_ADDR)[i] = v86_test_code[i];
    }
    *magic = 0;

    v86_test_map(1);

    ctx.eip    = 0x0000;
    ctx.cs     = (u32)(V86_TEST_CODE_ADDR >> 4);
    ctx.eflags = V86_EFLAGS_INIT;
    ctx.esp    = 0x0FFE;
    ctx.ss     = (u32)(V86_TEST_STACK_ADDR >> 4);
    ctx.es     = ctx.ss;
    ctx.ds     = ctx.ss;
    ctx.fs     = ctx.ss;
    ctx.gs     = ctx.ss;

    reason = v86_run(&ctx);

    v86_test_map(0);    /* USER 属性を剥がす */

    /* 期待値: HLT で抜け、magic が書き換わっている */
    if (reason == V86_EXIT_HLT && *magic == V86_TEST_MAGIC) {
        return 0;
    }
    return reason ? reason : -1;
}
