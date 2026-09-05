#include "shell.h"
#include "config.h"   /* SYS_GSHELL_BIN, SYS_SYSTEM_CFG (K4: os32gui) */

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
    g_api->kprintf(ATTR_CYAN, "%s", "Memory Info:\n");
    g_api->kprintf(ATTR_WHITE, "  Physical : %u KB (%u MB)\n", pmem_kb, pmem_kb / 1024);
    g_api->kprintf(ATTR_WHITE, "  Paging   : %s\n",
                   g_api->paging_enabled() ? "ENABLED" : "DISABLED");
    g_api->kprintf(ATTR_WHITE, "  Heap Tot : %u B, Used: %u B, Free: %u B\n",
                   g_api->kmalloc_total(), g_api->kmalloc_used(), g_api->kmalloc_free());
    g_api->kprintf(ATTR_CYAN, "%s", "Memory Map:\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x00000 - 0x00FFF  NP (NULL guard)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x01000 - 0x9FFFF  Font/Unicode/GFX (V86 guest window)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0xA0000 - 0xEFFFF  VRAM\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x100000-0x1FAFFF  Kernel Band (code+heap+SHM)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x1FB000-0x1FBFFF  NP (kernel stack guard)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x1FC000-0x1FFFFF  Kernel Stack (16KB)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x200000-0x2FFFFF  SQLite Band (1MB)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x300000-0x3FFFFF  Shell Band (1MB)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x400000-0x4FFFFF  Shared Library Band (1MB)\n");
    g_api->kprintf(ATTR_WHITE, "%s", "  0x500000-          Program Space\n");
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

/* ------------------------------------------------------------------------ */
/*  os32gui — CUI ⇄ GUI シェル切替 (契約 T9 / TASK_K4 作業 0・3)             */
/* ------------------------------------------------------------------------ */

/* 行 [p, p+len) が "GUI=" (前後空白許容) で始まるか判定する。 */
static int os32gui_line_is_gui(const char *p, int len)
{
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i + 3 > len) return 0;
    if (p[i] != 'G' || p[i + 1] != 'U' || p[i + 2] != 'I') return 0;
    i += 3;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return (i < len && p[i] == '=');
}

/* /etc/system.cfg の GUI= 行を on(1)/off(0) に差し替える (無ければ追記)。
 * 既存の他キー行は保存する。成功で 0、失敗で -1。 */
static int os32gui_set_cfg(int on)
{
    char buf[1024];
    char out[1152];
    int fd, n, i, o;

    /* 既存内容を読む (無ければ空から作る) */
    n = 0;
    fd = g_api->sys_open(SYS_SYSTEM_CFG, KAPI_O_RDONLY);
    if (fd >= 0) {
        int r = g_api->sys_read(fd, buf, (int)sizeof(buf) - 1);
        g_api->sys_close(fd);
        if (r > 0) n = r;
    }

    /* 行ごとにコピー。旧 GUI= 行は捨てる。 */
    o = 0;
    i = 0;
    while (i < n) {
        int ls = i;
        int len;
        while (i < n && buf[i] != '\n') i++;
        len = i - ls;          /* 改行を含まない行長 */
        if (i < n) i++;        /* 改行を飛ばす */
        if (os32gui_line_is_gui(&buf[ls], len)) continue;
        {
            int k;
            for (k = 0; k < len && o < (int)sizeof(out) - 8; k++)
                out[o++] = buf[ls + k];
            out[o++] = '\n';
        }
    }

    /* 新しい GUI 行を追記 */
    out[o++] = 'G'; out[o++] = 'U'; out[o++] = 'I'; out[o++] = '=';
    out[o++] = on ? '1' : '0';
    out[o++] = '\n';

    fd = g_api->sys_open(SYS_SYSTEM_CFG, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd < 0) return -1;
    {
        int w = g_api->sys_write(fd, out, o);
        g_api->sys_close(fd);
        if (w != o) return -1;
    }
    return 0;
}

static void cmd_os32gui(int argc, char **argv)
{
    if (argc >= 2) {
        /* on|off: system.cfg の GUI= を書き換える (次回起動から有効) */
        if (str_eq(argv[1], "on")) {
            if (os32gui_set_cfg(1) == 0)
                g_api->kprintf(ATTR_GREEN, "%s", "GUI enabled at next boot (system.cfg GUI=1)\n");
            else
                g_api->kprintf(ATTR_RED, "%s", "os32gui: failed to write /etc/system.cfg\n");
        } else if (str_eq(argv[1], "off")) {
            if (os32gui_set_cfg(0) == 0)
                g_api->kprintf(ATTR_GREEN, "%s", "GUI disabled at next boot (system.cfg GUI=0)\n");
            else
                g_api->kprintf(ATTR_RED, "%s", "os32gui: failed to write /etc/system.cfg\n");
        } else {
            shell_print_help(argv[0]);
        }
        return;
    }

    /* 引数なし: 今すぐ gshell へ切替。カーネルに次シェルを記録して自分は
     * 終了する → 起動ループが gshell を 0x300000 に載せる (契約 T9)。 */
    {
        int rc = g_api->sys_switch_shell(SYS_GSHELL_BIN);
        if (rc < 0) {
            g_api->kprintf(ATTR_RED, "os32gui: switch not permitted (rc=%d)\n", rc);
            return;
        }
        g_api->kprintf(ATTR_CYAN, "%s", "Switching to GUI shell...\n");
        g_api->sys_exit(0);
    }
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
    { "os32gui",cmd_os32gui,"[on|off]",      "Switch to GUI shell now, or set GUI at boot" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_sys_init(void)
{
    shell_register_cmds(sys_cmds);
}
