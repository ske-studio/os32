/* ======================================================================== */
/*  V86_VSYNC.C — V86 VSYNC (CRTV) 割り込み仮想化                          */
/*                                                                          */
/*  PC-98 VSYNC仕様 (Undocumented io_disp.md):                             */
/*    I/O 0064h (WRITE): 任意の値を出力すると、次のVSYNC開始時に             */
/*                       INT 0Ah割り込みを1回だけ発生する。                  */
/*    I/O 0064h (READ):  なし                                               */
/*                                                                          */
/*  実装方針:                                                               */
/*    - OUT 0x64 をトラップし vsync_armed フラグをセット                     */
/*    - タイマIRQ (100Hz) 経由で定期的に呼ばれる注入関数で                   */
/*      armedならゲストIVT[0x0A]にINT 0Ahを注入し、armedをクリア            */
/*    - 注入レート: NP21/Wは56.4Hz (400ライン) なので2tickに1回注入          */
/*      → 100Hz / 2 ≒ 50Hz (実機の56.4Hzに近い)                           */
/*                                                                          */
/*  出典: PC9800Bible §1-4, §2-6 表2-22                                    */
/*        Undocumented io_disp.md I/O 0064h                                 */
/* ======================================================================== */

#include "v86_vsync.h"
#include "v86.h"
#include "v86_pic.h"
#include "v86_mem.h"
#include "io.h"
#include "kprintf.h"

/* v86.c と同じマクロ: seg:off → リニアアドレス変換 */
#define v86_linear(seg, off)  v86_phys_addr((seg), (off))

/* VSYNC状態 */
static volatile int vsync_armed = 0;  /* OUT 0x64 でアーミングされた */

/* 注入レート制御: 実機VSYNC ≒ 56.4Hz, OS32タイマ = 100Hz
 * 2tickに1回 = 50Hz で注入 (実機に近い値) */
static u32 vsync_tick_counter = 0;
#define VSYNC_INJECT_PERIOD  2  /* 2 tick (100Hz) に1回 = 50Hz */

/* デバッグカウンタ */
u32 v86_vsync_arm_count    = 0;
u32 v86_vsync_inject_count = 0;

/* ====================================================================== */
/*  v86_vsync_init — VSYNC仮想化初期化                                     */
/* ====================================================================== */
void v86_vsync_init(void)
{
    vsync_armed = 0;
    vsync_tick_counter = 0;
    v86_vsync_arm_count = 0;
    v86_vsync_inject_count = 0;
}

/* ====================================================================== */
/*  v86_vsync_cleanup — VSYNC仮想化クリーンアップ                          */
/* ====================================================================== */
void v86_vsync_cleanup(void)
{
    vsync_armed = 0;
}

/* ====================================================================== */
/*  v86_vsync_io — I/Oポートハンドラ                                       */
/*                                                                          */
/*  I/O 0x64: VSYNC割り込みトリガ                                           */
/*    WRITE: 次のVSYNC時にINT 0Ahを1回発生させる (ワンショット)             */
/*    READ:  なし (0xFFを返す)                                              */
/* ====================================================================== */
int v86_vsync_io(u16 port, u8 *val, int is_write)
{
    /* I/O 0x60: GDCテキストステータスレジスタ (READ)
     * I/O 0xA0: GDCグラフィックステータスレジスタ (READ)
     * 両方とも bit5 (D5) = VSYNC信号。多くのゲームがこのビットをポーリングして
     * VSYNC待ちを行う。V86のGP例外オーバーヘッドにより実ハードウェアの
     * VSYNCタイミングを見逃すため、GPカウントベースで仮想化する。
     * 参照: PC9800Bible 2-6 テキスト 表2-22 / グラフィック */
    if ((port == 0x60 || port == 0xA0) && !is_write) {
        u8 hw_val = inp(port);  /* 実ハードウェアのGDCステータス */
        /* ネイティブモード: VSYNCビットをGPカウントベースでトグル
         * 実機VSYNC = 56.4Hz, V86のGP ≈ 数千〜数万回/フレーム
         * 約200GP周期でトグル → 十分なVSYNC検出機会を提供 */
        if (v86_native_mode) {
            static u32 vsync_gp_counter = 0;
            vsync_gp_counter++;
            if ((vsync_gp_counter / 200) & 1) {
                hw_val |= 0x20;   /* bit5 = 1: VSYNC期間 */
            } else {
                hw_val &= ~0x20;  /* bit5 = 0: 表示期間 */
            }
        }
        *val = hw_val;
        return 1;
    }

    if (port != 0x64) return 0;  /* 非対象 */

    if (is_write) {
        /* VSYNC割り込みをアーミング */
        vsync_armed = 1;
        v86_vsync_arm_count++;
    } else {
        /* READ は無効 */
        *val = 0xFF;
    }
    return 1;
}

/* ====================================================================== */
/*  v86_inject_vsync_irq — VSYNC IRQ注入                                   */
/*                                                                          */
/*  タイマIRQハンドラから100Hz周期で呼ばれる。                               */
/*  VSYNCレートに合わせて間引きし、armed状態ならINT 0Ahを注入する。         */
/*                                                                          */
/*  PC-98 VSYNC割り込み:                                                    */
/*    マスタPIC IR2 → INT 0Ah (ベクタ番号 0x0A)                             */
/*    IMR MASK = 0xFB (bit2)                                                */
/* ====================================================================== */

/* HWIRQ_REG_* (v86.cと同じ定義) */
#define HWIRQ_REG_EIP    10
#define HWIRQ_REG_CS     11
#define HWIRQ_REG_EFLAGS 12
#define HWIRQ_REG_ESP    13
#define HWIRQ_REG_SS     14



/* HW IRQ注入ヘルパー (v86.cと同じロジック) */
static void vsync_hw_inject(u32 *regs, u16 handler_seg, u16 handler_off)
{
    u16 *sp;

    regs[HWIRQ_REG_ESP] = (regs[HWIRQ_REG_ESP] - 2) & 0xFFFF;
    sp = (u16 *)v86_linear(regs[HWIRQ_REG_SS], regs[HWIRQ_REG_ESP]);
    *sp = (u16)((regs[HWIRQ_REG_EFLAGS] & 0xFFFF) | EFLAGS_IF);

    regs[HWIRQ_REG_ESP] = (regs[HWIRQ_REG_ESP] - 2) & 0xFFFF;
    sp = (u16 *)v86_linear(regs[HWIRQ_REG_SS], regs[HWIRQ_REG_ESP]);
    *sp = (u16)regs[HWIRQ_REG_CS];

    regs[HWIRQ_REG_ESP] = (regs[HWIRQ_REG_ESP] - 2) & 0xFFFF;
    sp = (u16 *)v86_linear(regs[HWIRQ_REG_SS], regs[HWIRQ_REG_ESP]);
    *sp = (u16)(regs[HWIRQ_REG_EIP] & 0xFFFF);

    regs[HWIRQ_REG_CS] = handler_seg;
    regs[HWIRQ_REG_EIP] = handler_off;
}

void v86_inject_vsync_irq(u32 *regs)
{
    u32 *ivt;
    u16 handler_off, handler_seg;
    u8 isr;

    /* V86モードか確認 */
    if ((regs[HWIRQ_REG_EFLAGS] & EFLAGS_VM) == 0) return;

    /* VSYNCレート制御: VSYNC_INJECT_PERIOD tick に1回のみ処理 */
    vsync_tick_counter++;
    if ((vsync_tick_counter % VSYNC_INJECT_PERIOD) != 0) return;

    /* armed でなければ何もしない (ワンショット方式)
     * TODO: ネイティブモード時のfree-run注入は、ゲストIVTの
     * INT 0Ahハンドラが未設定(IRET)の場合にIF復帰不能で
     * ハングするため、現時点では無効。ゲストがVSYNCハンドラを
     * セットアップしたことを検知してから有効にする必要がある。 */
    if (!vsync_armed) return;

    /* 仮想IFが無効なら保留 */
    if (!v86_virtual_if) {
        v86_set_pending_irq(2);  /* IRQ2 = VSYNC */
        return;
    }

    /* ISRでIRQ2処理中なら保留 */
    isr = v86_pic_get_isr(0);
    if (isr & 0x04) {  /* bit2 = IR2 */
        v86_set_pending_irq(2);
        return;
    }

    /* IVTからINT 0Ahのハンドラアドレスを取得 */
    ivt = (u32 *)v86_linear(0, 0);
    handler_off = (u16)(ivt[0x0A] & 0xFFFF);
    handler_seg = (u16)(ivt[0x0A] >> 16);

    /* ダミーIVT (IRET) ならスキップ */
    if (V86_IS_DUMMY_IVT(ivt[0x0A])) return;

    /* ISRにビットを立てる (EOI待ち) */
    v86_pic_set_isr(0, isr | 0x04);

    /* armed をクリア (ワンショット) */
    vsync_armed = 0;

    /* 仮想IFクリア */
    v86_virtual_if = 0;
    v86_pending_irq &= ~(1U << 2);

    /* ゲストスタックにフレームをpushしてハンドラに転送 */
    vsync_hw_inject(regs, handler_seg, handler_off);

    v86_vsync_inject_count++;
}
