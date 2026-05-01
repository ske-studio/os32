/* ======================================================================== */
/*  V86_TEST.C - V86モード動作検証テスト                                    */
/*                                                                          */
/*  Phase 0/1 検証用。コンベンショナルメモリの空き領域に小さなテスト         */
/*  プログラムを配置し、V86モードで実行する。                               */
/*                                                                          */
/*  テストプログラムはテキストVRAMに文字列を書き込んでからHLTする。          */
/*  #GPハンドラがHLTを検知するとV86モードを終了する。                        */
/* ======================================================================== */

#include "v86.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "io.h"

/* exec_setjmp / exec_longjmp (setjmp.asm) */
extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* V86テスト用setjmpバッファ */
static u32 v86_test_jmpbuf[6];

/* V86終了要求フラグ (v86.cから参照) */
volatile int v86_exit_request = 0;

/* V86テスト用カーネルスタック (4KB、上位メモリに配置) */
/* カーネルヒープから確保するのではなく、静的確保して安全性を保証 */
static u8 v86_kstack[4096] __attribute__((aligned(16)));

/* ====================================================================== */
/*  V86テストプログラム (16bit リアルモード機械語)                          */
/*                                                                          */
/*  このバイト列は V86モードで実行される。                                  */
/*  テキストVRAM (0xA0000) に "V86 OK!" を書き込み、HLTする。              */
/*                                                                          */
/*  セグメント:オフセット形式:                                             */
/*    CS = 0x8A00, IP = 0x0000 → リニア 0x8A000                            */
/*    (コンベンショナルメモリの空き領域 0x8A000-0x8EFFF を使用)             */
/*                                                                          */
/*  等価アセンブリ:                                                        */
/*    mov  ax, 0xA000                                                      */
/*    mov  es, ax           ; ES = テキストVRAMセグメント                  */
/*    mov  word [es:0x0000], 0x56  ; 'V'                                   */
/*    mov  word [es:0x0002], 0x38  ; '8'                                   */
/*    mov  word [es:0x0004], 0x36  ; '6'                                   */
/*    mov  word [es:0x0006], 0x20  ; ' '                                   */
/*    mov  word [es:0x0008], 0x4F  ; 'O'                                   */
/*    mov  word [es:0x000A], 0x4B  ; 'K'                                   */
/*    mov  word [es:0x000C], 0x21  ; '!'                                   */
/*    ; テキスト属性 (0xA2000) に白色を設定                                 */
/*    mov  ax, 0xA200                                                      */
/*    mov  es, ax                                                          */
/*    mov  word [es:0x0000], 0xE1  ; (7回)                                 */
/*    hlt                                                                  */
/* ====================================================================== */
static const u8 v86_test_program[] = {
    /* mov ax, 0xA000 */
    0xB8, 0x00, 0xA0,
    /* mov es, ax */
    0x8E, 0xC0,
    /* mov word [es:0x0000], 'V' (0x56) */
    0x26, 0xC7, 0x06, 0x00, 0x00, 0x56, 0x00,
    /* mov word [es:0x0002], '8' (0x38) */
    0x26, 0xC7, 0x06, 0x02, 0x00, 0x38, 0x00,
    /* mov word [es:0x0004], '6' (0x36) */
    0x26, 0xC7, 0x06, 0x04, 0x00, 0x36, 0x00,
    /* mov word [es:0x0006], ' ' (0x20) */
    0x26, 0xC7, 0x06, 0x06, 0x00, 0x20, 0x00,
    /* mov word [es:0x0008], 'O' (0x4F) */
    0x26, 0xC7, 0x06, 0x08, 0x00, 0x4F, 0x00,
    /* mov word [es:0x000A], 'K' (0x4B) */
    0x26, 0xC7, 0x06, 0x0A, 0x00, 0x4B, 0x00,
    /* mov word [es:0x000C], '!' (0x21) */
    0x26, 0xC7, 0x06, 0x0C, 0x00, 0x21, 0x00,

    /* mov ax, 0xA200 (テキスト属性VRAM) */
    0xB8, 0x00, 0xA2,
    /* mov es, ax */
    0x8E, 0xC0,
    /* mov word [es:0x0000], 0xE1 (白/青) ×7 */
    0x26, 0xC7, 0x06, 0x00, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x02, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x04, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x06, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x08, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x0A, 0x00, 0xE1, 0x00,
    0x26, 0xC7, 0x06, 0x0C, 0x00, 0xE1, 0x00,

    /* HLT — #GPがトラップしてV86終了 */
    0xF4
};

/* ====================================================================== */
/*  v86_test — V86モード動作検証                                           */
/*                                                                          */
/*  テキストVRAMの左上に "V86 OK!" と表示されれば成功。                      */
/*  戻り値: 0=成功, -1=失敗                                                */
/* ====================================================================== */
int v86_test(void)
{
    struct v86_context ctx;
    u32 saved_esp0;
    u8 *test_code_addr;
    u32 addr;

    /* V86モードはCPL=3で動作するため、ユーザアクセス可能フラグが必須 */
    #define V86_PAGE_FLAGS (PAGE_RW | PTE_USER)

    /* テストプログラムをコンベンショナルメモリの空き領域にコピー */
    /* 0x8A000-0x8EFFF は memmap.h で「空き (20KB, 将来用)」とされている */
    test_code_addr = (u8 *)0x8A000UL;

    /* テストコード領域 (0x8A000) を V86アクセス可能に設定 */
    paging_set_page(0x8A000UL, 0x8A000UL, V86_PAGE_FLAGS);

    /* V86スタック領域 (0x8B000-0x8EFFF) を V86アクセス可能に設定 */
    for (addr = 0x8B000UL; addr <= 0x8E000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }

    /* テキストVRAM (0xA0000-0xA3FFF) を V86アクセス可能に設定 */
    for (addr = 0xA0000UL; addr < 0xA4000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }

    /* ★ 重要: PDEにもPTE_USERが必要 (x86はPDEとPTE両方でU/Sチェック) */
    /* テストコード・スタック・VRAMは全てPDE[0] (0x00000-0x3FFFFF) 内 */
    paging_pde_set_flags(0x00000UL, PTE_USER);

    /* テストプログラムをコピー */
    kmemcpy(test_code_addr, v86_test_program, sizeof(v86_test_program));

    /* V86コンテキスト設定 */
    /* CS:IP = 0x8A00:0x0000 → リニア 0x8A000 */
    ctx.eip    = 0x0000;
    ctx.cs     = 0x8A00;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;  /* VM=1, IF=1, IOPL=0 */
    /* スタック: SS:SP = 0x8B00:0xFFFE → リニア 0x8BFFE + 0xFFFE = 0x9AFFE */
    /* 注意: 0x8F000はカーネルスタックガードなので避ける */
    /* SS=0x8B00, SP=0x3FFE → リニア 0x8B000 + 0x3FFE = 0x8EFFE (安全) */
    ctx.esp    = 0x3FFE;
    ctx.ss     = 0x8B00;
    ctx.es     = 0x0000;
    ctx.ds     = 0x0000;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    /* TSS ESP0 を V86用カーネルスタックに切り替え */
    /* (V86で#GPが発生するとCPUがこのスタックを使う) */
    saved_esp0 = 0x9FFF0UL;  /* 元のカーネルスタックトップ */
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* V86終了用のsetjmpポイントを設定 */
    v86_active = 1;
    v86_exit_request = 0;

    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        /* 最初の呼び出し: V86モードに遷移 */
        v86_enter(&ctx);
        /* ここには戻らない (V86モードに遷移する) */
    }
    /* longjmpで戻ってきた場合: V86テスト終了 */

    /* V86モード終了 */
    v86_active = 0;
    v86_exit_request = 0;

    /* TSS ESP0 を元に戻す */
    tss_set_esp0(saved_esp0);

    /* ページ属性を元に戻す (PTE_USERを除去) */
    paging_set_page(0x8A000UL, 0x8A000UL, PAGE_RW);
    for (addr = 0x8B000UL; addr <= 0x8E000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PAGE_RW);
    }
    for (addr = 0xA0000UL; addr < 0xA4000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, PAGE_RW);
    }
    paging_pde_clear_flags(0x00000UL, PTE_USER);

    return 0;
}

/* ====================================================================== */
/*  v86_test_exit — V86モードからの脱出                                    */
/*                                                                          */
/*  isr_stub.asm の .v86_exit パスから呼ばれる。                            */
/*  セグメントレジスタは呼び出し元で復元済み。                              */
/* ====================================================================== */
void v86_test_exit(void)
{
    /* デバッグ: この関数が呼ばれたことをVRAMに表示 */
    volatile u16 *tvram = (volatile u16 *)0xA0000UL;
    volatile u8  *tattr = (volatile u8  *)0xA2000UL;
    tvram[160] = 'E';  /* 行2の位置に 'E' */
    tattr[320] = 0xE1;
    tvram[161] = 'X';
    tattr[322] = 0xE1;
    tvram[162] = 'I';
    tattr[324] = 0xE1;
    tvram[163] = 'T';
    tattr[326] = 0xE1;

    __asm__ volatile("sti");  /* ISRスタブのCLIを解除 */
    exec_longjmp(v86_test_jmpbuf);
}
