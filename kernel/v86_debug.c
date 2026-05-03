/* ======================================================================== */
/*  V86_DEBUG.C - V86 デバッグ機能                                          */
/*                                                                          */
/*  V86セッション終了後のデバッグダンプ、ログファイル書き出しを行う。       */
/*  v86_debug_enabled フラグで有効/無効を制御する。                         */
/*                                                                          */
/*  元々 v86_test.c に散在していたデバッグコードを分離・集約したもの。      */
/* ======================================================================== */

#include "v86_debug.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_disk.h"
#include "v86_session.h"
#include "vfs.h"
#include "kprintf.h"
#include "io.h"

int v86_debug_enabled = 0;

/* シリアルヘルパー */
extern void serial_puts(const char *s);
extern void serial_putchar(char c);

void v86_dbg_hex8(u8 val)
{
    const char *hex = "0123456789ABCDEF";
    serial_putchar(hex[val >> 4]);
    serial_putchar(hex[val & 0xF]);
}

void v86_dbg_hex16(u16 val)
{
    v86_dbg_hex8((u8)(val >> 8));
    v86_dbg_hex8((u8)(val & 0xFF));
}

void v86_dbg_hex32(u32 val)
{
    v86_dbg_hex16((u16)(val >> 16));
    v86_dbg_hex16((u16)(val & 0xFFFF));
}

/* ====================================================================== */
/*  シリアルダンプ: V86統計情報                                             */
/* ====================================================================== */
static void v86_debug_dump_serial(void)
{
    extern volatile u32 tick_count;

    /* V86 END 統計 */
    serial_puts("\r\n[V86 END] ints=");
    v86_dbg_hex32(v86_int_count);
    serial_puts(" gp=");
    v86_dbg_hex32(v86_gp_count);
    serial_puts(" last=0x");
    v86_dbg_hex8((u8)v86_last_int);
    serial_puts(" at ");
    v86_dbg_hex16((u16)v86_last_cs);
    serial_puts(":");
    v86_dbg_hex16((u16)v86_last_ip);
    serial_puts("\r\n");

    /* タイムアウト位置 */
    {
        u8 *timeout_addr;
        int di;
        serial_puts("[V86 TIMEOUT] real CS:IP=");
        v86_dbg_hex16((u16)v86_timeout_cs);
        serial_puts(":");
        v86_dbg_hex16((u16)v86_timeout_ip);
        if (v86_timeout_cs || v86_timeout_ip) {
            serial_puts(" opcodes=");
            timeout_addr = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
            for (di = 0; di < 16; di++) {
                v86_dbg_hex8(timeout_addr[di]);
                serial_puts(" ");
            }
        }
        serial_puts("\r\n");
    }

    /* メモリダンプ */
    {
        u8 *p;
        int di;
        serial_puts("[MEM] 0060:0000=");
        p = v86_phys_addr(0x0060, 0x0000);
        for (di = 0; di < 32; di++) { v86_dbg_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
        serial_puts("[MEM] 1E00:0000=");
        p = v86_phys_addr(0x1E00, 0x0000);
        for (di = 0; di < 32; di++) { v86_dbg_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
    }

    /* ディスクログ */
    {
        u32 n, i, start;
        u32 total_count, log_idx;
        struct v86_disk_log_entry *log = v86_disk_get_log(&total_count, &log_idx);

        n = (total_count < 64) ? total_count : 64;
        serial_puts("[V86 DISK] total calls=");
        v86_dbg_hex32(total_count);
        serial_puts(" showing last ");
        v86_dbg_hex8((u8)n);
        serial_puts("\r\n");

        if (n > 0) {
            start = (total_count <= 64) ? 0 : log_idx;
            for (i = 0; i < n; i++) {
                u32 idx = (start + i) % 64;
                struct v86_disk_log_entry *e = &log[idx];
                serial_puts("  AH="); v86_dbg_hex8(e->func);
                serial_puts(" C="); v86_dbg_hex8(e->cylinder);
                serial_puts(" H="); v86_dbg_hex8(e->head);
                serial_puts(" S="); v86_dbg_hex8(e->sector);
                serial_puts(" BX="); v86_dbg_hex16(e->xfer_bytes);
                serial_puts(" ES:BP="); v86_dbg_hex16(e->es);
                serial_puts(":"); v86_dbg_hex16(e->bp);
                serial_puts(" st="); v86_dbg_hex8(e->status);
                serial_puts("\r\n");
            }
        }
    }

    /* I/O統計 */
    v86_dump_io_stats();

    /* PIC最終状態 */
    serial_puts("[V86 PIC] M.IMR=");
    v86_dbg_hex8(v86_pic_get_imr(0));
    serial_puts(" M.ISR=");
    v86_dbg_hex8(v86_pic_get_isr(0));
    serial_puts(" S.IMR=");
    v86_dbg_hex8(v86_pic_get_imr(1));
    serial_puts(" S.ISR=");
    v86_dbg_hex8(v86_pic_get_isr(1));
    serial_puts(" irq0=");
    v86_dbg_hex32(v86_irq0_inject_count);
    serial_puts(" eoi0=");
    v86_dbg_hex32(v86_pic_get_eoi_count(0));
    serial_puts(" eoi1=");
    v86_dbg_hex32(v86_pic_get_eoi_count(1));
    serial_puts("\r\n");

    /* IRQ0注入カウンタ */
    serial_puts("[V86 IRQ0] call=");
    v86_dbg_hex32(v86_irq0_call_count);
    serial_puts(" nonvm=");
    v86_dbg_hex32(v86_irq0_nonvm_count);
    serial_puts(" noif=");
    v86_dbg_hex32(v86_irq0_noif_count);
    serial_puts(" isr=");
    v86_dbg_hex32(v86_irq0_isr_count);
    serial_puts(" ivt=");
    v86_dbg_hex32(v86_irq0_ivt_count);
    serial_puts("\r\n");
    serial_puts("[V86 IRQ0 GP] inject=");
    v86_dbg_hex32(v86_irq0_gp_inject_count);
    serial_puts(" skip_isr=");
    v86_dbg_hex32(v86_irq0_gp_skip_isr);
    serial_puts(" skip_ivt=");
    v86_dbg_hex32(v86_irq0_gp_skip_ivt);
    serial_puts("\r\n");
}

/* ====================================================================== */
/*  ファイル書き出し用バッファ追記ヘルパー                                  */
/* ====================================================================== */
static char fbuf[4096];
static int fbuf_pos;

static void fbuf_str(const char *s)
{
    while (*s && fbuf_pos < (int)sizeof(fbuf) - 1)
        fbuf[fbuf_pos++] = *s++;
}

static void fbuf_hex8(u8 val)
{
    const char *hx = "0123456789ABCDEF";
    if (fbuf_pos < (int)sizeof(fbuf) - 2) {
        fbuf[fbuf_pos++] = hx[val >> 4];
        fbuf[fbuf_pos++] = hx[val & 0xF];
    }
}

static void fbuf_hex16(u16 val)
{
    fbuf_hex8((u8)(val >> 8));
    fbuf_hex8((u8)(val & 0xFF));
}

static void fbuf_hex32(u32 val)
{
    fbuf_hex16((u16)(val >> 16));
    fbuf_hex16((u16)(val & 0xFFFF));
}

static void fbuf_dec(u32 val)
{
    char tb[12];
    int tp = 0;
    if (val == 0) {
        if (fbuf_pos < (int)sizeof(fbuf) - 1)
            fbuf[fbuf_pos++] = '0';
        return;
    }
    while (val > 0) {
        tb[tp++] = '0' + (val % 10);
        val /= 10;
    }
    while (tp > 0 && fbuf_pos < (int)sizeof(fbuf) - 1)
        fbuf[fbuf_pos++] = tb[--tp];
}

/* ====================================================================== */
/*  ファイル書き出し: v86_fdos_log.txt                                     */
/* ====================================================================== */
static void v86_debug_write_fdos_log(void)
{
    u32 total_count, log_idx_v;
    struct v86_disk_log_entry *dlog;
    u32 n, i, start;

    fbuf_pos = 0;

    /* ヘッダ行 */
    fbuf_str("[V86] ints="); fbuf_dec(v86_int_count);
    fbuf_str(" gp="); fbuf_dec(v86_gp_count);
    fbuf_str(" last="); fbuf_hex8((u8)v86_last_int);
    fbuf_str(" "); fbuf_hex16((u16)v86_last_cs);
    fbuf_str(":"); fbuf_hex16((u16)v86_last_ip);
    fbuf_str("\n");

    /* IRQ0注入カウンタ */
    fbuf_str("Inj:"); fbuf_hex16((u16)v86_irq0_inject_count);
    fbuf_str("\n");

    fbuf_str("IRQ0:call="); fbuf_hex32(v86_irq0_call_count);
    fbuf_str(" nonvm="); fbuf_hex32(v86_irq0_nonvm_count);
    fbuf_str(" noif="); fbuf_hex32(v86_irq0_noif_count);
    fbuf_str(" isr="); fbuf_hex32(v86_irq0_isr_count);
    fbuf_str(" ivt="); fbuf_hex32(v86_irq0_ivt_count);
    fbuf_str("\n");

    fbuf_str("GP:skip_isr="); fbuf_hex16((u16)v86_irq0_gp_skip_isr);
    fbuf_str(" skip_ivt="); fbuf_hex16((u16)v86_irq0_gp_skip_ivt);
    fbuf_str("\n");

    fbuf_str("PIC:M.IMR="); fbuf_hex8(v86_pic_get_imr(0));
    fbuf_str(" M.ISR="); fbuf_hex8(v86_pic_get_isr(0));
    fbuf_str(" eoi0="); fbuf_hex16((u16)v86_pic_get_eoi_count(0));
    fbuf_str("\n");

    /* タイムアウト位置 */
    if (v86_timeout_cs || v86_timeout_ip) {
        u8 *taddr;
        int di;
        fbuf_str("TOUT:"); fbuf_hex16((u16)v86_timeout_cs);
        fbuf_str(":"); fbuf_hex16((u16)v86_timeout_ip);
        fbuf_str(" ");
        taddr = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
        for (di = 0; di < 32 && fbuf_pos < 3800; di++) {
            fbuf_hex8(taddr[di]);
            fbuf_str(" ");
        }
        fbuf_str("\n");
    }

    /* ディスクログ */
    dlog = v86_disk_get_log(&total_count, &log_idx_v);
    n = (total_count < 64) ? total_count : 64;
    start = (total_count <= 64) ? 0 : log_idx_v;
    for (i = 0; i < n && fbuf_pos < 3900; i++) {
        u32 ix = (start + i) % 64;
        struct v86_disk_log_entry *e = &dlog[ix];
        fbuf_hex8(e->func); fbuf_str(" ");
        fbuf_str("C"); fbuf_hex8(e->cylinder); fbuf_str(" ");
        fbuf_str("H"); fbuf_hex8(e->head); fbuf_str(" ");
        fbuf_str("S"); fbuf_hex8(e->sector); fbuf_str(" ");
        fbuf_str("N"); fbuf_hex8(e->sector_len); fbuf_str(" ");
        fbuf_hex16(e->xfer_bytes); fbuf_str(" ");
        fbuf_hex16(e->es); fbuf_str(":");
        fbuf_hex16(e->bp); fbuf_str(" ");
        fbuf_hex8(e->status);
        fbuf_str("\n");
    }

    fbuf[fbuf_pos] = '\0';
    vfs_write("/host/v86_fdos_log.txt", fbuf, (u32)fbuf_pos);
}



/* ====================================================================== */
/*  ファイル書き出し: v86_gptrace.txt                                      */
/* ====================================================================== */
static void v86_debug_write_gptrace(void)
{
    /* v86_trace_entry はv86.hで定義済み */
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

/* ====================================================================== */
/*  統合ダンプ関数                                                         */
/* ====================================================================== */
void v86_debug_dump_session(void)
{
    if (!v86_debug_enabled) return;
    v86_debug_dump_serial();
    v86_debug_write_fdos_log();
    v86_debug_write_gptrace();
}
