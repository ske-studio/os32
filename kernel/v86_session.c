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
#include "v86_bda.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "vfs.h"
#include "kprintf.h"
#include "io.h"
#include "kbd.h"
#include "fdc.h"

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

/* TSS ESP0保存 (static — longjmp後にスタック上のローカル変数が壊れるため) */
static u32 v86_saved_esp0;

/* ====================================================================== */
/*  §7.4 デバイス状態退避バッファ (FM音源 / EGC)                         */
/*                                                                          */
/*  V86開始前に保存し、V86終了後に復元する。                       */
/*  FM音源 (OPN): アドレスポート 0x188/0x18A を 0クリアしてサイレント化    */
/*  EGC: アクセスアドレス値 (0x04A0-0x04AE) を保存                          */
/* ====================================================================== */
static u8  v86_saved_opn_addr;     /* OPNアドレスレジスタ (0x188) */
static u8  v86_saved_opn2_addr;    /* OPN2アドレスレジスタ (0x18A, PC-9801-86/118等) */
static u16 v86_saved_egc[8];       /* EGCレジスタ (0x04A0/02/04/06/08/0A/0C/0E) */

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

    /* ============================================================ */
    /*  強制脱出ホットキー検知                                      */
    /*  方法1: Ctrl+GRPH+DEL (PC-98の GRPH = PC/AT の Alt)         */
    /*  方法2: STOP キー (PC-98固有キー、DOSでは未使用)             */
    /*  kbd_is_pressed() は物理キー押下状態を直接参照するため、     */
    /*  DOS側がキーバッファを消費しても影響を受けない。             */
    /* ============================================================ */
    if (kbd_is_pressed(KEY_DEL)
        && (kbd_shift_state & (SHIFT_CTRL | SHIFT_GRPH))
           == (SHIFT_CTRL | SHIFT_GRPH)) {
        v86_request_exit(V86_EXIT_HOTKEY);
        return;
    }
    if (kbd_is_pressed(KEY_STOP)) {
        v86_request_exit(V86_EXIT_HOTKEY);
        return;
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
            /* §1.2 修正: 生産側は TAIL を進める (kbd.c の物理キー経路と同規約)
             * BDA 規約: HEAD=取出ポインタ (DOS が進める), TAIL=入力ポインタ (生産側が進める)
             * NP21/W bios09.c 準拠 */
            u16 tail = *(u16 *)&bda[BDA_KB_TAIL];
            char c = current_session.auto_cmd[current_session.auto_cmd_idx++];
            u8 scancode = 0;

            if (c == '\r' || c == '\n') {
                c = 0x0D;
                scancode = 0x1C; /* Enter key */
            }

            bda[tail] = c;
            bda[tail + 1] = scancode;
            tail += 2;
            if (tail >= BDA_KB_BUF_END) tail = BDA_KB_BUF_START;
            *(u16 *)&bda[BDA_KB_TAIL] = tail;
            bda[BDA_KB_COUNT] = count + 1;
        }
    }
}




/* ====================================================================== */
/*  デバッグカウンタリセット                                               */
/* ====================================================================== */
static void v86_reset_counters(void)
{
    extern volatile u32 tick_count;

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
/*  v86_session_run_core — V86実行コア (共通ランタイム)                     */
/*                                                                          */
/*  V86メモリ空間・PIC/PIT・ディスク仮想化の初期化後に呼ばれ、             */
/*  V86コンテキストのセットアップ → 実行 → 後始末 を一括で行う。           */
/* ====================================================================== */
static void v86_session_run_core(void)
{
    struct v86_context ctx;

    /* §1.7 TVRAM 退避 (V86開始前に OS32 テキスト画面を保存) */
    v86_tvram_save();

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

    /* TSS ESP0 切り替え (§1.5: static変数に実値を保存 — longjmp後スタックが壊れるため)
     * ハードコード 0x9FFF0UL 廃止 → tss_get_esp0() で現在値を取得して保存 */
    v86_saved_esp0 = tss_get_esp0();
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* ================================================================ */
    /*  §7.4 FM音源・ EGC 状態退避                                    */
    /* ================================================================ */
    {
        int i;
        v86_saved_opn_addr  = (u8)inp(0x188);
        v86_saved_opn2_addr = (u8)inp(0x18A);
        for (i = 0; i < 8; i++) {
            v86_saved_egc[i] = (u16)inpw((unsigned int)(0x04A0 + i * 2));
        }
    }

    /* V86モード遷移準備 */
    v86_active = 1;
    v86_exit_request = 0;
    current_session.exit_reason = V86_EXIT_NONE;

    /* デバッグカウンタリセット */
    v86_reset_counters();

    /* ジャンプバッファをアクティブに設定 */
    v86_current_jmpbuf = v86_session_jmpbuf;

    if (exec_setjmp(v86_session_jmpbuf) == 0) {
        v86_enter(&ctx);
    }

    /* ============================================================ */
    /*  V86終了後の後始末                                           */
    /*                                                              */
    /*  ★重要: longjmpでカーネルスタック(0x9Fxxx)に戻った時点では  */
    /*  ページテーブルがまだV86バッキングRAMを指しており、           */
    /*  スタック内容はDOSに上書きされて壊れている。                 */
    /*  ローカル変数もスタックも使えないため:                       */
    /*  1. 一時的にv86_kstackにスタックを切り替え                   */
    /*  2. v86_mem_teardown()でページテーブル復元                   */
    /*  3. カーネルスタックの実体が復活してから通常のスタックに復帰 */
    /* ============================================================ */
    _disable();

    /* 一時スタックに切り替え → teardown → 元のスタックに戻す */
    {
        u32 tmp_stack = (u32)&v86_kstack[sizeof(v86_kstack) - 64];
        __asm__ volatile (
            "mov %%esp, %%esi\n\t"   /* 現在のESPを保存 */
            "mov %%ebp, %%edi\n\t"   /* 現在のEBPを保存 */
            "mov %0, %%esp\n\t"      /* 一時スタックに切り替え */
            "call v86_mem_teardown\n\t" /* ページテーブル復元 */
            "mov %%esi, %%esp\n\t"   /* ESPを元に戻す (実体が復活) */
            "mov %%edi, %%ebp\n\t"   /* EBPも復元 */
            : : "r"(tmp_stack)
            : "esi", "edi", "eax", "ecx", "edx", "memory"
        );
    }

    /* TSS ESP0 復元 (static変数から読む) */
    tss_set_esp0(v86_saved_esp0);

    /* V86状態フラグクリア */
    v86_current_jmpbuf = 0;
    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;

    _enable();

    /* デバッグダンプ (有効時のみ) */
    v86_debug_dump_session();

    /* リソース解放 */
    v86_disk_clear();

    /* 画面リストア */
    v86_restore_screen();

    /* §1.7 TVRAM 復元 (DOS が描いた文字を消去して OS32 画面を戻す) */
    v86_tvram_restore();

    /* ================================================================ */
    /*  §7.4 FM音源・ EGC 状態復元                                    */
    /*                                                                  */
    /*  DOSが変更した OPNアドレスラッチと EGCアクセスアドレスを復元する。   */
    /*  OPNは Key-Off (全チャンネルサイレンス) してからアドレス復元。     */
    /* ================================================================ */
    {
        int i;
        /* OPNサイレンス: 全チャンネル Key-Off (0x28レジスタ) */
        outp(0x188, 0x28); outp(0x18A, 0);
        outp(0x188, 0x29); outp(0x18A, 0);
        outp(0x188, 0x2A); outp(0x18A, 0);
        outp(0x188, 0x2C); outp(0x18A, 0);
        outp(0x188, 0x2D); outp(0x18A, 0);
        outp(0x188, 0x2E); outp(0x18A, 0);
        /* OPNアドレスラッチ復元 */
        outp(0x188, v86_saved_opn_addr);
        outp(0x18A, v86_saved_opn2_addr);
        /* EGCアクセスアドレス復元 */
        for (i = 0; i < 8; i++) {
            outpw((unsigned int)(0x04A0 + i * 2), v86_saved_egc[i]);
        }
    }

    /* 終了メッセージ */
    kprintf(0xA1, "[V86] Session ended: %s\n",
            v86_exit_reason_str(current_session.exit_reason));
}

/* ====================================================================== */
/*  v86_boot_freedos - FreeDOS(98) FDDイメージからブート                   */
/* ====================================================================== */
int v86_boot_freedos(const char *path, const char *cmdline)
{
    int fd;
    u8 *ipl_dst;

    /* §1.4 再入禁止ガード: V86セッションが既にアクティブなら即座に返る */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_freedos called while already active\n");
        return -2;
    }

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

    kprintf(0xA1, "[V86] Booting FreeDOS(98) IPL...\n");

    /* V86実行コア */
    v86_session_run_core();

    /* ファイルリソース解放 */
    vfs_close(fd);
    current_session.fd = -1;

    return 0;
}

/* ====================================================================== */
/*  v86_boot_physical_fdd - 実FDDからV86セッションを起動                   */
/*                                                                          */
/*  NP21/Wにマウント中のFDDから直接IPLを読み、FreeDOSを起動する。         */
/*  ファイルオープン不要。fdc_read_sector() でセクタを直接読む。           */
/* ====================================================================== */
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    u8 *ipl_dst;

    /* §1.4 再入禁止ガード */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_physical_fdd called while already active\n");
        return -2;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;
    current_session.img_data_size = V86_FDD_IMAGE_SIZE;

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT初期化 */
    v86_pic_init();
    v86_pit_init();

    /* 実FDDモードを設定 */
    v86_disk_set_physical(drv);

    /* IPLを実FDCから読み込み (シリンダ0, ヘッド0, セクタ1) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    if (fdc_read_sector(drv, 0, 0, 1, ipl_dst) != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }

    kprintf(0xA1, "[V86] Booting from physical FDD (drv=%d)...\n", drv);

    /* V86実行コア */
    v86_session_run_core();

    return 0;
}
