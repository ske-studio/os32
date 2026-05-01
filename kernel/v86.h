/* ======================================================================== */
/*  V86.H - 仮想8086モード (VDM) ヘッダ                                    */
/*                                                                          */
/*  OS32シェルから FreeDOS(98) を V86モードで実行するための定義。            */
/* ======================================================================== */

#ifndef V86_H
#define V86_H

#include "types.h"

/* EFLAGS定数 */
#define EFLAGS_IF    0x0200U
#define EFLAGS_VM    0x020000UL
#define EFLAGS_IOPL0 0x0000U

/* V86コンテキスト (v86_entry.asm に渡す構造体) */
struct v86_context {
    u32 eip;        /* V86エントリポイント */
    u32 cs;         /* V86コードセグメント */
    u32 eflags;     /* EFLAGS (VM=1, IF=1, IOPL=0) */
    u32 esp;        /* V86スタックポインタ */
    u32 ss;         /* V86スタックセグメント */
    u32 es;         /* ES */
    u32 ds;         /* DS */
    u32 fs;         /* FS */
    u32 gs;         /* GS */
};

/* V86 #GPハンドラのスタックフレーム配列インデックス
 * (isr_stub.asm の PUSHAD + CPUが自動pushしたフレーム) */
#define V86_REG_EDI     0
#define V86_REG_ESI     1
#define V86_REG_EBP     2
#define V86_REG_ESP_D   3   /* PUSHADが保存したESP (ダミー) */
#define V86_REG_EBX     4
#define V86_REG_EDX     5
#define V86_REG_ECX     6
#define V86_REG_EAX     7
#define V86_REG_ERRCODE 8   /* CPUが自動pushしたエラーコード */
#define V86_REG_EIP     9   /* フォルト時EIP */
#define V86_REG_CS      10  /* フォルト時CS */
#define V86_REG_EFLAGS  11  /* フォルト時EFLAGS */
#define V86_REG_ESP     12  /* V86スタックポインタ */
#define V86_REG_SS      13  /* V86スタックセグメント */
#define V86_REG_ES      14
#define V86_REG_DS      15
#define V86_REG_FS      16
#define V86_REG_GS      17

/* V86モードへ遷移 (アセンブリ、v86_entry.asm) */
extern void v86_enter(const struct v86_context *ctx);

/* V86 #GPハンドラ (isr_stub.asmから呼ばれる)
 * 戻り値: 0=V86続行, 1=V86終了要求 */
int v86_gp_handler(u32 *regs);

/* V86モードが有効かどうか */
extern volatile int v86_active;

/* V86タスクの仮想IFフラグ (v86.cで定義) */
extern u32 v86_virtual_if;

/* 保留中の仮想IRQビットマスク (v86.cで定義) */
extern u32 v86_pending_irq;

/* V86タスクに仮想割り込みを保留する (IRQハンドラから呼ぶ) */
void v86_set_pending_irq(int irq_no);

#endif /* V86_H */
