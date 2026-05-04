/* ======================================================================== */
/*  V86.C - 仮想8086モード (VDM) #GPハンドラ                               */
/*                                                                          */
/*  V86モードで特権命令が実行されると#GPが発生する。                        */
/*  このハンドラでV86命令をデコードし、エミュレーションを行う。             */
/*                                                                          */
/*  Phase 0: 基本命令デコーダ (INT/CLI/STI/PUSHF/POPF/HLT/IRET)           */
/*  後のフェーズでRust (os32_v86) に移行予定。                              */
/* ======================================================================== */

#include "v86.h"
#include "v86_mem.h"
#include "v86_bios.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_disk.h"
#include "v86_session.h"
#include "tvram.h"
#include "io.h"
#include "kprintf.h"

/* タイムアウトベースの V86 実行制限 (tick_count は 100Hz) */
extern volatile u32 tick_count;
#define V86_TIMEOUT_TICKS  0     /* 0=タイムアウト無効 */
u32 v86_start_tick = 0;

/* デバッグリングバッファ (最近のGPイベント記録)
 * struct v86_trace_entry は v86.h で定義 */
#define V86_TRACE_SIZE 128
static struct v86_trace_entry v86_trace[V86_TRACE_SIZE];
static u32 v86_trace_idx = 0;

/* タイムアウト時の実CS:EIP (HLT注入前の本来のアドレス) */
u32 v86_timeout_cs = 0;
u32 v86_timeout_ip = 0;

struct v86_trace_entry *v86_get_trace(u32 *count, u32 *idx)
{
    *count = v86_gp_count;
    *idx = v86_trace_idx;
    return v86_trace;
}

void v86_trace_reset(void)
{
    v86_trace_idx = 0;
}

static void tvram_hex(u32 val, int digits, int row, int *col) {
    volatile u16 *tvram = (volatile u16 *)TVRAM_BASE;
    volatile u16 *tattr = (volatile u16 *)TVRAM_ATTR;
    const char *hex = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) {
        int pos = row * 80 + *col;
        tvram[pos] = hex[(val >> (i * 4)) & 0xF];
        tattr[pos] = 0x41;
        (*col)++;
    }
}

static void tvram_puts(const char *str, int row, int *col) {
    volatile u16 *tvram = (volatile u16 *)TVRAM_BASE;
    volatile u16 *tattr = (volatile u16 *)TVRAM_ATTR;
    while (*str) {
        int pos = row * 80 + *col;
        tvram[pos] = *str;
        tattr[pos] = 0x41;
        (*col)++;
        str++;
    }
}

/* v86_gp_count は v86.h で extern 宣言済み */
static void __attribute__((unused)) dump_v86_trace_tvram(void) {
    int i, n;
    int row = 0;
    int col = 0;
    
    tvram_puts("--- V86 GP TRACE DUMP ---", row, &col);
    row++;

    n = (v86_gp_count < V86_TRACE_SIZE) ? v86_gp_count : V86_TRACE_SIZE;
    if (n > 23) n = 23; /* 最大23行表示 */
    
    for (i = 0; i < n; i++) {
        int idx = (v86_trace_idx + V86_TRACE_SIZE - n + i) % V86_TRACE_SIZE;
        struct v86_trace_entry *e = &v86_trace[idx];
        col = 0;
        tvram_puts("CS:", row, &col); tvram_hex(e->cs, 4, row, &col);
        tvram_puts(" IP:", row, &col); tvram_hex(e->ip, 4, row, &col);
        tvram_puts(" OP:", row, &col); tvram_hex(e->opcode, 2, row, &col);
        if (e->opcode == 0xCD) {
            tvram_puts(" INT:", row, &col); tvram_hex(e->intno, 2, row, &col);
            tvram_puts(" AX:", row, &col); tvram_hex((e->ah << 8) | e->al, 4, row, &col);
        }
        row++;
    }
}

/* V86モードの有効フラグ */
volatile int v86_active = 0;

/* V86タスクの仮想IFフラグ (CLI/STIで操作される) */
u32 v86_virtual_if = EFLAGS_IF;

/* 保留中の仮想IRQビットマスク */
u32 v86_pending_irq = 0;

/* デバッグ: V86 INT呼び出し統計 */
u32 v86_int_count = 0;     /* 総INT呼び出し回数 */
u32 v86_last_int = 0;      /* 最後に処理されたINT番号 */
u32 v86_last_cs = 0;       /* 最後のINT発行時のCS */
u32 v86_last_ip = 0;       /* 最後のINT発行時のIP */
u32 v86_gp_count = 0;      /* GPハンドラ呼び出し総数 */

/* I/Oポートアクセス統計 (V86終了後にダンプ用) */
#define V86_IO_STAT_SIZE 16
struct v86_io_stat {
    u16 port;
    u32 count;
};
static struct v86_io_stat v86_io_stats[V86_IO_STAT_SIZE];
static u32 v86_io_stat_count = 0;

static void v86_io_stat_record(u16 port)
{
    u32 i;
    for (i = 0; i < v86_io_stat_count; i++) {
        if (v86_io_stats[i].port == port) {
            v86_io_stats[i].count++;
            return;
        }
    }
    if (v86_io_stat_count < V86_IO_STAT_SIZE) {
        v86_io_stats[v86_io_stat_count].port = port;
        v86_io_stats[v86_io_stat_count].count = 1;
        v86_io_stat_count++;
    }
}

void v86_dump_io_stats(void)
{
    u32 i;
    kprintf(0xA1, "[V86] I/O port access stats (%d unique ports):\n", (int)v86_io_stat_count);
    for (i = 0; i < v86_io_stat_count; i++) {
        kprintf(0x07, "  port %xh: %d accesses\n",
                (unsigned)v86_io_stats[i].port,
                (int)v86_io_stats[i].count);
    }
}

void v86_reset_io_stats(void)
{
    v86_io_stat_count = 0;
}

/* V86終了要求フラグ (v86_session.h で宣言済み) */

/* ====================================================================== */
/*  V86アドレス → カーネル用リニアアドレス変換                             */
/*  v86_mem.h の v86_phys_addr() を使用する。                              */
/*  バッキングRAM (0x300000) にマッピングされた領域を正しく変換する。      */
/* ====================================================================== */
#define v86_linear(seg, off)  v86_phys_addr((seg), (off))

/* ====================================================================== */
/*  V86スタック操作ヘルパー                                                */
/* ====================================================================== */

/* V86スタックに16bitワードをpush */
static void v86_push16(u32 *regs, u16 val)
{
    u16 *sp;
    regs[V86_REG_ESP] = (regs[V86_REG_ESP] - 2) & 0xFFFF;
    sp = (u16 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
    *sp = val;
}

/* V86スタックから16bitワードをpop */
static u16 v86_pop16(u32 *regs)
{
    u16 *sp;
    u16 val;
    sp = (u16 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
    val = *sp;
    regs[V86_REG_ESP] = (regs[V86_REG_ESP] + 2) & 0xFFFF;
    return val;
}

/* ====================================================================== */
/*  GPハンドラ版 IRQ注入ヘルパー                                          */
/*  V86_REG_* インデックスを使用し、v86_push16 でスタック操作する             */
/* ====================================================================== */
static void v86_gp_inject_irq(u32 *regs, u16 handler_seg, u16 handler_off)
{
    v86_push16(regs, (u16)((regs[V86_REG_EFLAGS] & 0xFFFF) | EFLAGS_IF));
    v86_push16(regs, (u16)regs[V86_REG_CS]);
    v86_push16(regs, (u16)(regs[V86_REG_EIP] & 0xFFFF));
    regs[V86_REG_CS] = handler_seg;
    regs[V86_REG_EIP] = handler_off;
    v86_virtual_if = 0;
}

/* ====================================================================== */
/*  8ビットI/O ヘルパー: PIC/PIT仮想化チェック付き                         */
/* ====================================================================== */

/* 8ビットI/O入力: PIC/PIT仮想化チェック付き */
static u8 v86_in8_checked(u16 port)
{
    u8 val;
    if (v86_pic_io(port, &val, 0)) return val;
    if (v86_pit_io(port, &val, 0)) return val;
    return inp(port);
}

/* 8ビットI/O出力: PIC/PIT仮想化チェック付き */
static void v86_out8_checked(u16 port, u8 val)
{
    if (!v86_pic_io(port, &val, 1)) {
        if (!v86_pit_io(port, &val, 1)) {
            outp(port, val);
        }
    }
}

/* ====================================================================== */
/*  16ビットI/O ヘルパー: PIC/PIT仮想化チェック付き                         */
/*                                                                          */
/*  PIC/PITは8ビットポートデバイスのため、16ビットアクセスは                 */
/*  port と port+1 への連続8ビットアクセスに分解して仮想化を適用する。       */
/* ====================================================================== */

/* 16ビットI/O入力: PIC/PIT仮想化チェック付き */
static u16 v86_inw_checked(u16 port)
{
    u8 lo = v86_in8_checked(port);
    u8 hi = v86_in8_checked((u16)(port + 1));
    return (u16)lo | ((u16)hi << 8);
}

/* 16ビットI/O出力: PIC/PIT仮想化+リブート検知付き
 * 戻り値: 1=リブート検知(V86終了要求), 0=通常 */
static int v86_outw_checked(u16 port, u16 val)
{
    u8 lo = (u8)(val & 0xFF);
    u8 hi = (u8)((val >> 8) & 0xFF);
    /* リブート検知 (F0hポート) */
    if (v86_pic_is_reboot(port, lo)) return 1;
    if (v86_pic_is_reboot((u16)(port + 1), hi)) return 1;
    v86_out8_checked(port, lo);
    v86_out8_checked((u16)(port + 1), hi);
    return 0;
}

/* ====================================================================== */
/*  v86_gp_handler — V86モード #GP ハンドラ                                */
/*                                                                          */
/*  isr_stub.asm から呼ばれる。regs配列を操作して次の命令に進める。         */
/*  regs[] の内容は v86.h の V86_REG_* を参照。                            */
/* ====================================================================== */
int v86_gp_handler(u32 *regs)
{
    u8 *ip;
    u8 opcode;
    int prefix_len;   /* プレフィックスバイト数 (§2 対応) */
    int is_opsz32;    /* 0x66 プレフィックスあり */
    int is_rep;       /* 0xF3 REP/REPE */
    int is_repne;     /* 0xF2 REPNE */

    /* GPハンドラ呼び出しカウント (デバッグ) */
    v86_gp_count++;

    /* BIOS ROM領域 (0xF000:xxxx以降) でのGP: V86強制終了
     * IPLエラー後のJMP FAR 0xFFFF:0x0000 (リセットベクタ) で
     * BIOS ROM内コードが実行され無限GPループになるのを防止 */
    if (regs[V86_REG_CS] >= 0xF000) {
        v86_last_cs = regs[V86_REG_CS];
        v86_last_ip = regs[V86_REG_EIP];
        v86_request_exit(V86_EXIT_BIOS_ROM);
        return 1;
    }

    /* (ホットキー脱出はkbd_irq_handlerから直接longjmpで処理) */

    /* タイムアウトチェック: V86_TIMEOUT_TICKS=0 なら無効 */
    if (V86_TIMEOUT_TICKS &&
        ((tick_count - v86_start_tick) > V86_TIMEOUT_TICKS ||
         v86_gp_count > 5000000)) {
        ip = v86_linear(regs[V86_REG_CS], regs[V86_REG_EIP]);
        v86_last_int = *ip;
        v86_last_cs = regs[V86_REG_CS];
        v86_last_ip = regs[V86_REG_EIP];
        /* タイムアウト位置を記録 (IRQ0経由で既にセット済みの場合はそちらを優先) */
        if (v86_timeout_cs == 0 && v86_timeout_ip == 0) {
            v86_timeout_cs = regs[V86_REG_CS];
            v86_timeout_ip = regs[V86_REG_EIP];
        }
        v86_request_exit(V86_EXIT_TIMEOUT);
        return 1;  /* V86タイムアウト終了 */
    }

    /* フォルト位置の命令を取得 */
    ip = v86_linear(regs[V86_REG_CS], regs[V86_REG_EIP]);

    /* ================================================================== */
    /*  命令プレフィックスループ (§2 対応)                                      */
    /* ================================================================== */
    prefix_len = 0;
    is_opsz32  = 0;
    is_rep     = 0;
    is_repne   = 0;
    {
        int cont = 1;
        while (cont) {
            switch (*ip) {
            case 0x66: is_opsz32 = 1; ip++; prefix_len++; break;
            case 0x67:               ip++; prefix_len++; break;
            case 0x26: case 0x2E: case 0x36: case 0x3E:
            case 0x64: case 0x65:    ip++; prefix_len++; break;
            case 0xF0:               ip++; prefix_len++; break;  /* LOCK 読み飛ばす */
            case 0x9B:               ip++; prefix_len++; break;  /* FWAIT: NOP扱い */
            case 0xF3: is_rep    = 1; ip++; prefix_len++; break;
            case 0xF2: is_repne  = 1; ip++; prefix_len++; break;
            default: cont = 0; break;
            }
        }
    }
    opcode = *ip;  /* プレフィックス後のプライマリオペコード */

    /* トレース: 元の EIP 位置を記録 */
    {
        struct v86_trace_entry *e = &v86_trace[v86_trace_idx];
        e->cs = regs[V86_REG_CS];
        e->ip = regs[V86_REG_EIP];
        e->opcode = opcode;
        if (opcode == 0xCD) {
            e->intno = ip[1];
            e->ah = (regs[V86_REG_EAX] >> 8) & 0xFF;
            e->al = regs[V86_REG_EAX] & 0xFF;
        } else {
            e->intno = (u8)prefix_len; e->ah = 0; e->al = 0;
        }
        v86_trace_idx = (v86_trace_idx + 1) % V86_TRACE_SIZE;
    }

    /* ================================================================== */
    /*  INSB/INSW (0x6C/0x6D) — 文字列I/O入力 + REP対応                */
    /*                                                                    */
    /*  INSB: [ES:DI] ← IN(DX), DI += 1 (DF=0), ECX-- (REP時)        */
    /*  INSW: [ES:DI] ← INW(DX), DI += 2 (DF=0)                        */
    /* ================================================================== */
    if (opcode == 0x6C || opcode == 0x6D) {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        u16 di   = (u16)(regs[V86_REG_EDI] & 0xFFFF);
        u16 es   = (u16)(regs[V86_REG_ES]  & 0xFFFF);
        u32 ecx  = is_rep ? (regs[V86_REG_ECX] & 0xFFFF) : 1;
        int width = (opcode == 0x6D) ? 2 : 1;  /* INSW=2, INSB=1 */
        int df = (regs[V86_REG_EFLAGS] & (1U << 10)) ? 1 : 0;
        u32 count;

        if (ecx == 0) ecx = 1;  /* REPなしの時は1回 */

        for (count = 0; count < ecx; count++) {
            u8 *dst = v86_linear(es, di);
            if (width == 1) {
                *dst = v86_in8_checked(port);
                di = (u16)(di + (df ? -1 : 1));
            } else {
                u16 val = v86_inw_checked(port);
                *dst     = (u8)(val & 0xFF);
                *(dst+1) = (u8)(val >> 8);
                di = (u16)(di + (df ? -2 : 2));
            }
        }
        regs[V86_REG_EDI] = (regs[V86_REG_EDI] & 0xFFFF0000UL) | di;
        if (is_rep) regs[V86_REG_ECX] = (regs[V86_REG_ECX] & 0xFFFF0000UL); /* ECX=0 */
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)(prefix_len + 1)) & 0xFFFF;
        goto v86_gp_end;
    }

    /* ================================================================== */
    /*  OUTSB/OUTSW (0x6E/0x6F) — 文字列I/O出力 + REP対応               */
    /*                                                                    */
    /*  OUTSB: OUT(DX) ← [DS:SI], SI += 1 (DF=0), ECX-- (REP時)       */
    /*  OUTSW: OUT(DX) ← [DS:SI]の16bit値                             */
    /* ================================================================== */
    if (opcode == 0x6E || opcode == 0x6F) {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        u16 si   = (u16)(regs[V86_REG_ESI] & 0xFFFF);
        u16 ds   = (u16)(regs[V86_REG_DS]  & 0xFFFF);
        u32 ecx  = is_rep ? (regs[V86_REG_ECX] & 0xFFFF) : 1;
        int width = (opcode == 0x6F) ? 2 : 1;
        int df = (regs[V86_REG_EFLAGS] & (1U << 10)) ? 1 : 0;
        u32 count;

        if (ecx == 0) ecx = 1;

        for (count = 0; count < ecx; count++) {
            u8 *src = v86_linear(ds, si);
            if (width == 1) {
                v86_out8_checked(port, *src);
                si = (u16)(si + (df ? -1 : 1));
            } else {
                u16 val = (u16)(*src) | ((u16)(*(src+1)) << 8);
                if (v86_outw_checked(port, val)) {
                    regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)(prefix_len + 1)) & 0xFFFF;
                    return 1;  /* リブート検知 */
                }
                si = (u16)(si + (df ? -2 : 2));
            }
        }
        regs[V86_REG_ESI] = (regs[V86_REG_ESI] & 0xFFFF0000UL) | si;
        if (is_rep) regs[V86_REG_ECX] = (regs[V86_REG_ECX] & 0xFFFF0000UL);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)(prefix_len + 1)) & 0xFFFF;
        goto v86_gp_end;
    }

    /* ================================================================== */
    /*  0x66 プレフィックス付き PUSHFD/POPFD 対応                           */
    /*                                                                    */
    /*  0x66 + 0x9C = PUSHFD (32bit EFLAGS push)                          */
    /*  0x66 + 0x9D = POPFD  (32bit EFLAGS pop)                           */
    /* ================================================================== */
    if (is_opsz32 && opcode == 0x9C) {
        /* PUSHFD: 32bit EFLAGSをプッシュ */
        u32 flags = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                  | (v86_virtual_if ? EFLAGS_IF : 0)
                  | (regs[V86_REG_EFLAGS] & 0x7FD5UL);
        /* 32bit push: ESP -= 4 */
        regs[V86_REG_ESP] = (regs[V86_REG_ESP] - 4) & 0xFFFF;
        {
            u32 *sp32 = (u32 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
            *sp32 = flags;
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)(prefix_len + 1)) & 0xFFFF;
        goto v86_gp_end;
    }
    if (is_opsz32 && opcode == 0x9D) {
        /* POPFD: 32bit EFLAGSをポップ */
        u32 flags;
        u32 *sp32 = (u32 *)v86_linear(regs[V86_REG_SS], regs[V86_REG_ESP]);
        flags = *sp32;
        regs[V86_REG_ESP] = (regs[V86_REG_ESP] + 4) & 0xFFFF;
        v86_virtual_if = (flags & EFLAGS_IF) ? EFLAGS_IF : 0;
        regs[V86_REG_EFLAGS] = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                              | (flags & 0x7FD5UL);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)(prefix_len + 1)) & 0xFFFF;
        goto v86_gp_end;
    }

    /* ================================================================== */
    /*  0x66 + IN/OUT: 32bit I/O — 16bit I/Oにダウングレードして処理       */
    /* ================================================================== */
    if (is_opsz32) {
        /* 0x66 + 0xE4/E5/E6/E7/EC/ED/EE/EF は各 caseの prefix_len 計算で受ける */
        /* is_opsz32 フラグは保持したまま下の switchに落ちる */
        (void)is_opsz32;  /* 不使用警告抑制: 0x66+IN/OUTは16bitバイト列と同じ幅で処理する */
    }
    (void)is_repne;  /* 現時点では REPNE 対応を必要とする命令なし */

    switch (opcode) {

    /* ================================================================ */
    /*  INT n (0xCD nn) — ソフトウェア割り込み                          */
    /*  V86内のIVT (0x0000:0x0000) を参照してハンドラに転送する         */
    /* ================================================================ */
    case 0xCD: {
        u8 intno = ip[1];

        /* デバッグ統計 */
        v86_int_count++;
        v86_last_int = intno;
        v86_last_cs = regs[V86_REG_CS];
        v86_last_ip = regs[V86_REG_EIP];

        /* タイムアウトチェックはGPハンドラ冒頭で実施済み */
        /* ============================================================ */
        /*  DOS終了割り込みの特殊処理                                   */
        /*  INT 20h (Terminate Program) → V86終了                      */
        /*  INT 21h AH=4Ch (Exit Process) → V86終了                    */
        /* ============================================================ */
        if (intno == 0x20) {
            /* INT 20h: DOS Terminate — V86モード終了 */
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            v86_request_exit(V86_EXIT_DOS_TERM);
            return 1;
        }
        if (intno == 0x21 &&
            ((regs[V86_REG_EAX] >> 8) & 0xFF) == 0x4C) {
            /* INT 21h AH=4Ch: Exit Process — V86モード終了 */
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            v86_request_exit(V86_EXIT_DOS_TERM);
            return 1;
        }

        /* ============================================================ */
        /*  PC-98 BIOS割り込みのエミュレーション                               */
        /*  INT 18h (Text/KB/GFX BIOS) → v86_bios_int18()              */
        /*  INT 29h (DOS 1文字高速出力) → v86_bios_int29()              */
        /* ============================================================ */
        if (intno == 0x18) {
            int rc = v86_bios_int18(regs);
            if (rc == -2) {
                /* EIP加算なし: INT命令を再実行 (キー入力待ち) */
                break;
            }
            if (rc >= 0) {
                /* 処理済み: EIPを進めてV86に戻る */
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
                if (rc == 1) return 1;  /* V86終了要求 */
                break;
            }
            /* rc == -1: 未実装 — IVT転送にフォールスルー */
        }
        if (intno == 0x29) {
            v86_bios_int29(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            break;
        }
        if (intno == 0x1C) {
            int rc = v86_bios_int1c(regs);
            if (rc >= 0) {
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
                break;
            }
            /* rc == -1: IVT転送にフォールスルー */
        }

        /* INT 11h (機器構成取得) */
        if (intno == 0x11) {
            v86_bios_int11(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            break;
        }
        /* INT 12h (メモリサイズ取得) */
        if (intno == 0x12) {
            v86_bios_int12(regs);
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            break;
        }

        /* INT 1Bh (ディスクBIOS) */
        if (intno == 0x1B) {
            int rc = v86_bios_int1b(regs);
            if (rc >= 0) {
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
                break;
            }
            /* rc == -1: IVT転送にフォールスルー */
        }

        /* 通常のINT: IVT参照してV86内ハンドラに転送 */
        {
            u32 *ivt = (u32 *)v86_linear(0, 0);
            u16 handler_off = (u16)(ivt[intno] & 0xFFFF);
            u16 handler_seg = (u16)(ivt[intno] >> 16);

            /* ============================================================ */
            /*  §3 IVTダミー検出: 0x0050:0x0000 (ダミーIRET)の場合は       */
            /*  ゲストに CF=1 + AH=0x86 (機能未サポート) を返す。           */
            /*  ゲストが「成功」と誤認するのを防ぐ。                        */
            /*                                                              */
            /*  ダミーIVTの判定: V86_IS_DUMMY_IVT() マクロを使用            */
            /*  (seg=0x0050, off=0x0000 の固定パターン)                     */
            /* ============================================================ */
            if (V86_IS_DUMMY_IVT(ivt[intno])) {
                /* CF=1, AH=0x86 (機能未サポート) でゲストに即復帰 */
                regs[V86_REG_EFLAGS] |= 1;   /* CF=1 */
                regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL)
                                  | (0x86UL << 8);
                regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
                /* シリアルログ (最初の10件のみ) */
                {
                    static int dummy_ivt_count = 0;
                    if (dummy_ivt_count < 10) {
                        extern void serial_puts(const char *s);
                        static const char hex[] = "0123456789ABCDEF";
                        char buf[32];
                        int p = 0;
                        const char *msg = "\r\n[V86] dummy IVT INT 0x";
                        int mi;
                        for (mi = 0; msg[mi]; mi++) buf[p++] = msg[mi];
                        buf[p++] = hex[(intno >> 4) & 0xF];
                        buf[p++] = hex[intno & 0xF];
                        buf[p++] = '\r'; buf[p++] = '\n'; buf[p] = '\0';
                        serial_puts(buf);
                        dummy_ivt_count++;
                    }
                }
                break;
            }

            /* V86スタックにフラグ/CS/IPをpush (リアルモードINTと同じ) */
            v86_push16(regs, (u16)(regs[V86_REG_EFLAGS] & 0xFFFF));
            v86_push16(regs, (u16)regs[V86_REG_CS]);
            v86_push16(regs, (u16)(regs[V86_REG_EIP] + (u32)prefix_len + 2));

            /* IVTのハンドラに転送 */
            regs[V86_REG_CS] = handler_seg;
            regs[V86_REG_EIP] = handler_off;

            /* 仮想IFをクリア (INTはIF=0にする) */
            v86_virtual_if = 0;
        }
        break;
    }

    /* ================================================================ */
    /*  CLI (0xFA) — 仮想IFクリア                                      */
    /* ================================================================ */
    case 0xFA:
        v86_virtual_if = 0;
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;

    /* ================================================================ */
    /*  STI (0xFB) — 仮想IFセット                                      */
    /* ================================================================ */
    case 0xFB:
        v86_virtual_if = EFLAGS_IF;
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;

    /* ================================================================ */
    /*  PUSHF (0x9C) — 仮想EFLAGSをpush                                */
    /* ================================================================ */
    case 0x9C: {
        u16 flags = (u16)((regs[V86_REG_EFLAGS] & 0xFFFF) | v86_virtual_if);
        v86_push16(regs, flags);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  POPF (0x9D) — 仮想EFLAGSをpop                                  */
    /* ================================================================ */
    case 0x9D: {
        u16 flags = v86_pop16(regs);
        /* IFビットは仮想フラグに反映 */
        v86_virtual_if = (flags & EFLAGS_IF) ? EFLAGS_IF : 0;
        /* EFLAGS下位16bit更新 (VM,IOPL等は変更しない) */
        regs[V86_REG_EFLAGS] = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                              | (flags & 0x7FD5UL);  /* 安全なビットのみ */
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IRET (0xCF) — V86内での割り込みリターン                        */
    /* ================================================================ */
    case 0xCF: {
        u16 new_ip = v86_pop16(regs);
        u16 new_cs = v86_pop16(regs);
        u16 new_flags = v86_pop16(regs);

        regs[V86_REG_EIP] = new_ip;
        regs[V86_REG_CS]  = new_cs;

        /* IFビットを仮想フラグに反映 */
        v86_virtual_if = (new_flags & EFLAGS_IF) ? EFLAGS_IF : 0;
        regs[V86_REG_EFLAGS] = (regs[V86_REG_EFLAGS] & 0xFFFF0000UL)
                              | (new_flags & 0x7FD5UL);
        break;
    }

    /* ================================================================ */
    /*  HLT (0xF4) — V86テスト中はV86モードを終了する                   */
    /* ================================================================ */
    case 0xF4:
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        if (v86_exit_request) {
            /* V86終了要求: 戻り値 1 で isr_stub.asm が復帰処理を行う */
            return 1;
        }
        /* HLTは無視 (NOP扱い) — 次の命令に進む */
        break;

    /* ================================================================ */
    /*  IN AL, imm8 (0xE4 pp) — I/Oポート入力 (即値)                   */
    /* ================================================================ */
    case 0xE4: {
        u8 port = ip[1];
        v86_io_stat_record(port);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL)
                           | v86_in8_checked(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT imm8, AL (0xE6 pp) — I/Oポート出力 (即値)                  */
    /* ================================================================ */
    case 0xE6: {
        u8 port = ip[1];
        u8 val = (u8)(regs[V86_REG_EAX] & 0xFF);
        v86_io_stat_record(port);
        /* ゲストからの自発的なV86終了要求 (脱出トラップ) */
        if (port == 0xFE) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            v86_request_exit(V86_EXIT_TRAP_PORT);
            return 1;
        }
        /* リセットポート検知 */
        if (v86_pic_is_reboot(port, val)) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            v86_request_exit(V86_EXIT_REBOOT);
            return 1;
        }
        v86_out8_checked(port, val);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AL, DX (0xEC) — I/Oポート入力 (DXで指定)                    */
    /* ================================================================ */
    case 0xEC: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        v86_io_stat_record(port);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFFFF00UL)
                           | v86_in8_checked(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT DX, AL (0xEE) — I/Oポート出力 (DXで指定)                   */
    /* ================================================================ */
    case 0xEE: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        u8 val = (u8)(regs[V86_REG_EAX] & 0xFF);
        v86_io_stat_record(port);
        /* ゲストからの自発的なV86終了要求 (脱出トラップ) */
        if (port == 0xFE) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
            v86_request_exit(V86_EXIT_TRAP_PORT);
            return 1;
        }
        if (v86_pic_is_reboot(port, val)) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
            v86_request_exit(V86_EXIT_REBOOT);
            return 1;
        }
        v86_out8_checked(port, val);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AX, imm8 (0xE5 pp) — 16bit I/O入力 (即値)                   */
    /*  PIC/PIT仮想化チェック付き                                       */
    /* ================================================================ */
    case 0xE5: {
        u16 port = (u16)ip[1];
        v86_io_stat_record(port);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | v86_inw_checked(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT imm8, AX (0xE7 pp) — 16bit I/O出力 (即値)                  */
    /*  PIC/PIT仮想化+リブート検知付き                                  */
    /* ================================================================ */
    case 0xE7: {
        u16 port = (u16)ip[1];
        v86_io_stat_record(port);
        if (v86_outw_checked(port, (u16)(regs[V86_REG_EAX] & 0xFFFF))) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
            return 1;  /* V86終了 (リブート検知) */
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 2) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  IN AX, DX (0xED) — 16bit I/O入力 (DXで指定)                    */
    /*  PIC/PIT仮想化チェック付き                                       */
    /* ================================================================ */
    case 0xED: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        v86_io_stat_record(port);
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF0000UL) | v86_inw_checked(port);
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  OUT DX, AX (0xEF) — 16bit I/O出力 (DXで指定)                   */
    /*  PIC/PIT仮想化+リブート検知付き                                  */
    /* ================================================================ */
    case 0xEF: {
        u16 port = (u16)(regs[V86_REG_EDX] & 0xFFFF);
        v86_io_stat_record(port);
        if (v86_outw_checked(port, (u16)(regs[V86_REG_EAX] & 0xFFFF))) {
            regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
            return 1;  /* V86終了 (リブート検知) */
        }
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + (u32)prefix_len + 1) & 0xFFFF;
        break;
    }

    /* ================================================================ */
    /*  未対応命令 — デバッグ用に停止                                   */
    /* ================================================================ */
    default: {
        /* 未対応オペコード: デバッグ情報を記録してV86を終了する */
        v86_last_int = opcode;
        v86_last_cs = regs[V86_REG_CS];
        v86_last_ip = regs[V86_REG_EIP];
        regs[V86_REG_EIP] = (regs[V86_REG_EIP] + 1) & 0xFFFF;

        /* シリアルログに出力 */
        {
            extern void serial_puts(const char *s);
            static const char hex[] = "0123456789ABCDEF";
            char buf[64];
            int p = 0;
            const char *prefix = "\r\n[V86] Unknown opcode 0x";
            int pi;
            for (pi = 0; prefix[pi]; pi++) buf[p++] = prefix[pi];
            buf[p++] = hex[(opcode >> 4) & 0xF];
            buf[p++] = hex[opcode & 0xF];
            buf[p++] = ' ';
            buf[p++] = 'a';
            buf[p++] = 't';
            buf[p++] = ' ';
            buf[p++] = hex[(v86_last_cs >> 12) & 0xF];
            buf[p++] = hex[(v86_last_cs >> 8) & 0xF];
            buf[p++] = hex[(v86_last_cs >> 4) & 0xF];
            buf[p++] = hex[v86_last_cs & 0xF];
            buf[p++] = ':';
            buf[p++] = hex[(v86_last_ip >> 12) & 0xF];
            buf[p++] = hex[(v86_last_ip >> 8) & 0xF];
            buf[p++] = hex[(v86_last_ip >> 4) & 0xF];
            buf[p++] = hex[v86_last_ip & 0xF];
            buf[p++] = '\r';
            buf[p++] = '\n';
            buf[p] = '\0';
            serial_puts(buf);
        }
        v86_request_exit(V86_EXIT_UNKNOWN_OP);
        return 1;
    }
    } /* switch */

v86_gp_end:
    /* 保留中の割り込みがあれば注入する */
    if (v86_virtual_if && v86_pending_irq) {
        if (v86_pending_irq & (1U << 0)) { /* IRQ0: タイマー (INT 08h) */
            u8 isr = v86_pic_get_isr(0);

            /* 処理中 (再帰) なら保留を維持 — IMRチェックは行わない */
            if (!(isr & 1)) {
                u32 *ivt = (u32 *)v86_linear(0, 0);
                u16 handler_off = (u16)(ivt[0x08] & 0xFFFF);
                u16 handler_seg = (u16)(ivt[0x08] >> 16);
                int is_dummy = V86_IS_DUMMY_IVT(ivt[0x08]);

                v86_irq0_inject_count++;
                if (is_dummy) v86_irq0_gp_skip_ivt++;

                v86_pending_irq &= ~(1U << 0);

                /* ISR bit0 をセット: 実ハンドラの場合のみ
                 * ダミーIVTの場合はISRセットしない — IRETで即座に戻り
                 * EOIが発行されずISRが残るとその後の全注入がブロックされる */
                if (!is_dummy) {
                    v86_pic_set_isr(0, isr | 1);
                }

                /* ゲストスタックにフレームをpushしてハンドラに転送 */
                v86_gp_inject_irq(regs, handler_seg, handler_off);
            } else {
                v86_irq0_gp_skip_isr++;
            }
        }
        /* §1.3 A案: IRQ1 (キーボード) は BDA直書き運用に振り切り。
         * kbd.c と v86_session.c の Auto-Typer が BDA に直接書くため、
         * 仮想 IRQ1 注入は不要 (v86_set_pending_irq(1) を呼ぶ経路が無い)。
         * デッドコードとして削除済み。 */
    }

    return 0;
}

/* ====================================================================== */
/*  v86_inject_irq — V86タスクに仮想割り込みをインジェクトする              */
/*                                                                          */
/*  IRQハンドラ (isr_stub.asm) からV86モード中に呼ばれる。                  */
/*  V86のスタックフレームを書き換え、IVT経由でV86内ハンドラに転送する。     */
/*                                                                          */
/*  regs: V86スタックフレーム (isr_stub.asm のPUSHAD + CPUフレーム)         */
/*        ただしIRQ用はV86_REG_*とオフセットが異なる場合がある。            */
/*        ここではtimer/kbd共通の簡易方式を使う。                           */
/*                                                                          */
/*  intno: リフレクト先のINT番号 (08h=タイマ, 09h=キーボード)              */
/*                                                                          */
/*  ★注意: この関数は直接regs[]を操作せず、IVTアドレスとフラグだけを        */
/*  書き換える軽量な実装。irq_stub側でV86スタック操作を行う。               */
/* ====================================================================== */
void v86_set_pending_irq(int irq_no)
{
    v86_pending_irq |= (1U << irq_no);
}

/* ====================================================================== */
/*  v86_inject_timer_irq — タイマ割り込み(IRQ0)直接注入                  */
/*                                                                        */
/*  timer_handlerから呼ばれ、V86タスクが実行中であればハードウェア割り込み  */
/*  のスタックフレームを直接書き換えてINT 08hハンドラへジャンプさせる。      */
/* ====================================================================== */

/* hardware IRQ0 stack offsets (from irq_stub_0) */
#define HWIRQ_REG_EIP    10
#define HWIRQ_REG_CS     11
#define HWIRQ_REG_EFLAGS 12
#define HWIRQ_REG_ESP    13
#define HWIRQ_REG_SS     14

/* ====================================================================== */
/*  HW版 IRQ注入ヘルパー                                                   */
/*  HWIRQ_REG_* インデックスを使用し、直接スタック操作する                    */
/* ====================================================================== */
static void v86_hw_inject_irq(u32 *regs, u16 handler_seg, u16 handler_off)
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

/* IRQ0注入デバッグカウンタ */
u32 v86_irq0_call_count = 0;    /* v86_inject_timer_irq 呼び出し回数 */
u32 v86_irq0_nonvm_count = 0;   /* 非V86モードでスキップ */
u32 v86_irq0_noif_count = 0;    /* IF=0 で保留 */
u32 v86_irq0_isr_count = 0;     /* ISR処理中で保留 */
u32 v86_irq0_ivt_count = 0;     /* IVTダミーで保留 */
u32 v86_irq0_gp_inject_count = 0; /* GPハンドラ保留注入で実際に注入 */
u32 v86_irq0_gp_skip_if = 0;    /* GPハンドラ保留注入: IF=0でスキップ */
u32 v86_irq0_gp_skip_isr = 0;   /* GPハンドラ保留注入: ISR処理中でスキップ */
u32 v86_irq0_gp_skip_ivt = 0;   /* GPハンドラ保留注入: IVTダミーでスキップ */

void v86_inject_timer_irq(u32 *regs)
{
    u32 irq_divisor;
    v86_irq0_call_count++;

    /* V86モードからの割り込みか確認 */
    if ((regs[HWIRQ_REG_EFLAGS] & EFLAGS_VM) == 0) {
        v86_irq0_nonvm_count++;
        return;
    }

    /* ================================================================ */
    /*  §5 PITタイマレート反映 (分周比チェック)                          */
    /*                                                                  */
    /*  ゲストが Counter#0 の reload_value を OS32ベースレート(0x4E00=100Hz) */
    /*  より大きい値に変更した場合、低い頻度で IRQ0 を注入する。   */
    /*  divisor=1: 毎100Hzティック注入 (OS32デフォルト)               */
    /*  divisor=2: 2ティックに1回 = 50Hz                                */
    /* ================================================================ */
    irq_divisor = v86_pit_get_irq_divisor();
    if (irq_divisor > 1 && (v86_irq0_call_count % irq_divisor) != 0) {
        /* 分周スキップ: Auto-Typerとセッションtick処理は行う */
        v86_session_on_tick();
        return;
    }

    /* ================================================================ */
    /*  Auto-Typer (自動キー入力)                                       */
    /*  VDOS起動時に指定されたコマンド文字列をBDAキーボードバッファに   */
    /*  徐々に流し込む                                                  */
    /* ================================================================ */
    /* Auto-Typer + 強制脱出ホットキー (v86_session.c に委譲) */
    v86_session_on_tick();

    /* ================================================================ */
    /*  タイムアウト検出: GPハンドラが呼ばれない状況でも確実にV86終了   */
    /*                                                                  */
    /*  V86ゲストが通常命令(MOV/CMP/JMP等)だけのループに入った場合、   */
    /*  GPは発生せずGPハンドラ内のタイムアウトは実行されない。          */
    /*  IRQ0は100Hzで必ず発火するため、ここでタイムアウトを検出する。  */
    /*                                                                  */
    /*  方式: ゲストのCS:EIPを強制的にHLT命令(0x0050:0x0001)に設定。  */
    /*  次のIRETDでV86に戻るとHLTが実行され、GPハンドラが呼ばれて     */
    /*  v86_exit_requestにより安全にV86を終了する。                    */
    /* ================================================================ */
    if (V86_TIMEOUT_TICKS &&
        ((tick_count - v86_start_tick) > V86_TIMEOUT_TICKS ||
         v86_gp_count > 5000000)) {
        /* タイムアウト時の実CS:EIP (HLT注入前の位置) を記録 */
        v86_timeout_cs = regs[HWIRQ_REG_CS];
        v86_timeout_ip = regs[HWIRQ_REG_EIP];
        v86_last_cs = regs[HWIRQ_REG_CS];
        v86_last_ip = regs[HWIRQ_REG_EIP];

        /* HLT命令をバッキングRAMの0x501に配置 (0x0050:0x0001) */
        {
            u8 *hlt_ptr = v86_linear(0x0050, 0x0001);
            *hlt_ptr = 0xF4;  /* HLT */
        }

        /* V86終了要求フラグをセット */
        v86_exit_request = 1;

        /* ゲストのCS:EIPを強制的にHLT命令に書き換え */
        regs[HWIRQ_REG_CS] = 0x0050;
        regs[HWIRQ_REG_EIP] = 0x0001;
        return;
    }

    if (v86_virtual_if) {
        u8 isr = v86_pic_get_isr(0);
        u32 *ivt;
        int is_dummy_ivt;
        u16 handler_off, handler_seg;

        /* ISRで処理中なら保留する */
        if (isr & 1) {
            v86_irq0_isr_count++;
            v86_set_pending_irq(0);
            return;
        }

        /* IVTの状態を確認 (ダミーかどうか記録するが、ブロックはしない) */
        ivt = (u32 *)v86_linear(0, 0);
        is_dummy_ivt = V86_IS_DUMMY_IVT(ivt[0x08]);
        if (is_dummy_ivt) {
            v86_irq0_ivt_count++;
        }

        /* ISRにビットを立てる: 実ハンドラの場合のみ (EOI待ち)
         * ダミーIVTの場合はISRセットしない — IRETで即座に戻るため
         * ISRが残るとその後の全注入がブロックされてしまう */
        if (!is_dummy_ivt) {
            v86_pic_set_isr(0, isr | 1);
        }

        handler_off = (u16)(ivt[0x08] & 0xFFFF);
        handler_seg = (u16)(ivt[0x08] >> 16);

        v86_irq0_inject_count++;

        /* 仮想IFクリア */
        v86_virtual_if = 0;
        v86_pending_irq &= ~(1U << 0);

        /* ゲストスタックにフレームをpushしてハンドラに転送 */
        v86_hw_inject_irq(regs, handler_seg, handler_off);
    } else {
        /* Defer interrupt if IF=0 */
        v86_irq0_noif_count++;
        v86_set_pending_irq(0);
    }
    /* §1.3 A案: IRQ1 (キーボード, INT 09h) の HW注入を廃止。
     * BDA 直書き運用に統一したため、ここでの IRQ1 注入は不要。
     * v86_set_pending_irq(1) を呼ぶ経路が存在しないためデッドコードだった。 */
}

