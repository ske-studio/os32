#include "cmd_fs_shared.h"
#include "shell.h"
#include <stdio.h>

static const char* skip_space(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static int is_dir(const char *path)
{
    int rc = g_api->sys_ls(path, (void*)dummy_ls_cb, 0);
    return (rc == 0);
}

static void append_basename(char *dst_path, const char *src_path)
{
    char temp[256];
    int i = 0, j = 0;
    const char *base = get_basename(src_path);

    while (dst_path[i] && i < 254) { temp[i] = dst_path[i]; i++; }
    if (i > 0 && temp[i-1] != '/' && i < 254) temp[i++] = '/';
    while (base[j] && i < 255) temp[i++] = base[j++];
    temp[i] = '\0';

    for (i = 0; temp[i]; i++) dst_path[i] = temp[i];
    dst_path[i] = '\0';
}



static int parse_two_args(const char *cmd, int skip, char *arg1, char *arg2)
{
    const char *p = skip_space(cmd + skip);
    int i = 0;
    while (*p && *p != ' ' && i < 255) arg1[i++] = *p++;
    arg1[i] = '\0';
    p = skip_space(p);
    i = 0;
    while (*p && *p != ' ' && i < 255) arg2[i++] = *p++;
    arg2[i] = '\0';
    return (arg1[0] && arg2[0]);
}

static void join_path(char *dst_path, const char *dir_path, const char *name)
{
    int i = 0, j = 0;
    while (dir_path[i] && i < 254) { dst_path[i] = dir_path[i]; i++; }
    if (i > 0 && dst_path[i-1] != '/' && i < 254) dst_path[i++] = '/';
    while (name[j] && i < 255) dst_path[i++] = name[j++];
    dst_path[i] = '\0';
}

struct ls_opts {
    int format_long;
    int show_all;
};

static void vfs_ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    struct ls_opts *opts = (struct ls_opts *)ctx;
    int is_tty = g_api->sys_isatty(1);

    /* -a がなければ . と .. をスキップ */
    if (!opts->show_all) {
        if (entry->name[0] == '.' &&
            (entry->name[1]=='\0' || (entry->name[1]=='.' && entry->name[2]=='\0')))
            return;
    }

    if (opts->format_long) {
        char size_buf[16];
        if (entry->type == 2) {
            if (is_tty) g_api->kprintf(ATTR_CYAN, "  <DIR>    DIR   %s\n", entry->name);
            else printf("  <DIR>    DIR   %s\n", entry->name);
        } else {
            format_size(entry->size, size_buf, 10);
            if (is_tty) g_api->kprintf(ATTR_WHITE, "  %s B  FILE  %s\n", size_buf, entry->name);
            else printf("  %s B  FILE  %s\n", size_buf, entry->name);
        }
    } else {
        if (entry->type == 2) {
            if (is_tty) g_api->kprintf(ATTR_CYAN, "%s/  ", entry->name);
            else printf("%s/\n", entry->name);
        } else {
            if (is_tty) g_api->kprintf(ATTR_WHITE, "%s  ", entry->name);
            else printf("%s\n", entry->name);
        }
    }
}

static void cmd_ls(int argc, char **argv)
{
    struct ls_opts opts;
    int i;
    int path_idx_start = 1;
    int is_tty = g_api->sys_isatty(1);

    opts.format_long = 0;
    opts.show_all = 0;

    /* オプション解析: -l, -a, -la, -al 等に対応 */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') opts.format_long = 1;
                else if (argv[i][j] == 'a') opts.show_all = 1;
            }
            path_idx_start = i + 1;
        } else {
            break;
        }
    }

    if (path_idx_start >= argc) {
        if (opts.format_long) {
            if (is_tty) g_api->kprintf(ATTR_WHITE, "%s", "  SIZE     TYPE  NAME\n");
            else printf("%s", "  SIZE     TYPE  NAME\n");
        }
        g_api->sys_ls(".", vfs_ls_cb, &opts);
        if (!opts.format_long) printf("\n");
    } else {
        for (i = path_idx_start; i < argc; i++) {
            if (is_dir(argv[i])) {
                if (argc - path_idx_start > 1) {
                    if (is_tty) g_api->kprintf(ATTR_CYAN, "\n%s:\n", argv[i]);
                    else printf("\n%s:\n", argv[i]);
                }
                if (opts.format_long) {
                    if (is_tty) g_api->kprintf(ATTR_WHITE, "%s", "  SIZE     TYPE  NAME\n");
                    else printf("%s", "  SIZE     TYPE  NAME\n");
                }
                g_api->sys_ls(argv[i], vfs_ls_cb, &opts);
                if (!opts.format_long) printf("\n");
            } else {
                if (opts.format_long) printf("  ????     FILE  %s\n", argv[i]);
                else printf("%s  ", argv[i]);
                if (!opts.format_long && i == argc - 1) printf("\n");
            }
        }
    }
}



static void cmd_cd(int argc, char **argv)
{
    int rc;
    const char *target;
    if (argc < 2) {
        /* 引数なし: ホームディレクトリに移動 */
        target = env_get("HOME");
        if (!target) target = "/";
    } else {
        target = argv[1];
    }
    rc = g_api->sys_chdir(target);
    if (rc != 0) {
        g_api->kprintf(ATTR_RED, "cd: %s: not found (%d)\n", target, rc);
    }
}

static void cmd_pwd(int argc, char **argv)
{
    const char *cwd;
    (void)argc; (void)argv;
    cwd = g_api->sys_getcwd();
    printf("%s\n", cwd ? cwd : "/");
}

static void cmd_mkdir(int argc, char **argv)
{
    int i, rc;
    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    for (i = 1; i < argc; i++) {
        rc = g_api->sys_mkdir(argv[i]);
        if (rc != 0) {
            g_api->kprintf(ATTR_RED, "mkdir: %s: failed (%d)\n", argv[i], rc);
        }
    }
}

static void cmd_rmdir(int argc, char **argv)
{
    int i, rc;
    if (argc < 2) {
        shell_print_help(argv[0]);
        return;
    }
    for (i = 1; i < argc; i++) {
        rc = g_api->sys_rmdir(argv[i]);
        if (rc != 0) {
            g_api->kprintf(ATTR_RED, "rmdir: %s: failed (%d)\n", argv[i], rc);
        }
    }
}



static const ShellCmd dir_cmds[] = {
    { "ls",    cmd_ls,    "[-la] [path...]", "List directory contents" },
    { "cd",    cmd_cd,    "path",           "Change working directory" },
    { "pwd",   cmd_pwd,   "",               "Print working directory" },
    { "mkdir", cmd_mkdir, "dir...",         "Create directories" },
    { "rmdir", cmd_rmdir, "dir...",         "Remove directories" },
    { (const char *)0, 0, 0, 0 }
};
void shell_cmd_dir_init(void) { shell_register_cmds(dir_cmds); }
