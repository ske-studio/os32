#include "shell.h"

/* ======================================================================== */
/*  基本コマンドモジュール (cmd_base.c)                                     */
/* ======================================================================== */

static void cmd_help(int argc, char **argv)
{
    int count, i;
    const ShellCmd *cmds;
    
    if (argc > 1) {
        shell_print_help(argv[1]);
        return;
    }

    g_api->kprintf(ATTR_CYAN, "%s", "OS32 Shell Commands:\n");
    cmds = shell_get_cmds(&count);
    
    for (i = 0; i < count; i++) {
        if (cmds[i].description) {
            char pad[12];
            int len = str_len(cmds[i].name);
            int p = 0;
            while (len < 10 && p < 11) { pad[p++] = ' '; len++; }
            pad[p] = '\0';
            g_api->kprintf(ATTR_WHITE, "  %s%s- %s\n", cmds[i].name, pad, cmds[i].description);
        } else {
            g_api->kprintf(ATTR_WHITE, "  %s\n", cmds[i].name);
        }
    }
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
    g_api->kprintf(ATTR_GREEN, "%s", "PC-9801 OS32 v0.7 (External Shell Modular)\n");
}

static void cmd_date(int argc, char **argv)
{
    RTC_Time_Ext t;
    (void)argc; (void)argv;
    g_api->rtc_read(&t);
    g_api->kprintf(ATTR_WHITE, "20%02d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
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
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_base_init(void)
{
    shell_register_cmds(base_cmds);
}
