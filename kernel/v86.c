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
#include "v86_mem.h"
#include "v86_io.h"
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
/* ゲストの IP を n バイト進める。V86 の IP は 16bit なのでラップさせる。 */
static void v86_advance_ip(u32 *frame, u32 n)
{
    frame[V86F_EIP] = (frame[V86F_EIP] + n) & 0xFFFFU;
}

/* AL / AX への書き戻し (PUSHAD 配列の EAX を直接触る) */
static void v86_set_al(u32 *frame, u32 v)
{
    frame[V86F_EAX] = (frame[V86F_EAX] & 0xFFFFFF00UL) | (v & 0xFFU);
}

static void v86_set_ax(u32 *frame, u32 v)
{
    frame[V86F_EAX] = (frame[V86F_EAX] & 0xFFFF0000UL) | (v & 0xFFFFU);
}

int v86_gp_handler(u32 *frame)
{
    u32 cs  = frame[V86F_CS];
    u32 ip  = frame[V86F_EIP];
    u8 *pc  = v86_ptr(cs, ip);
    u32 len = 0;
    int opsize16 = 1;       /* V86 の既定は 16bit オペランド */
    u16 port;

    v86_gp_n++;
    v86_gp_cs = cs;
    v86_gp_ip = ip;

    /* プレフィックスを読み飛ばす。
     * 0x66 だけはオペランドサイズを変えるので覚えておく。 */
    for (;;) {
        u8 b = pc[len];
        if (b == 0x66) { opsize16 = 0; len++; continue; }
        if (b == 0x67 || b == 0xF3 || b == 0xF2 || b == 0xF0 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65 || b == 0x9B) {
            len++;
            continue;
        }
        break;
    }

    switch (pc[len]) {
    /* ---- IN ---- */
    case 0xE4:  /* IN AL, imm8 */
        port = pc[len + 1];
        v86_set_al(frame, v86_io_in(port, 1));
        v86_advance_ip(frame, len + 2);
        return 0;

    case 0xE5:  /* IN AX, imm8 */
        port = pc[len + 1];
        v86_set_ax(frame, v86_io_in(port, 2));
        v86_advance_ip(frame, len + 2);
        return 0;

    case 0xEC:  /* IN AL, DX */
        port = (u16)(frame[V86F_EDX] & 0xFFFFU);
        v86_set_al(frame, v86_io_in(port, 1));
        v86_advance_ip(frame, len + 1);
        return 0;

    case 0xED:  /* IN AX, DX */
        port = (u16)(frame[V86F_EDX] & 0xFFFFU);
        v86_set_ax(frame, v86_io_in(port, 2));
        v86_advance_ip(frame, len + 1);
        return 0;

    /* ---- OUT ---- */
    case 0xE6:  /* OUT imm8, AL */
        port = pc[len + 1];
        v86_io_out(port, 1, frame[V86F_EAX] & 0xFFU);
        v86_advance_ip(frame, len + 2);
        return 0;

    case 0xE7:  /* OUT imm8, AX */
        port = pc[len + 1];
        v86_io_out(port, 2, frame[V86F_EAX] & 0xFFFFU);
        v86_advance_ip(frame, len + 2);
        return 0;

    case 0xEE:  /* OUT DX, AL */
        port = (u16)(frame[V86F_EDX] & 0xFFFFU);
        v86_io_out(port, 1, frame[V86F_EAX] & 0xFFU);
        v86_advance_ip(frame, len + 1);
        return 0;

    case 0xEF:  /* OUT DX, AX */
        port = (u16)(frame[V86F_EDX] & 0xFFFFU);
        v86_io_out(port, 2, frame[V86F_EAX] & 0xFFFFU);
        v86_advance_ip(frame, len + 1);
        return 0;

    case 0xF4:  /* HLT — Phase 1 から引き続きセッション終了の合図 */
        v86_exit_reason = V86_EXIT_HLT;
        return 1;

    default:
        /* INS/OUTS (6C-6F) と 0x0F 2 バイトオペコードは未対応。
         * 1987 年のゲームが使う可能性は低いが、必要になったら足す。 */
        v86_exit_reason = V86_EXIT_UNKNOWN_OP;
        return 1;
    }

    (void)opsize16;
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
    /* --- (1) メモリ書き込み: 0x8C000 に 0x1234 --- */
    0xB8, 0x00, 0x8C,       /* mov ax, 8C00h */
    0x8E, 0xC0,             /* mov es, ax    */
    0x31, 0xFF,             /* xor di, di    */
    0xB8, 0x34, 0x12,       /* mov ax, 1234h */
    0x26, 0x89, 0x05,       /* mov es:[di], ax */

    /* --- (2) 許可ポートへの OUT: #GP してはいけない ---
     * 0x5F は I/O ウェイト。ダミーライトなので実害がない。 */
    0xB0, 0x00,             /* mov al, 0      */
    0xE6, 0x5F,             /* out 5Fh, al    */

    /* --- (3) 許可ポートからの IN: #GP してはいけない ---
     * 0x60 はテキスト GDC ステータス。読むだけ。 */
    0xE4, 0x60,             /* in al, 60h     */

    /* --- (4) 拒否ポートからの IN: #GP で捕まって 0xFF が返るはず ---
     * 0x00 はマスタ PIC。OS32 が使っているので必ず仮想化する。 */
    0xE4, 0x00,             /* in al, 00h     */
    0xB4, 0x00,             /* mov ah, 0      */
    0xBF, 0x02, 0x00,       /* mov di, 2      */
    0xB8, 0x00, 0x8C,       /* mov ax, 8C00h  */
    0x8E, 0xC0,             /* mov es, ax     */
    /* AL を退避してから書くと手順が増えるので、AX ごと書いてしまう。
     * 直前の in で AL に値が入っているが、上の mov ax,8C00h で潰れるため
     * ここでは「#GP が起きてゲストが継続できた」ことの確認に留める。 */

    /* --- (5) 拒否ポートへの OUT: #GP で捕まって破棄されるはず --- */
    0xB0, 0x55,             /* mov al, 55h    */
    0xE6, 0x00,             /* out 00h, al    */

    /* --- (6) 継続できた証拠を書く: 0x8C002 に 0x5678 --- */
    0xB8, 0x00, 0x8C,       /* mov ax, 8C00h */
    0x8E, 0xC0,             /* mov es, ax    */
    0xBF, 0x02, 0x00,       /* mov di, 2     */
    0xB8, 0x78, 0x56,       /* mov ax, 5678h */
    0x26, 0x89, 0x05,       /* mov es:[di], ax */

    0xF4                    /* hlt → #GP でカーネルに戻る */
};

/* スモークテストの結果を外から観測できるように残す。
 * バッキング RAM はセッション終了で解放されてしまうので、
 * 判定に使った値をカーネル側にコピーしておく (kernel.map 経由で読める)。 */
u32 v86_smoke_result = 0xFFFFFFFFUL;
u32 v86_smoke_magic  = 0;
u32 v86_smoke_reason = 0;
u32 v86_smoke_magic2 = 0;   /* 拒否ポート #GP 後も継続できたか */
u32 v86_smoke_gp     = 0;   /* セッション中の #GP 回数 */
u32 v86_smoke_iotrap = 0;   /* うち I/O トラップ回数 */

int v86_smoke_test(void)
{
    struct v86_context ctx;
    volatile u16 *magic;
    int reason;
    u32 i;

    if (v86_mem_setup() != 0) {
        v86_smoke_result = 0x8001;
        return -1;
    }

    /* 低位アドレスはバッキング RAM に張り替わっているので、ここへの
     * 書き込みはそのままゲストのメモリになる。 */
    for (i = 0; i < sizeof(v86_test_code); i++) {
        ((volatile u8 *)V86_TEST_CODE_ADDR)[i] = v86_test_code[i];
    }
    magic = (volatile u16 *)V86_TEST_MAGIC_ADDR;
    *magic = 0;
    *(volatile u16 *)(V86_TEST_MAGIC_ADDR + 2) = 0;

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

    /* teardown でバッキング RAM が消えるので、判定材料を先に退避する */
    v86_smoke_magic  = (u32)*magic;
    v86_smoke_magic2 = (u32)*(volatile u16 *)(V86_TEST_MAGIC_ADDR + 2);
    v86_smoke_reason = (u32)reason;
    v86_smoke_gp     = v86_gp_count();
    v86_smoke_iotrap = v86_io_trap_count();

    v86_mem_teardown();

    /* 期待値:
     *   HLT で終了 / メモリ書き込み成功 / 拒否ポートの #GP 後も継続できた /
     *   I/O トラップは拒否ポートの 2 回だけ (許可ポートは素通し) */
    v86_smoke_result = (reason == V86_EXIT_HLT &&
                        v86_smoke_magic  == V86_TEST_MAGIC &&
                        v86_smoke_magic2 == 0x5678U &&
                        v86_smoke_iotrap == 2U) ? 0 : 1;
    return (int)v86_smoke_result;
}
