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

static void vfs_ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    char size_buf[16];
    int use_long = ctx ? *(int*)ctx : 0;
    
    if (use_long) {
        if (entry->type == 2) { /* DIR */
            if (g_api->sys_isatty(1)) {
                g_api->kprintf(ATTR_CYAN, "d      <DIR> %s\n", entry->name);
            } else {
                printf("d      <DIR> %s\n", entry->name);
            }
        } else {
            format_size(entry->size, size_buf, 10);
            if (g_api->sys_isatty(1)) {
                g_api->kprintf(ATTR_WHITE, "- %s %s\n", size_buf, entry->name);
            } else {
                printf("- %s %s\n", size_buf, entry->name);
            }
        }
    } else {
        if (entry->type == 2) {
            if (g_api->sys_isatty(1)) {
                g_api->kprintf(ATTR_CYAN, "%s/  ", entry->name);
            } else {
                printf("%s/  ", entry->name);
            }
        } else {
            if (g_api->sys_isatty(1)) {
                g_api->kprintf(ATTR_WHITE, "%s  ", entry->name);
            } else {
                printf("%s  ", entry->name);
            }
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

    fd_out = g_api->sys_open(dst, 1 | 0x0100 | 0x0200); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd_out < 0) {
        g_api->kprintf(ATTR_RED, "%s: open failed %s\n", cmd_name, dst);
        g_api->sys_close(fd_in);
        return -1;
    }

    while (1) {
        sz = g_api->sys_read(fd_in, io_buf, 65536);
        if (sz < 0) {
            g_api->kprintf(ATTR_RED, "%s: read failed %s\n", cmd_name, src);
            break;
        }
        if (sz == 0) break; /* EOF */

        if (g_api->sys_write(fd_out, io_buf, sz) != sz) {
            g_api->kprintf(ATTR_RED, "%s: write failed %s\n", cmd_name, dst);
            break;
        }
    }

    g_api->sys_close(fd_in);
    g_api->sys_close(fd_out);
    return 0;
}

/* 再帰コピー: エントリ収集方式 */
#define MAX_COPY_ENTRIES 64
#define MAX_COPY_DEPTH   8

struct copy_entry {
    char name[32];
    int  is_dir;   /* 1=ディレクトリ, 0=ファイル */
};

/* 収集用バッファ (スタック節約のため static) */
static struct copy_entry g_copy_entries[MAX_COPY_ENTRIES];
static int g_copy_count;

/* sys_ls コールバック: エントリを収集するだけ */
static void collect_entries_cb(const DirEntry_Ext *entry, void *ctx)
{
    int nlen;
    (void)ctx;

    /* . と .. をスキップ */
    if (entry->name[0] == '.' &&
        (entry->name[1] == '\0' || (entry->name[1] == '.' && entry->name[2] == '\0')))
        return;

    if (g_copy_count >= MAX_COPY_ENTRIES) return;

    nlen = strlen(entry->name);
    if (nlen >= 31) nlen = 31;
    memcpy(g_copy_entries[g_copy_count].name, entry->name, nlen);
    g_copy_entries[g_copy_count].name[nlen] = '\0';
    g_copy_entries[g_copy_count].is_dir = (entry->type == 2) ? 1 : 0;
    g_copy_count++;
}

/* ディレクトリの再帰コピー (collect-then-copy) */
static void do_copy_recursive_impl(const char *src, const char *dst, int depth)
{
    /* ローカルにコピーしてから再帰 (static バッファを再帰で上書き対策) */
    struct copy_entry local_entries[MAX_COPY_ENTRIES];
    int local_count, i;

    if (depth >= MAX_COPY_DEPTH) {
        g_api->kprintf(ATTR_RED, "cp: max depth exceeded: %s\n", src);
        return;
    }

    /* 宛先ディレクトリを作成 */
    g_api->sys_mkdir(dst);

    /* エントリを全て収集 (static バッファに) */
    g_copy_count = 0;
    g_api->sys_ls(src, collect_entries_cb, (void *)0);

    /* ローカルにコピー (再帰で g_copy_entries が上書きされるため) */
    local_count = g_copy_count;
    for (i = 0; i < local_count; i++) {
        local_entries[i] = g_copy_entries[i];
    }

    /* 収集後にコピーを実行 */
    for (i = 0; i < local_count; i++) {
        char src_path[PATH_MAX_LEN];
        char dst_path[PATH_MAX_LEN];

        join_path(src_path, src, local_entries[i].name);
        join_path(dst_path, dst, local_entries[i].name);

        if (local_entries[i].is_dir) {
            do_copy_recursive_impl(src_path, dst_path, depth + 1);
        } else {
            do_copy_file("cp", src_path, dst_path);
        }
    }
}

static void do_copy_recursive(const char *src, const char *dst)
{
    do_copy_recursive_impl(src, dst, 0);
}

static void cmd_cp(int argc, char **argv)
{
    int i, is_dest_dir;
    int opt_recursive = 0;
    int file_start = 1;
    const char *dst;
    
    if (argc < 3) {
        shell_print_help(argv[0]);
        return;
    }
    
    /* オプション解析 */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r' || argv[i][j] == 'R')
                    opt_recursive = 1;
            }
            file_start = i + 1;
        } else {
            break;
        }
    }
    
    if (argc - file_start < 2) {
        shell_print_help(argv[0]);
        return;
    }
    
    dst = argv[argc - 1];
    is_dest_dir = is_dir(dst);
    
    if (argc - file_start > 2 && !is_dest_dir) {
        g_api->kprintf(ATTR_RED, "%s", "cp: multiple files must be copied into a directory\n");
        return;
    }
    
    for (i = file_start; i < argc - 1; i++) {
        const char *src = argv[i];
        if (argv[i][0] == '-') continue; /* オプションをスキップ */
        
        if (is_dir(src)) {
            if (!opt_recursive) {
                g_api->kprintf(ATTR_RED, "cp: -r not specified; omitting directory '%s'\n", src);
                continue;
            }
            /* 再帰コピー */
            if (is_dest_dir) {
                char dpath[PATH_MAX_LEN];
                join_path(dpath, dst, get_basename(src));
                do_copy_recursive(src, dpath);
            } else {
                do_copy_recursive(src, dst);
            }
        } else {
            if (is_dest_dir) {
                char dpath[PATH_MAX_LEN];
                join_path(dpath, dst, get_basename(src));
                do_copy_file("cp", src, dpath);
            } else {
                do_copy_file("cp", src, dst);
            }
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
/* バッファを行番号付きで出力 */
static void cat_with_linenum(const u8 *data, int len, int *line_num)
{
    int i, start;
    char num_buf[12];
    int nlen, j;

    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            /* 行番号を出力 */
            nlen = 0;
            {
                int n = *line_num;
                char tmp[12];
                int ti = 0;
                if (n == 0) tmp[ti++] = '0';
                while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
                /* 6桁右寄せ */
                for (j = 0; j < 6 - ti; j++) num_buf[nlen++] = ' ';
                while (ti > 0) num_buf[nlen++] = tmp[--ti];
            }
            num_buf[nlen++] = ' ';
            num_buf[nlen++] = ' ';
            g_api->sys_write(1, num_buf, nlen);

            /* 行の内容を出力 */
            if (i > start) {
                g_api->sys_write(1, &data[start], i - start);
            }
            g_api->sys_write(1, "\n", 1);
            start = i + 1;
            (*line_num)++;
        }
    }
}

static void cmd_cat(int argc, char **argv)
{
    int i;
    int show_linenum = 0;
    int file_start = 1;

    /* オプション解析 */
    if (argc > 1 && argv[1][0] == '-') {
        int j;
        for (j = 1; argv[1][j]; j++) {
            if (argv[1][j] == 'n') show_linenum = 1;
        }
        file_start = 2;
    }

    for (i = file_start; i < argc; i++) {
        int fd = g_api->sys_open(argv[i], 0);
        int r;
        if (fd < 0) {
            g_api->kprintf(ATTR_RED, "cat: %s not found (err %d)\n", argv[i], fd);
            continue;
        }
        
        {
            int line_num = 1;
            while (1) {
                r = g_api->sys_read(fd, io_buf, 65536);
                if (r <= 0) break;
                
                if (show_linenum) {
                    cat_with_linenum(io_buf, r, &line_num);
                } else {
                    g_api->sys_write(1, io_buf, r);
                }
            }
        }
        g_api->sys_close(fd);
    }
}

static void cmd_cat2(int argc, char **argv)
{
    cmd_cat(argc, argv);
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        int l = 0;
        while (argv[i][l]) l++;
        if (l > 0) {
            g_api->sys_write(1, argv[i], l);
        }
        if (i < argc - 1) g_api->sys_write(1, " ", 1);
    }
    g_api->sys_write(1, "\n", 1);
}

static const ShellCmd file_cmds[] = {
    { "cp",   cmd_cp,   "[-r] SRC DST / SRC... DIR", "Copy files" },
    { "mv",   cmd_mv,   "SRC DST / SRC... DIR", "Move files" },
    { "rm",   cmd_rm,   "FILE...",              "Remove files" },
    { "cat",  cmd_cat,  "[-n] FILE...",         "Print file contents" },
    { "cat2", cmd_cat2, "[-n] FILE...",         "Alias for cat" },
    { "echo", cmd_echo, "[args...] [> FILE]",   "Print or redirect text" },
    { (const char *)0, 0, 0, 0 }
};
void shell_cmd_file_init(void) { shell_register_cmds(file_cmds); }
