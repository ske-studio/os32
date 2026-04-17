#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  INSTALL.C — OS32 HDDインストーラー v4.0                                  */
/*                                                                          */
/*  FAT12フロッピー (サブディレクトリ構造) から起動して実行し、               */
/*  IDEドライブにシステムを書き込む。                                        */
/*                                                                          */
/*  v4.0: FDDサブディレクトリ対応                                            */
/*    /sys/boot_hdd.bin, /sys/loader_h.bin → HDDブートセクタ                */
/*    /kernel.bin → HDD LBA 6+                                              */
/*    /sys/, /bin/, /sbin/, /etc/ → /hd0/ 配下にディレクトリごとコピー       */
/* ======================================================================== */

#include "os32api.h"

#define IDE_DRIVE 0

/* PC-98ジオメトリ定数 (NHD: H=8, S=17) */
#define PC98_HEADS   8
#define PC98_SECTORS 17
#define PC98_CYL0_SECTORS  (PC98_HEADS * PC98_SECTORS)  /* 136 */
#define PC98_BOOT_CYLS     2
#define HDD_PARTITION_LBA  (PC98_CYL0_SECTORS * PC98_BOOT_CYLS)  /* 272 */

/* IOバッファ (ヒープから確保する) */
static u8 *file_buf;
#define FILE_BUF_SIZE (128 * 1024)  /* 128KB */

#define MAX_FILES 64
typedef struct {
    char names[MAX_FILES][32];
    u8   types[MAX_FILES];  /* 1=FILE, 2=DIR */
    int count;
} FileList;

/* IdeInfoの定義 */
typedef struct {
    u32 total_sectors;
    u16 cylinders;
    u16 heads;
    u16 sectors;
    u32 size_mb;
    char model[41];
    char serial[21];
    char firmware[9];
    int lba_supported;
} IdeInfoTemp;

static KernelAPI *g_api;

/* ======== 文字列・パスユーティリティ ======== */

static int str_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static void str_cat(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) { *dst++ = *src++; }
    *dst = '\0';
}

static void str_cpy(char *dst, const char *src) {
    while (*src) { *dst++ = *src++; }
    *dst = '\0';
}

static int str_endswith_ci(const char *s, const char *suffix) {
    int slen = str_len(s);
    int suflen = str_len(suffix);
    int i;
    char c1, c2;
    if (suflen > slen) return 0;
    for (i = 0; i < suflen; i++) {
        c1 = s[slen - suflen + i];
        c2 = suffix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return 1;
}

static void ls_cb(const DirEntry_Ext *entry, void *ctx) {
    FileList *fl = (FileList *)ctx;
    int i;
    if (fl->count >= MAX_FILES) return;
    i = 0;
    while (entry->name[i] && i < 31) {
        fl->names[fl->count][i] = entry->name[i];
        i++;
    }
    fl->names[fl->count][i] = '\0';
    fl->types[fl->count] = entry->type;
    fl->count++;
}

/* ======== Stream I/O コピー ======== */

static int copy_file(const char *src, const char *dst) {
    int fd_src, fd_dst;
    int read_len;
    int total = 0;

    fd_src = g_api->sys_open(src, O_RDONLY);
    if (fd_src < 0) return -1;

    fd_dst = g_api->sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd_dst < 0) {
        g_api->sys_close(fd_src);
        return -2;
    }

    while (1) {
        read_len = g_api->sys_read(fd_src, file_buf, FILE_BUF_SIZE);
        if (read_len <= 0) break;
        if (g_api->sys_write(fd_dst, file_buf, read_len) != read_len) {
            g_api->sys_close(fd_src);
            g_api->sys_close(fd_dst);
            return -3;
        }
        total += read_len;
    }

    g_api->sys_close(fd_src);
    g_api->sys_close(fd_dst);
    return total;
}

static int read_file_to_buf(const char *path, int max_size) {
    int fd = g_api->sys_open(path, 0);
    int len;
    if (fd < 0) return fd;
    len = g_api->sys_read(fd, file_buf, max_size);
    g_api->sys_close(fd);
    return len;
}

/* ======== ブートセクタ書き込み ======== */

static int write_partition_table(IdeInfoTemp *info)
{
    u8 *pt = file_buf;
    u32 end_cyl;
    int i;
    for (i = 0; i < 512; i++) pt[i] = 0;

    pt[0] = 0x80;
    pt[1] = 0xE2;
    pt[6] = 0;
    pt[7] = 0;
    pt[8] = PC98_BOOT_CYLS;
    pt[9] = 0;

    end_cyl = (info->total_sectors / PC98_CYL0_SECTORS) - 1;
    pt[10] = (u8)(PC98_SECTORS - 1);
    pt[11] = (u8)(PC98_HEADS - 1);
    pt[12] = (u8)(end_cyl & 0xFF);
    pt[13] = (u8)((end_cyl >> 8) & 0xFF);

    pt[16] = 'O'; pt[17] = 'S'; pt[18] = '3'; pt[19] = '2';
    for (i = 20; i < 32; i++) pt[i] = ' ';

    if (g_api->ide_write_sectors(IDE_DRIVE, 1, 1, pt) != 0) return -1;
    g_api->kprintf(0x0A, "  Written Partition Table (LBA 1, end cyl=%u)\n", end_cyl);
    return 0;
}

/* ======== 安全ロック ======== */

static int confirm_install(void) {
    int key;
    g_api->kprintf(ATTR_RED, "%s", "\nWARNING: All data on hd0 will be ERASED during installation!\n");
    g_api->kprintf(ATTR_YELLOW, "%s", "Do you want to proceed? [y/N]: ");
    while (1) {
        key = g_api->kbd_trygetchar();
        if (key <= 0) key = g_api->serial_trygetchar();

        if (key == 'y' || key == 'Y') {
            g_api->kprintf(ATTR_YELLOW, "%s", "Y\n\n");
            return 1;
        } else if (key == 'n' || key == 'N' || key == 0x0D || key == 0x1B) {
            g_api->kprintf(ATTR_YELLOW, "%s", "N\n\n");
            return 0;
        }
    }
}

/* ======== ディレクトリ再帰コピー ======== */

/* FDDのsrc_dir配下のファイル/ディレクトリを /hd0/dst_dir 配下にコピー */
static int copy_directory(const char *src_dir, const char *dst_dir, int depth)
{
    FileList fl;
    int i, ok_count = 0;

    if (depth > 4) return 0; /* 無限再帰防止 */

    fl.count = 0;
    g_api->sys_ls(src_dir, ls_cb, &fl);

    for (i = 0; i < fl.count; i++) {
        char src_path[128];
        char dst_path[128];

        /* "." と ".." をスキップ */
        if (fl.names[i][0] == '.') {
            if (fl.names[i][1] == '\0') continue;
            if (fl.names[i][1] == '.' && fl.names[i][2] == '\0') continue;
        }

        /* ソースパス構築 */
        str_cpy(src_path, src_dir);
        if (src_path[str_len(src_path) - 1] != '/') str_cat(src_path, "/");
        str_cat(src_path, fl.names[i]);

        /* 宛先パス構築: /hd0 + dst_dir + / + name */
        str_cpy(dst_path, "/hd0");
        str_cat(dst_path, dst_dir);
        if (dst_path[str_len(dst_path) - 1] != '/') str_cat(dst_path, "/");
        str_cat(dst_path, fl.names[i]);

        if (fl.types[i] == OS32_FILE_TYPE_DIR) {
            /* サブディレクトリ: HDD側にmkdirして再帰 */
            g_api->sys_mkdir(dst_path);

            {
                char sub_dst[128];
                str_cpy(sub_dst, dst_dir);
                if (sub_dst[str_len(sub_dst) - 1] != '/') str_cat(sub_dst, "/");
                str_cat(sub_dst, fl.names[i]);
                copy_directory(src_path, sub_dst, depth + 1);
            }
        } else {
            /* ファイル: コピー */
            int bytes = copy_file(src_path, dst_path);
            if (bytes >= 0) {
                g_api->kprintf(ATTR_GREEN, "  [OK] %s -> %s (%d b)\n", src_path, dst_path, bytes);
                ok_count++;
            } else {
                g_api->kprintf(ATTR_RED, "  [FAIL] %s -> %s (code: %d)\n", src_path, dst_path, bytes);
            }
        }
    }

    return ok_count;
}

/* ======== メイン ======== */

void __cdecl main(int argc, char **argv, KernelAPI *api)
{
    static IdeInfoTemp info;
    int ret, i;
    int loader_sects, kernel_sects;

    (void)argc;
    (void)argv;
    g_api = api;

    api->kprintf(ATTR_CYAN, "%s", "\n========================================\n");
    api->kprintf(ATTR_CYAN, "%s", "      OS32 HDD Installer v4.0        \n");
    api->kprintf(ATTR_CYAN, "%s", "========================================\n\n");

    file_buf = (u8 *)api->mem_alloc(FILE_BUF_SIZE);
    if (!file_buf) {
        api->kprintf(0x4F, "%s", "Error: Out of memory\n");
        return;
    }

    /* IDEの初期化とディスク情報の取得 */
    api->kprintf(0x07, "%s", "Initializing IDE controller...\n");
    api->ide_init();

    if (!api->ide_drive_present(IDE_DRIVE)) {
        api->kprintf(0x4F, "%s", "Error: IDE Master Drive (hd0) not found.\n");
        goto end;
    }

    if ((ret = api->ide_identify(IDE_DRIVE, &info)) != 0) {
        api->kprintf(0x4F, "%s", "Error: Could not identify drive.\n");
        goto end;
    }

    api->kprintf(ATTR_WHITE, "Target Drive Found: %s\n", info.model);
    api->kprintf(ATTR_WHITE, "Size: %u MB (%u sectors)\n", info.size_mb, info.total_sectors);

    if (info.total_sectors < 10000) {
         api->kprintf(0x4F, "%s", "Error: Drive is too small.\n");
         goto end;
    }

    /* 安全ロック */
    if (!confirm_install()) {
        api->kprintf(0x07, "%s", "Installation aborted.\n");
        goto end;
    }

    /* === Phase 1: ブートセクタ書き込み === */
    api->kprintf(ATTR_YELLOW, "%s", "[1/3] Writing Boot Sectors...\n");

    /* boot_hdd.bin → LBA 0 */
    ret = read_file_to_buf("/sys/boot_hdd.bin", FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing /sys/boot_hdd.bin\n"); goto end; }
    if (ret < 512) { for (i = ret; i < 512; i++) file_buf[i] = 0; }
    file_buf[8] = (u8)info.heads;
    file_buf[9] = (u8)info.sectors;
    file_buf[510] = 0x55; file_buf[511] = 0xAA;
    if (api->ide_write_sectors(IDE_DRIVE, 0, 1, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written IPL (LBA 0, patched geom %d/%d)\n", info.heads, info.sectors);

    /* Partition Table → LBA 1 */
    if (write_partition_table(&info) != 0) goto ioerr;

    /* loader_h.bin → LBA 2+ */
    ret = read_file_to_buf("/sys/loader_h.bin", FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing /sys/loader_h.bin\n"); goto end; }
    loader_sects = (ret + 511) / 512;
    if (api->ide_write_sectors(IDE_DRIVE, 2, loader_sects, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written LOADER (LBA 2, %d bytes)\n", ret);

    /* kernel.bin → LBA 6+ */
    ret = read_file_to_buf("/kernel.bin", FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing /kernel.bin\n"); goto end; }
    kernel_sects = (ret + 511) / 512;
    if (api->ide_write_sectors(IDE_DRIVE, 6, kernel_sects, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written KERNEL (LBA 6, %d bytes)\n", ret);

    /* === Phase 2: ext2フォーマット === */
    api->kprintf(ATTR_YELLOW, "%s", "\n[2/3] Formatting Ext2 Filesystem...\n");
    ret = api->ext2_format(IDE_DRIVE, info.total_sectors - HDD_PARTITION_LBA);
    if (ret != 0) {
        api->kprintf(0x4F, "%s", "Error: Format failed.\n");
        goto end;
    }
    api->kprintf(0x0A, "%s", "  Format OK.\n");

    /* === Phase 3: ファイルコピー === */
    api->kprintf(ATTR_YELLOW, "%s", "\n[3/3] Copying files to HDD...\n");

    /* HDDを /hd0 にマウント */
    if (api->sys_mount("/hd0", "hd0", "ext2") != 0) {
        api->kprintf(0x4F, "%s", "Error: Failed to mount hd0.\n");
        goto end;
    }

    /* HDD側ディレクトリ構造を作成 */
    api->kprintf(0x0A, "%s", "  Creating directories...\n");
    api->sys_mkdir("/hd0/sys");
    api->sys_mkdir("/hd0/bin");
    api->sys_mkdir("/hd0/sbin");
    api->sys_mkdir("/hd0/etc");
    api->sys_mkdir("/hd0/usr");
    api->sys_mkdir("/hd0/usr/bin");
    api->sys_mkdir("/hd0/usr/man");
    api->sys_mkdir("/hd0/data");
    api->sys_mkdir("/hd0/home");
    api->sys_mkdir("/hd0/home/user");
    api->sys_mkdir("/hd0/tmp");

    /* FDDのサブディレクトリ構造をそのままHDDにコピー */
    /* /sys/ → /hd0/sys/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /sys/ ...\n");
    copy_directory("/sys", "/sys", 0);

    /* /bin/ → /hd0/bin/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /bin/ ...\n");
    copy_directory("/bin", "/bin", 0);

    /* /sbin/ → /hd0/sbin/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /sbin/ ...\n");
    copy_directory("/sbin", "/sbin", 0);

    /* /etc/ → /hd0/etc/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /etc/ ...\n");
    copy_directory("/etc", "/etc", 0);

    /* ファイルシステム同期 */
    api->kprintf(ATTR_CYAN, "%s", "  Syncing filesystem...\n");
    api->vfs_sync();

    api->kprintf(0x7E, "%s", "\n========================================\n");
    api->kprintf(ATTR_CYAN, "%s", " OS32 Installation Complete!            \n");
    api->kprintf(ATTR_CYAN, "%s", " Remove the Floppy Disk and reboot.     \n");
    api->kprintf(0x7E, "%s", "========================================\n");
    goto end;

ioerr:
    api->kprintf(0x4F, "%s", "Error: IDE Write I/O Error\n");

end:
    api->mem_free(file_buf);
}
