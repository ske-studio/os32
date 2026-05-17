/* ======================================================================== */
/*  V86.H - 仮想8086モード (VDM) ヘッダ                                    */
/*                                                                          */
/*  OS32シェルから FreeDOS(98) を V86モードで実行するための定義。            */
/* ======================================================================== */

#ifndef V86_H
#define V86_H

#include "types.h"

/* EFLAGS定数 */
#define EFLAGS_IF    0x0200U
#define EFLAGS_VM    0x020000UL
#define EFLAGS_IOPL0 0x0000U

/* V86コンテキスト (v86_entry.asm に渡す構造体) */
struct v86_context {
    u32 eip;        /* V86エントリポイント */
    u32 cs;         /* V86コードセグメント */
    u32 eflags;     /* EFLAGS (VM=1, IF=1, IOPL=0) */
    u32 esp;        /* V86スタックポインタ */
    u32 ss;         /* V86スタックセグメント */
    u32 es;         /* ES */
    u32 ds;         /* DS */
    u32 fs;         /* FS */
    u32 gs;         /* GS */
};

/* V86 #GPハンドラのスタックフレーム配列インデックス
 * (isr_stub.asm の PUSHAD + CPUが自動pushしたフレーム) */
#define V86_REG_EDI     0
#define V86_REG_ESI     1
#define V86_REG_EBP     2
#define V86_REG_ESP_D   3   /* PUSHADが保存したESP (ダミー) */
#define V86_REG_EBX     4
#define V86_REG_EDX     5
#define V86_REG_ECX     6
#define V86_REG_EAX     7
#define V86_REG_ERRCODE 8   /* CPUが自動pushしたエラーコード */
#define V86_REG_EIP     9   /* フォルト時EIP */
#define V86_REG_CS      10  /* フォルト時CS */
#define V86_REG_EFLAGS  11  /* フォルト時EFLAGS */
#define V86_REG_ESP     12  /* V86スタックポインタ */
#define V86_REG_SS      13  /* V86スタックセグメント */
#define V86_REG_ES      14
#define V86_REG_DS      15
#define V86_REG_FS      16
#define V86_REG_GS      17

/* V86モードへ遷移 (アセンブリ、v86_entry.asm) */
extern void v86_enter(const struct v86_context *ctx);

/* V86 #GPハンドラ (isr_stub.asmから呼ばれる)
 * 戻り値: 0=V86続行, 1=V86終了要求 */
int v86_gp_handler(u32 *regs);

/* V86モードが有効かどうか */
extern volatile int v86_active;

/* V86タスクの仮想IFフラグ (v86.cで定義) */
extern u32 v86_virtual_if;

/* 保留中の仮想IRQビットマスク (v86.cで定義) */
extern u32 v86_pending_irq;

/* V86タスクに仮想割り込みを保留する (IRQハンドラから呼ぶ) */
void v86_set_pending_irq(int irq_no);

/* ====================================================================== */
/*  V86 キーボード仮想化 — スキャンコードバッファ                          */
/*  kbd_irq_handler で読み取ったスキャンコードを保持し、ゲストの            */
/*  INT 09h ハンドラがポート 0x41 を読む時に返す。                         */
/* ====================================================================== */
#define V86_KBD_BUF_SIZE 16
extern volatile u8  v86_kbd_buf[V86_KBD_BUF_SIZE];
extern volatile int v86_kbd_buf_head;
extern volatile int v86_kbd_buf_tail;
extern volatile int v86_kbd_buf_count;

/* kbd.c から呼ばれる: スキャンコードをバッファに追加しIRQ1をペンディング */
void v86_kbd_enqueue(u8 scancode);

/* タイマ割り込み (IRQ0) 注入 (isr_handlers.c timer_handler から呼ばれる) */
void v86_inject_timer_irq(u32 *regs);

/* ====================================================================== */
/*  デバッグ統計カウンタ (v86.c で定義)                                     */
/* ====================================================================== */
extern u32 v86_int_count;       /* 総INT呼び出し回数 */
extern u32 v86_gp_count;        /* GPハンドラ呼び出し総数 */
extern u32 v86_last_int;        /* 最後に処理されたINT番号 */
extern u32 v86_last_cs;         /* 最後のINT発行時のCS */
extern u32 v86_last_ip;         /* 最後のINT発行時のIP */
extern u32 v86_start_tick;      /* V86開始時のtick_count */
extern u32 v86_timeout_ticks;   /* タイムアウト (0=無効, デフォルト6000=60秒) */
extern int v86_native_mode;     /* ネイティブモード (DOS終了検知無効化) */
extern u32 v86_timeout_cs;      /* タイムアウト時のCS */
extern u32 v86_timeout_ip;      /* タイムアウト時のIP */

/* IRQ0注入デバッグカウンタ (v86.c で定義) */
extern u32 v86_irq0_call_count;
extern u32 v86_irq0_nonvm_count;
extern u32 v86_irq0_noif_count;
extern u32 v86_irq0_isr_count;
extern u32 v86_irq0_ivt_count;
extern u32 v86_irq0_gp_inject_count;
extern u32 v86_irq0_gp_skip_if;
extern u32 v86_irq0_gp_skip_isr;
extern u32 v86_irq0_gp_skip_ivt;

/* ====================================================================== */
/*  ユーティリティ (v86.c で定義)                                           */
/* ====================================================================== */

/* GPトレースリセット */
void v86_trace_reset(void);

/* I/Oポートアクセス統計のダンプ / リセット */
void v86_dump_io_stats(void);
void v86_reset_io_stats(void);

/* V86終了時のlongjmpターゲット (v86_test.c で定義) */
extern u32 *v86_current_jmpbuf;

/* ====================================================================== */
/*  GPトレースバッファ (T1.2 拡張版)                                       */
/*  128件→1024件、tick フィールド追加 (16B/エントリ)                       */
/* ====================================================================== */
#define V86_TRACE_SIZE 256

struct v86_trace_entry {
    u32 tick;       /* tick_count スナップ — 時系列復元用 */
    u16 cs;
    u16 ip;
    u8  opcode;
    u8  intno;
    u8  ah;
    u8  al;
    u16 cx;         /* ECX下位16bit — シーンID等のデバッグ用 */
    u16 reserved;   /* 16B アライメント用 */
};
struct v86_trace_entry *v86_get_trace(u32 *count, u32 *idx);

/* GPトレースフィルタ (T1.2)
 * v86_trace_filter_int: -1=全件記録, 0-255=指定INT番号のみ
 * v86_trace_filter_cs_min/max: 0=無効, 非0=CS範囲フィルタ */
extern int v86_trace_filter_int;
extern u16 v86_trace_filter_cs_min;
extern u16 v86_trace_filter_cs_max;
void v86_trace_set_int_filter(int int_no);
void v86_trace_set_cs_range(u16 lo, u16 hi);

/* ====================================================================== */
/*  ダミーIVT判定マクロ                                                    */
/*  IVTエントリが初期値 (0x003F:0x0000 = IRET) のままかを判定する           */
/* ====================================================================== */
#define V86_DUMMY_IVT_SEG   0x003F
#define V86_DUMMY_IVT_OFF   0x0000
#define V86_IS_DUMMY_IVT(ivt_entry) \
    ((ivt_entry) == ((u32)V86_DUMMY_IVT_SEG << 16 | V86_DUMMY_IVT_OFF))

/* IO.SYS デフォルト「不正な割り込み」ハンドラのバイトパターン検出 */
int v86_is_dos_default_handler(u16 seg, u16 off);

/* ====================================================================== */
/*  フリーズ検出 (B-1)                                                     */
/*                                                                          */
/*  IRQ0ハンドラ内でゲストCS:IPを監視し、同一位置に                         */
/*  V86_FREEZE_THRESHOLD tick以上停滞した場合に周辺メモリと                 */
/*  レジスタをスナップショットする。ISRコンテキストでは kprintf を           */
/*  呼べないため、静的バッファに記録しV86終了後にダンプする。               */
/* ====================================================================== */
#define V86_FREEZE_THRESHOLD  100  /* 100 tick (= 1秒 @100Hz) */
#define V86_FREEZE_MEMDUMP_SIZE 32 /* CS:IP周辺ダンプサイズ (バイト) */
#define V86_FREEZE_STACK_SIZE   16 /* スタックトップダンプ (バイト) */

struct v86_freeze_info {
    int      detected;              /* フリーズ検出済みフラグ */
    u16      cs;                    /* フリーズ時 CS */
    u16      ip;                    /* フリーズ時 IP */
    u32      stuck_ticks;           /* 停滞 tick 数 */
    u32      eax, ebx, ecx, edx;   /* レジスタスナップショット */
    u32      esi, edi, ebp, esp;
    u16      ds, es, ss;
    u32      eflags;
    u8       code_dump[V86_FREEZE_MEMDUMP_SIZE]; /* CS:IP-8 ~ CS:IP+23 */
    u8       stack_dump[V86_FREEZE_STACK_SIZE];   /* SS:SP ~ SS:SP+15 */
    u32      bda_timer;             /* BDA 0040:006C */
    u8       bda_disk_int;          /* BDA 0000:055E */
    u8       bda_motor_timeout;     /* BDA 0040:0040 */
    u8       bda_motor_status;      /* BDA 0040:003F */
};

extern struct v86_freeze_info v86_freeze;

/* V86終了後に呼ぶ: フリーズ情報をシリアル/コンソールにダンプ */
void v86_freeze_dump(void);

#endif /* V86_H */
