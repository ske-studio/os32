/* ======================================================================== */
/*  ISR_HANDLERS.C — C言語割り込みハンドラ                                  */
/*                                                                          */
/*  例外: テキストVRAMにエラー情報を表示して停止                            */
/*  タイマ: フレームカウンタの管理                                          */
/* ======================================================================== */

#include "idt.h"
#include "io.h"
#include "paging.h"

/* exec フォルト復帰用 (exec.c で定義) */
extern volatile int is_exec_running;
extern void exec_fault_recover(void);

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

void exception_handler(u32 error_code, u32 vector)
{
    const char *name;
    int row = 16;

    _disable();

    if (vector < 15)
        name = exception_names[vector];
    else
        name = "Unknown Exception";

    /* 赤背景で例外情報を表示 */
    tvram_puts_at(row,   0, "========================================", 0x41);
    tvram_puts_at(row+1, 0, " EXCEPTION: ", 0x41);
    tvram_puts_at(row+1, 12, name, 0x41);
    tvram_puts_at(row+2, 0, " Vector:     0x", 0xE1);
    tvram_put_hex32(row+2, 15, vector, 0xE1);
    tvram_puts_at(row+3, 0, " Error Code: 0x", 0xE1);
    tvram_put_hex32(row+3, 15, error_code, 0xE1);
    tvram_puts_at(row+4, 0, "========================================", 0x41);

    /* exec実行中なら復帰、それ以外はシステム停止 */
    if (is_exec_running) {
        tvram_puts_at(row+5, 0, " >> Returning to shell...               ", 0xA1);
        _enable();
        exec_fault_recover();
    }

    tvram_puts_at(row+5, 0, " System halted.", 0x41);
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
void page_fault_handler(u32 error_code, u32 fault_addr, u32 fault_eip)
{
    int row = 14;

    _disable();



    tvram_puts_at(row,   0, "========================================", 0x41);
    tvram_puts_at(row+1, 0, " PAGE FAULT (#PF)                       ", 0x41);
    tvram_puts_at(row+2, 0, " Fault Addr: 0x", 0xE1);
    tvram_put_hex32(row+2, 15, fault_addr, 0xE1);
    tvram_puts_at(row+3, 0, " Error Code: 0x", 0xE1);
    tvram_put_hex32(row+3, 15, error_code, 0xE1);
    tvram_puts_at(row+4, 0, " Fault EIP:  0x", 0xE1);
    tvram_put_hex32(row+4, 15, fault_eip, 0xE1);

    /* 原因表示 */
    tvram_puts_at(row+5, 0, " Cause: ", 0xE1);
    if (error_code & 0x02) {
        tvram_puts_at(row+5, 8, "WRITE to ", 0xC1);
    } else {
        tvram_puts_at(row+5, 8, "READ from ", 0xC1);
    }
    if (error_code & 0x01) {
        tvram_puts_at(row+5, 18, "Read-Only page", 0xC1);
    } else {
        tvram_puts_at(row+5, 18, "Not-Present page", 0xC1);
    }

    /* スタックガード検出 (0x8F000-0x8FFFF) */
    if (fault_addr >= 0x8F000UL && fault_addr <= 0x8FFFFUL) {
        tvram_puts_at(row+6, 0, " >>> STACK OVERFLOW DETECTED <<<        ", 0x41);
    }
    /* NULLポインタ検出 (0x0000-0x0FFF) */
    else if (fault_addr < 0x1000UL) {
        tvram_puts_at(row+6, 0, " >>> NULL POINTER ACCESS <<<            ", 0x41);
    }
    /* IVT/BIOSデータ書き込み検出 */
    else if (fault_addr < 0x7000UL && (error_code & 0x02)) {
        tvram_puts_at(row+6, 0, " >>> IVT/BIOS DATA CORRUPTION <<<       ", 0x41);
    }
    else {
        tvram_puts_at(row+6, 0, "                                        ", 0x41);
    }

    tvram_puts_at(row+7, 0, "========================================", 0x41);

    /* exec実行中なら復帰、それ以外はシステム停止 */
    if (is_exec_running) {
        tvram_puts_at(row+8, 0, " >> Returning to shell...               ", 0xA1);
        _enable();
        exec_fault_recover();
    }

    tvram_puts_at(row+8, 0, " System halted.                         ", 0x41);
    for (;;) { /* hlt */ }
}

/* ======================================================================== */
/*  timer_handler — タイマ割り込みハンドラ (IRQ0)                           */
/*  tick_countのインクリメントはASMスタブで行う                             */
/*  ここでは追加処理（将来のBGM再生等）のみ                                */
/* ======================================================================== */
void timer_handler(void)
{
    /* 将来: BGM再生、タスクスケジューリングなど */
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
