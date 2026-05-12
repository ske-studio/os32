/* ======================================================================== */
/*  V86_SSTEP.C - V86 シングルステップ (TF) モード (T3.4)                   */
/*                                                                          */
/*  EFLAGS.TF (bit 8) を立てた状態で V86 に入ると、1命令実行後に            */
/*  #DB (INT 1) 例外が発火する。                                            */
/*                                                                          */
/*  #DB ハンドラで GP TRACE と同等の記録を行い、上限に達したら TF を         */
/*  クリアして通常モードに戻す。                                            */
/* ======================================================================== */

#include "v86_sstep.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_event.h"
#include "kprintf.h"

/* tick_count (isr_stub.asm で 100Hz インクリメント) */
extern volatile u32 tick_count;

/* ====================================================================== */
/*  グローバル状態                                                          */
/* ====================================================================== */
int  v86_singlestep_enabled = 0;
u32  v86_singlestep_max_count = 0;
u32  v86_singlestep_count = 0;

/* シングルステップログ (リングバッファ 256件) */
#define V86_SSTEP_LOG_SIZE 256

struct v86_sstep_entry {
    u32 tick;
    u16 cs;
    u16 ip;
    u16 ax;
    u16 flags;
};  /* 12B */

static struct v86_sstep_entry sstep_log[V86_SSTEP_LOG_SIZE];
static u32 sstep_log_idx = 0;

/* ====================================================================== */
/*  v86_singlestep_set — シングルステップの有効化/無効化                    */
/* ====================================================================== */
void v86_singlestep_set(int enabled, u32 max_count)
{
    v86_singlestep_enabled = enabled ? 1 : 0;
    v86_singlestep_max_count = max_count;
    v86_singlestep_count = 0;
    sstep_log_idx = 0;
}

/* ====================================================================== */
/*  v86_db_handler — V86 #DB ハンドラ                                       */
/*                                                                          */
/*  isr_stub.asm の isr_stub_1 (#DB) から呼ばれる。                         */
/*  regs[] は V86_REG_* インデックスでアクセスする。                         */
/*                                                                          */
/*  戻り値: 0 = V86 続行 (TF 維持 or クリア)                               */
/*          1 = V86 終了                                                    */
/* ====================================================================== */
int v86_db_handler(u32 *regs)
{
    struct v86_sstep_entry *e;
    u16 cs, ip_val, ax, flags;

    cs = (u16)regs[V86_REG_CS];
    ip_val = (u16)(regs[V86_REG_EIP] & 0xFFFF);
    ax = (u16)(regs[V86_REG_EAX] & 0xFFFF);
    flags = (u16)(regs[V86_REG_EFLAGS] & 0xFFFF);

    /* カウンタ増加 */
    v86_singlestep_count++;

    /* リングバッファに記録 */
    e = &sstep_log[sstep_log_idx % V86_SSTEP_LOG_SIZE];
    e->tick = tick_count;
    e->cs = cs;
    e->ip = ip_val;
    e->ax = ax;
    e->flags = flags;
    sstep_log_idx++;

    /* イベントログにも記録 */
    {
        u8 *code = v86_phys_addr(cs, ip_val);
        v86_event_record(V86_EV_GP,  /* GPイベントとして記録 */
                         cs, ip_val,
                         *code,      /* opcode */
                         (u8)(ax >> 8),  /* AH */
                         (u8)(ax & 0xFF), /* AL */
                         (u16)(regs[V86_REG_ECX] & 0xFFFF));
    }

    /* 上限チェック */
    if (v86_singlestep_max_count > 0 &&
        v86_singlestep_count >= v86_singlestep_max_count) {
        /* TF をクリアしてシングルステップ終了 */
        regs[V86_REG_EFLAGS] &= ~(1U << 8);  /* TF クリア */
        v86_singlestep_enabled = 0;
        kprintf(0x0A, "[V86_SS] single-step limit reached (%u)\n",
                (unsigned)v86_singlestep_count);
    }
    /* TF は EFLAGS に維持されているので、iretd で V86 に戻ると再び #DB 発火 */

    return 0;
}
