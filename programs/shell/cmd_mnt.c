#include "cmd_fs_shared.h"
#include "shell.h"

static void cmd_mount(int argc, char **argv)
{
    int ret;
    if (argc < 4) {
        shell_print_help(argv[0]);
        return;
    }
    ret = g_api->sys_mount(argv[1], argv[2], argv[3]);
    if (ret != 0) g_api->kprintf(ATTR_RED, "mount: failed %d\n", ret);
}

static void cmd_umount(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    g_api->sys_umount(argv[1]);
}

static void cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_api->vfs_sync();
}

static void cmd_exec(int argc, char **argv)
{
    int rc;
    char cmdline[256];
    int i, pos;

    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }

    /* argv[1]以降を結合してcmdline全体を構築 */
    pos = 0;
    for (i = 1; i < argc; i++) {
        int len = 0;
        const char *s = argv[i];
        if (i > 1 && pos < (int)sizeof(cmdline) - 1) cmdline[pos++] = ' ';
        while (*s && pos < (int)sizeof(cmdline) - 1) {
            cmdline[pos++] = *s++;
            len++;
        }
    }
    cmdline[pos] = '\0';

    rc = g_api->exec_run(cmdline);
    if (rc == -1) g_api->kprintf(ATTR_RED, "%s", "exec: file not found\n");
    else if (rc == -2) g_api->kprintf(ATTR_RED, "%s", "exec: invalid executable or crashed\n");
    else if (rc == -3) g_api->kprintf(ATTR_RED, "%s", "exec: file not found\n");
    else g_api->kprintf(ATTR_GREEN, "exec: exited with %d\n", rc);
}




static const ShellCmd mnt_cmds[] = {
    { "mount",  cmd_mount,  "PREFIX DEV FS", "Mount a filesystem" },
    { "umount", cmd_umount, "PREFIX",        "Unmount a filesystem" },
    { "sync",   cmd_sync,   "",              "Sync file buffers to disk" },
    { "exec",   cmd_exec,   "FILE.BIN",      "Execute a binary program" },
    { (const char *)0, 0, 0, 0 }
};
void shell_cmd_mnt_init(void) { shell_register_cmds(mnt_cmds); }
