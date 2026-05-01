/* ======================================================================== */
/*  V86.C - 仮想8086モード (VDM) #GPハンドラ                               */
/*                                                                          */
/*  V86モードで特権命令が実行されると#GPが発生する。                        */
/*  このハンドラでV86命令をデコードし、エミュレーションを行う。             */
/*                                                                          */
/*  Phase 0: 基本命令デコーダ (INT/CLI/STI/PUSHF/POPF/HLT/IRET)           */
/*  後のフェーズでRust (os32_v86) に移行予定。                              */
/* ======================================================================== */

#include "v86.h"
#include "v86_mem.h"
#include "v86_bios.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "io.h"

/* V86モードの有効フラグ */
volatile int v86_active = 0;

/* V86タスクの仮想IFフラグ (CLI/STIで操作される) */
u32 v86_virtual_if = EFLAGS_IF;

/* 保留中の仮想IRQビットマスク */
u32 v86_pending_irq = 0;

/* V86終了要求フラグ (v86_test.cから設定される) */
extern volatile int v86_exit_request;

/* ====================================================================== */
/*  V86アドレス → カーネル用リニアアドレス変換                             */
/*  v86_mem.h の v86_phys_addr() を使用する。                              */
/*  バッキングRAM (0x300000) にマッピングされた領域を正しく変換する。      */
/* ====================================================================== */
#define v86_linear(seg, off)  v86_phys_addr((seg), (off))

/* ====================================================================== */
/*  V86スタック操作ヘルパー                                                */
/* ====================================================================== */

/* V86スタックに16bitワードをpush */
static void v86_push16(u32 *regs, u16 val)
{
    u16 *sp;
    regs[V86_REG_ESP] = (regs[V86_REG_ESP] - 2) & 0xFFFF;
    sp = (u16 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
    *sp = val;
}

/* V86スタックから16bitワードをpop */
static u16 v86_pop16(u32 *regs)
{
    u16 *sp;
    u16 val;
    sp = (u16 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
    val = *sp;
    regs[V86_REG_ESP] = (regs[V86_REG_ESP] + 2) & 0xFFFF;
    return val;
}

/* ====================================================================== */
/*  v86_gp_handler — V86モード #GP ハンドラ                                */
/*                                                                          */
/*  isr_stub.asm から呼ばれる。regs配列を操作して次の命令に進める。         */
/*  regs[] の内容は v86.h の V86_REG_* を参照。                            */
/* ====================================================================== */
int v86_gp_handler(u32 *regs)
{
    u8 *ip;
    u8 opcode;

    /* フォルト位置の命令を取得 */
    ip = v86_linear(regs[V86_REG_CS], regs[V86_REG_EIP]);
    opcode = *ip;

    switch (opcode) {

    /* ================================================================ */
    /*  INT n (0xCD nn) — ソフトウェア割り込み                          */
    /*  V86内のIVT (0x0000:0x0000) を参照してハンドラに転送する         */
    /* ================================================================ */
    case 0xCD: {
        u8 intno = ip[1];

        /* ============================================================ */
        /*  DOS終了割り込みの特殊処理                                   */
        /*  INT 20h (Terminate Program) → V86終了                      */
        /*  INT 21h AH=4Ch (Exit Process) → V86終了                    */
        /* ============================================================ */
        if (intno == 0x20) {
            /* INT 20h: DOS Terminate — V86モード終了 */
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            return 1;
        }
        if (intno == 0x21 &&
            ((regs[V86_REG_EAX] >> 8) & 0xFF) == 0x4C) {
            /* INT 21h AH=4Ch: Exit Process — V86モード終了 */
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            return 1;
        }

        /* ============================================================ */
        /*  PC-98 BIOS割り込みのエミュレーション                               */
        /*  INT 18h (Text/KB/GFX BIOS) → v86_bios_int18()              */
        /*  INT 29h (DOS 1文字高速出力) → v86_bios_int29()              */
        /* ============================================================ */
        if (intno == 0x18) {
            int rc = v86_bios_int18(regs);
            if (rc >= 0) {
                /* 処理済み: EIPを進めてV86に戻る */
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
                if (rc == 1) return 1;  /* V86終了要求 */
                break;
            }
            /* rc == -1: 未実装 — IVT転送にフォールスルー */
        }
        if (intno == 0x29) {
            v86_bios_int29(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            break;
        }
        if (intno == 0x1C) {
            int rc = v86_bios_int1c(regs);
            if (rc >= 0) {
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
                break;
            }
            /* rc == -1: IVT転送にフォールスルー */
        }

        /* INT 11h (機器構成取得) */
        if (intno == 0x11) {
            v86_bios_int11(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            break;
        }
        /* INT 12h (メモリサイズ取得) */
        if (intno == 0x12) {
            v86_bios_int12(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            break;
        }

        /* 通常のINT: IVT参照してV86内ハンドラに転送 */
        {
            u32 *ivt = (u32 *)v86_linear(0, 0);
            u16 handler_off = (u16)(ivt[intno] & 0xFFFF);
            u16 handler_seg = (u16)(ivt[intno] >> 16);

            /* V86スタックにフラグ/CS/IPをpush (リアルモードINTと同じ) */
            v86_push16(regs, (u16)(regs[V86_REG_EFLAGS] & 0xFFFF));
            v86_push16(regs, (u16)regs[V86_REG_CS]);
            v86_push16(regs, (u16)(regs[V86_REG_EIP] + 2));

            /* IVTのハンドラに転送 */
            regs[V86_REG_CS] = handler_seg;
            regs[V86_REG_EIP] = handler_off;

            /* 仮想IFをクリア (INTはIF=0にする) */
            v86_virtual_if = 0;
        }
        break;
    }

    /* ================================================================ */
    /*  CLI (0xFA) — 仮想IFクリア                                      */
    /* ================================================================ */
    case 0xFA:
        v86_virtual_if = 0;
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;

    /* ================================================================ */
    /*  STI (0xFB) — 仮想IFセット                                      */
    /* ================================================================ */
    case 0xFB:
        v86_virtual_if = EFLAGS_IF;
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;

    /* ================================================================ */
    /*  PUSHF (0x9C) — 仮想EFLAGSをpush                                */
    /* ================================================================ */
    case 0x9C: {
        u16 flags = (u16)((regs[V86_REG_EFLAGS] & 0xFFFF) | v86_virtual_if);
        v86_push16(regs, flags);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  POPF (0x9D) — 仮想EFLAGSをpop                                  */
    /* ================================================================ */
    case 0x9D: {
        u16 flags = v86_pop16(regs);
        /* IFビットは仮想フラグに反映 */
        v86_virtual_if = (flags & EFLAGS_IF) ? EFLAGS_IF : 0;
        /* EFLAGS下位16bit更新 (VM,IOPL等は変更しない) */
        regs[V86_REG_EFLAGS] = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                              | (flags & 0x7FD5UL);  /* 安全なビットのみ */
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IRET (0xCF) — V86内での割り込みリターン                        */
    /* ================================================================ */
    case 0xCF: {
        u16 new_ip = v86_pop16(regs);
        u16 new_cs = v86_pop16(regs);
        u16 new_flags = v86_pop16(regs);

        regs[V86_REG_EIP] = new_ip;
        regs[V86_REG_CS]  = new_cs;

        /* IFビットを仮想フラグに反映 */
        v86_virtual_if = (new_flags & EFLAGS_IF) ? EFLAGS_IF : 0;
        regs[V86_REG_EFLAGS] = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                              | (new_flags & 0x7FD5UL);
        break;
    }

    /* ================================================================ */
    /*  HLT (0xF4) — V86テスト中はV86モードを終了する                   */
    /* ================================================================ */
    case 0xF4:
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        if (v86_exit_request || v86_active) {
            /* V86終了要求: 戻り値 1 で isr_stub.asm が復帰処理を行う */
            return 1;
        }
        break;

    /* ================================================================ */
    /*  IN AL, imm8 (0xE4 pp) — I/Oポート入力 (即値)                   */
    /* ================================================================ */
    case 0xE4: {
        u8 port = ip[1];
        u8 val;
        /* PIC仮想化 */
        if (v86_pic_io(port, &val, 0)) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | val;
        } else if (v86_pit_io(port, &val, 0)) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | val;
        } else {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | inp(port);
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT imm8, AL (0xE6 pp) — I/Oポート出力 (即値)                  */
    /* ================================================================ */
    case 0xE6: {
        u8 port = ip[1];
        u8 val = (u8)(regs[V86_REG_EAX] & 0xFF);
        /* リセットポート検知 */
        if (v86_pic_is_reboot(port, val)) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
            return 1;  /* V86終了 */
        }
        /* PIC仮想化 → PIT仮想化 → 実ハードウェア */
        if (!v86_pic_io(port, &val, 1)) {
            if (!v86_pit_io(port, &val, 1)) {
                outp(port, val);
            }
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AL, DX (0xEC) — I/Oポート入力 (DXで指定)                    */
    /* ================================================================ */
    case 0xEC: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        u8 val;
        if (v86_pic_io(port, &val, 0)) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | val;
        } else if (v86_pit_io(port, &val, 0)) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | val;
        } else {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL) | inp(port);
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT DX, AL (0xEE) — I/Oポート出力 (DXで指定)                   */
    /* ================================================================ */
    case 0xEE: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        u8 val = (u8)(regs[V86_REG_EAX] & 0xFF);
        if (v86_pic_is_reboot(port, val)) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
            return 1;
        }
        if (!v86_pic_io(port, &val, 1)) {
            if (!v86_pit_io(port, &val, 1)) {
                outp(port, val);
            }
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AX, imm8 (0xE5 pp) — 16bit I/O入力 (即値)                   */
    /* ================================================================ */
    case 0xE5: {
        u8 port = ip[1];
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | inpw(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT imm8, AX (0xE7 pp) — 16bit I/O出力 (即値)                  */
    /* ================================================================ */
    case 0xE7: {
        u8 port = ip[1];
        outpw(port, (u16)(regs[V86_REG_EAX] & 0xFFFF));
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AX, DX (0xED) — 16bit I/O入力 (DXで指定)                    */
    /* ================================================================ */
    case 0xED: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | inpw(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT DX, AX (0xEF) — 16bit I/O出力 (DXで指定)                   */
    /* ================================================================ */
    case 0xEF: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        outpw(port, (u16)(regs[V86_REG_EAX] & 0xFFFF));
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  未対応命令 — デバッグ用に停止                                   */
    /* ================================================================ */
    default: {
        /* テキストVRAMにエラー情報を表示 */
        volatile u16 *tvram = (volatile u16 *)0xA0000UL;
        volatile u16 *tattr = (volatile u16 *)0xA2000UL;
        static const char msg[] = "V86 #GP: unknown opcode 0x";
        static const char hex[] = "0123456789ABCDEF";
        int i;
        int pos = 0;
        for (i = 0; msg[i]; i++) {
            tvram[pos] = (u16)(u8)msg[i];
            tattr[pos] = 0x41;
            pos++;
        }
        tvram[pos] = (u16)(u8)hex[(opcode >> 4) & 0xF];
        tattr[pos] = 0x41;
        pos++;
        tvram[pos] = (u16)(u8)hex[opcode & 0xF];
        tattr[pos] = 0x41;

        /* 停止 */
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }
    } /* switch */
    return 0;
}

/* ====================================================================== */
/*  v86_inject_irq — V86タスクに仮想割り込みをインジェクトする              */
/*                                                                          */
/*  IRQハンドラ (isr_stub.asm) からV86モード中に呼ばれる。                  */
/*  V86のスタックフレームを書き換え、IVT経由でV86内ハンドラに転送する。     */
/*                                                                          */
/*  regs: V86スタックフレーム (isr_stub.asm のPUSHAD + CPUフレーム)         */
/*        ただしIRQ用はV86_REG_*とオフセットが異なる場合がある。            */
/*        ここではtimer/kbd共通の簡易方式を使う。                           */
/*                                                                          */
/*  intno: リフレクト先のINT番号 (08h=タイマ, 09h=キーボード)              */
/*                                                                          */
/*  ★注意: この関数は直接regs[]を操作せず、IVTアドレスとフラグだけを        */
/*  書き換える軽量な実装。irq_stub側でV86スタック操作を行う。               */
/* ====================================================================== */
void v86_set_pending_irq(int irq_no)
{
    v86_pending_irq |= (1U << irq_no);
}
