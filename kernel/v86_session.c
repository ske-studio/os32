/* ======================================================================== */
/*  V86_SESSION.C - V86 セッションマネージャ                                */
/*                                                                          */
/*  V86 セッションのライフサイクル管理を行う。                              */
/*  Auto-Typer、強制脱出ホットキー、終了理由管理を統合する。                */
/* ======================================================================== */

#include "v86_session.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_bios.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_fdc.h"
#include "v86_dma.h"
#include "v86_disk.h"
#include "v86_debug.h"
#include "v86_event.h"
#include "v86_bda.h"
#include "v86_iocore.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "vfs.h"
#include "kprintf.h"
#include "io.h"
#include "kbd.h"
#include "fdc.h"
#include "v86_vsync.h"
#include "loop_dev.h"   /* loop_dev_attach / read_chs */

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

/* T1.3: asm ブロックから参照される BDA exit タグ文字列 */
const char v86_bda_exit_tag[] = "exit";

/* V86カーネルスタック (32KB, v86_test.cと共用) */
u8 v86_kstack[32768] __attribute__((aligned(16)));

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
    case V86_EXIT_PAGE_FAULT: return "page fault (#PF)";
    }
    return "unknown";
}

/* ====================================================================== */
/*  v86_debug_get_exit_reason - 終了理由コードを返す (デバッグログ用)       */
/* ====================================================================== */
int v86_debug_get_exit_reason(void)
{
    return (int)current_session.exit_reason;
}

/* ====================================================================== */
/*  v86_request_exit - V86終了を要求する                                   */
/* ====================================================================== */
void v86_request_exit(enum v86_exit_reason reason)
{
    if (current_session.exit_reason == V86_EXIT_NONE) {
        current_session.exit_reason = reason;
    }

    /* T1.1: クリティカルイベントを記録 (即時flush) */
    {
        u8 ev_kind;
        switch (reason) {
        case V86_EXIT_TIMEOUT:    ev_kind = V86_EV_TIMEOUT;    break;
        case V86_EXIT_UNKNOWN_OP: ev_kind = V86_EV_UNKNOWN_OP; break;
        default:                  ev_kind = V86_EV_EXIT;       break;
        }
        v86_event_record(ev_kind,
                         (u16)v86_last_cs, (u16)v86_last_ip,
                         (u8)reason, 0, 0, 0);
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
    /* T1.1: イベントログ定期 flush (30 tick = 300ms) */
    v86_event_tick_check();

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

    /* #PF 診断情報リセット */
    {
        extern int v86_pf_recorded;
        v86_pf_recorded = 0;
    }
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

    /* ================================================================ */
    /*  §7.5 V86 enter前にFM音源 (OPN) を完全リセット                   */
    /*                                                                  */
    /*  OS32のサウンドエンジンが残したレジスタ状態をクリアする。          */
    /*  Ys等のゲームはSSGレジスタ#0への書き込み→読み戻しでFM検出を     */
    /*  行うため、ミキサーやタイマー制御レジスタに不整合があると         */
    /*  検出が失敗し、サウンドが初期化されない。                         */
    /* ================================================================ */
    {
        int i;
        /* SSGレジスタクリア (00h-0Dh) */
        for (i = 0; i <= 0x0D; i++) {
            outp(0x188, (u8)i);
            io_wait(); io_wait(); io_wait();
            io_wait(); io_wait(); io_wait();
            outp(0x18A, 0);
            io_wait(); io_wait(); io_wait(); io_wait();
            io_wait(); io_wait(); io_wait(); io_wait();
            io_wait(); io_wait(); io_wait(); io_wait();
            io_wait(); io_wait(); io_wait(); io_wait();
        }
        /* SSGミキサー: I/Oポート方向を入力に設定 (D7=1, D6=0)
         * BIOSデフォルト値を模倣。ゲームのジョイスティック検出に影響。
         * Tone/Noise全OFF (bit0-5=1) */
        outp(0x188, 0x07);
        io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait();
        outp(0x18A, 0xBF);
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
        /* FM全チャンネル Key-OFF */
        outp(0x188, 0x28);
        io_wait(); io_wait(); io_wait();
        outp(0x18A, 0x00);
        io_wait(); io_wait(); io_wait(); io_wait();
        outp(0x188, 0x28);
        io_wait(); io_wait(); io_wait();
        outp(0x18A, 0x01);
        io_wait(); io_wait(); io_wait(); io_wait();
        outp(0x188, 0x28);
        io_wait(); io_wait(); io_wait();
        outp(0x18A, 0x02);
        io_wait(); io_wait(); io_wait(); io_wait();
        /* タイマー停止 + フラグリセット */
        outp(0x188, 0x27);
        io_wait(); io_wait(); io_wait();
        outp(0x18A, 0x30);  /* RSETA+RSETB=1, 他=0 → フラグクリア+タイマー停止 */
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
        io_wait(); io_wait(); io_wait(); io_wait();
    }

    /* ================================================================ */
    /*  §7.6 メモリスイッチ SW4 (0xA3FEE) の初期化                      */
    /*                                                                  */
    /*  PC-9800Bible §1-6: SW4 bit3 = サウンドボード有無                */
    /*  V86ではBIOS POSTをスキップするため、メモリスイッチが             */
    /*  未初期化(=0)の場合がある。ゲームがこのフラグを参照して           */
    /*  FM音楽の有効/無効を判定するため、明示的に設定する。              */
    /*                                                                  */
    /*  メモリスイッチ書き込み手順:                                      */
    /*    1. I/O 68H に 0DH を出力 (書き込み許可)                       */
    /*    2. メモリに書き込み                                            */
    /*    3. I/O 68H に 0CH を出力 (書き込み禁止)                       */
    /* ================================================================ */
    {
        volatile u8 *sw4 = (volatile u8 *)0xA3FEE;
        u8 val = *sw4;
        kprintf(0x0A, "[V86] MemSW4(A3FEE)=0x%02X", (unsigned)val);
        /* §7.6 DOS5 IO.SYS 互換性:
         * SW4 bit3=1 (サウンドボードあり) に設定すると、IO.SYS は
         * CC000h-CFFFF のサウンドBIOS ROM の存在を検証する。
         * NP21/W上ではこの領域にROMが存在しない (全て0xFF) ため、
         * 「基本BIOS.ROMが見つかりません」エラーで停止する。
         *
         * 対策: DOS ブート時は SW4 bit3=0 のままにする。
         * Ys等のゲームは BIOS SW4 を参照せず FM 音源ポートに直接
         * アクセスして検出するため、bit3=0 でも問題ない。
         *
         * 将来 CC000h にダミー ROM を配置した場合は bit3=1 に復活させる。 */
        kprintf(0x0A, " (SndBoard OFF for DOS compat)\n");
        (void)val;
    }

    /* ================================================================ */
    /*  §7.7 サウンドBIOS ROM (CC000h) の確認                           */
    /*                                                                  */
    /*  PC-9800Bible §1-6: SW4 bit3=1 のとき CC000-CFFFF に             */
    /*  サウンドBIOS ROMが存在する。ゲームがこの領域を参照して           */
    /*  FM音楽初期化の可否を判定している可能性がある。                   */
    /* ================================================================ */
    {
        volatile u8 *snd_rom = (volatile u8 *)0xCC000;
        int si;
        kprintf(0x0A, "[V86] SndBIOS(CC000)=");
        for (si = 0; si < 16; si++)
            kprintf(0x0A, "%02X", (unsigned)snd_rom[si]);
        kprintf(0x0A, "\n");
    }

    /* ★ デバッグ: V86 enter前にPIC IMRとtick_countを確認
     * IRQ0がマスクされている場合はアンマスクする。 */
    {
        extern volatile u32 tick_count;
        u8 imr = inp(0x02);
        kprintf(0x0A, "[V86] PRE-ENTER: IMR=0x%02X tick=%u\n",
                (unsigned)imr, (unsigned)tick_count);
        if (imr & 0x01) {
            /* IRQ0がマスクされている! アンマスクする */
            kprintf(0xE1, "[V86] WARNING: IRQ0 masked! Unmasking...\n");
            outp(0x02, imr & ~0x01);
        }
    }

    /* debug: V86 enter PDE/PTE check via CR3 */
    {
        u32 cr3_val, pde0, pte0, pte_ipl;
        u32 *pd, *pt;
        u32 ipl_va, ipl_pti;
        u32 idt_base;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
        pd = (u32 *)cr3_val;
        pde0 = pd[0];
        pt = (u32 *)(pde0 & 0xFFFFF000UL);
        pte0 = pt[0];
        /* IPL_SEG (0x1FC0) のリニアアドレス = 0x1FC00 */
        ipl_va = 0x1FC00UL;
        ipl_pti = (ipl_va >> 12) & 0x3FF;
        pte_ipl = pt[ipl_pti];
        kprintf(0xA1, "[V86] CR3=%X PDE0=%X(%s%s%s)\n",
                cr3_val, pde0,
                (pde0 & 1) ? "P" : "-",
                (pde0 & 2) ? "W" : "R",
                (pde0 & 4) ? "U" : "S");
        kprintf(0xA1, "[V86] PTE[0]=%X(%s%s%s) PTE[IPL:%X]=%X(%s%s%s)\n",
                pte0,
                (pte0 & 1) ? "P" : "-",
                (pte0 & 2) ? "W" : "R",
                (pte0 & 4) ? "U" : "S",
                ipl_pti,
                pte_ipl,
                (pte_ipl & 1) ? "P" : "-",
                (pte_ipl & 2) ? "W" : "R",
                (pte_ipl & 4) ? "U" : "S");
        /* IDT ページの PTE */
        __asm__ volatile ("sidt %0" : "=m"(idt_base));
        {
            u32 idt_addr = *(u32 *)((u8 *)&idt_base + 2);
            u32 idt_pti = (idt_addr >> 12) & 0x3FF;
            u32 idt_pdi = idt_addr >> 22;
            u32 pte_idt = 0;
            if (idt_pdi < 4) {
                u32 *pt_idt = (u32 *)(pd[idt_pdi] & 0xFFFFF000UL);
                pte_idt = pt_idt[idt_pti];
            }
            kprintf(0xA1, "[V86] IDT@%X PTE=%X ESP0=%X\n",
                    idt_addr, pte_idt, tss_get_esp0());
        }
    }

    /* V86 enter 直前に TLB を明示的にフラッシュ
     * paging_set_page() が各呼び出しでフラッシュするが、
     * v86_session_run_core() の kprintf 等がTLBを再ポピュレートする可能性あり */
    {
        u32 cr3_val;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
    }

    if (exec_setjmp(v86_session_jmpbuf) == 0) {
#if 0  /* §BUG-CTX ホットフィックス無効化: 初回設定(L260)のみでテスト */
        /* ★ ホットフィックス: ctx がスタック破壊で壊れる問題 (§BUG-CTX)
         *
         * v86_session_run_core() のローカル変数 ctx (アドレス ~0xEFF6xx) が
         * L260 での初期化後、L470 に到達するまでの間にスタック上で破壊される。
         * 観測: ctx.eip = 0x0014 (本来 0x0000)。
         *
         * 原因未特定のため、v86_enter() 直前で ctx 全体を再設定する。
         * 根本原因が判明したらこのブロックを除去すること。
         */
        if (ctx.eip != 0x0000) {
            kprintf(0xE1, "[V86] WARNING: ctx corrupted! eip=%X (expected 0)\n",
                    (unsigned)ctx.eip);
        }
        ctx.eip    = 0x0000;
        ctx.cs     = IPL_SEG;
        ctx.eflags = EFLAGS_VM | EFLAGS_IF;
        ctx.esp    = 0xFFFE;
        ctx.ss     = 0x0000;
        ctx.es     = IPL_SEG;
        ctx.ds     = IPL_SEG;
        ctx.fs     = 0x0000;
        ctx.gs     = 0x0000;
#endif
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
            "call v86_debug_snapshot_memsw_exit\n\t" /* T2.1: MEMSW exitスナップショット */
            "push $v86_bda_exit_tag\n\t"  /* T1.3: BDA exitスナップショット */
            "call v86_debug_dump_bda_named\n\t"
            "add $4, %%esp\n\t"
            "call v86_debug_dump_memory_pre\n\t" /* teardown前にメモリダンプ */
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

    /* シリアルポート再初期化 (V86ゲストが設定を壊した場合の安全策) */
    {
        extern void serial_init(unsigned long baud);
        serial_init(9600);
    }

    /* デバッグダンプ (有効時のみ) */
    v86_debug_dump_session();

    /* ディスクI/Oログをダンプ (デバッグ用) */
    v86_disk_dump_log();

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
    /* タイムアウト終了時は停止位置を表示 */
    if (current_session.exit_reason == V86_EXIT_TIMEOUT) {
        kprintf(0xA1, "[V86] TIMEOUT at CS:IP=%04X:%04X GP#=%u\n",
                (unsigned)(v86_timeout_cs & 0xFFFF),
                (unsigned)(v86_timeout_ip & 0xFFFF),
                (unsigned)v86_gp_count);
    }
    /* #PF 終了時は診断情報を表示 */
    if (current_session.exit_reason == V86_EXIT_PAGE_FAULT) {
        extern u32 v86_pf_cr2;
        extern u32 v86_pf_error_code;
        extern u16 v86_pf_cs;
        extern u16 v86_pf_ip;
        kprintf(0xE1, "[V86] #PF cr2=%x err=%x CS:IP=%x:%x\n",
                (unsigned)v86_pf_cr2, (unsigned)v86_pf_error_code,
                (unsigned)v86_pf_cs, (unsigned)v86_pf_ip);
    }
}

/* ====================================================================== */
/*  C-1: v86_open_image_to_loop — イメージを loop_dev にアタッチ           */
/*  フォーマット判定は loop_dev_attach_fd に丸投げ。                        */
/*  戻り値: 0=成功, -1=open失敗, -2=attach失敗, -3=IPL読み失敗             */
/* ====================================================================== */
static int v86_open_image_to_loop(const char *path, int *out_slot)
{
    int fd, slot, ret;
    const char *ext;

    fd = vfs_open(path, 0);
    if (fd < 0) return -1;

    /* 拡張子からフォーマット指定 */
    ext = path;
    {
        const char *p = path;
        while (*p) {
            if (*p == '.') ext = p;
            p++;
        }
    }

    /* 空きスロットを探してアタッチ */
    for (slot = 0; slot < 4; slot++) {
        int fmt = LOOP_FMT_NONE;
        if (kstrcmp(ext, ".d88") == 0 || kstrcmp(ext, ".D88") == 0)
            fmt = LOOP_FMT_D88;
        else if (kstrcmp(ext, ".fdi") == 0 || kstrcmp(ext, ".FDI") == 0)
            fmt = LOOP_FMT_FDI;
        else if (kstrcmp(ext, ".hdi") == 0 || kstrcmp(ext, ".HDI") == 0)
            fmt = LOOP_FMT_HDI;
        else
            fmt = LOOP_FMT_RAW;
        ret = loop_dev_attach_fd(fd, slot, fmt);
        if (ret == 0) break;
    }
    if (ret != 0) {
        vfs_close(fd);
        return -2;
    }

    /* v86_disk にアタッチ */
    v86_disk_attach_loop(slot);
    *out_slot = slot;

    current_session.fd = fd;
    current_session.img_data_size = vfs_get_size(fd);

    return 0;
}

/* ====================================================================== */
/*  C-2: v86_boot_image — 統合ブート関数 (常にネイティブモード)            */
/* ====================================================================== */
static int v86_boot_image(const char *path, const char *cmdline)
{
    extern u32 v86_timeout_ticks;
    extern int v86_native_mode;
    int slot;
    u8 *ipl_dst;
    u8 ipl_buf[1024];

    /* 再入禁止ガード */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_image called while already active\n");
        return -2;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = cmdline ? V86_AUTO_TYPE_DELAY : 0;

    /* ネイティブモード設定
     * デバッグモード時はGPなし無限ループ検出のため60秒タイムアウトを設定。
     * v86_inject_timer_irq() の冒頭でtick_countと比較して自動脱出する。
     * 非デバッグ時はユーザーがホットキーで手動脱出する想定。 */
    v86_timeout_ticks = v86_debug_enabled ? 2000 : 0;
    v86_native_mode = 1;

    /* イメージを loop_dev にアタッチ */
    {
        int rc = v86_open_image_to_loop(path, &slot);
        if (rc != 0) {
            kprintf(0xE1, "[V86] Image open failed: %s (rc=%d)\n", path, rc);
            v86_timeout_ticks = 6000;
            v86_native_mode = 0;
            return -1;
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* T2.1: MEMSW 初期スナップショット */
    if (v86_debug_enabled) v86_debug_snapshot_memsw_init();

    /* T1.3: BDA init スナップショット */
    v86_debug_dump_bda_named("init");

    /* T2.3: IVT 初期スナップショット */
    if (v86_debug_enabled) v86_debug_snapshot_ivt_init();

    /* PIC/PIT/FDC/DMA仮想化初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_fdc_virt_init();
    v86_dma_init();

    /* I/Oディスパッチテーブル初期化 (NP21/W iocore準拠) */
    v86_iocore_init();

    /* IPL: track0/head0/sect1 を読んで V86 メモリにコピー */
    {
        u16 bps;
        u32 ipl_size;
        loop_dev_get_geometry(slot, NULL, NULL, NULL, &bps, NULL);
        ipl_size = (u32)bps;
        if (ipl_size > sizeof(ipl_buf)) ipl_size = sizeof(ipl_buf);

        if (loop_dev_read_chs(slot, 0, 0, 1, ipl_buf) != 0) {
            kprintf(0xE1, "[V86] IPL read failed\n");
            v86_disk_clear();
            vfs_close(current_session.fd);
            v86_timeout_ticks = 6000;
            v86_native_mode = 0;
            return -1;
        }
        ipl_dst = v86_phys_addr(IPL_SEG, 0);
        kmemcpy(ipl_dst, ipl_buf, ipl_size);
        kprintf(0xA1, "[V86] IPL loaded: %u bytes at %04X:0000\n",
                (unsigned)ipl_size, IPL_SEG);

        /* T1.3: BDA post_ipl スナップショット */
        v86_debug_dump_bda_named("post_ipl");
    }

    /* ネイティブモード: 画面表示を強制有効化 */
    outp(0x68, 0x0F);
    outp(0xA2, 0x0D);
    outp(0x6A, 0x01);
    outp(0x68, 0x08);

    /* VSYNC仮想化を初期化 */
    v86_vsync_init();

    /* デバッグヘッダ */
    v86_debug_write_header("Native", path, cmdline);

    kprintf(0xA1, "[V86] Booting: %s\n", path);

    /* V86実行コア */
    v86_session_run_core();

    /* リソース解放 */
    v86_vsync_cleanup();
    v86_disk_clear();
    vfs_close(current_session.fd);
    current_session.fd = -1;

    v86_timeout_ticks = 6000;
    v86_native_mode = 0;

    return 0;
}

/* KAPI ラッパー (Phase C-3 用) */
int v86_boot_image_kapi(const char *path, const char *cmdline)
{
    return v86_boot_image(path, cmdline);
}



/* ====================================================================== */
/*  v86_boot_native - 互換ラッパー (v86_boot_image に委譲)                 */
/* ====================================================================== */
int v86_boot_native(const char *path, const char *cmdline)
{
    return v86_boot_image(path, cmdline);
}

/* ====================================================================== */
/*  v86_boot_physical_fdd - 実FDDからV86セッションを起動 (2HD固定ラッパー)   */
/*                                                                          */
/*  media=0 (2HD 1232KB) で v86_boot_physical_fdd_ex() に委譲する。         */
/*  既存 KAPI (sys_v86_boot_physical) との後方互換性を維持する。            */
/* ====================================================================== */
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    /* media=0 → 2HD (1232KB) デフォルト */
    return v86_boot_physical_fdd_ex(drv, 0, cmdline);
}

/* ====================================================================== */
/*  v86_boot_physical_fdd_ex - 実FDDからV86セッションを起動 (メディア指定版) */
/*                                                                          */
/*  media: 0=2HD(1.2MB), 1=2DD(640KB), 2=2DD(720KB)                       */
/*  kapi.json の sys_v86_boot_physical_ex から呼び出される。               */
/* ====================================================================== */
int v86_boot_physical_fdd_ex(int drv, int media, const char *cmdline)
{
    fdc_media_t fdc_media;
    u8 *ipl_dst;
    int retry;
    int ipl_rc = -1;
    const struct fdc_geom *g;

    /* §1.4 再入禁止ガード */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_physical_fdd_ex called while already active\n");
        return -2;
    }

    /* メディア種別を enum に変換 */
    switch (media) {
    case 1:  fdc_media = FDC_MEDIA_2DD_640;  break;
    case 2:  fdc_media = FDC_MEDIA_2DD_720;  break;
    default: fdc_media = FDC_MEDIA_2HD_1232; break;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;

    /* ネイティブモード設定 (DOSモード廃止に伴い、全ブートパスを統一) */
    {
        extern u32 v86_timeout_ticks;
        extern int v86_native_mode;
        v86_timeout_ticks = 0;
        v86_native_mode = 1;
    }

    /* FDC再初期化 */
    {
        int init_rc = fdc_init();
        if (init_rc != 0) {
            kprintf(0xE1, "[V86] fdc_init failed (rc=%d).\n", init_rc);
            return -1;
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* T2.1: MEMSW 初期スナップショット */
    if (v86_debug_enabled) v86_debug_snapshot_memsw_init();

    /* T1.3: BDA init スナップショット */
    v86_debug_dump_bda_named("init");

    /* T2.3: IVT 初期スナップショット */
    if (v86_debug_enabled) v86_debug_snapshot_ivt_init();

    /* PIC/PIT/FDC/DMA初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_fdc_virt_init();
    v86_dma_init();

    /* I/Oディスパッチテーブル初期化 (NP21/W iocore準拠) */
    v86_iocore_init();

    /* 実FDDモードを設定 (指定メディア) */
    v86_disk_set_physical(drv, fdc_media);
    g = v86_disk_get_geom();
    current_session.img_data_size = (u32)g->cyls * g->heads
                                     * g->spt * g->bps;

    /* IPL読み込み (3回リトライ) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    for (retry = 0; retry < 3; retry++) {
        ipl_rc = fdc_read_sector_geom(drv, 0, 0, 1, g, ipl_dst);
        if (ipl_rc == 0) break;
        kprintf(0xA1, "[V86] IPL read retry %d/3 (rc=%d)\n", retry + 1, ipl_rc);
        if (fdc_init() != 0) break;
    }
    if (ipl_rc != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed after 3 retries (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }

    kprintf(0xA1, "[V86] Booting from physical FDD (drv=%d, media=%d)...\n",
            drv, media);

    /* T1.3: BDA post_ipl スナップショット */
    v86_debug_dump_bda_named("post_ipl");

    /* デバッグヘッダ即時書き込み */
    v86_debug_write_header("PhysicalFDD_EX", "(physical)", cmdline);

    /* V86実行コア */
    v86_session_run_core();

    return 0;
}
