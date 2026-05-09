/* ======================================================================== */
/*  V86_DEBUG.C - V86 体系化デバッグログ                                    */
/*                                                                          */
/*  /host/debug/v86_YYMMDD_HHMMSS.log にセクション構造化ログを出力する。   */
/*  セッション開始時にヘッダを即時書き込み、終了時に残セクションを追記。    */
/*  古いログは最新10件を残して自動パージする。                              */
/* ======================================================================== */

#include "v86_debug.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_disk.h"
#include "v86_session.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_vsync.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "io.h"
#include "rtc.h"
#include "paging.h"

int v86_debug_enabled = 0;
int v86_debug_serial_enabled = 1;

/* シリアルヘルパー */
extern void serial_puts(const char *s);
extern void serial_putchar(char c);

/* I/O統計 (v86.c) */
extern u32 v86_io_stat_count;
extern struct v86_io_stat { u16 port; u32 count; } v86_io_stats[];

/* tick_count (isr_stub.asm) */
extern volatile u32 tick_count;

/* ====================================================================== */
/*  ログファイルパス管理                                                    */
/* ====================================================================== */
#define V86_LOG_PATH  "/host/debug/v86_diag.log"
#define V86_LOG_MAX   10
static char log_path[64];

static void build_log_path(void)
{
    kstrncpy(log_path, V86_LOG_PATH, sizeof(log_path));
}

/* ====================================================================== */
/*  バッファ書き込みインフラ                                                */
/* ====================================================================== */
#define WBUF_SIZE 4096
static char wbuf[WBUF_SIZE];
static int  wpos;

static void wb_reset(void) { wpos = 0; }

static void wb_ch(char c)
{
    if (wpos < WBUF_SIZE - 1) wbuf[wpos++] = c;
}

static void wb_str(const char *s)
{
    while (*s && wpos < WBUF_SIZE - 1) wbuf[wpos++] = *s++;
}

static void wb_hex8(u8 v)
{
    const char *h = "0123456789ABCDEF";
    wb_ch(h[v >> 4]); wb_ch(h[v & 0xF]);
}

static void wb_hex16(u16 v) { wb_hex8((u8)(v>>8)); wb_hex8((u8)v); }
static void wb_hex32(u32 v) { wb_hex16((u16)(v>>16)); wb_hex16((u16)v); }

static void wb_dec(u32 v)
{
    char tb[12]; int tp = 0;
    if (v == 0) { wb_ch('0'); return; }
    while (v > 0) { tb[tp++] = '0' + (v % 10); v /= 10; }
    while (tp > 0) wb_ch(tb[--tp]);
}

static void wb_pad(int width, int used)
{
    while (used < width) { wb_ch(' '); used++; }
}

static void wb_nl(void) { wb_ch('\n'); }

/* バッファをファイルに追記してリセット */
static int log_fd = -1;

static void log_open(void)
{
    if (log_path[0] && log_fd < 0) {
        log_fd = vfs_open(log_path, O_WRONLY | O_CREAT | O_TRUNC);
    }
}

static void log_close(void)
{
    if (log_fd >= 0) {
        vfs_close(log_fd);
        log_fd = -1;
    }
}

static void wb_flush(void)
{
    if (wpos > 0 && log_fd >= 0) {
        vfs_write_fd(log_fd, wbuf, (u32)wpos);
    }
    wpos = 0;
}

/* セパレータ行 */
static void wb_separator(void)
{
    int i;
    for (i = 0; i < 80; i++) wb_ch('=');
    wb_nl();
}

/* ====================================================================== */
/*  シリアルダンプ (従来互換)                                               */
/* ====================================================================== */
void v86_dbg_hex8(u8 val)
{
    const char *hex = "0123456789ABCDEF";
    serial_putchar(hex[val >> 4]);
    serial_putchar(hex[val & 0xF]);
}
void v86_dbg_hex16(u16 val)
{
    v86_dbg_hex8((u8)(val >> 8)); v86_dbg_hex8((u8)(val & 0xFF));
}
void v86_dbg_hex32(u32 val)
{
    v86_dbg_hex16((u16)(val >> 16)); v86_dbg_hex16((u16)(val & 0xFFFF));
}

static void v86_debug_dump_serial(void)
{
    u32 n, i, start, total_count, log_idx;
    struct v86_disk_log_entry *log;

    serial_puts("\r\n[V86 END] ints=");
    v86_dbg_hex32(v86_int_count);
    serial_puts(" gp="); v86_dbg_hex32(v86_gp_count);
    serial_puts(" last=0x"); v86_dbg_hex8((u8)v86_last_int);
    serial_puts(" at "); v86_dbg_hex16((u16)v86_last_cs);
    serial_puts(":"); v86_dbg_hex16((u16)v86_last_ip);
    serial_puts("\r\n");

    serial_puts("[V86 PIC] M.IMR="); v86_dbg_hex8(v86_pic_get_imr(0));
    serial_puts(" S.IMR="); v86_dbg_hex8(v86_pic_get_imr(1));
    serial_puts(" irq0="); v86_dbg_hex32(v86_irq0_inject_count);
    serial_puts("\r\n");

    log = v86_disk_get_log(&total_count, &log_idx);
    n = (total_count < 64) ? total_count : 64;
    serial_puts("[V86 DISK] total=");
    v86_dbg_hex32(total_count); serial_puts("\r\n");
    if (n > 0) {
        start = (total_count <= 64) ? 0 : log_idx;
        for (i = 0; i < n; i++) {
            u32 idx = (start + i) % 64;
            struct v86_disk_log_entry *e = &log[idx];
            serial_puts("  AH="); v86_dbg_hex8(e->func);
            serial_puts(" C="); v86_dbg_hex8(e->cylinder);
            serial_puts(" H="); v86_dbg_hex8(e->head);
            serial_puts(" S="); v86_dbg_hex8(e->sector);
            serial_puts(" st="); v86_dbg_hex8(e->status);
            serial_puts("\r\n");
        }
    }
    v86_dump_io_stats();
}

/* ====================================================================== */
/*  セクション書き込み関数群                                                */
/* ====================================================================== */

/* [SESSION] ヘッダ — セッション開始時に即時書き込み */
void v86_debug_write_header(const char *boot_mode,
                            const char *image_path,
                            const char *auto_cmd)
{
    if (!v86_debug_enabled) return;

    /* ディレクトリ確保 + ファイル名生成 + ファイルオープン */
    vfs_mkdir("/host/debug");
    build_log_path();
    log_open();

    /* ファイルログ有効時はシリアルダンプを無効化 */
    v86_debug_serial_enabled = 0;

    wb_reset();
    wb_separator();
    wb_str("V86 DEBUG LOG\n");
    {
        RTC_Time t;
        rtc_read(&t);
        wb_str("Generated: 20");
        wb_dec((u32)t.year); wb_ch('-');
        if (t.month < 10) { wb_ch('0'); } wb_dec((u32)t.month); wb_ch('-');
        if (t.day < 10) { wb_ch('0'); } wb_dec((u32)t.day); wb_ch(' ');
        if (t.hour < 10) { wb_ch('0'); } wb_dec((u32)t.hour); wb_ch(':');
        if (t.min < 10) { wb_ch('0'); } wb_dec((u32)t.min); wb_ch(':');
        if (t.sec < 10) { wb_ch('0'); } wb_dec((u32)t.sec);
        wb_nl();
    }
    wb_separator();
    wb_nl();
    wb_str("[SESSION]\n");
    wb_str("  Boot Mode     : "); wb_str(boot_mode); wb_nl();
    wb_str("  Disk Image    : "); wb_str(image_path ? image_path : "N/A"); wb_nl();
    wb_str("  Auto Command  : "); wb_str(auto_cmd ? auto_cmd : "none"); wb_nl();
    wb_str("  Start Tick    : "); wb_dec(v86_start_tick); wb_nl();
    wb_str("  Timeout       : "); wb_dec(v86_timeout_ticks);
    wb_str(" ticks"); wb_nl();
    wb_str("  Native Mode   : ");
    wb_str(v86_native_mode ? "Yes" : "No"); wb_nl();
    wb_nl();
    wb_flush();
}

/* [EXIT] + [GP HANDLER] + [IRQ0] */
static void write_section_exit(void)
{
    u32 duration = tick_count - v86_start_tick;

    wb_reset();
    wb_str("[EXIT]\n");
    wb_str("  Reason        : ");
    wb_str(v86_exit_reason_str(V86_EXIT_NONE)); /* プレースホルダ — 下で上書き */
    /* 実際の終了理由は v86_session.c から取れないため、統計から推定 */
    wb_nl();
    wb_str("  Duration      : "); wb_dec(duration);
    wb_str(" ticks ("); wb_dec(duration / 100);
    wb_str("."); wb_dec((duration % 100) / 10); wb_str("s)"); wb_nl();
    wb_str("  Final CS:IP   : ");
    wb_hex16((u16)v86_last_cs); wb_ch(':');
    wb_hex16((u16)v86_last_ip); wb_nl();
    wb_nl();

    wb_separator();
    wb_str("EXECUTION STATISTICS\n");
    wb_separator();
    wb_nl();

    wb_str("[GP HANDLER]\n");
    wb_str("  Total #GP     : "); wb_dec(v86_gp_count); wb_nl();
    wb_str("  Total INTs    : "); wb_dec(v86_int_count); wb_nl();
    wb_str("  Last INT      : 0x");
    wb_hex8((u8)v86_last_int); wb_str(" at ");
    wb_hex16((u16)v86_last_cs); wb_ch(':');
    wb_hex16((u16)v86_last_ip); wb_nl();
    wb_nl();

    wb_str("[IRQ0 INJECTION]\n");
    wb_str("  call          : "); wb_dec(v86_irq0_call_count); wb_nl();
    wb_str("  non-vm        : "); wb_dec(v86_irq0_nonvm_count); wb_nl();
    wb_str("  no-if         : "); wb_dec(v86_irq0_noif_count); wb_nl();
    wb_str("  isr-pend      : "); wb_dec(v86_irq0_isr_count); wb_nl();
    wb_str("  ivt-forward   : "); wb_dec(v86_irq0_ivt_count); wb_nl();
    wb_str("  gp-inject     : "); wb_dec(v86_irq0_gp_inject_count); wb_nl();
    wb_str("  gp-skip-isr   : "); wb_dec(v86_irq0_gp_skip_isr); wb_nl();
    wb_str("  gp-skip-ivt   : "); wb_dec(v86_irq0_gp_skip_ivt); wb_nl();
    wb_nl();

    wb_str("[VSYNC]\n");
    wb_str("  arm (OUT 64h) : "); wb_dec(v86_vsync_arm_count); wb_nl();
    wb_str("  inject (0Ah)  : "); wb_dec(v86_vsync_inject_count); wb_nl();
    wb_nl();
    wb_flush();
}

/* [PIC] + [PIT] */
static void write_section_hw(void)
{
    wb_reset();
    wb_separator();
    wb_str("VIRTUAL HARDWARE STATE\n");
    wb_separator();
    wb_nl();

    wb_str("[PIC (8259A)]\n");
    wb_str("                   Master    Slave\n");
    wb_str("  IMR            : ");
    wb_hex8(v86_pic_get_imr(0));
    wb_str("        ");
    wb_hex8(v86_pic_get_imr(1)); wb_nl();
    wb_str("  ISR            : ");
    wb_hex8(v86_pic_get_isr(0));
    wb_str("        ");
    wb_hex8(v86_pic_get_isr(1)); wb_nl();
    wb_str("  IRR            : ");
    wb_hex8(v86_pic_get_irr(0));
    wb_str("        ");
    wb_hex8(v86_pic_get_irr(1)); wb_nl();
    wb_str("  EOI count      : ");
    wb_dec(v86_pic_get_eoi_count(0));
    wb_pad(10, 0); /* おおよそ */
    wb_dec(v86_pic_get_eoi_count(1)); wb_nl();
    wb_nl();

    wb_str("[PIT (8253A)]\n");
    wb_str("  IRQ divisor    : "); wb_dec(v86_pit_get_irq_divisor()); wb_nl();
    wb_nl();
    wb_flush();
}

/* [I/O PORT ACCESS LOG] */
static void write_section_io(void)
{
    u32 i;
    wb_reset();
    wb_separator();
    wb_str("I/O PORT ACCESS LOG\n");
    wb_separator();
    wb_nl();
    wb_str("  Port   Count\n");
    wb_str("  ----   -----\n");
    for (i = 0; i < v86_io_stat_count && i < 16; i++) {
        wb_str("  ");
        wb_hex16(v86_io_stats[i].port);
        wb_str("h  ");
        wb_dec(v86_io_stats[i].count);
        wb_nl();
        if (wpos > WBUF_SIZE - 64) wb_flush();
    }
    wb_nl();
    wb_flush();
}

/* [DISK I/O LOG] */
static void write_section_disk(void)
{
    u32 total_count, log_idx, n, i, start;
    struct v86_disk_log_entry *dlog;

    wb_reset();
    wb_separator();
    wb_str("DISK I/O LOG (INT 1Bh)\n");
    wb_separator();
    wb_nl();

    dlog = v86_disk_get_log(&total_count, &log_idx);
    wb_str("  Total calls: "); wb_dec(total_count);
    wb_str(" (showing last 64)\n\n");
    wb_str("  #   AH  Cyl  Hd  Sec  SLen  Xfer    ES:BP       St  Offset\n");
    wb_str("  --- --  ---  --  ---  ----  ------  ----------  --  ------\n");

    n = (total_count < 64) ? total_count : 64;
    start = (total_count <= 64) ? 0 : log_idx;
    for (i = 0; i < n; i++) {
        u32 idx = (start + i) % 64;
        struct v86_disk_log_entry *e = &dlog[idx];
        wb_str("  ");
        if (i < 100) { if (i < 10) wb_ch('0'); }
        wb_dec(i + 1); wb_str("  ");
        wb_hex8(e->func); wb_str("  ");
        if (e->cylinder < 10) wb_ch(' ');
        wb_dec((u32)e->cylinder); wb_str("   ");
        wb_dec((u32)e->head); wb_str("    ");
        if (e->sector < 10) wb_ch(' ');
        wb_dec((u32)e->sector); wb_str("   ");
        wb_hex8(e->sector_len); wb_str("    ");
        wb_hex16(e->xfer_bytes); wb_str("    ");
        wb_hex16(e->es); wb_ch(':');
        wb_hex16(e->bp); wb_str("  ");
        wb_hex8(e->status); wb_str("  ");
        wb_hex32((u32)e->result_offset);
        wb_nl();
        if (wpos > WBUF_SIZE - 100) wb_flush();
    }
    wb_nl();
    wb_flush();
}

/* [GP TRACE] */
static void write_section_gptrace(void)
{
    u32 total, tidx, tn, ti, tstart;
    struct v86_trace_entry *tlog;

    wb_reset();
    wb_separator();
    wb_str("GP TRACE (last 128 entries)\n");
    wb_separator();
    wb_nl();

    tlog = v86_get_trace(&total, &tidx);
    tn = (total < 128) ? total : 128;
    tstart = (total <= 128) ? 0 : tidx;

    wb_str("  #     CS:IP       Op  INT#  AX    CX\n");
    wb_str("  ----  ----------  --  ----  ----  ----\n");

    for (ti = 0; ti < tn; ti++) {
        u32 tix = (tstart + ti) % 128;
        struct v86_trace_entry *te = &tlog[tix];
        wb_str("  ");
        if (ti < 1000) { if (ti < 100) { if (ti < 10) wb_ch('0'); wb_ch('0'); } }
        wb_dec(ti + 1); wb_str("  ");
        wb_hex16(te->cs); wb_ch(':'); wb_hex16(te->ip); wb_str("  ");
        wb_hex8(te->opcode); wb_str("  ");
        wb_hex8(te->intno); wb_str("    ");
        wb_hex8(te->ah); wb_hex8(te->al); wb_str("  ");
        wb_hex16(te->cx);
        wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();
    wb_flush();
}

/* [MEMORY DUMP] + [TIMEOUT] + END */
static void write_section_mem_and_end(void)
{
    int di;
    u8 *p;

    wb_reset();
    wb_separator();
    wb_str("MEMORY DUMP\n");
    wb_separator();
    wb_nl();

    /* ★注意: v86_mem_teardown()後にはバッキングRAMのページマッピングが
     * 解除されているため、v86_phys_addr()の結果にアクセスすると#PFが発生する。
     * paging_is_presentでページの有効性を確認してからアクセスする。 */
    p = v86_phys_addr(0x0060, 0x0000);
    if (paging_is_present((u32)p)) {
        wb_str("[0060:0000] (IPL area, 32 bytes)\n  ");
        for (di = 0; di < 32; di++) {
            wb_hex8(p[di]); wb_ch(' ');
            if (di == 15) { wb_str("\n  "); }
        }
        wb_nl(); wb_nl();
    } else {
        wb_str("[0060:0000] (IPL area) — page not present (teardown済)\n\n");
    }

    p = v86_phys_addr(0x1E00, 0x0000);
    if (paging_is_present((u32)p)) {
        wb_str("[1E00:0000] (DOS work area, 32 bytes)\n  ");
        for (di = 0; di < 32; di++) {
            wb_hex8(p[di]); wb_ch(' ');
            if (di == 15) { wb_str("\n  "); }
        }
        wb_nl(); wb_nl();
    } else {
        wb_str("[1E00:0000] (DOS work area) — page not present (teardown済)\n\n");
    }

    /* IVT スナップショット (INT 00h-1Fh) */
    {
        u32 *ivt = (u32 *)v86_phys_addr(0, 0);
        if (paging_is_present((u32)ivt)) {
            wb_str("[IVT snapshot] (INT 00h-1Fh)\n");
            for (di = 0; di < 0x20; di++) {
                u16 seg = (u16)(ivt[di] >> 16);
                u16 off = (u16)(ivt[di] & 0xFFFF);
                wb_str("  INT "); wb_hex8((u8)di); wb_str("h: ");
                wb_hex16(seg); wb_ch(':'); wb_hex16(off);
                if (seg == 0x0050 && off == 0x0000) wb_str(" (dummy)");
                wb_nl();
                if (wpos > WBUF_SIZE - 80) wb_flush();
            }
        } else {
            wb_str("[IVT snapshot] — page not present (teardown済)\n");
        }
    }
    wb_nl();
    wb_flush();

    /* TIMEOUT */
    wb_reset();
    if (v86_timeout_cs || v86_timeout_ip) {
        u8 *ta;
        wb_separator();
        wb_str("TIMEOUT DIAGNOSTICS\n");
        wb_separator();
        wb_nl();
        wb_str("  Timeout CS:IP : ");
        wb_hex16((u16)v86_timeout_cs); wb_ch(':');
        wb_hex16((u16)v86_timeout_ip); wb_nl();
        ta = v86_phys_addr(v86_timeout_cs, v86_timeout_ip);
        if (paging_is_present((u32)ta)) {
            wb_str("  Opcodes:\n  ");
            for (di = 0; di < 32; di++) {
                wb_hex8(ta[di]); wb_ch(' ');
                if (di == 15) { wb_str("\n  "); }
            }
        } else {
            wb_str("  Opcodes: page not present (teardown済)");
        }
        wb_nl(); wb_nl();
    }

    wb_separator();
    wb_str("END OF LOG\n");
    wb_separator();
    wb_flush();
}

/* ====================================================================== */
/*  統合ダンプ関数                                                         */
/* ====================================================================== */
void v86_debug_dump_session(void)
{
    if (!v86_debug_enabled) return;

    kprintf(0x0A, "[V86_DBG] dump_session: log_fd=%d\n", log_fd);

    /* ファイルログ出力 */
    if (log_fd >= 0) {
        kprintf(0x0A, "[V86_DBG] writing sections...\n");
        write_section_exit();
        write_section_hw();
        write_section_io();
        write_section_disk();
        write_section_gptrace();
        write_section_mem_and_end();
        log_close();
        kprintf(0x0A, "[V86_DBG] Log written: %s\n", log_path);
    } else {
        kprintf(0x0E, "[V86_DBG] WARNING: log_fd < 0, skipping file dump\n");
    }

    /* シリアルダンプ (有効時のみ) */
    if (v86_debug_serial_enabled) {
        v86_debug_dump_serial();
    }

    /* シリアル有効状態を復元 (次のセッション用) */
    v86_debug_serial_enabled = 1;
}

/* ====================================================================== */
/*  v86_debug_dump_memory_pre — teardown前メモリダンプ                     */
/*                                                                          */
/*  v86_mem_teardown() の直前、バッキングRAMが有効な状態で呼ばれる。       */
/*  IPLコード、BDA、IVT、GP TRACEで頻出するCS:IP周辺をダンプする。        */
/* ====================================================================== */
void v86_debug_dump_memory_pre(void)
{
    int di;
    u8 *p;

    if (!v86_debug_enabled) return;
    if (log_fd < 0) return;

    wb_reset();
    wb_separator();
    wb_str("MEMORY DUMP (pre-teardown)\n");
    wb_separator();
    wb_nl();

    /* IPL area: 0x0060:0x0000 (リニア 0x600) — 256バイト */
    p = v86_phys_addr(0x0060, 0x0000);
    wb_str("[0060:0000] (IPL code, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)di);
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* IPL area: 0x0060:0x0100-0x01FF — CLI/STI周辺 */
    p = v86_phys_addr(0x0060, 0x0100);
    wb_str("[0060:0100] (IPL code page2, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x100 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* IPL area: 0x0060:0x0500-0x06FF — ディスクI/Oサブルーチン */
    p = v86_phys_addr(0x0060, 0x0500);
    wb_str("[0060:0500] (IPL disk routines, 512 bytes)\n");
    for (di = 0; di < 512; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x500 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* IPL area: 0x0060:0x0900-0x09FF — GP TRACEのループ箇所 */
    p = v86_phys_addr(0x0060, 0x0900);
    wb_str("[0060:0900] (IPL loop area, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x900 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* 第2ステージ: 0x0160:0x0000-0x02FF — エントリ+CLI周辺 */
    p = v86_phys_addr(0x0160, 0x0000);
    wb_str("[0160:0000] (Stage2 entry, 768 bytes)\n");
    for (di = 0; di < 768; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)di);
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* 第2ステージ: 0x0160:0x0700-0x08FF — ディスクI/Oループ */
    p = v86_phys_addr(0x0160, 0x0700);
    wb_str("[0160:0700] (Stage2 disk loop, 512 bytes)\n");
    for (di = 0; di < 512; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x700 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* ディレクトリテーブル: 0x1500:0x0000-0x00FF — ファイル検索対象 */
    p = v86_phys_addr(0x1500, 0x0000);
    wb_str("[1500:0000] (Directory table, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)di);
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* 第2ステージ: 0x0160:0x0300-0x06FF — ファイルロード関数 0x6CF 周辺 */
    p = v86_phys_addr(0x0160, 0x0300);
    wb_str("[0160:0300] (Stage2 file loader, 1024 bytes)\n");
    for (di = 0; di < 1024; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x300 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* 第2ステージ: 0x0160:0x1100-0x11FF — TITLEP ロードバッファ */
    p = v86_phys_addr(0x0160, 0x1100);
    wb_str("[0160:1100] (TITLEP load buffer, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x1100 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* グラフィックVRAM B面: A800:0000 (256 bytes) — 描画有無確認 */
    p = v86_phys_addr(0xA800, 0x0000);
    wb_str("[A800:0000] (GVRAM plane-B, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)di);
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* 0x0050:0x0000 — IVTダミー/OUT命令エリア (256 bytes) */
    p = v86_phys_addr(0x0050, 0x0000);
    wb_str("[0050:0000] (IVT handler area, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)di);
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* BDA (0000:0400-05FF) — 512バイト */
    p = v86_phys_addr(0x0000, 0x0400);
    wb_str("[0000:0400] (BDA, 512 bytes)\n");
    for (di = 0; di < 512; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x400 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* IVT (INT 00h-1Fh) */
    {
        u32 *ivt = (u32 *)v86_phys_addr(0, 0);
        wb_str("[IVT] (INT 00h-1Fh)\n");
        for (di = 0; di < 0x20; di++) {
            u16 seg = (u16)(ivt[di] >> 16);
            u16 off = (u16)(ivt[di] & 0xFFFF);
            wb_str("  INT "); wb_hex8((u8)di); wb_str("h: ");
            wb_hex16(seg); wb_ch(':'); wb_hex16(off);
            if (seg == 0x0050 && off == 0x0000) wb_str(" (dummy)");
            wb_nl();
            if (wpos > WBUF_SIZE - 80) wb_flush();
        }
    }

    /* タイマーハンドラ周辺コード (IVT[0x08]) — 音楽ドライバ解析用
     * Ysのタイマーハンドラ(1F30:01D3)は0x0238でCALLする音楽ティック関数を含む。
     * 256バイトでは足りないので1024バイトダンプする。 */
    {
        u32 *ivt = (u32 *)v86_phys_addr(0, 0);
        u16 seg = (u16)(ivt[0x08] >> 16);
        u16 off = (u16)(ivt[0x08] & 0xFFFF);
        if (seg != 0x003F && seg != 0x0050) {
            u16 base = off & 0xFFF0;
            u8 *hp = v86_phys_addr(seg, base);
            wb_str("["); wb_hex16(seg); wb_str(":"); wb_hex16(base);
            wb_str("] (Timer handler, 1024 bytes)\n");
            for (di = 0; di < 1024; di++) {
                if ((di % 16) == 0) {
                    wb_str("  ");
                    wb_hex16((u16)(base + di));
                    wb_str(": ");
                }
                wb_hex8(hp[di]); wb_ch(' ');
                if ((di % 16) == 15) wb_nl();
                if (wpos > WBUF_SIZE - 80) wb_flush();
            }
            wb_nl();
        }
    }

    /* 音楽エンジンフラグ領域 (Ys: ES=0x0060, offset 0x0530-0x053F)
     * ES:[0532]が音楽有効フラグ、DS:[03C1]が音符カウンタ */
    p = v86_phys_addr(0x0060, 0x0500);
    wb_str("[0060:0500] (Music engine work area, 256 bytes)\n");
    for (di = 0; di < 256; di++) {
        if ((di % 16) == 0) {
            wb_str("  ");
            wb_hex16((u16)(0x500 + di));
            wb_str(": ");
        }
        wb_hex8(p[di]); wb_ch(' ');
        if ((di % 16) == 15) wb_nl();
        if (wpos > WBUF_SIZE - 80) wb_flush();
    }
    wb_nl();

    /* FM音源ステータスレジスタ読み取り (0x0188)
     * bit7=BUSY, bit1=FlagA, bit0=FlagB
     * 音楽ルーチンがTimer A/Bを使用しているかの手がかり */
    {
        u8 fm_status = inp(0x0188);
        wb_str("[FM STATUS] port 0188h = ");
        wb_hex8(fm_status);
        wb_str("  (BUSY="); wb_ch((fm_status & 0x80) ? '1' : '0');
        wb_str(" FlagA="); wb_ch((fm_status & 0x02) ? '1' : '0');
        wb_str(" FlagB="); wb_ch((fm_status & 0x01) ? '1' : '0');
        wb_str(")\n");
    }

    /* PIT Counter#0 情報 */
    {
        u16 reload = v86_pit_get_counter0_reload();
        u32 divisor = v86_pit_get_irq_divisor();
        wb_str("[PIT] Counter#0 reload=0x");
        wb_hex16(reload);
        wb_str(" (");  wb_dec((u32)reload); wb_str(")");
        wb_str("  divisor="); wb_dec(divisor);
        wb_str("  expected_hz=");
        if (reload > 0) {
            /* 1996800 / reload = 期待される割り込みHz */
            wb_dec(1996800UL / (u32)reload);
        } else {
            wb_str("N/A");
        }
        wb_str("  actual=100Hz\n");
    }

    wb_nl();
    wb_flush();
}

/* ====================================================================== */
/*  v86_set_debug — 外部プログラムからデバッグフラグを設定 (KAPI公開)      */
/* ====================================================================== */
void __cdecl v86_set_debug(int enabled)
{
    v86_debug_enabled = enabled ? 1 : 0;
}
