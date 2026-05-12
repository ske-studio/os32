/* ======================================================================== */
/*  V86_TEST.C - V86モード動作検証テスト                                    */
/*                                                                          */
/*  COMバイナリ実行のテスト機能のみ。                                      */
/*  FreeDOSブート処理は v86_session.c に移動済み。                         */
/*  デバッグダンプ機能は v86_debug.c に移動済み。                          */
/* ======================================================================== */

#include "v86.h"
#include "v86_mem.h"
#include "v86_session.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "io.h"
#include "kprintf.h"

/* 外部TSSアクセス */
extern struct tss_entry kernel_tss;

/* exec_setjmp / exec_longjmp (setjmp.asm) */
extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* V86テスト用setjmpバッファ */
static u32 v86_test_jmpbuf[6];

/* V86カーネルスタック (v86_session.c で定義、共用) */
extern u8 v86_kstack[32768];

/* ====================================================================== */
/*  HELLO.COM 埋め込みバイナリ (tests/hello_v86.asm から生成)              */
/* ====================================================================== */
static const u8 hello_com[] = {
    0xb8, 0x00, 0xa0, 0x8e, 0xc0, 0x26, 0xc7, 0x06, 0x00, 0x00, 0x48, 0x00,
    0x26, 0xc7, 0x06, 0x02, 0x00, 0x45, 0x00, 0x26, 0xc7, 0x06, 0x04, 0x00,
    0x4c, 0x00, 0x26, 0xc7, 0x06, 0x06, 0x00, 0x4c, 0x00, 0x26, 0xc7, 0x06,
    0x08, 0x00, 0x4f, 0x00, 0x26, 0xc7, 0x06, 0x0a, 0x00, 0x20, 0x00, 0x26,
    0xc7, 0x06, 0x0c, 0x00, 0x56, 0x00, 0x26, 0xc7, 0x06, 0x0e, 0x00, 0x38,
    0x00, 0x26, 0xc7, 0x06, 0x10, 0x00, 0x36, 0x00, 0x26, 0xc7, 0x06, 0x12,
    0x00, 0x21, 0x00, 0xb8, 0x00, 0xa2, 0x8e, 0xc0, 0xb9, 0x0a, 0x00, 0x31,
    0xff, 0x26, 0xc7, 0x05, 0xe1, 0x00, 0x83, 0xc7, 0x02, 0xe2, 0xf6, 0xcd,
    0x20
};
#define HELLO_COM_SIZE  97

/* ====================================================================== */
/*  HELLO_BIOS.COM 埋め込みバイナリ (tests/hello_bios.asm から生成)        */
/* ====================================================================== */
static const u8 hello_bios_com[] = {
    0xb4, 0x16, 0xcd, 0x18, 0xb4, 0x13, 0x31, 0xd2, 0xcd, 0x18, 0xbe, 0x7a,
    0x01, 0xe8, 0x47, 0x00, 0xb4, 0x00, 0x1e, 0x07, 0x8d, 0x1e, 0xab, 0x01,
    0xcd, 0x1c, 0xbe, 0x88, 0x01, 0xe8, 0x37, 0x00, 0xa0, 0xab, 0x01, 0xe8,
    0x3b, 0x00, 0xb0, 0x2f, 0xcd, 0x29, 0xa0, 0xac, 0x01, 0xe8, 0x31, 0x00,
    0xb0, 0x0d, 0xcd, 0x29, 0xb0, 0x0a, 0xcd, 0x29, 0xbe, 0x8f, 0x01, 0xe8,
    0x19, 0x00, 0xe4, 0x02, 0xe8, 0x1e, 0x00, 0xb0, 0x0d, 0xcd, 0x29, 0xb0,
    0x0a, 0xcd, 0x29, 0xb0, 0x20, 0xe6, 0x00, 0xbe, 0x99, 0x01, 0xe8, 0x02,
    0x00, 0xcd, 0x20, 0xac, 0x08, 0xc0, 0x74, 0x04, 0xcd, 0x29, 0xeb, 0xf7,
    0xc3, 0x50, 0xc0, 0xe8, 0x04, 0xe8, 0x07, 0x00, 0x58, 0x24, 0x0f, 0xe8,
    0x01, 0x00, 0xc3, 0x04, 0x30, 0x3c, 0x39, 0x76, 0x02, 0x04, 0x07, 0xcd,
    0x29, 0xc3, 0x48, 0x45, 0x4c, 0x4c, 0x4f, 0x20, 0x42, 0x49, 0x4f, 0x53,
    0x21, 0x0d, 0x0a, 0x00, 0x44, 0x61, 0x74, 0x65, 0x3a, 0x20, 0x00, 0x50,
    0x49, 0x43, 0x20, 0x49, 0x4d, 0x52, 0x3a, 0x20, 0x00, 0x56, 0x38, 0x36,
    0x20, 0x50, 0x68, 0x61, 0x73, 0x65, 0x20, 0x32, 0x20, 0x4f, 0x4b, 0x21,
    0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#define HELLO_BIOS_COM_SIZE  177

/* ====================================================================== */
/*  COM ローダー定数                                                       */
/* ====================================================================== */
#define COM_LOAD_BASE   0x8A000UL
#define COM_SEG         0x8A00
#define COM_ENTRY       0x0100
#define COM_STACK_SEG   0x8D00
#define COM_STACK_OFF   0x1FFE

#define V86_PAGE_FLAGS (PAGE_RW | PTE_USER)

/* ====================================================================== */
/*  v86_run_com - COMバイナリをV86モードで実行                             */
/* ====================================================================== */
static int v86_run_com(const u8 *data, u32 size)
{
    struct v86_context ctx;
    u32 saved_esp0;
    u32 addr;
    u8 *psp;
    u8 *code;

    /* ================================================================== */
    /*  ページテーブル設定                                                */
    /*                                                                      */
    /*  COMバイナリ実行に必要な最小限の領域 + IRQ安全対策:                 */
    /*    0x00000-0x00FFF : IVT + BDA + ダミーIRETハンドラ (R/W, USER)     */
    /*    0x8A000-0x8E000 : COM ロード/スタック領域 (R/W, USER)            */
    /*    0xA0000-0xA3FFF : TVRAM (R/W, USER)                              */
    /*    0xF0000-0xFFFFF : BIOS ROM (R/O, USER) — IRQ0 IVT安全帯        */
    /* ================================================================== */

    /* IVT/BDA ページ (0x00000): IVT読み取り + ダミーIRETハンドラに必要 */
    paging_set_page(0x00000UL, 0x00000UL, V86_PAGE_FLAGS);

    /* COMロード・スタック領域 */
    paging_set_page(0x8A000UL, 0x8A000UL, V86_PAGE_FLAGS);
    for (addr = 0x8B000UL; addr <= 0x8E000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }

    /* TVRAM */
    for (addr = 0xA0000UL; addr < 0xA4000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }

    /* BIOS ROM (0xF0000-0xFFFFF): R/O + PTE_USER
     * タイマーIRQ0がIVT[0x08]経由でBIOS ROMハンドラにジャンプした場合の
     * #PFを防ぐ安全帯。ダミーIVT設定があれば通常は到達しないが念のため。 */
    for (addr = 0xF0000UL; addr <= 0xFF000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_USER);
    }

    paging_pde_set_flags(0x00000UL, PTE_USER);

    v86_pic_init();
    v86_pit_init();

    /* ================================================================== */
    /*  IVT[0x08] ダミーベクタ設定                                        */
    /*                                                                      */
    /*  低クロック環境ではCOM実行中にIRQ0(タイマー)が発火する。             */
    /*  v86_inject_timer_irq() は IVT[0x08] を読んでゲストCSEIPを          */
    /*  そのハンドラに書き換える。IVTが未初期化だとBIOS ROM内の            */
    /*  アドレスに飛び、PTE_USER不足で#PFになる。                          */
    /*                                                                      */
    /*  v86_mem.c と同じ方式: IVT領域内(0x3F0)にIRET(0xCF)を配置し、      */
    /*  IVT[0x08] をそこに向ける。                                        */
    /* ================================================================== */
    {
        u32 *ivt = (u32 *)0x00000UL;
        u8  *iret_ptr = (u8 *)0x3F0UL;
        u16 dummy_seg = 0x003F;
        u16 dummy_off = 0x0000;
        u32 dummy_vec = ((u32)dummy_seg << 16) | dummy_off;

        /* ダミーIRETハンドラ配置 */
        *iret_ptr = 0xCF;  /* IRET */

        /* IRQ0 タイマー (INT 08h) */
        ivt[0x08] = dummy_vec;
        /* IRQ1 キーボード (INT 09h) — 念のため */
        ivt[0x09] = dummy_vec;
    }

    /* PSP構築 */
    psp = (u8 *)COM_LOAD_BASE;
    kmemset(psp, 0, 256);
    psp[0] = 0xCD;
    psp[1] = 0x20;

    /* COMバイナリをロード */
    code = (u8 *)(COM_LOAD_BASE + COM_ENTRY);
    kmemcpy(code, data, size);

    /* V86コンテキスト設定 */
    ctx.eip    = COM_ENTRY;
    ctx.cs     = COM_SEG;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;
    ctx.esp    = COM_STACK_OFF;
    ctx.ss     = COM_STACK_SEG;
    ctx.es     = COM_SEG;
    ctx.ds     = COM_SEG;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    saved_esp0 = tss_get_esp0();
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    v86_active = 1;
    v86_exit_request = 0;

    {
        extern volatile u32 tick_count;
        v86_start_tick = tick_count;
    }

    v86_current_jmpbuf = v86_test_jmpbuf;
    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        v86_enter(&ctx);
    }
    v86_current_jmpbuf = 0;

    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;
    tss_set_esp0(saved_esp0);

    v86_restore_screen();

    /* ================================================================== */
    /*  ページ属性復元                                                    */
    /* ================================================================== */

    /* IVTページ復元 (NULLポインタガードは paging_init で設定済み) */
    paging_set_page(0x00000UL, 0x00000UL, PAGE_RW);

    paging_set_page(0x8A000UL, 0x8A000UL, PAGE_RW);
    for (addr = 0x8B000UL; addr <= 0x8E000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PAGE_RW);
    }
    for (addr = 0xA0000UL; addr < 0xA4000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PAGE_RW);
    }

    /* BIOS ROM復元 (PTE_USER除去) */
    for (addr = 0xF0000UL; addr <= 0xFF000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PTE_PRESENT);
    }

    paging_pde_clear_flags(0x00000UL, PTE_USER);

    return 0;
}

/* ====================================================================== */
/*  v86_test - V86モード動作検証                                           */
/* ====================================================================== */
int v86_test(void)
{
    int rc;

    rc = v86_run_com(hello_com, HELLO_COM_SIZE);
    if (rc != 0) return -1;

    rc = v86_run_com(hello_bios_com, HELLO_BIOS_COM_SIZE);
    if (rc != 0) return -2;

    return 0;
}

/* ====================================================================== */
/*  v86_test_exit - V86モードからの脱出 (ISRスタブから呼ばれる共通関数)     */
/*                                                                          */
/*  isr_stub.asm .v86_exit から呼ばれる。                                  */
/*  現在アクティブなジャンプバッファ (COMテスト or FreeDOSセッション) に    */
/*  longjmpで復帰する。                                                    */
/* ====================================================================== */

/* 現在のアクティブなジャンプバッファ */
u32 *v86_current_jmpbuf = 0;

void v86_test_exit(void)
{
    __asm__ volatile("sti");
    if (v86_current_jmpbuf) {
        exec_longjmp(v86_current_jmpbuf);
    }
    /* フォールバック: jmpbufが未設定の場合はハング (安全のため) */
    for (;;) { __asm__ volatile("hlt"); }
}
