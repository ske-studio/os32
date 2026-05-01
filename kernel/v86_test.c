/* ======================================================================== */
/*  V86_TEST.C - V86モード動作検証テスト (Phase 1 完成版)                   */
/*                                                                          */
/*  テスト内容:                                                            */
/*    Test 1: HELLO.COM (TVRAM直接書き込み + INT 20h 終了)                  */
/*    Test 2: HELLO_BIOS.COM (INT 18h BIOS呼び出し + INT 29h 文字出力)     */
/*                                                                          */
/*  COMローダーの仕様:                                                     */
/*    - COMファイルはセグメント先頭+0x100にロード (DOSと同じ)              */
/*    - 0x000-0x0FF は PSP (Program Segment Prefix) 領域                   */
/*    - PSP先頭に INT 20h (CD 20) を配置 (DOS互換)                         */
/*    - CS:IP = seg:0x0100 でエントリ                                      */
/* ======================================================================== */

#include "v86.h"
#include "v86_mem.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_disk.h"
#include "io.h"
#include "vfs.h"
#include "kmalloc.h"
#include "kprintf.h"

/* exec_setjmp / exec_longjmp (setjmp.asm) */
extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* V86テスト用setjmpバッファ */
static u32 v86_test_jmpbuf[6];

/* V86終了要求フラグ (v86.cから参照) */
volatile int v86_exit_request = 0;

/* V86テスト用カーネルスタック (16KB)
 * V86 #GPハンドラ内でタイマ割り込みがネストするため、
 * 十分なサイズが必要 */
static u8 v86_kstack[16384] __attribute__((aligned(16)));

/* ====================================================================== */
/*  HELLO.COM 埋め込みバイナリ (tests/hello_v86.asm から生成)              */
/*  TVRAM直接書き込み + INT 20h 終了                                       */
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
/*  INT 18h AH=16h (VRAMクリア) + AH=13h (カーソル設定)                   */
/*  + INT 29h (1文字出力) + INT 1Ch (カレンダ) + PIC I/O + INT 20h        */
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
#define COM_LOAD_BASE   0x8A000UL   /* PSP + COMのベースアドレス */
#define COM_SEG         0x8A00      /* COMセグメント */
#define COM_ENTRY       0x0100      /* COMエントリオフセット (PSP直後) */
#define COM_STACK_SEG   0x8D00      /* スタックセグメント */
#define COM_STACK_OFF   0x1FFE      /* スタックオフセット */

/* V86アクセスに必要なページフラグ */
#define V86_PAGE_FLAGS (PAGE_RW | PTE_USER)

/* ====================================================================== */
/*  v86_run_com — COMバイナリをV86モードで実行                             */
/*                                                                          */
/*  data: COMバイナリデータ                                                */
/*  size: バイト数                                                          */
/*  戻り値: 0=成功                                                         */
/* ====================================================================== */
static int v86_run_com(const u8 *data, u32 size)
{
    struct v86_context ctx;
    u32 saved_esp0;
    u32 addr;
    u8 *psp;
    u8 *code;

    /* ページテーブル設定 (必要な領域のみ PTE_USER 追加) */
    paging_set_page(0x8A000UL, 0x8A000UL, V86_PAGE_FLAGS);
    for (addr = 0x8B000UL; addr <= 0x8E000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }
    for (addr = 0xA0000UL; addr < 0xA4000UL; addr += 0x1000UL) {
        paging_set_page(addr, addr, V86_PAGE_FLAGS);
    }
    paging_pde_set_flags(0x00000UL, PTE_USER);

    /* PIC仮想化初期化 */
    v86_pic_init();

    /* PIT仮想化初期化 */
    v86_pit_init();

    /* PSP構築 */
    psp = (u8 *)COM_LOAD_BASE;
    kmemset(psp, 0, 256);
    psp[0] = 0xCD;  /* INT */
    psp[1] = 0x20;  /* 20h */

    /* COMバイナリをPSP直後 (offset 0x100) にコピー */
    code = (u8 *)(COM_LOAD_BASE + COM_ENTRY);
    kmemcpy(code, data, size);

    /* V86コンテキスト設定 (COM形式) */
    ctx.eip    = COM_ENTRY;
    ctx.cs     = COM_SEG;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;
    ctx.esp    = COM_STACK_OFF;
    ctx.ss     = COM_STACK_SEG;
    ctx.es     = COM_SEG;
    ctx.ds     = COM_SEG;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    /* TSS ESP0 切り替え */
    saved_esp0 = 0x9FFF0UL;
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* V86モード遷移 */
    v86_active = 1;
    v86_exit_request = 0;

    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        v86_enter(&ctx);
    }
    /* longjmpで復帰 */

    /* 後始末 */
    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;
    tss_set_esp0(saved_esp0);

    /* 画面リストア (DOSが変更した可能性のあるハードウェア状態を復帰) */
    v86_restore_screen();

    /* ページ属性復元 */
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
/*  v86_test — V86モード動作検証                                           */
/*                                                                          */
/*  Test 1: HELLO.COM (TVRAM直接書き込み + INT 20h)                        */
/*  Test 2: HELLO_BIOS.COM (INT 18h + INT 29h)                            */
/*                                                                          */
/*  戻り値: 0=全テスト成功                                                 */
/* ====================================================================== */
int v86_test(void)
{
    int rc;

    /* Test 1: TVRAM直接書き込み + INT 20h */
    rc = v86_run_com(hello_com, HELLO_COM_SIZE);
    if (rc != 0) return -1;

    /* Test 2: BIOS INT 18h + INT 29h */
    rc = v86_run_com(hello_bios_com, HELLO_BIOS_COM_SIZE);
    if (rc != 0) return -2;

    return 0;
}

/* ====================================================================== */
/*  v86_test_exit — V86モードからの脱出                                    */
/* ====================================================================== */
void v86_test_exit(void)
{
    __asm__ volatile("sti");
    exec_longjmp(v86_test_jmpbuf);
}

/* ====================================================================== */
/*  v86_boot_freedos — FreeDOS(98) FDDイメージからブート                   */
/*                                                                          */
/*  path: FDDイメージファイルパス (例: "/host/fd98_2hd.img")              */
/*                                                                          */
/*  処理:                                                                  */
/*    1. VFS経由でFDDイメージをメモリにロード                              */
/*    2. IPL (先頭1024バイト) をV86メモリ 0x1FC0:0000 にコピー             */
/*    3. INT 1Bh をFDDイメージからサーブするよう設定                       */
/*    4. V86モードでIPLを実行                                              */
/*                                                                          */
/*  PC-98 FDD IPL ロードアドレス: 0x1FC00 (seg 0x1FC0, off 0x0000)        */
/* ====================================================================== */
#define IPL_SEG      0x1FC0
#define IPL_LINEAR   0x1FC00UL
#define IPL_SIZE     1024

int v86_boot_freedos(const char *path)
{
    struct v86_context ctx;
    u32 saved_esp0;
    int fd;
    u32 fsize;
    u8 *img_buf;
    u8 *ipl_dst;

    /* 1. FDDイメージをVFS経由でロード */
    fd = vfs_open(path, 0);
    if (fd < 0) {
        kprintf(0xE1, "[V86] FDD image not found: %s\n", path);
        return -1;
    }

    fsize = vfs_get_size(fd);
    if (fsize == 0 || fsize > V86_FDD_IMAGE_SIZE) {
        kprintf(0xE1, "[V86] Invalid image size: %u\n", fsize);
        vfs_close(fd);
        return -2;
    }

    img_buf = (u8 *)kmalloc(fsize);
    if (!img_buf) {
        kprintf(0xE1, "[V86] kmalloc failed for FDD image\n");
        vfs_close(fd);
        return -3;
    }

    {
        int rd = vfs_read_fd(fd, img_buf, fsize);
        if (rd < 0 || (u32)rd != fsize) {
            kprintf(0xE1, "[V86] Image read error: %d\n", rd);
            kfree(img_buf);
            vfs_close(fd);
            return -4;
        }
    }
    vfs_close(fd);

    kprintf(0xA1, "[V86] FDD image loaded: %u bytes\n", fsize);

    /* 2. V86メモリ空間を構築 */
    v86_mem_setup();

    /* 3. PIC/PIT/ディスク仮想化初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_disk_set_image(img_buf, fsize);

    /* 4. IPLをV86メモリにコピー (0x1FC00) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    kmemcpy(ipl_dst, img_buf, IPL_SIZE);

    /* 5. V86コンテキスト: IPLエントリ */
    ctx.eip    = 0x0000;
    ctx.cs     = IPL_SEG;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;
    ctx.esp    = 0xFFFE;    /* スタック: 0x0000:FFFE */
    ctx.ss     = 0x0000;
    ctx.es     = IPL_SEG;
    ctx.ds     = IPL_SEG;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    /* TSS ESP0 切り替え */
    saved_esp0 = 0x9FFF0UL;
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* V86モード遷移 */
    v86_active = 1;
    v86_exit_request = 0;

    kprintf(0xA1, "[V86] Booting FreeDOS(98) IPL...\n");

    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        v86_enter(&ctx);
    }

    /* V86終了後の後始末 */
    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;
    tss_set_esp0(saved_esp0);

    /* ディスクイメージ解放 */
    v86_disk_clear();
    kfree(img_buf);

    /* メモリ空間復元 */
    v86_mem_teardown();

    /* 画面リストア */
    v86_restore_screen();

    kprintf(0xA1, "[V86] FreeDOS(98) session ended.\n");
    return 0;
}

