/* ======================================================================== */
/*  IDT.H — 割り込みディスクリプタテーブル / PIC / PIT 定義                 */
/*                                                                          */
/*  PC-98固有のポートアドレスを使用 (FreeBSD pc98 atpic.c 参照)             */
/* ======================================================================== */

#ifndef __IDT_H
#define __IDT_H

/* ======== 基本型 ======== */
#include "types.h"

/* ======================================================================== */
/*  IDT ゲートディスクリプタ (8バイト)                                      */
/* ======================================================================== */
struct idt_entry {
    u16 offset_low;     /* ハンドラアドレス下位16ビット */
    u16 selector;       /* コードセグメントセレクタ (0x08) */
    u8  zero;           /* 予約 (0) */
    u8  type_attr;      /* タイプ/属性 */
    u16 offset_high;    /* ハンドラアドレス上位16ビット */
} __attribute__((packed));

/* IDTR レジスタ用構造体 (パック) */
struct idt_ptr {
    u16 limit;          /* IDTサイズ - 1 (= 256*8 - 1 = 2047) */
    u32 base;           /* IDTベースアドレス */
} __attribute__((packed));

/* IDTタイプ/属性ビット */
#define IDT_ATTR_INT_GATE32  0x8E   /* P=1, DPL=0, 32ビット割込みゲート */
#define IDT_ATTR_TRAP_GATE32 0x8F   /* P=1, DPL=0, 32ビットトラップゲート */

/* GDTコードセグメントセレクタ */
#define KERNEL_CS  0x08

/* IDTエントリ数 */
#define IDT_ENTRIES  256

/* ======================================================================== */
/*  PIC (8259A) — PC-98固有ポートアドレス                                   */
/*  出典: FreeBSD sys/x86/isa/atpic.c, PC9800Bible §1-4                    */
/* ======================================================================== */

/* マスタPIC (IO_ICU1 = 0x00) */
#define PIC1_CMD   0x00    /* ICW1/OCW2/OCW3 ライト, IRR/ISR リード */
#define PIC1_DATA  0x02    /* ICW2-4/OCW1(IMR) ライト, IMR リード */

/* スレーブPIC (IO_ICU2 = 0x08) */
#define PIC2_CMD   0x08
#define PIC2_DATA  0x0A

/* PC-98: カスケードはIR7 (PC/ATはIR2) */
#define ICU_SLAVEID  7

/* ICW1 */
#define ICW1_INIT    0x11  /* IC4=1, カスケード, エッジトリガ */

/* ICW3 */
#define ICW3_MASTER  0x80  /* IR7にスレーブ接続 (1<<7) */
#define ICW3_SLAVE   0x07  /* マスタのIR7に接続 (ID=7) */

/* ICW4 — PC-98固有 (FreeBSD実装より) */
/*   PC-98: バッファモード + SFNM, Auto EOI不可 */
#define ICW4_MASTER  0x1D  /* SFNM=1, BUF=1, M/S=1, AEOI=0, 8086=1 */
#define ICW4_SLAVE   0x09  /* SFNM=0, BUF=1, M/S=0, AEOI=0, 8086=1 */

/* OCW2 (EOI) */
#define OCW2_EOI     0x20  /* 非特定EOI */

/* OCW3 */
#define OCW3_IRR     0x0A  /* IRR読み出し指定 */
#define OCW3_ISR     0x0B  /* ISR読み出し指定 */

/* 再マッピング後のベクタ番号 */
#define PIC_MASTER_OFFSET  0x20   /* IRQ0-7 → INT 0x20-0x27 */
#define PIC_SLAVE_OFFSET   0x28   /* IRQ8-15 → INT 0x28-0x2F */

/* IRQ番号 → INTベクタ */
#define IRQ_TO_INT(irq)  ((irq) < 8 ? PIC_MASTER_OFFSET + (irq) \
                                     : PIC_SLAVE_OFFSET + (irq) - 8)

/* ======================================================================== */
/*  PIT (8254) — PC-98固有ポートアドレス                                    */
/*  出典: FreeBSD sys/pc98/include/timerreg.h, PC9800Bible §2-3            */
/* ======================================================================== */

#define PIT_CNTR0    0x71  /* カウンタ#0 (インターバルタイマ) */
#define PIT_CNTR1    0x3FDB /* カウンタ#1 (スピーカー) — 特殊アドレス */
#define PIT_CNTR2    0x75  /* カウンタ#2 (RS-232C) */
#define PIT_MODE     0x77  /* モードレジスタ */

/* システムクロック: NP21/W動作確認済み                                      */
/* 注意: PC9800BibleとUNDOCUMENTEDでMHz系との対応が逆転 (値自体は正しい)     */
#define PIT_CLOCK    1996800UL

/* PITモードバイト: カウンタ#0, LSB/MSB, モード2(レートジェネレータ), バイナリ */
#define PIT_MODE_TIMER0  0x34  /* 00 11 010 0 */

/* ======================================================================== */
/*  公開API                                                                */
/* ======================================================================== */

void idt_init(void);
void pic_init(void);
void pit_init(unsigned int hz);

void irq_enable(unsigned int irq);
void irq_disable(unsigned int irq);
void pic_eoi(unsigned int irq);

/* フレームカウンタ (タイマ割り込みで更新) */
extern volatile u32 tick_count;

#endif /* __IDT_H */
