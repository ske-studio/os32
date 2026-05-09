#include "cmd_fs_shared.h"
#include "shell.h"
#include <string.h>
#include <stdlib.h>

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
        const char *s = argv[i];
        if (i > 1 && pos < (int)sizeof(cmdline) - 1) cmdline[pos++] = ' ';
        while (*s && pos < (int)sizeof(cmdline) - 1) {
            cmdline[pos++] = *s++;
        }
    }
    cmdline[pos] = '\0';

    rc = g_api->exec_run(cmdline);
    if (rc == EXEC_ERR_GENERAL) g_api->kprintf(ATTR_RED, "%s", "exec: general error\n");
    else if (rc == EXEC_ERR_FAULT) g_api->kprintf(ATTR_RED, "%s", "exec: invalid executable or crashed\n");
    else if (rc == EXEC_ERR_NOT_FOUND) g_api->kprintf(ATTR_RED, "%s", "exec: file not found\n");
    else g_api->kprintf(ATTR_GREEN, "exec: exited with %d\n", rc);
}

/* ======================================================================== */
/*  losetup — ループバックデバイス管理 (Linux losetup 準拠)                 */
/*                                                                          */
/*  losetup <path> <slot>    イメージをアタッチ (フォーマット自動判別)       */
/*  losetup -d <slot>        デタッチ                                       */
/*  losetup -l [slot]        ステータス表示                                 */
/* ======================================================================== */
static void cmd_losetup(int argc, char **argv)
{
    int slot, ret, in_use, bps;
    u32 total;

    if (argc < 2) {
        /* 引数なし: 全スロット表示 */
        for (slot = 0; slot < 4; slot++) {
            in_use = g_api->loop_status(slot, &total, &bps);
            if (in_use) {
                g_api->kprintf(ATTR_GREEN, "lo%d: total_lba=%u bps=%d\n",
                               slot, total, bps);
            } else {
                g_api->kprintf(ATTR_WHITE, "lo%d: (empty)\n", slot);
            }
        }
        return;
    }

    /* -d: デタッチ */
    if (argv[1][0] == '-' && argv[1][1] == 'd') {
        if (argc < 3) {
            g_api->kprintf(ATTR_WHITE, "Usage: losetup -d <slot(0-3)>\n");
            return;
        }
        slot = atoi(argv[2]);
        g_api->loop_detach(slot);
        g_api->kprintf(ATTR_GREEN, "losetup: lo%d detached\n", slot);
        return;
    }

    /* -l: ステータス表示 */
    if (argv[1][0] == '-' && argv[1][1] == 'l') {
        if (argc >= 3) {
            slot   = atoi(argv[2]);
            in_use = g_api->loop_status(slot, &total, &bps);
            if (in_use) {
                g_api->kprintf(ATTR_GREEN, "lo%d: total_lba=%u bps=%d\n",
                               slot, total, bps);
            } else {
                g_api->kprintf(ATTR_WHITE, "lo%d: (empty)\n", slot);
            }
        } else {
            for (slot = 0; slot < 4; slot++) {
                in_use = g_api->loop_status(slot, &total, &bps);
                if (in_use) {
                    g_api->kprintf(ATTR_GREEN, "lo%d: total_lba=%u bps=%d\n",
                                   slot, total, bps);
                } else {
                    g_api->kprintf(ATTR_WHITE, "lo%d: (empty)\n", slot);
                }
            }
        }
        return;
    }

    /* アタッチ: losetup <path> <slot> */
    if (argc < 3) {
        g_api->kprintf(ATTR_WHITE,
            "Usage: losetup <image_path> <slot(0-3)>\n"
            "       losetup -d <slot>          detach\n"
            "       losetup -l [slot]          status\n");
        return;
    }
    slot = atoi(argv[2]);
    ret  = g_api->loop_attach(argv[1], slot);
    switch (ret) {
    case 0:
        g_api->kprintf(ATTR_GREEN, "losetup: lo%d attached: %s\n",
                       slot, argv[1]);
        break;
    case -1:
        g_api->kprintf(ATTR_RED, "losetup: invalid path or slot\n");
        break;
    case -2:
        g_api->kprintf(ATTR_RED, "losetup: unsupported format\n");
        break;
    case -3:
        g_api->kprintf(ATTR_RED, "losetup: slot %d already in use\n", slot);
        break;
    case -4:
        g_api->kprintf(ATTR_RED, "losetup: I/O error\n");
        break;
    default:
        g_api->kprintf(ATTR_RED, "losetup: error %d\n", ret);
        break;
    }
}

/* ======================================================================== */
/*  dd — ブロックデバイスのセクタ読み出し                                  */
/*  使い方: dd <devname> lba=<N> count=<M> [file=<vfs_path>] [noerr]       */
/*           noerr: 読み取りエラーをゼロ埋めしてスキップ                    */
/* ======================================================================== */
static void cmd_dd(int argc, char **argv)
{
    const char *dev_name;
    int lba, count, i;
    char *out_path;
    void *buf;
    u32  bps;
    int  dummy_bps;
    u32  dummy_total;
    int  noerr, err_count;

    if (argc < 4) {
        g_api->kprintf(ATTR_WHITE,
            "Usage: dd <dev> lba=<N> count=<M> [file=<path>] [noerr]\n"
            "  noerr: skip read errors (zero-fill)\n");
        return;
    }

    dev_name  = argv[1];
    lba       = 0;
    count     = 1;
    out_path  = (char *)0;
    noerr     = 0;
    err_count = 0;

    for (i = 2; i < argc; i++) {
        if (strncmp(argv[i], "lba=", 4) == 0) {
            lba = atoi(argv[i] + 4);
        } else if (strncmp(argv[i], "count=", 6) == 0) {
            count = atoi(argv[i] + 6);
        } else if (strncmp(argv[i], "file=", 5) == 0) {
            out_path = argv[i] + 5;
        } else if (strcmp(argv[i], "noerr") == 0) {
            noerr = 1;
        }
    }

    /* デバイスの bps 取得 */
    {
        int slot = -1;
        if (dev_name[0] == 'l' && dev_name[1] == 'o' &&
            dev_name[2] >= '0' && dev_name[2] <= '3' && dev_name[3] == '\0') {
            slot = dev_name[2] - '0';
        }
        if (slot >= 0) {
            if (!g_api->loop_status(slot, &dummy_total, &dummy_bps)) {
                g_api->kprintf(ATTR_RED, "dd: lo%d not attached\n", slot);
                return;
            }
            bps = (u32)dummy_bps;
        } else {
            bps = 1024;
        }
    }

    if (count <= 0) {
        g_api->kprintf(ATTR_RED, "dd: count must be >= 1\n");
        return;
    }
    if (dummy_total > 0 && (u32)(lba + count) > dummy_total) {
        g_api->kprintf(ATTR_RED, "dd: lba+count exceeds total (%u)\n",
                       dummy_total);
        return;
    }

    buf = g_api->mem_alloc(bps);
    if (!buf) {
        g_api->kprintf(ATTR_RED, "dd: alloc failed\n");
        return;
    }

    if (out_path) {
        u32 total_bytes = 0;
        int fd = g_api->sys_open(out_path, 0x0301);
        if (fd < 0) {
            g_api->kprintf(ATTR_RED, "dd: cannot create %s (err=%d)\n",
                           out_path, fd);
            g_api->mem_free(buf);
            return;
        }
        for (i = 0; i < count; i++) {
            if (g_api->dev_blk_read(dev_name, (u32)(lba + i), 1, buf) != 0) {
                if (!noerr) {
                    g_api->kprintf(ATTR_RED, "dd: read error at lba=%d\n",
                                   lba + i);
                    g_api->sys_close(fd);
                    g_api->mem_free(buf);
                    return;
                }
                { u8 *z = (u8 *)buf; u32 k; for (k = 0; k < bps; k++) z[k] = 0; }
                err_count++;
            }
            g_api->sys_write(fd, buf, bps);
            total_bytes += bps;
        }
        g_api->sys_close(fd);
        if (err_count > 0)
            g_api->kprintf(ATTR_YELLOW,
                "dd: wrote %u bytes -> %s (%d errors skipped)\n",
                total_bytes, out_path, err_count);
        else
            g_api->kprintf(ATTR_GREEN, "dd: wrote %u bytes -> %s\n",
                           total_bytes, out_path);
    } else {
        u32 global_off = 0;
        for (i = 0; i < count; i++) {
            u8 *p;
            u32 off;
            if (g_api->dev_blk_read(dev_name, (u32)(lba + i), 1, buf) != 0) {
                if (!noerr) {
                    g_api->kprintf(ATTR_RED, "dd: read error at lba=%d\n",
                                   lba + i);
                    g_api->mem_free(buf);
                    return;
                }
                { u8 *z = (u8 *)buf; u32 k; for (k = 0; k < bps; k++) z[k] = 0; }
                err_count++;
            }
            p = (u8 *)buf;
            for (off = 0; off < bps; off += 16) {
                u32 end = off + 16;
                u32 k;
                if (end > bps) end = bps;
                g_api->kprintf(ATTR_WHITE, "%05X: ", global_off + off);
                for (k = off; k < end; k++)
                    g_api->kprintf(ATTR_WHITE, "%02X ", (unsigned)p[k]);
                for (k = end; k < off + 16; k++)
                    g_api->kprintf(ATTR_WHITE, "   ");
                g_api->kprintf(ATTR_CYAN, " ");
                for (k = off; k < end; k++) {
                    u8 c = p[k];
                    g_api->kprintf(ATTR_CYAN, "%c",
                                   (c >= 0x20 && c < 0x7F) ? (char)c : '.');
                }
                g_api->kprintf(ATTR_WHITE, "\n");
            }
            global_off += bps;
        }
        if (err_count > 0)
            g_api->kprintf(ATTR_YELLOW,
                "dd: %u bytes from %s lba=%d count=%d (%d errors)\n",
                global_off, dev_name, lba, count, err_count);
        else
            g_api->kprintf(ATTR_GREEN,
                "dd: %u bytes from %s lba=%d count=%d\n",
                global_off, dev_name, lba, count);
    }

    g_api->mem_free(buf);
}


static const ShellCmd mnt_cmds[] = {
    { "mount",     cmd_mount,     "PREFIX DEV FS",                    "Mount a filesystem" },
    { "umount",    cmd_umount,    "PREFIX",                           "Unmount a filesystem" },
    { "sync",      cmd_sync,      "",                                 "Sync file buffers to disk" },
    { "exec",      cmd_exec,      "FILE.BIN",                         "Execute a binary program" },
    { "losetup",   cmd_losetup,   "<path> <slot> | -d <slot> | -l",   "Loop device management" },
    { "dd",        cmd_dd,        "<dev> lba=N count=M [file=PATH]",  "Block device sector read" },
    { (const char *)0, 0, 0, 0 }
};
void shell_cmd_mnt_init(void) { shell_register_cmds(mnt_cmds); }
