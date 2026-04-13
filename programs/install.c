#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  INSTALL.C — OS32 HDDインストーラー v3.0                                  */
/*                                                                          */
/*  FAT12フロッピーから起動して実行し、IDEドライブにシステムを書き込む。     */
/*  - フォーマット前のユーザー確認追加                                       */
/*  - /boot ディレクトリへのブート系ファイルの分離                           */
/*  - file_ls を用いた全ファイルの動的抽出＆ Stream I/O による安定コピー     */
/* ======================================================================== */

#include "os32api.h"

#define IDE_DRIVE 0

/* PC-98ジオメトリ定数 (NHD: H=8, S=17) */
#define PC98_HEADS   8
#define PC98_SECTORS 17
#define PC98_CYL0_SECTORS  (PC98_HEADS * PC98_SECTORS)  /* 136 */
#define PC98_BOOT_CYLS     2  /* ブート用に確保するシリンダ数 (IPL+PT+loader+kernel) */

/* ext2パーティション開始 = シリンダ1 */
#define HDD_PARTITION_LBA  (PC98_CYL0_SECTORS * PC98_BOOT_CYLS)  /* 272: kernel 155セクタを収容 */

/* IOバッファ (ヒープから確保する) */
static u8 *file_buf;
#define FILE_BUF_SIZE (128 * 1024)  /* 128KB */

#define MAX_FILES 64
typedef struct {
    char names[MAX_FILES][32];
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

/* ======== 文字列・パスユーティリティ ======== */

static void str_cat(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) { *dst++ = *src++; }
    *dst = '\0';
}

/* ファイル名を小文字に変換 (ext2は大文字小文字を区別するため) */
static void str_tolower(char *s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z') *s += 32;
        s++;
    }
}

static int is_boot_file(const char *name) {
    int i, j = 0;
    char c1, c2;
    const char *boot_files[] = { "KERNEL.BIN", "BOOT_HDD.BIN", "LOADER_H.BIN", "LOADER.BIN", NULL };
    while (boot_files[j]) {
        int match = 1;
        for (i = 0; boot_files[j][i] || name[i]; i++) {
            c1 = name[i];
            c2 = boot_files[j][i];
            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
            if (c1 != c2) { match = 0; break; }
        }
        if (match) return 1;
        j++;
    }
    return 0;
}

static void ls_cb(const DirEntry_Ext *entry, void *ctx) {
    FileList *fl = (FileList *)ctx;
    if (fl->count < MAX_FILES && entry->type == 1) { /* 1 = FILE */
        int i = 0;
        while (entry->name[i] && i < 31) {
            fl->names[fl->count][i] = entry->name[i];
            i++;
        }
        fl->names[fl->count][i] = '\0';
        fl->count++;
    }
}

/* ======== Stream I/O コピー ======== */

static int copy_file(KernelAPI *api, const char *src, const char *dst, u8 *buf, int buf_size) {
    int fd_src, fd_dst;
    int read_len;
    int total = 0;

    fd_src = api->sys_open(src, O_RDONLY);
    if (fd_src < 0) {
        return -1;
    }
    
    fd_dst = api->sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd_dst < 0) {
        api->sys_close(fd_src);
        return -2;
    }

    while (1) {
        read_len = api->sys_read(fd_src, buf, buf_size);
        if (read_len <= 0) break;
        if (api->sys_write(fd_dst, buf, read_len) != read_len) {
            api->sys_close(fd_src);
            api->sys_close(fd_dst);
            return -3;
        }
        total += read_len;
    }

    api->sys_close(fd_src);
    api->sys_close(fd_dst);
    return total;
}

static int do_read_file(KernelAPI *api, const char *path, u8 *buf, int max_size) {
    int fd = api->sys_open(path, 0);
    int read_len;
    if (fd < 0) return fd;
    read_len = api->sys_read(fd, buf, max_size);
    api->sys_close(fd);
    return read_len;
}


/* ======== インストール手続き ======== */

static int confirm_install(KernelAPI *api) {
    int key;
    api->kprintf(ATTR_RED, "%s", "\nWARNING: All data on hd0 will be ERASED during installation!\n");
    api->kprintf(ATTR_YELLOW, "%s", "Do you want to step into installation? [y/N]: ");
    while (1) {
        key = api->kbd_trygetchar();
        if (key <= 0) key = api->serial_trygetchar();

        if (key == 'y' || key == 'Y') {
            api->kprintf(ATTR_YELLOW, "%s", "Y\n\n");
            return 1;
        } else if (key == 'n' || key == 'N' || key == 0x0D || key == 0x1B) {
            api->kprintf(ATTR_YELLOW, "%s", "N\n\n");
            return 0;
        }
    }
}

static int write_partition_table(KernelAPI *api, IdeInfoTemp *info)
{
    u8 *pt = file_buf;
    u32 end_cyl;
    int i;
    for (i = 0; i < 512; i++) pt[i] = 0;

    /* PC-98 パーティションテーブルエントリ (32バイト)
     * offset 0:    bootable (bit7=boot)
     * offset 1:    system type
     * offset 2-5:  reserved / IPC
     * offset 6:    start sector (0開始)
     * offset 7:    start head
     * offset 8-9:  start cylinder (LE 16bit)
     * offset 10:   end sector
     * offset 11:   end head
     * offset 12-13: end cylinder (LE 16bit)
     * offset 14-15: reserved
     * offset 16-31: volume name */
    pt[0] = 0x80; /* bootable */
    pt[1] = 0xE2; /* system type: ext2 */

    /* 開始CHS: シリンダ2, ヘッド0, セクタ0 (シリンダ0-1はブート用) */
    pt[6] = 0;                          /* start sector (0開始) */
    pt[7] = 0;                          /* start head */
    pt[8] = PC98_BOOT_CYLS;            /* start cylinder low */
    pt[9] = 0;                          /* start cylinder high */

    /* 終了CHS */
    end_cyl = (info->total_sectors / PC98_CYL0_SECTORS) - 1;
    pt[10] = (u8)(PC98_SECTORS - 1);    /* end sector */
    pt[11] = (u8)(PC98_HEADS - 1);      /* end head */
    pt[12] = (u8)(end_cyl & 0xFF);      /* end cylinder low */
    pt[13] = (u8)((end_cyl >> 8) & 0xFF); /* end cylinder high */

    pt[16] = 'O'; pt[17] = 'S'; pt[18] = '3'; pt[19] = '2';
    for (i = 20; i < 32; i++) pt[i] = ' ';

    if (api->ide_write_sectors(IDE_DRIVE, 1, 1, pt) != 0) return -1;
    api->kprintf(0x0A, "  Written Partition Table (LBA 1, end cyl=%u)\n", end_cyl);
    return 0;
}

void __cdecl main(int argc, char **argv, KernelAPI *api)
{
    static IdeInfoTemp info;
    FileList fl;
    int ret, i;
    int loader_sects, kernel_sects;

    api->kprintf(ATTR_CYAN, "%s", "\n========================================\n");
    api->kprintf(ATTR_CYAN, "%s", "      OS32 HDD Installer v3.0        \n");
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

    /* 安全ロック：ユーザー確認 */
    if (!confirm_install(api)) {
        api->kprintf(0x07, "%s", "Installation aborted.\n");
        goto end;
    }

    /* === Phase 1: コピー対象ファイルの事前列挙 === */
    api->kprintf(ATTR_YELLOW, "%s", "[1/4] Scanning source floppy files...\n");
    fl.count = 0;
    api->sys_ls("/", ls_cb, &fl);
    api->kprintf(0x0A, "  Found %d file(s) to install.\n", fl.count);

    /* === Phase 2: シリンダ0 (IPL + パーティション + ローダ + カーネル) === */
    api->kprintf(ATTR_YELLOW, "%s", "\n[2/4] Writing Boot Sectors (Cylinder 0)...\n");

    /* boot_hdd.bin */
    ret = do_read_file(api,"boot_hdd.bin", file_buf, FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing boot_hdd.bin\n"); goto end; }
    if (ret < 512) { for (i = ret; i < 512; i++) file_buf[i] = 0; }
    
    /* ジオメトリ情報のパッチ (offset 8: Heads, 9: SPT) */
    file_buf[8] = (u8)info.heads;
    file_buf[9] = (u8)info.sectors;
    
    file_buf[510] = 0x55; file_buf[511] = 0xAA;
    if (api->ide_write_sectors(IDE_DRIVE, 0, 1, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written IPL (LBA 0, patched geom %d/%d)\n", info.heads, info.sectors);

    /* Partition Table */
    if (write_partition_table(api, &info) != 0) goto ioerr;

    /* loader_h.bin */
    ret = do_read_file(api,"loader_h.bin", file_buf, FILE_BUF_SIZE);
    if (ret <= 0) {
        ret = do_read_file(api,"loader.bin", file_buf, FILE_BUF_SIZE);
        if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing loader_h.bin\n"); goto end; }
    }
    
    /* 注意: 最新の loader_hdd.asm ではジオメトリはレジスタ渡しになったためパッチ不要 */

    loader_sects = (ret + 511) / 512;
    if (api->ide_write_sectors(IDE_DRIVE, 2, loader_sects, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written LOADER_H.BIN (LBA 2, %d bytes)\n", ret);

    /* kernel.bin */
    ret = do_read_file(api,"kernel.bin", file_buf, FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing kernel.bin\n"); goto end; }
    kernel_sects = (ret + 511) / 512;
    if (api->ide_write_sectors(IDE_DRIVE, 6, kernel_sects, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written KERNEL.BIN   (LBA 6, %d bytes)\n", ret);

    /* === Phase 3: ext2フォーマット (シリンダ1以降) === */
    api->kprintf(ATTR_YELLOW, "%s", "\n[3/4] Formatting Ext2 Filesystem...\n");
    ret = api->ext2_format(IDE_DRIVE, info.total_sectors - HDD_PARTITION_LBA);
    if (ret != 0) {
        api->kprintf(0x4F, "%s", "Error: Format failed.\n");
        goto end;
    }
    api->kprintf(0x0A, "%s", "  Format OK.\n");

    /* === Phase 4: 全ファイルの階層的コピー === */
    api->kprintf(ATTR_YELLOW, "%s", "\n[4/4] Activating HDD and copying files...\n");

    /* HDDを /hd0/ として独立マウント (フロッピーへのアクセスは残す) */
    if (api->sys_mount("/hd0", "hd0", "ext2") != 0) {
        api->kprintf(0x4F, "%s", "Error: Failed to mount hd0.\n");
        goto end;
    }

    /* ブートディレクトリ作成 (/hd0自体はマウントポイントなのでmkdir不要) */
    api->sys_mkdir("/hd0/boot"); 
    api->kprintf(0x0A, "  Created /boot directory on hd0.\n");

    /* 各ファイルの転送ループ */
    for (i = 0; i < fl.count; i++) {
        char src_path[256];
        char dst_path[256];
        int bytes;

        src_path[0] = '/'; src_path[1] = '\0';
        str_cat(src_path, fl.names[i]);

        dst_path[0] = '\0';
        if (is_boot_file(fl.names[i])) {
            str_cat(dst_path, "/hd0/boot/");
        } else {
            str_cat(dst_path, "/hd0/");
        }
        str_cat(dst_path, fl.names[i]);
        /* FAT12は大文字名を返すが、ext2/カーネルは小文字パスを使用 */
        str_tolower(dst_path + 5); /* "/hd0/" の後のファイル名部分を小文字化 */

        bytes = copy_file(api, src_path, dst_path, file_buf, FILE_BUF_SIZE);
        if (bytes >= 0) {
            api->kprintf(ATTR_GREEN, "  [OK] %s -> %s (%d b)\n", src_path, dst_path, bytes);
        } else {
            api->kprintf(ATTR_RED, "  [FAIL] %s -> %s (code: %d)\n", src_path, dst_path, bytes);
        }
    }

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
