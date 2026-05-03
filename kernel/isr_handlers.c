/* ======================================================================== */
/*  ISR_HANDLERS.C — C言語割り込みハンドラ                                  */
/*                                                                          */
/*  例外: テキストVRAMにエラー情報を表示して停止                            */
/*  タイマ: フレームカウンタの管理                                          */
/* ======================================================================== */

#include "idt.h"
#include "io.h"
#include "paging.h"
#include "memmap.h"

/* exec フォルト復帰用 (exec.c で定義) */
extern volatile int exec_nest_level;
extern void exec_fault_recover(void);

#include "serial.h"

/* シリアルポートにもログを出力するためのヘルパー */
static void sputs(const char *str)
{
    serial_puts_polled(str);
}

static void sput_hex32(u32 val)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 7; i >= 0; i--) {
        buf[i+2] = hex[val & 0xF];
        val >>= 4;
    }
    buf[10] = '\0';
    serial_puts_polled(buf);
}

/* テキストVRAM直接アクセス (ベアメタル) */
#define TVRAM_CHAR  ((volatile u16 *)0xA0000UL)
#define TVRAM_ATTR  ((volatile u16 *)0xA2000UL)

/* ------------------------------------------------------------------------ */
/*  tvram_puts_at — テキストVRAMに文字列を直接書き込み                     */
/* ------------------------------------------------------------------------ */
static void tvram_puts_at(int row, int col, const char *str, u8 attr)
{
    int pos = row * 80 + col;
    while (*str) {
        TVRAM_CHAR[pos] = (u16)(u8)*str;
        TVRAM_ATTR[pos] = (u16)attr;
        str++;
        pos++;
    }
}

/* ------------------------------------------------------------------------ */
/*  tvram_put_hex32 — 32ビット値を16進数で表示                              */
/* ------------------------------------------------------------------------ */
static void tvram_put_hex32(int row, int col, u32 val, u8 attr)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
    tvram_puts_at(row, col, buf, attr);
}

/* ======================================================================== */
/*  exception_handler — CPU例外ハンドラ                                    */
/*  テキストVRAMにエラー情報を表示して停止する                              */
/* ======================================================================== */

/* 例外名テーブル */
static const char *exception_names[] = {
    "#DE Divide Error",        /* 0 */
    "#DB Debug",               /* 1 */
    "NMI",                     /* 2 */
    "#BP Breakpoint",          /* 3 */
    "#OF Overflow",            /* 4 */
    "#BR Bound Range",         /* 5 */
    "#UD Invalid Opcode",      /* 6 */
    "#NM No Math",             /* 7 */
    "#DF Double Fault",        /* 8 */
    "Coprocessor Overrun",     /* 9 */
    "#TS Invalid TSS",         /* 10 */
    "#NP Segment Not Present", /* 11 */
    "#SS Stack Fault",         /* 12 */
    "#GP General Protection",  /* 13 */
    "#PF Page Fault",          /* 14 */
};

void exception_handler(u32 error_code, u32 vector, u32 fault_eip,
                       u32 *regs)
{
    const char *name;
    int row = 0;

    _disable();

    /* 画面上部クリア */
    {
        int r;
        for (r = 0; r < 15; r++) {
            tvram_puts_at(r, 0,
                "                                        "
                "                                        ", 0x07);
        }
    }

    if (vector < 15)
        name = exception_names[vector];
    else
        name = "Unknown Exception";

    /* 例外情報 */
    tvram_puts_at(row,   0, "==== EXCEPTION ====", 0x41);
    tvram_puts_at(row+1, 0, " Type: ", 0x41);
    tvram_puts_at(row+1, 7, name, 0x41);
    tvram_puts_at(row+2, 0, " Vec: 0x", 0xE1);
    tvram_put_hex32(row+2, 8, vector, 0xE1);
    tvram_puts_at(row+2, 17, " ErrC: 0x", 0xE1);
    tvram_put_hex32(row+2, 26, error_code, 0xE1);
    tvram_puts_at(row+3, 0, " EIP: 0x", 0xE1);
    tvram_put_hex32(row+3, 8, fault_eip, 0xE1);

    /* EIPの領域判定 */
    {
        extern u32 __sqlite_start, __sqlite_end;
        u32 sq_s = (u32)&__sqlite_start;
        u32 sq_e = (u32)&__sqlite_end;
        if (fault_eip >= sq_s && fault_eip < sq_e) {
            tvram_puts_at(row+3, 17, "[sqlite]", 0xA1);
        } else if (fault_eip >= KERNEL_LOAD_ADDR && fault_eip < 0x200000) {
            tvram_puts_at(row+3, 17, "[.text]", 0xA1);
        } else {
            tvram_puts_at(row+3, 17, "[OUT OF CODE!]", 0xC1);
        }
    }

    /* レジスタダンプ (PUSHADの保存順序: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX) */
    tvram_puts_at(row+4, 0, " EAX=", 0xC1);
    tvram_put_hex32(row+4, 5, regs[7], 0xC1);
    tvram_puts_at(row+4, 14, " EBX=", 0xC1);
    tvram_put_hex32(row+4, 19, regs[4], 0xC1);
    tvram_puts_at(row+4, 28, " ECX=", 0xC1);
    tvram_put_hex32(row+4, 33, regs[6], 0xC1);

    tvram_puts_at(row+5, 0, " EDX=", 0xC1);
    tvram_put_hex32(row+5, 5, regs[5], 0xC1);
    tvram_puts_at(row+5, 14, " ESI=", 0xC1);
    tvram_put_hex32(row+5, 19, regs[1], 0xC1);
    tvram_puts_at(row+5, 28, " EDI=", 0xC1);
    tvram_put_hex32(row+5, 33, regs[0], 0xC1);

    tvram_puts_at(row+6, 0, " EBP=", 0xC1);
    tvram_put_hex32(row+6, 5, regs[2], 0xC1);
    tvram_puts_at(row+6, 14, " ESP=", 0xC1);
    tvram_put_hex32(row+6, 19, regs[3], 0xC1);

    /* EBPチェーンによるスタックトレース (最大8フレーム) */
    tvram_puts_at(row+7, 0, "---- Stack Trace ----", 0xE1);
    {
        u32 ebp = regs[2];
        int frame;
        int tr = row + 8;
        for (frame = 0; frame < 8 && tr < 20; frame++) {
            u32 ret_addr;
            u32 prev_ebp;
            if (ebp < 0x90000 || ebp >= 0xF00000) break;
            if (!paging_is_present(ebp) ||
                !paging_is_present(ebp + 4)) break;
            prev_ebp = *(u32 *)ebp;
            ret_addr = *(u32 *)(ebp + 4);
            tvram_puts_at(tr, 0, " #", 0xA1);
            tvram_put_hex32(tr, 2, (u32)frame, 0xA1);
            tvram_puts_at(tr, 10, " ret=0x", 0xA1);
            tvram_put_hex32(tr, 17, ret_addr, 0xA1);
            tvram_puts_at(tr, 26, " ebp=0x", 0xA1);
            tvram_put_hex32(tr, 33, prev_ebp, 0xA1);
            tr++;
            ebp = prev_ebp;
        }
    }

    /* ESPからのスタックダンプ */
    {
        int dr = 20;
        u32 esp = regs[3];
        int wi;
        tvram_puts_at(dr, 0, "---- Stack Dump (ESP) ----", 0xE1);
        dr++;
        for (wi = 0; wi < 8 && dr < 25; wi += 2) {
            u32 addr0 = esp + (u32)wi * 4;
            u32 addr1 = esp + (u32)(wi + 1) * 4;
            if (paging_is_present(addr0)) {
                tvram_put_hex32(dr, 0, addr0, 0x07);
                tvram_puts_at(dr, 9, ":", 0x07);
                tvram_put_hex32(dr, 10, *(u32 *)addr0, 0xE1);
            }
            if (paging_is_present(addr1)) {
                tvram_puts_at(dr, 19, " ", 0x07);
                tvram_put_hex32(dr, 20, addr1, 0x07);
                tvram_puts_at(dr, 29, ":", 0x07);
                tvram_put_hex32(dr, 30, *(u32 *)addr1, 0xE1);
            }
            dr++;
        }
    }

    /* exec実行中なら復帰、それ以外はシステム停止 */
    if (exec_nest_level > 0) {
        tvram_puts_at(24, 0, " >> Returning to shell...               ", 0xA1);
        _enable();
        exec_fault_recover();
    }

    tvram_puts_at(24, 0, " System halted.                         ", 0x41);
    for (;;) { /* hlt */ }
}

/* ======================================================================== */
/*  page_fault_handler — ページフォルト (#PF) 専用ハンドラ                  */
/*                                                                          */
/*  error_code ビット:                                                      */
/*    bit 0: P   — 0=Not-Present, 1=Protection violation                   */
/*    bit 1: W/R — 0=Read, 1=Write                                         */
/*    bit 2: U/S — 0=Supervisor, 1=User                                    */
/*  fault_addr: CR2 (障害が発生した仮想アドレス)                            */
/* ======================================================================== */
void page_fault_handler(u32 error_code, u32 fault_addr, u32 fault_eip, u32 *regs)
{
    int row = 0;  /* 画面最上部から表示 (最大限の情報量) */

    _disable();

    /* 画面上部をクリア (15行のみ — row 15以降のテスト出力を保持) */
    {
        int r;
        for (r = 0; r < 15; r++) {
            tvram_puts_at(r, 0,
                "                                        "
                "                                        ", 0x07);
        }
    }

    tvram_puts_at(row,   0, "==== PAGE FAULT (#PF) ====", 0x41);
    sputs("\n\n==== PAGE FAULT (#PF) ====\n");
    
    tvram_puts_at(row+1, 0, " Addr: 0x", 0xE1);
    tvram_put_hex32(row+1, 9, fault_addr, 0xE1);
    sputs("Addr: "); sput_hex32(fault_addr);
    
    tvram_puts_at(row+1, 18, " ErrC: 0x", 0xE1);
    tvram_put_hex32(row+1, 27, error_code, 0xE1);
    sputs(" ErrC: "); sput_hex32(error_code); sputs("\n");
    
    tvram_puts_at(row+2, 0, " EIP:  0x", 0xE1);
    tvram_put_hex32(row+2, 9, fault_eip, 0xE1);
    sputs("EIP:  "); sput_hex32(fault_eip);

    /* EIPがコードセクション内かチェック */
    {
        extern u32 __sqlite_start, __sqlite_end;
        u32 sq_s = (u32)&__sqlite_start;
        u32 sq_e = (u32)&__sqlite_end;
        if (fault_eip >= sq_s && fault_eip < sq_e) {
            tvram_puts_at(row+2, 18, "[.sqlite_text]", 0xA1);
            sputs(" [.sqlite_text]\n");
        } else if (fault_eip >= KERNEL_LOAD_ADDR && fault_eip < 0x200000) {
            tvram_puts_at(row+2, 18, "[.text]", 0xA1);
            sputs(" [.text]\n");
        } else {
            tvram_puts_at(row+2, 18, "[OUT OF CODE!]", 0xC1);
            sputs(" [OUT OF CODE!]\n");
        }
    }

    /* 原因 */
    tvram_puts_at(row+3, 0, " Cause: ", 0xE1);
    sputs("Cause: ");
    if (error_code & 0x02) {
        tvram_puts_at(row+3, 8, "WRITE ", 0xC1);
        sputs("WRITE ");
    } else {
        tvram_puts_at(row+3, 8, "READ  ", 0xC1);
        sputs("READ  ");
    }
    if (error_code & 0x01) {
        tvram_puts_at(row+3, 14, "R/O page", 0xC1);
        sputs("R/O page\n");
    } else {
        tvram_puts_at(row+3, 14, "Not-Present", 0xC1);
        sputs("Not-Present\n");
    }

    /* レジスタダンプ (PUSHADの保存順序: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX) */
    tvram_puts_at(row+4, 0, " EAX=", 0xC1);
    tvram_put_hex32(row+4, 5, regs[7], 0xC1);
    sputs("EAX="); sput_hex32(regs[7]);
    
    tvram_puts_at(row+4, 14, " EBX=", 0xC1);
    tvram_put_hex32(row+4, 19, regs[4], 0xC1);
    sputs(" EBX="); sput_hex32(regs[4]);
    
    tvram_puts_at(row+4, 28, " ECX=", 0xC1);
    tvram_put_hex32(row+4, 33, regs[6], 0xC1);
    sputs(" ECX="); sput_hex32(regs[6]); sputs("\n");

    tvram_puts_at(row+5, 0, " EDX=", 0xC1);
    tvram_put_hex32(row+5, 5, regs[5], 0xC1);
    sputs("EDX="); sput_hex32(regs[5]);
    
    tvram_puts_at(row+5, 14, " ESI=", 0xC1);
    tvram_put_hex32(row+5, 19, regs[1], 0xC1);
    sputs(" ESI="); sput_hex32(regs[1]);
    
    tvram_puts_at(row+5, 28, " EDI=", 0xC1);
    tvram_put_hex32(row+5, 33, regs[0], 0xC1);
    sputs(" EDI="); sput_hex32(regs[0]); sputs("\n");

    tvram_puts_at(row+6, 0, " EBP=", 0xC1);
    tvram_put_hex32(row+6, 5, regs[2], 0xC1);
    sputs("EBP="); sput_hex32(regs[2]);
    
    tvram_puts_at(row+6, 14, " ESP=", 0xC1);
    tvram_put_hex32(row+6, 19, regs[3], 0xC1);
    sputs(" ESP="); sput_hex32(regs[3]); sputs("\n");

    /* EBPチェーンによるスタックトレース (最大8フレーム) */
    tvram_puts_at(row+7, 0, "---- Stack Trace ----", 0xE1);
    sputs("---- Stack Trace ----\n");
    {
        u32 ebp = regs[2]; /* 保存された EBP */
        int frame;
        int tr = row + 8;
        for (frame = 0; frame < 8 && tr < 24; frame++) {
            u32 ret_addr;
            u32 prev_ebp;
            /* EBPが有効なアドレスか安全チェック */
            if (ebp < 0x90000 || ebp >= 0xF00000) break;
            if (!paging_is_present(ebp) ||
                !paging_is_present(ebp + 4)) break;
            prev_ebp = *(u32 *)ebp;
            ret_addr = *(u32 *)(ebp + 4);
            tvram_puts_at(tr, 0, " #", 0xA1);
            tvram_put_hex32(tr, 2, (u32)frame, 0xA1);
            tvram_puts_at(tr, 10, " ret=", 0xA1);
            tvram_put_hex32(tr, 15, ret_addr, 0xA1);
            tvram_puts_at(tr, 26, " ebp=", 0xA1);
            tvram_put_hex32(tr, 31, prev_ebp, 0xA1);
            
            sputs("#"); sput_hex32((u32)frame);
            sputs(" ret="); sput_hex32(ret_addr);
            sputs(" ebp="); sput_hex32(prev_ebp); sputs("\n");
            
            tr++;
            ebp = prev_ebp;
        }
    }

    /* スタック先頭付近のメモリダンプ (16ワード) */
    {
        int dr = row + 15;
        u32 esp = regs[3];
        int wi;
        tvram_puts_at(dr, 0, "---- Stack Dump (ESP) ----", 0xE1);
        sputs("---- Stack Dump (ESP) ----\n");
        dr++;
        for (wi = 0; wi < 8 && dr < 21; wi += 2) {
            u32 addr0 = esp + (u32)wi * 4;
            u32 addr1 = esp + (u32)(wi + 1) * 4;
            if (paging_is_present(addr0)) {
                tvram_put_hex32(dr, 0, addr0, 0x07);
                tvram_puts_at(dr, 9, ":", 0x07);
                tvram_put_hex32(dr, 10, *(u32 *)addr0, 0xE1);
                
                sput_hex32(addr0); sputs(":"); sput_hex32(*(u32 *)addr0);
            }
            if (paging_is_present(addr1)) {
                tvram_puts_at(dr, 19, " ", 0x07);
                tvram_put_hex32(dr, 20, addr1, 0x07);
                tvram_puts_at(dr, 29, ":", 0x07);
                tvram_put_hex32(dr, 30, *(u32 *)addr1, 0xE1);
                
                sputs("  "); sput_hex32(addr1); sputs(":"); sput_hex32(*(u32 *)addr1);
            }
            sputs("\n");
            dr++;
        }
    }

    /* exec実行中なら復帰、それ以外はシステム停止 */
    if (exec_nest_level > 0) {
        tvram_puts_at(24, 0, " >> Returning to shell...               ", 0xA1);
        _enable();
        exec_fault_recover();
    }

    tvram_puts_at(24, 0, " System halted.                         ", 0x41);
    for (;;) { /* hlt */ }
}

/* ======================================================================== */
/*  timer_handler — タイマ割り込みハンドラ (IRQ0)                           */
/*  tick_countのインクリメントはASMスタブで行う                             */
/* ======================================================================== */
extern void snd_tick(void);  /* kernel/snd_engine.c */

/* V86割り込みリフレクト用 (v86.h) */
extern volatile int v86_active;
extern void v86_set_pending_irq(int irq_no);

extern void v86_inject_timer_irq(u32 *regs);

void timer_handler(u32 *regs)
{
    snd_tick();

    /* V86モード中: IRQ0 (INT 08h) をV86タスクにリフレクト予約 */
    if (v86_active) {
        v86_inject_timer_irq(regs);
    }
}

/* ======================================================================== */
/*  fdc_irq_handler — FDD割り込みハンドラ (IRQ11)                          */
/*  µPD765Aコマンド完了時に呼ばれ、完了フラグをセットする                   */
/* ======================================================================== */
extern volatile u32 fdc_irq_fired;  /* fdc.c で定義 */

void fdc_irq_handler(void)
{
    fdc_irq_fired = 1;
}

