#include "cmd_fs_shared.h"
#include "shell.h"

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

static void vfs_ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    char size_buf[16];
    int use_long = ctx ? *(int*)ctx : 0;
    
    if (use_long) {
        if (entry->type == 2) { /* DIR */
            g_api->kprintf(ATTR_CYAN, "d      <DIR> %s\n", entry->name);
        } else {
            format_size(entry->size, size_buf, 10);
            g_api->kprintf(ATTR_WHITE, "- %s %s\n", size_buf, entry->name);
        }
    } else {
        if (entry->type == 2) {
            g_api->kprintf(ATTR_CYAN, "%s/  ", entry->name);
        } else {
            g_api->kprintf(ATTR_WHITE, "%s  ", entry->name);
        }
    }
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

static u8 io_buf[65536];

static int do_copy_file(const char *cmd_name, const char *src, const char *dst) {
    int fd_in, fd_out, sz;
    fd_in = g_api->sys_open(src, 0); /* O_RDONLY */
    if (fd_in < 0) {
        g_api->kprintf(ATTR_RED, "%s: %s not found\n", cmd_name, src);
        return -1;
    }
    sz = g_api->sys_read(fd_in, io_buf, 65536);
    g_api->sys_close(fd_in);

    if (sz < 0) return -1;

    fd_out = g_api->sys_open(dst, 1 | 0x0100 | 0x0200); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd_out < 0) {
        g_api->kprintf(ATTR_RED, "%s: open failed %s\n", cmd_name, dst);
        return -1;
    }
    if (g_api->sys_write(fd_out, io_buf, sz) != sz) {
        g_api->kprintf(ATTR_RED, "%s: write failed %s\n", cmd_name, dst);
        g_api->sys_close(fd_out);
        return -1;
    }
    g_api->sys_close(fd_out);
    return 0;
}

static void cmd_cp(int argc, char **argv)
{
    int i, is_dest_dir;
    const char *dst;
    
    if (argc < 3) {
        shell_print_help(argv[0]);
        return;
    }
    
    dst = argv[argc - 1];
    is_dest_dir = is_dir(dst);
    
    if (argc > 3 && !is_dest_dir) {
        g_api->kprintf(ATTR_RED, "%s", "cp: multiple files must be copied into a directory\n");
        return;
    }
    
    for (i = 1; i < argc - 1; i++) {
        const char *src = argv[i];
        if (is_dir(src)) {
            g_api->kprintf(ATTR_RED, "%s", "cp: recursively copying directories is not yet supported in glob refractor\n");
            continue;
        }
        
        if (is_dest_dir) {
            char dpath[PATH_MAX_LEN];
            join_path(dpath, dst, get_basename(src));
            do_copy_file("cp", src, dpath);
        } else {
            do_copy_file("cp", src, dst);
        }
    }
}

static void cmd_mv(int argc, char **argv)
{
    int i, is_dest_dir;
    const char *dst;
    
    if (argc < 3) {
        shell_print_help(argv[0]);
        return;
    }
    
    dst = argv[argc - 1];
    is_dest_dir = is_dir(dst);
    
    if (argc > 3 && !is_dest_dir) {
        g_api->kprintf(ATTR_RED, "%s", "mv: multiple files must be moved into a directory\n");
        return;
    }
    
    for (i = 1; i < argc - 1; i++) {
        const char *src = argv[i];
        
        if (is_dest_dir) {
            char dpath[PATH_MAX_LEN];
            join_path(dpath, dst, get_basename(src));
            if (do_copy_file("mv", src, dpath) == 0) g_api->sys_unlink(src);
        } else {
            if (do_copy_file("mv", src, dst) == 0) g_api->sys_unlink(src);
        }
    }
}

static void cmd_rm(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (is_dir(argv[i])) {
            g_api->kprintf(ATTR_RED, "rm: cannot remove directory %s\n", argv[i]);
        } else {
            int ret = g_api->sys_unlink(argv[i]);
            if (ret != 0) g_api->kprintf(ATTR_RED, "rm: failed to remove %s (%d)\n", argv[i], ret);
        }
    }
}

static void cmd_cat(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        int fd = g_api->sys_open(argv[i], 0);
        int r;
        if (fd < 0) {
            g_api->kprintf(ATTR_RED, "cat: %s not found (err %d)\n", argv[i], fd);
            continue;
        }
        r = g_api->sys_read(fd, io_buf, 65536);
        g_api->sys_close(fd);
        if (r <= 0) {
            continue;
        }
        g_api->sys_write(1, io_buf, r);
    }
}

static void cmd_cat2(int argc, char **argv)
{
    cmd_cat(argc, argv);
}

static void cmd_echo(int argc, char **argv)
{
    int i, redir = 0;
    const char *outfile = 0;
    
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '>' && argv[i][1] == '\0') {
            redir = i;
            if (i + 1 < argc) outfile = argv[i+1];
            break;
        } else if (argv[i][0] == '>' && argv[i][1] != '\0') {
            redir = i;
            outfile = &argv[i][1];
            break;
        }
    }
    
    if (redir) {
        char t[PATH_MAX_LEN]; int tpos = 0;
        if (!outfile) { g_api->kprintf(ATTR_RED, "%s", "echo: missing redirect target\n"); return; }
        for (i = 1; i < redir; i++) {
            const char* s = argv[i];
            while (*s && tpos < PATH_MAX_LEN - 3) t[tpos++] = *s++;
            if (i < redir - 1 && tpos < PATH_MAX_LEN - 3) t[tpos++] = ' ';
        }
        t[tpos++] = '\n'; t[tpos] = '\0';
        {
            int fd = g_api->sys_open(outfile, 1 | 0x0100 | 0x0200);
            if (fd >= 0) {
                g_api->sys_write(fd, t, tpos);
                g_api->sys_close(fd);
            }
        }
    } else {
        for (i = 1; i < argc; i++) {
            int l = 0; while (argv[i][l]) l++;
            g_api->sys_write(1, argv[i], l);
            if (i < argc - 1) g_api->sys_write(1, " ", 1);
        }
        g_api->sys_write(1, "\n", 1);
    }
}

static const ShellCmd file_cmds[] = {
    { "cp",   cmd_cp,   "SRC DST / SRC... DIR", "Copy files" },
    { "mv",   cmd_mv,   "SRC DST / SRC... DIR", "Move files" },
    { "rm",   cmd_rm,   "FILE...",              "Remove files" },
    { "cat",  cmd_cat,  "FILE...",              "Print file contents" },
    { "cat2", cmd_cat2, "FILE...",              "Alias for cat" },
    { "echo", cmd_echo, "[args...] [> FILE]",   "Print or redirect text" },
    { (const char *)0, 0, 0, 0 }
};
void shell_cmd_file_init(void) { shell_register_cmds(file_cmds); }
