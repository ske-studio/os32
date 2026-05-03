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

static void dbg_hex8(u8 val)
{
    const char *hex = "0123456789ABCDEF";
    serial_putchar(hex[val >> 4]);
    serial_putchar(hex[val & 0xF]);
}

static void dbg_hex16(u16 val)
{
    dbg_hex8((u8)(val >> 8));
    dbg_hex8((u8)(val & 0xFF));
}

static void dbg_hex32(u32 val)
{
    dbg_hex16((u16)(val >> 16));
    dbg_hex16((u16)(val & 0xFFFF));
}

/* ====================================================================== */
/*  シリアルダンプ: V86統計情報                                             */
/* ====================================================================== */
static void v86_debug_dump_serial(void)
{
    extern volatile u32 tick_count;

    /* V86 END 統計 */
    serial_puts("\r\n[V86 END] ints=");
    dbg_hex32(v86_int_count);
    serial_puts(" gp=");
    dbg_hex32(v86_gp_count);
    serial_puts(" last=0x");
    dbg_hex8((u8)v86_last_int);
    serial_puts(" at ");
    dbg_hex16((u16)v86_last_cs);
    serial_puts(":");
    dbg_hex16((u16)v86_last_ip);
    serial_puts("\r\n");

    /* タイムアウト位置 */
    {
        u8 *timeout_addr;
        int di;
        serial_puts("[V86 TIMEOUT] real CS:IP=");
        dbg_hex16((u16)v86_timeout_cs);
        serial_puts(":");
        dbg_hex16((u16)v86_timeout_ip);
        if (v86_timeout_cs || v86_timeout_ip) {
            serial_puts(" opcodes=");
            timeout_addr = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
            for (di = 0; di < 16; di++) {
                dbg_hex8(timeout_addr[di]);
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
        for (di = 0; di < 32; di++) { dbg_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
        serial_puts("[MEM] 1E00:0000=");
        p = v86_phys_addr(0x1E00, 0x0000);
        for (di = 0; di < 32; di++) { dbg_hex8(p[di]); serial_puts(" "); }
        serial_puts("\r\n");
    }

    /* ディスクログ */
    {
        u32 n, i, start;
        u32 total_count, log_idx;
        struct v86_disk_log_entry *log = v86_disk_get_log(&total_count, &log_idx);

        n = (total_count < 64) ? total_count : 64;
        serial_puts("[V86 DISK] total calls=");
        dbg_hex32(total_count);
        serial_puts(" showing last ");
        dbg_hex8((u8)n);
        serial_puts("\r\n");

        if (n > 0) {
            start = (total_count <= 64) ? 0 : log_idx;
            for (i = 0; i < n; i++) {
                u32 idx = (start + i) % 64;
                struct v86_disk_log_entry *e = &log[idx];
                serial_puts("  AH="); dbg_hex8(e->func);
                serial_puts(" C="); dbg_hex8(e->cylinder);
                serial_puts(" H="); dbg_hex8(e->head);
                serial_puts(" S="); dbg_hex8(e->sector);
                serial_puts(" BX="); dbg_hex16(e->xfer_bytes);
                serial_puts(" ES:BP="); dbg_hex16(e->es);
                serial_puts(":"); dbg_hex16(e->bp);
                serial_puts(" st="); dbg_hex8(e->status);
                serial_puts("\r\n");
            }
        }
    }

    /* I/O統計 */
    v86_dump_io_stats();

    /* PIC最終状態 */
    serial_puts("[V86 PIC] M.IMR=");
    dbg_hex8(v86_pic_get_imr(0));
    serial_puts(" M.ISR=");
    dbg_hex8(v86_pic_get_isr(0));
    serial_puts(" S.IMR=");
    dbg_hex8(v86_pic_get_imr(1));
    serial_puts(" S.ISR=");
    dbg_hex8(v86_pic_get_isr(1));
    serial_puts(" irq0=");
    dbg_hex32(v86_irq0_inject_count);
    serial_puts(" eoi0=");
    dbg_hex32(v86_pic_get_eoi_count(0));
    serial_puts(" eoi1=");
    dbg_hex32(v86_pic_get_eoi_count(1));
    serial_puts("\r\n");

    /* IRQ0注入カウンタ */
    serial_puts("[V86 IRQ0] call=");
    dbg_hex32(v86_irq0_call_count);
    serial_puts(" nonvm=");
    dbg_hex32(v86_irq0_nonvm_count);
    serial_puts(" noif=");
    dbg_hex32(v86_irq0_noif_count);
    serial_puts(" isr=");
    dbg_hex32(v86_irq0_isr_count);
    serial_puts(" ivt=");
    dbg_hex32(v86_irq0_ivt_count);
    serial_puts("\r\n");
    serial_puts("[V86 IRQ0 GP] inject=");
    dbg_hex32(v86_irq0_gp_inject_count);
    serial_puts(" skip_isr=");
    dbg_hex32(v86_irq0_gp_skip_isr);
    serial_puts(" skip_ivt=");
    dbg_hex32(v86_irq0_gp_skip_ivt);
    serial_puts("\r\n");
}

/* ====================================================================== */
/*  ファイル書き出し: v86_fdos_log.txt                                     */
/* ====================================================================== */
static void v86_debug_write_fdos_log(void)
{
    extern volatile u32 tick_count;

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
    fbuf[fp++] = hx[(v86_irq0_inject_count>>12)&0xF];
    fbuf[fp++] = hx[(v86_irq0_inject_count>>8)&0xF];
    fbuf[fp++] = hx[(v86_irq0_inject_count>>4)&0xF];
    fbuf[fp++] = hx[v86_irq0_inject_count&0xF];
    fbuf[fp++] = '\n';

    /* IRQ0カウンタ */
    {
        const char *s;
        int si;
        s = "IRQ0:call="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_call_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>24)&0xF];
        fbuf[fp++]=hx[(v86_irq0_call_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>16)&0xF];
        fbuf[fp++]=hx[(v86_irq0_call_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_call_count>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_call_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_call_count&0xF];
        s = " nonvm="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_nonvm_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>24)&0xF];
        fbuf[fp++]=hx[(v86_irq0_nonvm_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>16)&0xF];
        fbuf[fp++]=hx[(v86_irq0_nonvm_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_nonvm_count>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_nonvm_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_nonvm_count&0xF];
        s = " noif="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_noif_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>24)&0xF];
        fbuf[fp++]=hx[(v86_irq0_noif_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>16)&0xF];
        fbuf[fp++]=hx[(v86_irq0_noif_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_noif_count>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_noif_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_noif_count&0xF];
        s = " isr="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_isr_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>24)&0xF];
        fbuf[fp++]=hx[(v86_irq0_isr_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>16)&0xF];
        fbuf[fp++]=hx[(v86_irq0_isr_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_isr_count>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_isr_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_isr_count&0xF];
        s = " ivt="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_ivt_count>>28)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>24)&0xF];
        fbuf[fp++]=hx[(v86_irq0_ivt_count>>20)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>16)&0xF];
        fbuf[fp++]=hx[(v86_irq0_ivt_count>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_ivt_count>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_ivt_count>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_ivt_count&0xF];
        fbuf[fp++] = '\n';
        s = "GP:skip_isr="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_gp_skip_isr>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_gp_skip_isr&0xF];
        s = " skip_ivt="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>12)&0xF]; fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>8)&0xF];
        fbuf[fp++]=hx[(v86_irq0_gp_skip_ivt>>4)&0xF]; fbuf[fp++]=hx[v86_irq0_gp_skip_ivt&0xF];
        fbuf[fp++] = '\n';
        s = "PIC:M.IMR="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[v86_pic_get_imr(0)>>4]; fbuf[fp++]=hx[v86_pic_get_imr(0)&0xF];
        s = " M.ISR="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[v86_pic_get_isr(0)>>4]; fbuf[fp++]=hx[v86_pic_get_isr(0)&0xF];
        s = " eoi0="; for(si=0;s[si];si++)fbuf[fp++]=s[si];
        fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>12)&0xF]; fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>8)&0xF];
        fbuf[fp++]=hx[(v86_pic_get_eoi_count(0)>>4)&0xF]; fbuf[fp++]=hx[v86_pic_get_eoi_count(0)&0xF];
        fbuf[fp++] = '\n';
    }

    /* タイムアウト位置 */
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
        fbuf[fp++] = '\n'; fbuf[fp++] = '\n';
    }

    fbuf[fp] = '\0';
    vfs_write("/host/v86_fdos_log.txt", fbuf, (u32)fp);
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
