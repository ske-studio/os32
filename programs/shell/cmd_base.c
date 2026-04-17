#include "shell.h"
#include "libos32/help.h"

/* ======================================================================== */
/*  基本コマンドモジュール (cmd_base.c)                                     */
/* ======================================================================== */

static void cmd_help(int argc, char **argv)
{
    int count, i;
    const ShellCmd *cmds;
    
    if (argc > 1) {
        /* manページを参照 */
        if (os32_help_show(argv[1]) != 0) {
            g_api->kprintf(ATTR_RED, "No manual entry for %s\n", argv[1]);
            g_api->kprintf(ATTR_WHITE, "%s", "  Use 'help' to list available commands.\n");
        }
        return;
    }

    g_api->kprintf(ATTR_CYAN, "%s", "OS32 Shell Commands:\n");
    cmds = shell_get_cmds(&count);
    
    for (i = 0; i < count; i++) {
        char pad[12];
        int len = str_len(cmds[i].name);
        int p = 0;
        while (len < 10 && p < 11) { pad[p++] = ' '; len++; }
        pad[p] = '\0';
        g_api->kprintf(ATTR_WHITE, "  %s%s", cmds[i].name, pad);
        /* manページ有無チェック */
        if (os32_help_exists(cmds[i].name)) {
            g_api->kprintf(ATTR_GREEN, "%s", "[man]\n");
        } else {
            g_api->kprintf(ATTR_WHITE, "%s", "\n");
        }
    }
    g_api->kprintf(ATTR_WHITE, "%s", "\n  Use 'help <cmd>' or 'man <cmd>' for details.\n");
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->tvram_clear();
}

static void cmd_tick(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->kprintf(ATTR_WHITE, "Timer ticks: %u (%u sec)\n", g_api->get_tick(), g_api->get_tick()/100);
}

static void cmd_ver(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->kprintf(ATTR_GREEN, "%s", "PC-9801 OS32 v1.0 (External Shell Modular)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  CPU: Intel 386+ (Protected Mode + Paging)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  PIC: 8259A x2 (remapped to INT 20h+)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  PIT: 8254 @ 100Hz\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  KBD: uPD8251A (IRQ1)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  SER: uPD8251A RS-232C (IRQ4)\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  SND: YM2203 (OPN) FM3+SSG3\n");
    g_api->kprintf(ATTR_CYAN, "%s", "  GFX: 640x400x16 CPU direct\n");
    g_api->kprintf(ATTR_WHITE, "  API: v%u\n", g_api->version);
    g_api->kprintf(ATTR_WHITE, "  Build: %s %s\n", __DATE__, __TIME__);
}

static const char *wday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static void cmd_date(int argc, char **argv)
{
    RTC_Time_Ext t;
    const char *w;
    (void)argc; (void)argv;
    g_api->rtc_read(&t);
    w = (t.wday < 7) ? wday_names[t.wday] : "???";
    g_api->kprintf(ATTR_WHITE, "20%02u-%02u-%02u %02u:%02u:%02u (%s)\n",
                   (u32)t.year, (u32)t.month, (u32)t.day,
                   (u32)t.hour, (u32)t.min, (u32)t.sec,
                   w);
}

static void cmd_beep(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->fm_startup_sound();
}

static void cmd_uptime(int argc, char **argv)
{
    u32 s = g_api->get_tick() / 100;
    (void)argc; (void)argv;
    g_api->kprintf(ATTR_WHITE, "up %u min %u sec\n", s / 60, s % 60);
}

static void cmd_np2(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_api->np2_detect()) {
        char buf[64];
        g_api->kprintf(ATTR_GREEN, "%s", "NP21/W detected!\n");
        g_api->np2_get_version(buf, sizeof(buf));
        g_api->kprintf(ATTR_WHITE, "  Ver: %s\n", buf);
    } else {
        g_api->kprintf(ATTR_RED, "%s", "Not NP21/W.\n");
    }
}

static void cmd_time(int argc, char **argv)
{
    u32 start, end, elapsed_ms;
    char cmd_buf[512];
    int i, bp;

    if (argc < 2) {
        g_api->kprintf(ATTR_RED, "%s", "Usage: time COMMAND [ARGS...]\n");
        return;
    }

    /* argv[1..] からコマンドライン文字列を再構築 */
    bp = 0;
    for (i = 1; i < argc && bp < 510; i++) {
        const char *s = argv[i];
        if (i > 1 && bp < 510) cmd_buf[bp++] = ' ';
        while (*s && bp < 510) cmd_buf[bp++] = *s++;
    }
    cmd_buf[bp] = '\0';

    start = g_api->get_tick();
    execute_command(cmd_buf);
    end = g_api->get_tick();

    elapsed_ms = (end - start) * 10; /* 1ティック = 10ms */
    g_api->kprintf(ATTR_CYAN, "\nreal  %u.%03us\n",
                   elapsed_ms / 1000, elapsed_ms % 1000);
}

/* 登録用テーブル */
static const ShellCmd base_cmds[] = {
    { "help",   cmd_help,   "[cmd]", "Show help" },
    { "?",      cmd_help,   "[cmd]", "Alias for help" },
    { "clear",  cmd_clear,  "",      "Clear the screen" },
    { "cls",    cmd_clear,  "",      "Alias for clear" },
    { "tick",   cmd_tick,   "",      "Show system uptime ticks" },
    { "ver",    cmd_ver,    "",      "Show OS version" },
    { "uname",  cmd_ver,    "",      "Alias for ver" },
    { "date",   cmd_date,   "",      "Show RTC date/time" },
    { "beep",   cmd_beep,   "",      "Play startup beep" },
    { "uptime", cmd_uptime, "",      "Show sys uptime" },
    { "np2",    cmd_np2,    "",      "Detect NP21/W emulator" },
    { "time",   cmd_time,   "CMD",   "Measure command time" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_base_init(void)
{
    shell_register_cmds(base_cmds);
}

