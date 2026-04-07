#include "shell.h"

/* ======================================================================== */
/*  シリアル・リモート連携モジュール (rshell.c)                             */
/* ======================================================================== */

/* upload/recv 共用転送バッファ (BSS配置, 64KB) */
static u8 xfer_buf[65536];

static void cmd_serial(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->serial_init(38400);
    g_api->kprintf(ATTR_GREEN, "%s", "RS-232C initialized (38400bps)\n");
}

static void cmd_terminal(int argc, char **argv)
{
    int kch, sch;
    (void)argc; (void)argv;
    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "RS-232C not initialized. Run 'serial' first.\n");
        return;
    }
    g_api->kprintf(ATTR_CYAN, "%s", "Terminal mode (ESC to exit)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "--------------------------------\n");
    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) break;
        if (kch >= 0) {
            g_api->serial_putchar((u8)kch);
            g_api->shell_putchar((char)kch, ATTR_GREEN);
        }
        sch = g_api->serial_trygetchar();
        if (sch >= 0) {
            if (sch == '\r') {
                g_api->shell_putchar('\n', ATTR_YELLOW);
            } else if (sch >= 0x20 || sch == '\n') {
                g_api->shell_putchar((char)sch, ATTR_YELLOW);
            }
        }
        if (kch < 0 && sch < 0) {
            u32 w = g_api->get_tick() + 1;
            while (g_api->get_tick() < w);
        }
    }
    g_api->kprintf(ATTR_CYAN, "%s", "\n[Terminal closed]\n");
}

static void cmd_rshell(int argc, char **argv)
{
    char rbuf[128];
    int rpos, ch, kch;
    (void)argc; (void)argv;

    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "Serial not initialized. Run 'serial' first.\n");
        return;
    }

    g_api->rshell_set_active(1);
    g_api->kprintf(ATTR_GREEN, "%s", "Remote shell active (ESC to exit)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "Waiting for commands via serial...\n");
    g_api->serial_putchar(0x04);

    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) break;

        rpos = 0;
        rbuf[0] = '\0';

        if (kch >= 0x20 && kch < 0x7F) {
            ch = kch;
            goto read_rest;
        }

        for (;;) {
            ch = g_api->kbd_trygetchar();
            if (ch >= 0) {
                if (ch == 0x1B) goto rshell_exit;
                break;
            }
            {
                u32 w = g_api->get_tick() + 1;
                while (g_api->get_tick() < w);
            }
        }

    read_rest:
        while (ch >= 0 && ch != '\n' && ch != '\r' && rpos < 126) {
            rbuf[rpos++] = (char)ch;
            {
                int t = 0;
                while (t < 50000) {
                    ch = g_api->kbd_trygetchar();
                    if (ch >= 0) {
                        if (ch == 0x1B) goto rshell_exit;
                        break;
                    }
                    t++;
                }
            }
        }
        rbuf[rpos] = '\0';

        if (rpos == 0) continue;

        g_api->buz_off();

        if (rbuf[0]=='e' && rbuf[1]=='x' && rbuf[2]=='i' && rbuf[3]=='t' && rbuf[4]=='\0') break;

        g_api->kprintf(ATTR_YELLOW, "%s", "> ");
        g_api->kprintf(ATTR_WHITE, "%s", rbuf);
        g_api->kprintf(ATTR_WHITE, "%s", "\n");

        execute_command(rbuf);

        g_api->buz_off();

        {
            u32 wait_end = g_api->get_tick() + 1;
            while (g_api->get_tick() < wait_end);
        }

        g_api->serial_putchar(0x04);
    }
rshell_exit:
    g_api->rshell_set_active(0);
    g_api->kprintf(ATTR_CYAN, "%s", "\n[Remote shell closed]\n");
}

static void cmd_send(int argc, char **argv)
{
    int i;
    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "RS-232C not initialized. Run 'serial' first.\n");
        return;
    }
    for (i = 1; i < argc; i++) {
        g_api->serial_puts(argv[i]);
        if (i < argc - 1) g_api->serial_putchar(' ');
    }
    g_api->serial_putchar('\r');
    g_api->serial_putchar('\n');
    g_api->kprintf(ATTR_YELLOW, "%s", "Sent\n");
}

static void cmd_upload(int argc, char **argv)
{
    char *fname;
    const char *arg;
    u32 size, ui, hi, lo, byte_val, checksum;
    int ch;

    if (argc < 3) {
        shell_print_help(argv[0]);
        return;
    }
    
    fname = argv[1];
    arg = argv[2];

    size = 0;
    while (*arg) {
        ch = *arg++;
        if (ch >= '0' && ch <= '9') size = (size << 4) | (u32)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') size = (size << 4) | (u32)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') size = (size << 4) | (u32)(ch - 'A' + 10);
        else break;
    }

    if (size == 0 || size > sizeof(xfer_buf)) {
        shell_print_help(argv[0]);
        return;
    }

    g_api->kprintf(ATTR_CYAN, "%s", "READY\n");
    g_api->serial_puts("READY\r\n");

    checksum = 0;
    for (ui = 0; ui < size; ui++) {
        do { ch = g_api->serial_getchar(); } while (ch == '\r' || ch == '\n');
        if (ch >= '0' && ch <= '9') hi = (u32)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') hi = (u32)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') hi = (u32)(ch - 'A' + 10);
        else { g_api->serial_puts("ERR bad hex\r\n"); return; }

        ch = g_api->serial_getchar();
        if (ch >= '0' && ch <= '9') lo = (u32)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') lo = (u32)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') lo = (u32)(ch - 'A' + 10);
        else { g_api->serial_puts("ERR bad hex\r\n"); return; }

        byte_val = (hi << 4) | lo;
        xfer_buf[ui] = (u8)byte_val;
        checksum += byte_val;
    }

    {
        int fd = g_api->sys_open(fname, 1 | 0x0100 | 0x0200); /* O_WRONLY|O_CREAT|O_TRUNC */
        if (fd >= 0) {
            if (g_api->sys_write(fd, xfer_buf, (u32)size) == (u32)size) {
                g_api->kprintf(ATTR_GREEN, "Uploaded %u bytes\n", size);
                g_api->serial_puts("OK\r\n");
            } else {
                g_api->kprintf(ATTR_RED, "%s", "Write failed\n");
                g_api->serial_puts("ERR write\r\n");
            }
            g_api->sys_close(fd);
        } else {
            g_api->kprintf(ATTR_RED, "%s", "Write failed (open)\n");
            g_api->serial_puts("ERR write\r\n");
        }
    }
}

static void cmd_recv(int argc, char **argv)
{
    char fname[13];
    u32 fsize, received;
    u8 checksum, calc_sum;
    int i, ch, timeout, kch;
    (void)argc; (void)argv;

    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "RS-232C not initialized.\n");
        return;
    }

    g_api->kprintf(ATTR_CYAN, "%s", "Waiting for file transfer... (ESC to cancel)\n");

    i = 0;
    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) {
            g_api->kprintf(ATTR_RED, "%s", "\nCancelled.\n");
            return;
        }
        ch = g_api->serial_trygetchar();
        if (ch < 0) {
            u32 w = g_api->get_tick() + 1;
            while (g_api->get_tick() < w);
            continue;
        }
        if (i == 0 && ch == 'O') { i = 1; continue; }
        if (i == 1 && ch == 'S') { i = 2; continue; }
        if (i == 2 && ch == '3') { i = 3; continue; }
        if (i == 3 && ch == '2') { i = 4; break; }
        i = 0;
    }

    g_api->kprintf(ATTR_GREEN, "%s", "  Magic: OS32 OK\n");

    for (i = 0; i < 12; i++) {
        timeout = 0;
        while ((ch = g_api->serial_trygetchar()) < 0) {
            if (g_api->kbd_trygetchar() == 0x1B) { g_api->kprintf(ATTR_RED, "%s", "\nCancelled.\n"); return; }
            timeout++;
            if (timeout > 500000) { g_api->kprintf(ATTR_RED, "%s", "\nTimeout.\n"); return; }
        }
        fname[i] = (char)ch;
    }
    fname[12] = '\0';
    g_api->kprintf(ATTR_YELLOW, "  File: %s\n", fname);

    fsize = 0;
    for (i = 0; i < 4; i++) {
        timeout = 0;
        while ((ch = g_api->serial_trygetchar()) < 0) {
            if (g_api->kbd_trygetchar() == 0x1B) { g_api->kprintf(ATTR_RED, "%s", "\nCancelled.\n"); return; }
            timeout++;
            if (timeout > 500000) { g_api->kprintf(ATTR_RED, "%s", "\nTimeout.\n"); return; }
        }
        fsize |= ((u32)(u8)ch) << (i * 8);
    }
    g_api->kprintf(ATTR_YELLOW, "  Size: %u bytes\n", fsize);

    if (fsize > sizeof(xfer_buf)) {
        g_api->kprintf(ATTR_RED, "%s", "  Error: file too large\n");
        return;
    }

    g_api->kprintf(ATTR_CYAN, "%s", "  Receiving");
    calc_sum = 0;
    received = 0;

    while (received < fsize) {
        timeout = 0;
        while ((ch = g_api->serial_trygetchar()) < 0) {
            if (g_api->kbd_trygetchar() == 0x1B) { g_api->kprintf(ATTR_RED, "%s", "\n  Cancelled!\n"); return; }
            timeout++;
            if (timeout > 1000000) { g_api->kprintf(ATTR_RED, "%s", "\n  Timeout!\n"); return; }
        }
        xfer_buf[received] = (u8)ch;
        calc_sum += (u8)ch;
        received++;
        if ((received & 511) == 0) g_api->shell_putchar('.', ATTR_CYAN);
    }
    g_api->kprintf(ATTR_WHITE, "%s", "\n");

    timeout = 0;
    while ((ch = g_api->serial_trygetchar()) < 0) {
        if (g_api->kbd_trygetchar() == 0x1B) break;
        timeout++;
        if (timeout > 500000) break;
    }
    checksum = (u8)ch;

    if ((calc_sum & 0xFF) == checksum) g_api->kprintf(ATTR_GREEN, "%s", "  Checksum: OK\n");
    else g_api->kprintf(ATTR_RED, "%s", "  Checksum: MISMATCH\n");

    for (i = 0; i < 3; i++) {
        timeout = 0;
        while ((ch = g_api->serial_trygetchar()) < 0) {
            if (g_api->kbd_trygetchar() == 0x1B) break;
            timeout++;
            if (timeout > 100000) break;
        }
    }

    if (1) {
        {
            int fd = g_api->sys_open(fname, 1 | 0x0100 | 0x0200);
            if (fd >= 0) {
                if (g_api->sys_write(fd, xfer_buf, (u32)fsize) == (u32)fsize) {
                    g_api->kprintf(ATTR_GREEN, "  Saved: %s\n", fname);
                } else {
                    g_api->kprintf(ATTR_RED, "%s", "  Error: failed to save\n");
                }
                g_api->sys_close(fd);
            } else {
                g_api->kprintf(ATTR_RED, "%s", "  Error: failed to save (open)\n");
            }
        }
    } else g_api->kprintf(ATTR_RED, "%s", "  Error: no fs mounted\n");
}

static void cmd_tvdump(int argc, char **argv)
{
    volatile u16 *text = (volatile u16 *)0xA0000UL;
    volatile u8  *attr_base = (volatile u8 *)0xA2000UL;
    int row, col;
    (void)argc; (void)argv;

    if (!g_api->serial_is_initialized()) {
        g_api->kprintf(ATTR_RED, "%s", "Serial not initialized.\n");
        return;
    }

    g_api->serial_putchar('T'); g_api->serial_putchar('V');
    g_api->serial_putchar('D'); g_api->serial_putchar('M');
    g_api->serial_putchar(80);  g_api->serial_putchar(25);

    for (row = 0; row < 25; row++) {
        for (col = 0; col < 80; col++) {
            int idx = row * 80 + col;
            u16 ch_val = text[idx];
            u8  at = attr_base[idx * 2];
            g_api->serial_putchar((u8)(ch_val & 0xFF));
            g_api->serial_putchar(at);
        }
    }
    g_api->kprintf(ATTR_GREEN, "%s", "TVRAM dump sent\n");
}

/* 登録用テーブル */
static const ShellCmd rshell_cmds[] = {
    { "serial",   cmd_serial,   "",              "Initialize RS-232C (38400bps)" },
    { "terminal", cmd_terminal, "",              "Enter serial terminal mode" },
    { "rshell",   cmd_rshell,   "",              "Start remote shell host" },
    { "send",     cmd_send,     "TEXT...",       "Send text via serial" },
    { "upload",   cmd_upload,   "FILE HEXSIZE",  "Upload file via serial" },
    { "recv",     cmd_recv,     "",              "Receive file via serial" },
    { "tvdump",   cmd_tvdump,   "",              "Dump Text VRAM over serial" },
    { (const char *)0, 0, 0, 0 }
};

void shell_rshell_init(void)
{
    shell_register_cmds(rshell_cmds);
}
