/* ======================================================================== */
/*  V86_SESSION.C - V86 セッションマネージャ                                */
/*                                                                          */
/*  V86 (VDOS) セッションのライフサイクル管理を行う。                       */
/*  v86_test.c から v86_boot_freedos() を分離し、Auto-Typer、               */
/*  強制脱出ホットキー、終了理由管理を統合する。                            */
/* ======================================================================== */

#include "v86_session.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_bios.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_disk.h"
#include "v86_debug.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "vfs.h"
#include "kprintf.h"
#include "io.h"
#include "kbd.h"

extern void serial_puts(const char *s);

/* exec_setjmp / exec_longjmp (setjmp.asm) */
extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* 外部TSSアクセス */
extern struct tss_entry kernel_tss;

/* ====================================================================== */
/*  グローバル変数 (v86_session.h で extern 宣言済み)                       */
/* ====================================================================== */
volatile int v86_exit_request = 0;
u32 v86_irq0_inject_count = 0;

/* ====================================================================== */
/*  セッションローカル変数                                                 */
/* ====================================================================== */
static V86Session current_session;
static u32 v86_session_jmpbuf[6];

/* V86テスト用カーネルスタック (16KB) */
static u8 v86_kstack[16384] __attribute__((aligned(16)));

/* ====================================================================== */
/*  IPL 定数                                                               */
/* ====================================================================== */
#define IPL_SEG      0x1FC0
#define IPL_LINEAR   0x1FC00UL
#define IPL_SIZE     1024

/* ====================================================================== */
/*  v86_exit_reason_str - 終了理由を文字列に変換                           */
/* ====================================================================== */
const char *v86_exit_reason_str(enum v86_exit_reason reason)
{
    switch (reason) {
    case V86_EXIT_NONE:       return "none";
    case V86_EXIT_TRAP_PORT:  return "trap port (0xFE)";
    case V86_EXIT_DOS_TERM:   return "DOS terminate (INT 20h/21h)";
    case V86_EXIT_REBOOT:     return "reboot detected";
    case V86_EXIT_BIOS_ROM:   return "BIOS ROM (CS>=F000)";
    case V86_EXIT_TIMEOUT:    return "timeout";
    case V86_EXIT_UNKNOWN_OP: return "unknown opcode";
    case V86_EXIT_HOTKEY:     return "hotkey (F12)";
    }
    return "unknown";
}

/* ====================================================================== */
/*  v86_request_exit - V86終了を要求する                                   */
/* ====================================================================== */
void v86_request_exit(enum v86_exit_reason reason)
{
    if (current_session.exit_reason == V86_EXIT_NONE) {
        current_session.exit_reason = reason;
    }
    v86_exit_request = 1;
}

/* ====================================================================== */
/*  v86_session_on_tick - IRQ0 (100Hz) ごとのコールバック                   */
/*                                                                          */
/*  v86_inject_timer_irq() から呼ばれる。                                  */
/*  1. Auto-Typer: BDAキーバッファへのキー注入                             */
/*  2. 強制脱出: F12キー検知                                               */
/* ====================================================================== */
void v86_session_on_tick(void)
{
    extern u32 v86_irq0_call_count;

    /* ============================================================ */
    /*  強制脱出ホットキー検知 (F12)                                */
    /* ============================================================ */
    {
        int key = kbd_peekkey();
        if (key >= 0) {
            u8 scan = (u8)((key >> 8) & 0xFF);
            if (scan == PC98_SCANCODE_F12) {
                kbd_trygetkey(); /* バッファから消費 */
                v86_request_exit(V86_EXIT_HOTKEY);
                return;
            }
        }
    }

    /* ============================================================ */
    /*  Auto-Typer                                                  */
    /* ============================================================ */
    if (current_session.auto_cmd == 0 || current_session.auto_done) {
        return;
    }

    /* 初期ディレイ */
    if (current_session.auto_delay_remaining > 0) {
        current_session.auto_delay_remaining--;
        return;
    }

    /* コマンド文字列の終端チェック */
    if (current_session.auto_cmd[current_session.auto_cmd_idx] == '\0') {
        current_session.auto_done = 1;
        return;
    }

    /* V86_AUTO_TYPE_INTERVAL ticks ごとに1文字注入 */
    if ((v86_irq0_call_count % V86_AUTO_TYPE_INTERVAL) == 0) {
        u8 *bda = v86_phys_addr(0, 0);
        u8 count = bda[BDA_KB_COUNT];

        if (count < 0x10) {
            u16 head = *(u16 *)&bda[BDA_KB_HEAD];
            char c = current_session.auto_cmd[current_session.auto_cmd_idx++];
            u8 scancode = 0;

            if (c == '\r' || c == '\n') {
                c = 0x0D;
                scancode = 0x1C; /* Enter key */
            }

            bda[head] = c;
            bda[head + 1] = scancode;
            head += 2;
            if (head >= BDA_KB_BUF_END) head = BDA_KB_BUF_START;
            *(u16 *)&bda[BDA_KB_HEAD] = head;
            bda[BDA_KB_COUNT] = count + 1;
        }
    }
}




/* ====================================================================== */
/*  デバッグカウンタリセット                                               */
/* ====================================================================== */
static void v86_reset_counters(void)
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
    extern void v86_reset_io_stats(void);

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
    v86_disk_reset_log();
    v86_reset_io_stats();
}

/* ====================================================================== */
/*  v86_boot_freedos - FreeDOS(98) FDDイメージからブート                   */
/* ====================================================================== */
int v86_boot_freedos(const char *path, const char *cmdline)
{
    struct v86_context ctx;
    u32 saved_esp0;
    int fd;
    u8 *ipl_dst;

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;

    /* イメージファイルオープン */
    fd = vfs_open(path, 0);
    if (fd < 0) {
        kprintf(0xE1, "[V86] FDD image open failed: %s (rc=%d)\n", path, fd);
        return -1;
    }
    current_session.fd = fd;
    current_session.img_data_size = vfs_get_size(fd);

    /* FDIヘッダ判定 */
    {
        u8 hdr[12];
        vfs_read_fd(fd, hdr, 12);
        if (current_session.img_data_size > 0x1000) {
            u32 hdr_size = *(u32 *)(hdr + 8);
            if (hdr_size == 0x1000 || hdr_size == 0x2000) {
                current_session.img_offset = hdr_size;
                current_session.img_data_size -= hdr_size;
                kprintf(0xA1, "[V86] FDI header: offset=0x%x\n",
                        current_session.img_offset);
            }
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT/ディスク仮想化初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_disk_set_file(fd, current_session.img_offset,
                      current_session.img_data_size);

    /* IPLをV86メモリにコピー */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    vfs_seek(fd, current_session.img_offset, 0);
    vfs_read_fd(fd, ipl_dst, IPL_SIZE);

    /* V86コンテキスト: IPLエントリ */
    ctx.eip    = 0x0000;
    ctx.cs     = IPL_SEG;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;
    ctx.esp    = 0xFFFE;
    ctx.ss     = 0x0000;
    ctx.es     = IPL_SEG;
    ctx.ds     = IPL_SEG;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    /* TSS ESP0 切り替え */
    saved_esp0 = 0x9FFF0UL;
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* V86モード遷移準備 */
    v86_active = 1;
    v86_exit_request = 0;
    current_session.exit_reason = V86_EXIT_NONE;

    /* デバッグカウンタリセット */
    v86_reset_counters();

    kprintf(0xA1, "[V86] Booting FreeDOS(98) IPL...\n");

    /* ジャンプバッファをアクティブに設定 */
    {
        extern u32 *v86_current_jmpbuf;
        v86_current_jmpbuf = v86_session_jmpbuf;
    }

    if (exec_setjmp(v86_session_jmpbuf) == 0) {
        v86_enter(&ctx);
    }

    /* ============================================================ */
    /*  V86終了後の後始末                                           */
    /* ============================================================ */
    {
        extern u32 *v86_current_jmpbuf;
        v86_current_jmpbuf = 0;
    }

    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;
    tss_set_esp0(saved_esp0);

    /* デバッグダンプ (有効時のみ) */
    v86_debug_dump_session();

    /* リソース解放 */
    v86_disk_clear();
    vfs_close(fd);
    current_session.fd = -1;

    /* メモリ空間復元 */
    v86_mem_teardown();

    /* 画面リストア */
    v86_restore_screen();

    /* 終了メッセージ */
    kprintf(0xA1, "[V86] Session ended: %s\n",
            v86_exit_reason_str(current_session.exit_reason));

    return 0;
}
