#include "shell.h"

/* ======================================================================== */
/*  システム操作モジュール (cmd_sys.c)                                      */
/* ======================================================================== */

typedef struct {
    u32 total_sectors;
    u16 cylinders;
    u16 heads;
    u16 sectors;
    u32 size_mb;
    char model[41];
    char serial[21];
    char firmware[9];
    int  lba_supported;
} IdeInfo;


static void cmd_mem(int argc, char **argv)
{
    u32 pmem_kb;
    (void)argc; (void)argv;
    pmem_kb = g_api->sys_get_mem_kb();
    g_api->kprintf(ATTR_CYAN, "Memory Info:\n  Physical: %u KB (%u MB)\n  Paging:   %s\n  Heap Tot: %u B, Used: %u B, Free: %u B\n",
                  pmem_kb, pmem_kb / 1024,
                  g_api->paging_enabled() ? "ENABLED" : "DISABLED",
                  g_api->kmalloc_total(), g_api->kmalloc_used(), g_api->kmalloc_free());
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->sys_reboot();
}

static void cmd_dev(int argc, char **argv)
{
    int i, n = g_api->dev_count();
    (void)argc; (void)argv;
    g_api->kprintf(ATTR_CYAN, "%s", "Devices:\n");
    for (i=0; i<n; i++) {
        char name[32]; int type; u32 sects;
        if (g_api->dev_get_info(i, name, 32, &type, &sects) == 0) {
            if (type == 1) g_api->kprintf(ATTR_WHITE, "  %s: block %u sects\n", name, sects);
            else g_api->kprintf(ATTR_WHITE, "  %s: char\n", name);
        }
    }
}

static void cmd_ide(int argc, char **argv)
{
    int drv = 0, i;
    IdeInfo info;
    if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '3') {
        drv = argv[1][0] - '0';
    }
    if (!g_api->ide_drive_present(drv)) {
        g_api->kprintf(ATTR_RED, "IDE drive %d not present.\n", drv);
        return;
    }
    if (g_api->ide_identify(drv, &info) == 0) {
        char model[41];
        for (i = 0; i < 40; i++) model[i] = info.model[i];
        model[40] = '\0';
        g_api->kprintf(ATTR_WHITE, "IDE %d: %s\n  C/H/S: %u/%u/%u\n  LBA Segs: %u\n",
                       drv, model, info.cylinders, info.heads, info.sectors, info.total_sectors);
    } else {
        g_api->kprintf(ATTR_RED, "IDE %d: Identify fail\n", drv);
    }
}

static void cmd_format(int argc, char **argv)
{
    int drv = 0;
    u32 sects = 2880;
    int ret;
    const char *p;

    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    drv = argv[1][0] - '0';
    if (argc > 2) {
        p = argv[2];
        sects = 0;
        while (*p >= '0' && *p <= '9') sects = sects * 10 + (*p++ - '0');
    }
    g_api->kprintf(ATTR_YELLOW, "Formatting drive %d (%u sectors)...\n", drv, sects);
    ret = g_api->ext2_format(drv, sects);
    if (ret == 0) g_api->kprintf(ATTR_GREEN, "%s", "Format complete.\n");
    else g_api->kprintf(ATTR_RED, "Format failed: %d\n", ret);
}

static void cmd_play(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    g_api->fm_play_mml(argv[1]);
}

/* 登録用テーブル */
static const ShellCmd sys_cmds[] = {
    { "mem",    cmd_mem,    "",              "Show memory statistics" },
    { "heap",   cmd_mem,    "",              "Alias for mem" },
    { "reboot", cmd_reboot, "",              "Reboot the system" },
    { "dev",    cmd_dev,    "",              "List block/char devices" },
    { "df",     cmd_dev,    "",              "Alias for dev" },
    { "ide",    cmd_ide,    "[0-3]",         "Show IDE drive geometry" },
    { "format", cmd_format, "[0-3] [sects]", "Format a drive to ext2" },
    { "play",   cmd_play,   "MML",           "Play MML via FM synth" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_sys_init(void)
{
    shell_register_cmds(sys_cmds);
}
