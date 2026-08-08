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
#include "v86_bios.h"
#include "loop_dev.h"
#include "idt.h"
#include "kprintf.h"

/* setjmp/longjmp (kernel/setjmp.asm) — セッションからの脱出に使う */
extern int  exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf, int val);

static u32 v86_jmpbuf[6];
static volatile int v86_active = 0;
static enum v86_exit_reason v86_exit_reason = V86_EXIT_NONE;

/* 直近の #GP の観測値。
 *
 * static にすると kernel.map に出ずホストから読めない。セッションが
 * 終わるまで診断値が見えないと、走り続けているゲストの様子が分からず
 * 手が止まる。実際それで詰まったのでグローバルにしてある。 */
u32 v86_gp_cs = 0;
u32 v86_gp_ip = 0;
u32 v86_gp_fl = 0;
u32 v86_gp_n  = 0;

/* #GP 以外の例外が V86 中に起きたときの観測値。
 * v86_fault_handler() が埋める。 */
u32 v86_flt_vec = 0;            /* 例外ベクタ */
u32 v86_flt_cs  = 0;
u32 v86_flt_ip  = 0;
u32 v86_flt_fl  = 0;
u32 v86_flt_op  = 0;            /* CS:IP の先頭 4 バイト */
u32 v86_flt_n   = 0;            /* 発生回数 */

enum v86_exit_reason v86_get_exit_reason(void) { return v86_exit_reason; }
u32 v86_last_gp_cs(void) { return v86_gp_cs; }
u32 v86_last_gp_ip(void) { return v86_gp_ip; }
u32 v86_last_gp_eflags(void) { return v86_gp_fl; }
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
/*  IOPL=3 なので CLI / STI / PUSHF / POPF / IRET はここに来ない。           */
/*  来るのは I/O 許可ビットマップで塞いだポートへの IN/OUT、INT n、          */
/*  そして本当に未対応の命令。                                              */
/*                                                                          */
/*  INT n が来る理由: V86 の INT n は IOPL に関係なくプロテクトモードの      */
/*  IDT を引く仕様で、OS32 のゲートは DPL=0 / ゲストは CPL=3 なので          */
/*  「softint && gate.DPL < CPL」で #GP になる。おかげでトランポリンを       */
/*  仕込まなくても BIOS コールを横取りできる。                              */
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

/* ======================================================================== */
/*  v86_fault_handler — #GP 以外の例外を V86 中に食らったとき                */
/*                                                                          */
/*  #GP だけ V86 分岐を持たせていた頃は、ゲストが不正命令を実行すると        */
/*  OS32 の汎用例外ハンドラが「カーネルが EIP=0x120 で落ちた」と表示して     */
/*  シェルへ戻していた。**V86 のフォルトがカーネルのバグに化けて見える**の   */
/*  が最悪で、EFLAGS の VM ビットに気づくまで原因が分からない。              */
/*                                                                          */
/*  ここに落ちたらセッションは畳むしかないが、少なくとも                    */
/*  「どのベクタが、ゲストのどの命令で起きたか」は残す。                    */
/* ======================================================================== */
void v86_fault_handler(u32 *frame, u32 vector)
{
    u32 cs = frame[V86F_CS];
    u32 ip = frame[V86F_EIP];
    const u8 *pc = v86_ptr(cs, ip);

    v86_flt_n++;
    v86_flt_vec = vector;
    v86_flt_cs  = cs;
    v86_flt_ip  = ip;
    v86_flt_fl  = frame[V86F_EFLAGS];
    v86_flt_op  = (u32)pc[0] | ((u32)pc[1] << 8) |
                  ((u32)pc[2] << 16) | ((u32)pc[3] << 24);

    v86_exit_reason = V86_EXIT_FAULT;
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
    v86_gp_fl = frame[V86F_EFLAGS];

    /* ゲストの割り込み状態に依存しない歯止め。
     * CLI したまま #GP を出し続けるゲストはここで止める。 */
    if (v86_gp_n > V86_GP_LIMIT) {
        v86_exit_reason = V86_EXIT_TIMEOUT;
        return 1;
    }


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

    case 0xCD:  /* INT imm8 */
        /* V86 の INT n は IOPL に関係なくプロテクトモードの IDT を引く。
         * OS32 のゲートは DPL=0、ゲストは CPL=3 なので必ずここに落ちる。
         * (IOPL=3 が消してくれるのは CLI/STI/PUSHF/POPF/IRET のほう) */
        {
            u32 vec = pc[len + 1];
            v86_advance_ip(frame, len + 2);
            if (v86_bios_is_hle(vec)) {
                /* HLE 対象: レジスタと CF を直接書いて戻す。
                 * INT は配送前に落ちているのでゲストスタックは無傷。 */
                v86_bios_dispatch(frame, vec);
            } else {
                /* それ以外はゲスト自身の IVT (多くは BIOS ROM) へ流す */
                v86_inject_int(frame, vec);
            }
        }
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

/* ======================================================================== */
/*  割り込み反射                                                            */
/*                                                                          */
/*  V86 実行中に実ハードウェアの IRQ が入ると、CPU は IDT 経由で Ring0 の    */
/*  IRQ スタブに来る。OS32 自身の処理を済ませたあと、ゲストにも同じ割り込みを */
/*  見せたい場合はここでリアルモードのソフトウェア割り込みを合成する。      */
/*                                                                          */
/*  やることは実機の INT n と同じ:                                          */
/*    ゲストスタックに FLAGS, CS, IP を積む → IVT[vector] から CS:IP を     */
/*    ロード → IF と TF を落とす                                            */
/*                                                                          */
/*  Phase 0 の実測で、Ys が差し替えている割り込みは IRQ12 (INT 14h) ただ 1 本 */
/*  で、FM の演奏はそれだけに依存していることが分かっている。前回の実装は    */
/*  IRQ0/1/2 しか注入しておらず、ゲストが使う唯一の IRQ を落としていた       */
/*  (docs/tasks/v86v2/03_ys_profile.md §4)。                                */
/* ======================================================================== */

static u32 v86_irq_n = 0;

/* セッションのタイムアウト。タイマ反射のたびに数え、上限を超えたら
 * セッションを畳む。
 *
 * 注意: IOPL=3 ではゲストの CLI が実 IF を落とすため、ゲストが CLI した
 * まま暴走するとタイマ割り込み自体が来なくなり、この計測も止まる。
 * その場合の脱出手段は別途必要 (docs/tasks/v86v2/04_implementation_status.md §7)。 */
static u32 v86_ticks = 0;
static u32 v86_tick_limit = 0;

u32 v86_irq_reflect_count(void) { return v86_irq_n; }
int v86_is_active(void)         { return v86_active; }

/* 割り込み合成の本体。#GP フレームと IRQ フレームでインデックスが
 * 1 ワードずれるだけで、やることは同じ (実機の INT n と等価)。 */
static void v86_do_int(u32 *frame, u32 vector,
                       int i_eip, int i_cs, int i_eflags, int i_esp, int i_ss)
{
    u32 sp;
    u16 *gstack;
    const u16 *ivt;

    /* ゲストスタックに 3 ワード積む。SP は 16bit なのでラップさせる。 */
    sp = (frame[i_esp] - 6) & 0xFFFFU;
    gstack = (u16 *)v86_ptr(frame[i_ss], sp);

    /* ゲストには VM ビットを見せない (下位 16bit だけ積む) */
    gstack[0] = (u16)(frame[i_eip] & 0xFFFFU);
    gstack[1] = (u16)(frame[i_cs]  & 0xFFFFU);
    gstack[2] = (u16)(frame[i_eflags] & 0xFFFFU);

    frame[i_esp] = sp;

    /* IVT からハンドラを引く。IVT はゲストの物理 0 番地にある。 */
    ivt = (const u16 *)(vector * 4);
    frame[i_eip] = ivt[0];
    frame[i_cs]  = ivt[1];

    /* 実機の割り込み受理と同じく IF と TF を落とす。
     * ゲストの IRET がスタックから FLAGS を戻すので IF は自動で復帰する。 */
    frame[i_eflags] &= ~(EFLAGS_IF | 0x100UL);
}

void v86_reflect_irq(u32 *frame, u32 vector)
{
    if (!v86_active) {
        return;
    }
    v86_do_int(frame, vector,
               V86I_EIP, V86I_CS, V86I_EFLAGS, V86I_ESP, V86I_SS);
    v86_irq_n++;
}

void v86_inject_int(u32 *frame, u32 vector)
{
    v86_do_int(frame, vector,
               V86F_EIP, V86F_CS, V86F_EFLAGS, V86F_ESP, V86F_SS);
}

/* タイマ IRQ の反射経路から呼ばれる。上限を超えたら非 0 を返す。 */
int v86_tick_and_check_timeout(void)
{
    if (!v86_active || v86_tick_limit == 0) {
        return 0;
    }
    if (++v86_ticks >= v86_tick_limit) {
        v86_exit_reason = V86_EXIT_TIMEOUT;
        return 1;
    }
    return 0;
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
    u32 saved_eflags;

    if (v86_active) {
        return -1;      /* 再入禁止 */
    }

    /* セッションから抜けるのは #GP ハンドラからの longjmp なので、
     * 戻ってきた時点の EFLAGS は「例外に入った直後の状態」になっている。
     *
     *   - IF は割り込みゲートが自動でクリアしている
     *   - IOPL は V86 ゲストの値 (3) がそのまま残る
     *
     * longjmp は EFLAGS を復元しないため、これを直さずに返すと
     * カーネルが割り込み禁止のまま動き続ける。実際にこれを踏んで、
     * シェルがタイマ待ちループから二度と抜けなくなった。 */
    __asm__ volatile("pushfl\n\tpopl %0" : "=r"(saved_eflags));

    v86_exit_reason = V86_EXIT_NONE;
    v86_gp_n = 0;
    v86_irq_n = 0;
    v86_ticks = 0;
    v86_tick_limit = V86_TICK_LIMIT;
    saved_esp0 = tss_get_esp0();

    /* サウンドボードの IRQ をセッション中だけ開ける。
     * ゲストの FM ドライバは OPN のタイマ割り込みで演奏を進めるので、
     * これを開けないと「音源は見えるのに曲が鳴らない」状態になる。 */
    irq_enable(V86_IRQ_SOUND);

    if (exec_setjmp(v86_jmpbuf) == 0) {
        v86_active = 1;
        v86_enter(ctx);         /* 正常には戻らない */
    }

    /* longjmp で戻ってきた */
    v86_active = 0;
    irq_disable(V86_IRQ_SOUND);
    tss_set_esp0(saved_esp0);

    /* IF と IOPL を呼び出し前の状態に戻す */
    __asm__ volatile("pushl %0\n\tpopfl" : : "r"(saved_eflags) : "cc");

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
/* ゲストコード。正本は kernel/v86_test16.asm。
 * 変更したら次で再生成すること:
 *   nasm -f bin kernel/v86_test16.asm -o /tmp/t.bin
 *
 * 動作: メモリ書き込み → 許可ポート I/O (トラップしない) →
 *       拒否ポート I/O (#GP で捕まる) → IVT[08h] に自前 ISR を登録 →
 *       STI して IRQ0 の反射を 3 回待つ → 証拠を書いて HLT */
static const u8 v86_test_code[] = {
    0xB8, 0x00, 0x8C, 0x8E, 0xC0, 0x31, 0xFF, 0xB8, 0x34, 0x12, 0x26, 0x89,
    0x05, 0xB0, 0x00, 0xE6, 0x5F, 0xE4, 0x60, 0xE4, 0x00, 0xB0, 0x55, 0xE6,
    0x00, 0x31, 0xC0, 0x8E, 0xC0, 0x26, 0xC7, 0x06, 0x20, 0x00, 0x5E, 0x00,
    0x26, 0xC7, 0x06, 0x22, 0x00, 0x00, 0x8A, 0xB8, 0x00, 0x8C, 0x8E, 0xD8,
    0xC7, 0x06, 0x04, 0x00, 0x00, 0x00, 0xFB, 0xA1, 0x04, 0x00, 0x83, 0xF8,
    0x03, 0x72, 0xF8, 0xFA, 0xB4, 0x04, 0xB0, 0x90, 0xCD, 0x1B, 0xBB, 0x00,
    0x00, 0x73, 0x08, 0x80, 0xFC, 0x60, 0x75, 0x03, 0xBB, 0x05, 0xB1, 0x89,
    0x1E, 0x06, 0x00, 0xC7, 0x06, 0x02, 0x00, 0x78, 0x56, 0xF4, 0x50, 0x1E,
    0xB8, 0x00, 0x8C, 0x8E, 0xD8, 0xFF, 0x06, 0x04, 0x00, 0x1F, 0x58, 0xCF
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
u32 v86_smoke_irq    = 0;   /* ゲストへ反射した IRQ 回数 */
u32 v86_smoke_gcnt   = 0;   /* ゲスト ISR が数えた回数 */
u32 v86_smoke_bios   = 0;   /* ゲストから見た BIOS コールの結果 */
u32 v86_smoke_bcnt   = 0;   /* カーネルが処理した BIOS コール数 */
u32 v86_smoke_gpcs   = 0;   /* 最後に #GP したゲスト CS */
u32 v86_smoke_gpip   = 0;   /* 同 IP */
u32 v86_smoke_gpop   = 0;   /* 同 先頭オペコード 4 バイト */
u32 v86_smoke_gpfl   = 0;   /* 同 ゲスト EFLAGS (IOPL 確認用) */

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
    v86_smoke_irq    = v86_irq_reflect_count();
    v86_smoke_gcnt   = (u32)*(volatile u16 *)(V86_TEST_MAGIC_ADDR + 4);
    v86_smoke_bios   = (u32)*(volatile u16 *)(V86_TEST_MAGIC_ADDR + 6);
    v86_smoke_bcnt   = v86_bios_call_count();
    v86_smoke_gpcs   = v86_last_gp_cs();
    v86_smoke_gpip   = v86_last_gp_ip();
    v86_smoke_gpop   = *(volatile u32 *)v86_ptr(v86_smoke_gpcs, v86_smoke_gpip);
    v86_smoke_gpfl   = v86_last_gp_eflags();

    v86_mem_teardown();

    /* 期待値:
     *   HLT で終了 / メモリ書き込み成功 / 拒否ポートの #GP 後も継続できた /
     *   I/O トラップは拒否ポートの 2 回だけ (許可ポートは素通し) */
    /* 期待値:
     *   HLT で終了 / メモリ書き込み成功 / 拒否ポートの #GP 後も継続 /
     *   I/O トラップは拒否ポートの 2 回だけ (許可ポートは素通し) /
     *   ゲスト ISR が 3 回以上呼ばれた (= IRQ 反射が届いた) */
    v86_smoke_result = (reason == V86_EXIT_HLT &&
                        v86_smoke_magic  == V86_TEST_MAGIC &&
                        v86_smoke_magic2 == 0x5678U &&
                        v86_smoke_iotrap == 2U &&
                        v86_smoke_gcnt   >= 3U &&
                        v86_smoke_bios   == 0xB105U &&
                        v86_smoke_bcnt   == 1U) ? 0 : 1;
    return (int)v86_smoke_result;
}

/* ======================================================================== */
/*  ディスク読み出しテスト (Phase 3-3b)                                     */
/*                                                                          */
/*  ゲストに INT 1Bh の READ DATA を発行させ、読めた内容をカーネルが         */
/*  loop_dev で直接読んだものと突き合わせる。BIOS の HLE が本当に            */
/*  ディスクの中身を運べているかを、ゲスト側の自己申告に頼らず確かめる。    */
/* ======================================================================== */

/* 正本は kernel/v86_test16_disk.asm。
 *   nasm -f bin kernel/v86_test16_disk.asm -o /tmp/t.bin で再生成する。 */
static const u8 v86_disk_code[] = {
    0xB8, 0x00, 0x8C, 0x8E, 0xD8, 0xC7, 0x06, 0x08, 0x00, 0x00, 0x00, 0xB8,
    0x00, 0x8D, 0x8E, 0xC0, 0x31, 0xED, 0xB1, 0x00, 0xB5, 0x01, 0xB6, 0x00,
    0xB2, 0x01, 0xBB, 0x00, 0x01, 0xB8, 0x90, 0x06, 0xCD, 0x1B, 0x72, 0x06,
    0xC7, 0x06, 0x08, 0x00, 0x5C, 0xD1, 0xF4
};

/* 検証用に外へ出す値 */
u32 v86_disk_result = 0xFFFFFFFFUL;
u32 v86_disk_marker = 0;        /* ゲストが書いた成功マーカー */
u32 v86_disk_cmp    = 0;        /* 直読との一致バイト数 */
u32 v86_disk_reason = 0;

int v86_disk_test(const char *path)
{
    struct v86_context ctx;
    static u8 ref[V86_DISK_SECLEN];
    const u8 *got;
    u32 i, same;
    int reason;
    u16 cyls; u8 heads, spt; u16 bps; u32 total;

    v86_disk_result = 0xFFFFFFFFUL;
    v86_disk_marker = 0;
    v86_disk_cmp    = 0;

    /* 先にカーネル側で「正解」を読んでおく。
     * バッキング RAM を張る前に読むこと (張った後は VFS が使う低位メモリが
     * ゲストのものに差し替わっている)。 */
    if (v86_bios_attach_disk(path) != 0) {
        v86_disk_result = 0x8001;
        return -1;
    }
    /* ゲスト側のテストコードは 256 バイト/セクタ固定で書いてある
     * (PC-98 2HD のブートトラックは 256B x 16 が標準)。
     * イメージがそれ以外なら、突き合わせが成立しないので弾く。 */
    if (loop_dev_get_geometry(V86_DISK_SLOT, &cyls, &heads, &spt,
                              &bps, &total) != 0) {
        v86_bios_detach_disk();
        v86_disk_result = 0x8004;
        return -1;
    }
    if (bps != V86_DISK_SECLEN) {
        v86_bios_detach_disk();
        v86_disk_result = 0x8005;
        return -1;
    }
    if (loop_dev_read_chs(V86_DISK_SLOT, 0, 0, 1, ref) != 0) {
        v86_bios_detach_disk();
        v86_disk_result = 0x8002;
        return -1;
    }

    if (v86_mem_setup() != 0) {
        v86_bios_detach_disk();
        v86_disk_result = 0x8003;
        return -1;
    }

    for (i = 0; i < sizeof(v86_disk_code); i++) {
        ((volatile u8 *)V86_TEST_CODE_ADDR)[i] = v86_disk_code[i];
    }
    *(volatile u16 *)(V86_TEST_MAGIC_ADDR + 8) = 0;

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

    /* teardown でバッキング RAM が消える前に結果を回収する */
    v86_disk_marker = (u32)*(volatile u16 *)(V86_TEST_MAGIC_ADDR + 8);
    got = (const u8 *)V86_TEST_BUF_ADDR;
    same = 0;
    for (i = 0; i < V86_DISK_SECLEN; i++) {
        if (got[i] == ref[i]) {
            same++;
        }
    }
    v86_disk_cmp = same;
    v86_disk_reason = (u32)reason;

    v86_mem_teardown();
    v86_bios_detach_disk();

    v86_disk_result = (reason == V86_EXIT_HLT &&
                       v86_disk_marker == 0xD15CU &&
                       same == V86_DISK_SECLEN) ? 0 : 1;
    return (int)v86_disk_result;
}

/* ======================================================================== */
/*  ディスクイメージからのブート                                            */
/*                                                                          */
/*  PC-98 の起動シーケンスをなぞる: IPL (C0/H0/S1) を 1FC0:0000 に読み、     */
/*  SS:SP をBIOS 相当の低位スタックに置いて、そこへ制御を渡す。             */
/*  あとはゲストが INT 1Bh を使って自力で続きを読み込む。                   */
/* ======================================================================== */

u32 v86_boot_reason  = 0;   /* 終了理由 */
u32 v86_boot_gp      = 0;   /* #GP 回数 */
u32 v86_boot_bios    = 0;   /* BIOS コール回数 */
u32 v86_boot_iotrap  = 0;   /* I/O トラップ回数 */
u32 v86_boot_ioport  = 0;   /* 最後にトラップした I/O ポート */
u32 v86_boot_gpcs    = 0;   /* 最後の #GP のゲスト CS */
u32 v86_boot_gpip    = 0;   /* 同 IP */
u32 v86_boot_gpop    = 0;   /* 同 オペコード 4 バイト */
u32 v86_boot_irq     = 0;   /* ゲストへ反射した IRQ 回数 */

int v86_boot(const char *path)
{
    struct v86_context ctx;
    int reason;

    if (v86_bios_attach_disk(path) != 0) {
        return -1;
    }
    if (v86_mem_setup() != 0) {
        v86_bios_detach_disk();
        return -2;
    }

    /* IPL を読む。ここはカーネルが直接読む (ゲストはまだ動いていない)。 */
    if (loop_dev_read_chs(V86_DISK_SLOT, 0, 0, 1,
                          (void *)V86_IPL_ADDR) != 0) {
        v86_mem_teardown();
        v86_bios_detach_disk();
        return -3;
    }

    ctx.eip    = 0x0000;
    ctx.cs     = V86_IPL_SEG;
    ctx.eflags = V86_EFLAGS_INIT;
    ctx.esp    = V86_BOOT_SP;
    ctx.ss     = V86_BOOT_SS;
    ctx.es     = 0x0000;
    ctx.ds     = 0x0000;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    reason = v86_run(&ctx);

    /* teardown 前に観測値を回収する */
    v86_boot_reason = (u32)reason;
    v86_boot_gp     = v86_gp_count();
    v86_boot_bios   = v86_bios_call_count();
    v86_boot_iotrap = v86_io_trap_count();
    v86_boot_ioport = (u32)v86_io_last_port();
    v86_boot_irq    = v86_irq_reflect_count();
    v86_boot_gpcs   = v86_last_gp_cs();
    v86_boot_gpip   = v86_last_gp_ip();
    v86_boot_gpop   = *(volatile u32 *)v86_ptr(v86_boot_gpcs, v86_boot_gpip);

    v86_mem_teardown();
    v86_bios_detach_disk();
    return reason;
}
