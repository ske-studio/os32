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
#include "paging.h"
#include "kprintf.h"

extern void serial_puts(const char *s);
extern void serial_putchar(char c);
u32 v86_irq0_inject_count = 0;

static void serial_hex8(u8 val) {
    const char *hex = "0123456789ABCDEF";
    serial_putchar(hex[val >> 4]);
    serial_putchar(hex[val & 0xF]);
}
static void serial_hex16(u16 val) {
    serial_hex8((u8)(val >> 8));
    serial_hex8((u8)(val & 0xFF));
}
static void serial_hex32(u32 val) {
    serial_hex16((u16)(val >> 16));
    serial_hex16((u16)(val & 0xFFFF));
}

/* V86ディスクログエントリ (v86_disk.cと同一レイアウト) */
struct v86_disk_log_entry {
    u8  func;
    u8  daua;
    u8  cylinder;
    u8  sector_len;
    u8  head;
    u8  sector;
    u16 xfer_bytes;
    u16 es;
    u16 bp;
    i32 result_offset;
    u8  status;
    u8  pad;
};
extern struct v86_disk_log_entry *v86_disk_get_log(u32 *count, u32 *idx);

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

    /* タイムアウト開始tick設定 */
    {
        extern u32 v86_start_tick;
        extern volatile u32 tick_count;
        v86_start_tick = tick_count;
    }

    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        kprintf(0x07, "ESP0 is %x\n", kernel_tss.esp0); io_wait(); v86_enter(&ctx);
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
    u8 *img_buf;
    u8 *ipl_dst;
    int rd;

    /* FDDイメージ配置先: 0x500000 (プログラム空間上位)
     * カーネルブートシーケンス中 (シェル・外部プログラム起動前) のみ安全。
     * 0x500000 + 0x134000(1.2MB) = 0x634000, 16MBメモリでは安全な範囲。 */
#define V86_FDD_IMG_ADDR  0x500000UL
    img_buf = (u8 *)V86_FDD_IMG_ADDR;

    /* 0x500000〜0x634000 を確実に PRESENT+RW にマッピング */
    {
        u32 pa;
        for (pa = V86_FDD_IMG_ADDR; pa < V86_FDD_IMG_ADDR + V86_FDD_IMAGE_SIZE; pa += 0x1000) {
            paging_set_page(pa, pa, PAGE_RW);
        }
    }

    /* VFS パスベースreadでFDDイメージを一括読み込み */
    rd = vfs_read(path, img_buf, V86_FDD_IMAGE_SIZE);
    if (rd <= 0) {
        kprintf(0xE1, "[V86] FDD image read failed: %s (rc=%d)\n", path, rd);
        return -1;
    }

    kprintf(0xA1, "[V86] FDD image loaded: %d bytes at 0x%x\n", rd, (unsigned)V86_FDD_IMG_ADDR);

    /* 2. V86メモリ空間を構築 */
    v86_mem_setup();

    /* FDIヘッダ判定 */
    {
        u32 img_offset = 0;
        u32 img_data_size = rd;

        /* FDIヘッダサイズを0x08位置から取得してチェック */
        if (rd > 0x1000) {
            u32 hdr_size = *(u32 *)(img_buf + 8);
            if (hdr_size == 0x1000 || hdr_size == 0x2000) {
                img_offset = hdr_size;
                img_data_size = rd - hdr_size;
                kprintf(0xA1, "[V86] FDI format detected. Header size: 0x%x\n", img_offset);
            }
        }

        /* 3. PIC/PIT/ディスク仮想化初期化 */
        v86_pic_init();
        v86_pit_init();
        v86_disk_set_image(img_buf + img_offset, img_data_size);

        /* 4. IPLをV86メモリにコピー (0x1FC00) */
        ipl_dst = v86_phys_addr(IPL_SEG, 0);
        kmemcpy(ipl_dst, img_buf + img_offset, IPL_SIZE);
    }


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

    /* デバッグカウンタリセット */
    {
        extern u32 v86_int_count, v86_gp_count;
        extern u32 v86_start_tick;
        extern u32 v86_timeout_cs, v86_timeout_ip;
        extern volatile u32 tick_count;
        extern void v86_trace_reset(void);
        extern u32 v86_irq0_call_count, v86_irq0_nonvm_count;
        extern u32 v86_irq0_noif_count, v86_irq0_isr_count;
        extern u32 v86_irq0_ivt_count;
        extern u32 v86_irq0_gp_inject_count;
        extern u32 v86_irq0_gp_skip_if, v86_irq0_gp_skip_isr;
        extern u32 v86_irq0_gp_skip_ivt;
        v86_int_count = 0;
        v86_gp_count = 0;
        v86_irq0_inject_count = 0;
        v86_irq0_call_count = 0;
        v86_irq0_nonvm_count = 0;
        v86_irq0_noif_count = 0;
        v86_irq0_isr_count = 0;
        v86_irq0_ivt_count = 0;
        v86_irq0_gp_inject_count = 0;
        v86_irq0_gp_skip_if = 0;
        v86_irq0_gp_skip_isr = 0;
        v86_irq0_gp_skip_ivt = 0;
        v86_start_tick = tick_count;
        v86_timeout_cs = 0;
        v86_timeout_ip = 0;
        v86_trace_reset();
    }

    kprintf(0xA1, "[V86] Booting FreeDOS(98) IPL...\n");

    /* INT 1Bh デバッグログをリセット */
    v86_disk_reset_log();

    /* I/O統計リセット */
    {
        extern void v86_reset_io_stats(void);
        v86_reset_io_stats();
    }

    if (exec_setjmp(v86_test_jmpbuf) == 0) {
        kprintf(0x07, "ESP0 is %x\n", kernel_tss.esp0); io_wait(); v86_enter(&ctx);
    }

    /* V86終了後の後始末 */
    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;
    tss_set_esp0(saved_esp0);

    /* V86統計情報をシリアル経由で出力 */
    {
        extern u32 v86_int_count, v86_gp_count;
        extern u32 v86_last_int, v86_last_cs, v86_last_ip;
        extern u32 v86_timeout_cs, v86_timeout_ip;
        serial_puts("\r\n[V86 END] ints=");
        serial_hex32(v86_int_count);
        serial_puts(" gp=");
        serial_hex32(v86_gp_count);
        serial_puts(" last=0x");
        serial_hex8((u8)v86_last_int);
        serial_puts(" at ");
        serial_hex16((u16)v86_last_cs);
        serial_puts(":");
        serial_hex16((u16)v86_last_ip);
        serial_puts("\r\n");

        /* タイムアウト位置の実CS:EIPとオペコードダンプ (常に出力) */
        {
            u8 *timeout_addr;
            int di;
            serial_puts("[V86 TIMEOUT] real CS:IP=");
            serial_hex16((u16)v86_timeout_cs);
            serial_puts(":");
            serial_hex16((u16)v86_timeout_ip);
            if (v86_timeout_cs || v86_timeout_ip) {
                serial_puts(" opcodes=");
                timeout_addr = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
                for (di = 0; di < 16; di++) {
                    serial_hex8(timeout_addr[di]);
                    serial_puts(" ");
                }
            }
            serial_puts("\r\n");
        }
    }

    /* メモリ領域ダンプ: kernel entry / temp buffers */
    {
        u8 *p;
        int di;
        /* 0060:0000 = カーネルエントリポイント (物理 0x600) */
        serial_puts("[MEM] 0060:0000=");
        p = v86_phys_addr(0x0060, 0x0000);
        for (di = 0; di < 32; di++) { serial_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
        /* 1E00:0000 = 初期一時バッファ (物理 0x1E000) */
        serial_puts("[MEM] 1E00:0000=");
        p = v86_phys_addr(0x1E00, 0x0000);
        for (di = 0; di < 32; di++) { serial_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
        /* 1994:0000 = 実際の一時バッファ (物理 0x19940) */
        serial_puts("[MEM] 1994:0000=");
        p = v86_phys_addr(0x1994, 0x0000);
        for (di = 0; di < 32; di++) { serial_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
    }

    /* INT 1Bh デバッグログをシリアル出力 */
    {
        u32 n, i, start;
        u32 total_count, log_idx;
        struct v86_disk_log_entry *log = v86_disk_get_log(&total_count, &log_idx);

        n = (total_count < 64) ? total_count : 64;
        serial_puts("[V86 DISK] total calls=");
        serial_hex32(total_count);
        serial_puts(" showing last ");
        serial_hex8((u8)n);
        serial_puts("\r\n");

        if (n > 0) {
            start = (total_count <= 64) ? 0 : log_idx;
            for (i = 0; i < n; i++) {
                u32 idx = (start + i) % 64;
                struct v86_disk_log_entry *e = &log[idx];
                serial_puts("  AH=");
                serial_hex8(e->func);
                serial_puts(" C=");
                serial_hex8(e->cylinder);
                serial_puts(" H=");
                serial_hex8(e->head);
                serial_puts(" S=");
                serial_hex8(e->sector);
                serial_puts(" BX=");
                serial_hex16(e->xfer_bytes);
                serial_puts(" ES:BP=");
                serial_hex16(e->es);
                serial_puts(":");
                serial_hex16(e->bp);
                serial_puts(" st=");
                serial_hex8(e->status);
                serial_puts("\r\n");
            }
        }
    }

    /* I/O統計ダンプ */
    {
        extern void v86_dump_io_stats(void);
        v86_dump_io_stats();
    }

    /* 仮想PIC最終状態をシリアルに出力 */
    {
        extern u8 v86_pic_get_imr(int idx);
        extern u8 v86_pic_get_isr(int idx);
        extern u32 v86_pic_get_eoi_count(int idx);
        serial_puts("[V86 PIC] M.IMR=");
        serial_hex8(v86_pic_get_imr(0));
        serial_puts(" M.ISR=");
        serial_hex8(v86_pic_get_isr(0));
        serial_puts(" S.IMR=");
        serial_hex8(v86_pic_get_imr(1));
        serial_puts(" S.ISR=");
        serial_hex8(v86_pic_get_isr(1));
        serial_puts(" irq0=");
        serial_hex32(v86_irq0_inject_count);
        serial_puts(" eoi0=");
        serial_hex32(v86_pic_get_eoi_count(0));
        serial_puts(" eoi1=");
        serial_hex32(v86_pic_get_eoi_count(1));
        serial_puts("\r\n");
    }

    /* IRQ0注入デバッグカウンタ出力 */
    {
        extern u32 v86_irq0_call_count, v86_irq0_nonvm_count;
        extern u32 v86_irq0_noif_count, v86_irq0_isr_count;
        extern u32 v86_irq0_ivt_count;
        extern u32 v86_irq0_gp_inject_count;
        extern u32 v86_irq0_gp_skip_if, v86_irq0_gp_skip_isr;
        extern u32 v86_irq0_gp_skip_ivt;
        serial_puts("[V86 IRQ0] call=");
        serial_hex32(v86_irq0_call_count);
        serial_puts(" nonvm=");
        serial_hex32(v86_irq0_nonvm_count);
        serial_puts(" noif=");
        serial_hex32(v86_irq0_noif_count);
        serial_puts(" isr=");
        serial_hex32(v86_irq0_isr_count);
        serial_puts(" ivt=");
        serial_hex32(v86_irq0_ivt_count);
        serial_puts("\r\n");
        serial_puts("[V86 IRQ0 GP] inject=");
        serial_hex32(v86_irq0_gp_inject_count);
        serial_puts(" skip_isr=");
        serial_hex32(v86_irq0_gp_skip_isr);
        serial_puts(" skip_ivt=");
        serial_hex32(v86_irq0_gp_skip_ivt);
        serial_puts("\r\n");
    }

    /* ディスクイメージ参照解除 */
    v86_disk_clear();

    /* メモリ空間復元 */
    v86_mem_teardown();

    /* 画面リストア */
    v86_restore_screen();

    /* V86統計+ディスクログを /host/v86_disklog.txt に書き出し */
    {
        extern u32 v86_int_count, v86_gp_count;
        extern u32 v86_last_int, v86_last_cs, v86_last_ip;
        /* 大きめバッファを使って一括書き出し */
        static char fbuf[4096];
        int fp = 0;
        u32 total_count, log_idx_v;
        struct v86_disk_log_entry *dlog;
        u32 n, i, start;
        const char *hx = "0123456789ABCDEF";

        /* ヘッダ行 */
        {
            const char *h = "[V86] ints=";
            int hi;
            u32 v;
            char tb[12];
            int tp;
            for (hi = 0; h[hi]; hi++) fbuf[fp++] = h[hi];
            v = v86_int_count; tp = 0;
            if (v == 0) fbuf[fp++] = '0';
            else { while(v>0){tb[tp++]='0'+(v%10);v/=10;} while(tp>0)fbuf[fp++]=tb[--tp]; }
            fbuf[fp++] = ' ';
            h = "gp=";
            for (hi = 0; h[hi]; hi++) fbuf[fp++] = h[hi];
            v = v86_gp_count; tp = 0;
            if (v == 0) fbuf[fp++] = '0';
            else { while(v>0){tb[tp++]='0'+(v%10);v/=10;} while(tp>0)fbuf[fp++]=tb[--tp]; }
            fbuf[fp++] = ' ';
            h = "last=";
            for (hi = 0; h[hi]; hi++) fbuf[fp++] = h[hi];
            fbuf[fp++] = hx[(v86_last_int>>4)&0xF];
            fbuf[fp++] = hx[v86_last_int&0xF];
            fbuf[fp++] = ' ';
            fbuf[fp++] = hx[(v86_last_cs>>12)&0xF];
            fbuf[fp++] = hx[(v86_last_cs>>8)&0xF];
            fbuf[fp++] = hx[(v86_last_cs>>4)&0xF];
            fbuf[fp++] = hx[v86_last_cs&0xF];
            fbuf[fp++] = ':';
            fbuf[fp++] = hx[(v86_last_ip>>12)&0xF];
            fbuf[fp++] = hx[(v86_last_ip>>8)&0xF];
            fbuf[fp++] = hx[(v86_last_ip>>4)&0xF];
            fbuf[fp++] = hx[v86_last_ip&0xF];
            fbuf[fp++] = '\n';
        }

        fbuf[fp++] = 'I'; fbuf[fp++] = 'n'; fbuf[fp++] = 'j'; fbuf[fp++] = ':';
        fbuf[fp++] = hx[(v86_irq0_inject_count>>12)&0xF]; fbuf[fp++] = hx[(v86_irq0_inject_count>>8)&0xF];
        fbuf[fp++] = hx[(v86_irq0_inject_count>>4)&0xF]; fbuf[fp++] = hx[v86_irq0_inject_count&0xF];
        fbuf[fp++] = '\n';

        /* IRQ0デバッグカウンタをファイルに書き出し */
        {
            extern u32 v86_irq0_call_count, v86_irq0_nonvm_count;
            extern u32 v86_irq0_noif_count, v86_irq0_isr_count;
            extern u32 v86_irq0_ivt_count;
            extern u32 v86_irq0_gp_inject_count;
            extern u32 v86_irq0_gp_skip_isr, v86_irq0_gp_skip_ivt;
            extern u8 v86_pic_get_imr(int idx);
            extern u8 v86_pic_get_isr(int idx);
            extern u32 v86_pic_get_eoi_count(int idx);
            /* IRQ0直接注入カウンタ */
            { const char *s = "IRQ0:call="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_call_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>24)&0xF];
            fbuf[fp++]=hx[(v86_irq0_call_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>16)&0xF];
            fbuf[fp++]=hx[(v86_irq0_call_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_call_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_call_count&0xF];
            { const char *s = " nonvm="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_nonvm_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>24)&0xF];
            fbuf[fp++]=hx[(v86_irq0_nonvm_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>16)&0xF];
            fbuf[fp++]=hx[(v86_irq0_nonvm_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_nonvm_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_nonvm_count&0xF];
            { const char *s = " noif="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_noif_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>24)&0xF];
            fbuf[fp++]=hx[(v86_irq0_noif_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>16)&0xF];
            fbuf[fp++]=hx[(v86_irq0_noif_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_noif_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_noif_count&0xF];
            { const char *s = " isr="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_isr_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>24)&0xF];
            fbuf[fp++]=hx[(v86_irq0_isr_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>16)&0xF];
            fbuf[fp++]=hx[(v86_irq0_isr_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_isr_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_isr_count&0xF];
            { const char *s = " ivt="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_ivt_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>24)&0xF];
            fbuf[fp++]=hx[(v86_irq0_ivt_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>16)&0xF];
            fbuf[fp++]=hx[(v86_irq0_ivt_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_ivt_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_ivt_count&0xF];
            fbuf[fp++] = '\n';
            /* GP保留注入カウンタ */
            { const char *s = "GP:skip_isr="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_gp_skip_isr&0xF];
            { const char *s = " skip_ivt="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>8)&0xF];
            fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_gp_skip_ivt&0xF];
            fbuf[fp++] = '\n';
            /* PIC状態 */
            { const char *s = "PIC:M.IMR="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[v86_pic_get_imr(0)>>4]; fbuf[fp++]=hx[v86_pic_get_imr(0)&0xF];
            { const char *s = " M.ISR="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[v86_pic_get_isr(0)>>4]; fbuf[fp++]=hx[v86_pic_get_isr(0)&0xF];
            { const char *s = " eoi0="; int si; for(si=0;s[si];si++)fbuf[fp++]=s[si]; }
            fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>12)&0xF]; fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>8)&0xF];
            fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>4)&0xF]; fbuf[fp++]=hx[v86_pic_get_eoi_count(0)&0xF];
            fbuf[fp++] = '\n';
        }

        /* タイムアウト位置の実CS:EIPとオペコードダンプ */
        {
            extern u32 v86_timeout_cs, v86_timeout_ip;
            if (v86_timeout_cs || v86_timeout_ip) {
                u8 *taddr;
                int di;
                const char *tp2 = "TOUT:";
                for (di = 0; tp2[di]; di++) fbuf[fp++] = tp2[di];
                fbuf[fp++] = hx[(v86_timeout_cs>>12)&0xF]; fbuf[fp++] = hx[(v86_timeout_cs>>8)&0xF];
                fbuf[fp++] = hx[(v86_timeout_cs>>4)&0xF]; fbuf[fp++] = hx[v86_timeout_cs&0xF];
                fbuf[fp++] = ':';
                fbuf[fp++] = hx[(v86_timeout_ip>>12)&0xF]; fbuf[fp++] = hx[(v86_timeout_ip>>8)&0xF];
                fbuf[fp++] = hx[(v86_timeout_ip>>4)&0xF]; fbuf[fp++] = hx[v86_timeout_ip&0xF];
                fbuf[fp++] = ' ';
                taddr = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
                for (di = 0; di < 32 && fp < 3800; di++) {
                    fbuf[fp++] = hx[taddr[di]>>4];
                    fbuf[fp++] = hx[taddr[di]&0xF];
                    fbuf[fp++] = ' ';
                }
                fbuf[fp++] = '\n';
            }
        }
        /* ディスクログ */
        dlog = (struct v86_disk_log_entry *)v86_disk_get_log(&total_count, &log_idx_v);
        n = (total_count < 64) ? total_count : 64;
        start = (total_count <= 64) ? 0 : log_idx_v;
        for (i = 0; i < n && fp < 3900; i++) {
            u32 ix = (start + i) % 64;
            struct v86_disk_log_entry *e = (struct v86_disk_log_entry *)&dlog[ix];
            fbuf[fp++] = hx[e->func>>4]; fbuf[fp++] = hx[e->func&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = 'C'; fbuf[fp++] = hx[e->cylinder>>4]; fbuf[fp++] = hx[e->cylinder&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = 'H'; fbuf[fp++] = hx[e->head>>4]; fbuf[fp++] = hx[e->head&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = 'S'; fbuf[fp++] = hx[e->sector>>4]; fbuf[fp++] = hx[e->sector&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = 'N'; fbuf[fp++] = hx[e->sector_len>>4]; fbuf[fp++] = hx[e->sector_len&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = hx[(e->xfer_bytes>>12)&0xF]; fbuf[fp++] = hx[(e->xfer_bytes>>8)&0xF];
            fbuf[fp++] = hx[(e->xfer_bytes>>4)&0xF]; fbuf[fp++] = hx[e->xfer_bytes&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = hx[(e->es>>12)&0xF]; fbuf[fp++] = hx[(e->es>>8)&0xF];
            fbuf[fp++] = hx[(e->es>>4)&0xF]; fbuf[fp++] = hx[e->es&0xF]; fbuf[fp++] = ':';
            fbuf[fp++] = hx[(e->bp>>12)&0xF]; fbuf[fp++] = hx[(e->bp>>8)&0xF];
            fbuf[fp++] = hx[(e->bp>>4)&0xF]; fbuf[fp++] = hx[e->bp&0xF]; fbuf[fp++] = ' ';
            fbuf[fp++] = hx[e->status>>4]; fbuf[fp++] = hx[e->status&0xF];
            fbuf[fp++] = '\n';
            fbuf[fp++] = '\n';
        }

        fbuf[fp] = '\0';
        vfs_write("/host/v86_fdos_log.txt", fbuf, (u32)fp);
    }

    /* V86 GPトレース(直近128件)を /host/v86_gptrace.txt に書き出し */
    {
        struct v86_trace_entry {
            u16 cs; u16 ip; u8 opcode; u8 intno; u8 ah; u8 al;
        };
        extern struct v86_trace_entry *v86_get_trace(u32 *count, u32 *idx);
        static char tbuf[8192];
        int tp = 0;
        u32 total, tidx;
        struct v86_trace_entry *tlog;
        u32 tn, ti, tstart;
        const char *hx = "0123456789ABCDEF";

        tlog = v86_get_trace(&total, &tidx);
        tn = (total < 128) ? total : 128;
        tstart = (total <= 128) ? 0 : tidx;
        for (ti = 0; ti < tn && tp < 7900; ti++) {
            u32 tix = (tstart + ti) % 128;
            struct v86_trace_entry *te = &tlog[tix];
            tbuf[tp++] = hx[te->cs>>12]; tbuf[tp++] = hx[(te->cs>>8)&0xF];
            tbuf[tp++] = hx[(te->cs>>4)&0xF]; tbuf[tp++] = hx[te->cs&0xF];
            tbuf[tp++] = ':';
            tbuf[tp++] = hx[te->ip>>12]; tbuf[tp++] = hx[(te->ip>>8)&0xF];
            tbuf[tp++] = hx[(te->ip>>4)&0xF]; tbuf[tp++] = hx[te->ip&0xF];
            tbuf[tp++] = ' ';
            tbuf[tp++] = hx[te->opcode>>4]; tbuf[tp++] = hx[te->opcode&0xF];
            tbuf[tp++] = ' ';
            tbuf[tp++] = hx[te->intno>>4]; tbuf[tp++] = hx[te->intno&0xF];
            tbuf[tp++] = ' ';
            tbuf[tp++] = hx[te->ah>>4]; tbuf[tp++] = hx[te->ah&0xF];
            tbuf[tp++] = hx[te->al>>4]; tbuf[tp++] = hx[te->al&0xF];
            tbuf[tp++] = '\n';
        }
        tbuf[tp] = '\0';
        vfs_write("/host/v86_gptrace.txt", tbuf, (u32)tp);
    }

    kprintf(0xA1, "[V86] FreeDOS(98) session ended.\n");
    return 0;
}

