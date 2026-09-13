#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  INSTALL.C — OS32 HDDインストーラー v4.1                                  */
/*                                                                          */
/*  FAT12フロッピー (サブディレクトリ構造) から起動して実行し、               */
/*  IDEドライブにシステムを書き込む。                                        */
/*                                                                          */
/*  v4.0: FDDサブディレクトリ対応                                            */
/*  v4.1 (票 S3I2-I): 生カーネル (/kernel.bin → LBA 6) を廃止し、ローダ v3   */
/*    が読む ext2 の /boot/vmkernel.lz4 を置く。あわせて                     */
/*      - 承認前に /VMKRNL.LZ4 / /sys/boot_hdd.bin / /sys/loader_h.bin を検査 */
/*        (欠損なら 1 バイトも書かずに中止)                                   */
/*      - 宛先名を ASCII 小文字へ正規化 (FAT は大文字を返す、ext2 は区別する)  */
/*      - /etc/profile は写さない (FDD 用の PATH)                            */
/*      - コピー / 列挙 / sync の失敗を終了コードまで伝播                     */
/*    /sys/boot_hdd.bin, /sys/loader_h.bin → HDDブートセクタ                */
/*    /VMKRNL.LZ4 → /hd0/boot/vmkernel.lz4                                  */
/*    /sys/, /bin/, /sbin/, /etc/ → /hd0/ 配下にディレクトリごとコピー       */
/* ======================================================================== */

#include "os32api.h"

#define IDE_DRIVE 0

/* PC-98ジオメトリ定数 (NHD: H=8, S=17) */
#define PC98_HEADS   8
#define PC98_SECTORS 17
#define PC98_CYL0_SECTORS  (PC98_HEADS * PC98_SECTORS)  /* 136 */
/* ブート予約シリンダ数。LBA 0=IPL / 1=PT / 2..=loader を収める。カーネルは
 * ext2 の /boot/vmkernel.lz4 に置くので予約帯には入らないが、
 * tools/nhd_deploy.py の HDD_PARTITION_LBA と必ず一致させること。 */
#define PC98_BOOT_CYLS     12
#define HDD_PARTITION_LBA  (PC98_CYL0_SECTORS * PC98_BOOT_CYLS)  /* 1632 */

/* IOバッファ (ヒープから確保する) */
static u8 *file_buf;
#define FILE_BUF_SIZE (128 * 1024)  /* 128KB */

#define MAX_FILES 64
typedef struct {
    char names[MAX_FILES][32];
    u8   types[MAX_FILES];  /* 1=FILE, 2=DIR */
    int count;
    int overflow;           /* MAX_FILES を超えて取りこぼした */
} FileList;

/* 媒体側の名前 (FDD)。FAT の 8.3 名は大文字で返るので、開くときは媒体の
 * 名前のまま、HDD へ作るときは小文字に正規化する。 */
#define SRC_KERNEL_LZ4  "/VMKRNL.LZ4"
#define SRC_BOOT_HDD    "/sys/boot_hdd.bin"
#define SRC_LOADER_H    "/sys/loader_h.bin"
#define DST_BOOT_DIR    "/hd0/boot"
#define DST_KERNEL_LZ4  "/hd0/boot/vmkernel.lz4"

/* I2-1: drivers/ide.h の IdeInfo と**同じ並び** (i386 で 96 B) でなければ
 * ならない。ide_identify はカーネル側の定義で書くので、ここが 92 B のままだと
 * 末尾 phys_sector_size の 2 バイトが呼び手の領域を踏む。drivers/ide.h は
 * カーネル内部ヘッダで外部プログラムからは引けないため写しを置く —
 * ide.h を変えたら必ず一緒に直すこと (userland/shell/cmd_sys.c も同じ写し)。 */
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
    u16 phys_sector_size;   /* 物理セクタサイズ (SASI=256, IDE=512) */
} IdeInfo;

static KernelAPI *g_api;

/* ======== 回復モードの KAPI 境界 (票 S3-I §1) ======== */
/* `--recover-settings` / `--revert-settings` の本体は install_recover.inc に
 * あり、KAPI へは RecoverOps (関数ポインタ表) 経由でしか触らない。ホスト
 * TDD (tools/tests/install_recover_host.c) が同じ .inc を取り込めるように
 * するため。通常インストール経路はこの表を使わない。 */

static int rop_stat(const char *p, OS32_Stat *st)  { return g_api->sys_stat(p, st); }
static int rop_open(const char *p, int mode)       { return g_api->sys_open(p, mode); }
static int rop_read(int fd, void *b, u32 n)        { return g_api->sys_read(fd, b, n); }
static int rop_write(int fd, const void *b, u32 n) { return g_api->sys_write(fd, b, n); }
static void rop_close(int fd)                      { g_api->sys_close(fd); }
static int rop_unlink(const char *p)               { return g_api->sys_unlink(p); }
static int rop_rename(const char *o, const char *n){ return g_api->sys_rename(o, n); }
static int rop_mount(const char *pre, const char *dev, const char *fs)
{ return g_api->sys_mount(pre, dev, fs); }
static int rop_is_mounted(const char *pre)         { return g_api->sys_is_mounted(pre); }
static int rop_sync(void)                          { return g_api->vfs_sync(); }
static int rop_db_open_existing(const char *p, int w)
{ return g_api->db_open_existing(p, w); }
static int rop_db_prepare_only(int h, const char *sql)
{ return g_api->db_prepare_only(h, sql); }
static int rop_db_step(int h)                      { return g_api->db_step(h); }
static int rop_db_finalize(int h)                  { return g_api->db_finalize(h); }
static int rop_db_close(int h)                     { return g_api->db_close(h); }
static int rop_db_error_code(int h)                { return g_api->db_error_code(h); }
static unsigned char *rop_shm(void)                { return (unsigned char *)g_api->shm_base; }

static int rop_getkey(void)
{
    int key;
    for (;;) {
        key = g_api->kbd_trygetchar();
        if (key <= 0) key = g_api->serial_trygetchar();
        if (key > 0) return key;
    }
}

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

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char lower_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

/* 宛先名の小文字化 (票 S3I2-I §1 の B1)。FAT (FF_USE_LFN 0) の列挙は
 * SHELL.BIN のように大文字を返し、ext2 は大小文字を区別するので、そのまま
 * 写すと /hd0/etc/SETTINGS.DB になり seed も /sys/shell.bin の起動も
 * 成立しない。ASCII だけを畳む (8.3 名に非 ASCII は来ない)。 */
static void str_cat_lower(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) { *dst++ = lower_c(*src); src++; }
    *dst = '\0';
}

static int str_eq_lower(const char *s, const char *lower) {
    while (*s && lower_c(*s) == *lower) { s++; lower++; }
    return lower_c(*s) == *lower;
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
    if (fl->count >= MAX_FILES) { fl->overflow = 1; return; }
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

/* 成功なら写したバイト数 (>= 0)、失敗なら負。
 *   -1 = src が開けない / -2 = dst が開けない
 *   -3 = short write / -4 = read が負 (EOF ではなく I/O 失敗)
 * read の負を EOF と同じ扱いにすると、途中で落ちたコピーが「成功」に
 * 見えてしまう (票 S3I2-I §1 の B3)。 */
static int copy_file(const char *src, const char *dst) {
    int fd_src, fd_dst;
    int read_len;
    int total = 0;
    int rc = 0;

    fd_src = g_api->sys_open(src, O_RDONLY);
    if (fd_src < 0) return -1;

    fd_dst = g_api->sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd_dst < 0) {
        g_api->sys_close(fd_src);
        return -2;
    }

    while (1) {
        read_len = g_api->sys_read(fd_src, file_buf, FILE_BUF_SIZE);
        if (read_len < 0) { rc = -4; break; }
        if (read_len == 0) break;
        if (g_api->sys_write(fd_dst, file_buf, read_len) != read_len) {
            rc = -3;
            break;
        }
        total += read_len;
    }

    g_api->sys_close(fd_src);
    g_api->sys_close(fd_dst);
    return rc != 0 ? rc : total;
}

static int read_file_to_buf(const char *path, int max_size) {
    int fd = g_api->sys_open(path, KAPI_O_RDONLY);
    int len;
    if (fd < 0) return fd;
    len = g_api->sys_read(fd, file_buf, max_size);
    g_api->sys_close(fd);
    return len;
}

/* ======== ブートセクタ書き込み ======== */

static int write_partition_table(IdeInfo *info)
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

/* 写さない媒体専用ファイル。/etc/profile は FDD 用で PATH に /usr/bin が
 * 無いので HDD へ持っていかない (HDD の profile は通常配備が置く)。 */
static int skip_on_hdd(const char *dst_dir, const char *name)
{
    return str_eq(dst_dir, "/etc") && str_eq_lower(name, "profile");
}

/* FDDのsrc_dir配下のファイル/ディレクトリを /hd0/dst_dir 配下にコピー。
 * 戻り値は **失敗の件数** (0 = 全部成功)。sys_ls の負 (列挙途中の I/O
 * 失敗) / mkdir / コピー / 再帰の失敗をすべて数える (票 S3I2-I §1 の B3、
 * 往復 2 の R2)。宛先の名前は各成分を ASCII 小文字に正規化する。 */
static int copy_directory(const char *src_dir, const char *dst_dir, int depth)
{
    FileList fl;
    int i, fail_count = 0;
    int ls_rc;

    if (depth > 4) {
        /* 無限再帰防止。ここに来たら写し漏れなので失敗として数える。 */
        g_api->kprintf(ATTR_RED, "  [FAIL] %s: too deep\n", src_dir);
        return 1;
    }

    fl.count = 0;
    fl.overflow = 0;
    ls_rc = g_api->sys_ls(src_dir, ls_cb, &fl);
    if (ls_rc < 0) {
        g_api->kprintf(ATTR_RED, "  [FAIL] list %s (code: %d)\n", src_dir, ls_rc);
        return 1;
    }
    if (fl.overflow) {
        g_api->kprintf(ATTR_RED, "  [FAIL] %s: more than %d entries\n",
                       src_dir, MAX_FILES);
        fail_count++;
    }

    for (i = 0; i < fl.count; i++) {
        char src_path[128];
        char dst_path[128];

        /* "." と ".." をスキップ */
        if (fl.names[i][0] == '.') {
            if (fl.names[i][1] == '\0') continue;
            if (fl.names[i][1] == '.' && fl.names[i][2] == '\0') continue;
        }

        if (skip_on_hdd(dst_dir, fl.names[i])) continue;

        /* ソースパス構築 (媒体の名前のまま開く) */
        str_cpy(src_path, src_dir);
        if (src_path[str_len(src_path) - 1] != '/') str_cat(src_path, "/");
        str_cat(src_path, fl.names[i]);

        /* 宛先パス構築: /hd0 + dst_dir + / + 小文字にした名前 */
        str_cpy(dst_path, "/hd0");
        str_cat(dst_path, dst_dir);
        if (dst_path[str_len(dst_path) - 1] != '/') str_cat(dst_path, "/");
        str_cat_lower(dst_path, fl.names[i]);

        if (fl.types[i] == OS32_FILE_TYPE_DIR) {
            /* サブディレクトリ: HDD側にmkdirして再帰 */
            if (g_api->sys_mkdir(dst_path) != 0) {
                g_api->kprintf(ATTR_RED, "  [FAIL] mkdir %s\n", dst_path);
                fail_count++;
                continue;
            }

            {
                char sub_dst[128];
                str_cpy(sub_dst, dst_dir);
                if (sub_dst[str_len(sub_dst) - 1] != '/') str_cat(sub_dst, "/");
                str_cat_lower(sub_dst, fl.names[i]);
                fail_count += copy_directory(src_path, sub_dst, depth + 1);
            }
        } else {
            /* ファイル: コピー */
            int bytes = copy_file(src_path, dst_path);
            if (bytes >= 0) {
                g_api->kprintf(ATTR_GREEN, "  [OK] %s -> %s (%d b)\n", src_path, dst_path, bytes);
            } else {
                g_api->kprintf(ATTR_RED, "  [FAIL] %s -> %s (code: %d)\n", src_path, dst_path, bytes);
                fail_count++;
            }
        }
    }

    return fail_count;
}

#include "install_recover.inc"

static const RecoverOps recover_kapi_ops = {
    rop_stat, rop_open, rop_read, rop_write, rop_close, rop_unlink,
    rop_rename, rop_mount, rop_is_mounted, rop_sync,
    rop_db_open_existing, rop_db_prepare_only, rop_db_step, rop_db_finalize,
    rop_db_close, rop_db_error_code, rop_shm,
    0,                    /* kprintf — main() で api から埋める */
    rop_getkey
};

/* ======== メイン ======== */

/* Phase 0 (承認前): 媒体に要る 3 つが在って空でないことを確かめる。
 * 1 つでも欠ければ **1 バイトも書かずに** 中止する (票 S3I2-I §1)。
 * 戻り値 0 = 揃っている。size_out に /VMKRNL.LZ4 の大きさを返す。 */
static int precheck_media(u32 *lz4_size_out)
{
    static const char *const need[3] = {
        SRC_KERNEL_LZ4, SRC_BOOT_HDD, SRC_LOADER_H
    };
    OS32_Stat st;
    int i, rc;

    *lz4_size_out = 0;
    for (i = 0; i < 3; i++) {
        rc = g_api->sys_stat(need[i], &st);
        if (rc != 0) {
            g_api->kprintf(0x4F, "Error: Missing %s (stat %d)\n", need[i], rc);
            return -1;
        }
        if (st.st_size == 0) {
            g_api->kprintf(0x4F, "Error: Empty %s\n", need[i]);
            return -1;
        }
        if (i == 0) *lz4_size_out = st.st_size;
    }
    return 0;
}

/* HDD に最初に作るディレクトリ (親が先)。1 つでも作れなければインストールは
 * 未完成 (往復 1 の B1)。 */
static const char *const init_dirs[] = {
    DST_BOOT_DIR,
    "/hd0/sys", "/hd0/bin", "/hd0/sbin", "/hd0/etc",
    "/hd0/usr", "/hd0/usr/bin", "/hd0/usr/man",
    "/hd0/data", "/hd0/home", "/hd0/home/user", "/hd0/tmp"
};

int __cdecl main(int argc, char **argv, KernelAPI *api)
{
    static IdeInfo info;
    static RecoverOps rops;
    int ret, i;
    int loader_sects;
    int rc = 1;                 /* 失敗を既定にする (票 S3I2-I §1) */
    u32 lz4_size = 0;
    int copied;

    g_api = api;

    /* --- 回復モード (票 S3-I §1)。通常インストール経路には入らない --- */
    if (argc >= 2 && argv && argv[1] &&
        (str_eq(argv[1], "--recover-settings") ||
         str_eq(argv[1], "--revert-settings"))) {
        const char *drive = (argc >= 3 && argv[2]) ? argv[2] : "hd0";
        rops = recover_kapi_ops;
        rops.kprintf = api->kprintf;
        return recover_settings_main(&rops, drive,
                                     str_eq(argv[1], "--revert-settings"));
    }
    (void)argc;
    (void)argv;

    api->kprintf(ATTR_CYAN, "%s", "\n========================================\n");
    api->kprintf(ATTR_CYAN, "%s", "      OS32 HDD Installer v4.1        \n");
    api->kprintf(ATTR_CYAN, "%s", "========================================\n\n");

    file_buf = (u8 *)api->mem_alloc(FILE_BUF_SIZE);
    if (!file_buf) {
        api->kprintf(0x4F, "%s", "Error: Out of memory\n");
        return 1;
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

    /* === Phase 0: 媒体の中身の事前検査 (承認前 = まだ何も書いていない) === */
    if (precheck_media(&lz4_size) != 0) {
        api->kprintf(0x4F, "%s",
                     "Nothing was written; use a complete install floppy.\n");
        goto end;
    }

    /* 安全ロック */
    if (!confirm_install()) {
        api->kprintf(0x07, "%s", "Installation aborted.\n");
        rc = 0;                 /* 利用者が断っただけ = 失敗ではない */
        goto end;
    }

    /* === Phase 1: ブートセクタ書き込み === */
    api->kprintf(ATTR_YELLOW, "%s", "[1/3] Writing Boot Sectors...\n");

    /* boot_hdd.bin → LBA 0 */
    ret = read_file_to_buf(SRC_BOOT_HDD, FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing /sys/boot_hdd.bin\n"); goto end; }
    if (ret < 512) { for (i = ret; i < 512; i++) file_buf[i] = 0; }
    file_buf[8] = (u8)info.heads;
    file_buf[9] = (u8)info.sectors;
    file_buf[510] = 0x55; file_buf[511] = 0xAA;
    if (api->ide_write_sectors(IDE_DRIVE, 0, 1, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written IPL (LBA 0, patched geom %d/%d)\n", info.heads, info.sectors);

    /* Partition Table → LBA 1 */
    if (write_partition_table(&info) != 0) goto ioerr;

    /* loader_h.bin → LBA 2+ (ローダ v3。カーネルは ext2 の /boot から読む
     * ので、ここから先へ生のカーネルを書くことはもう無い) */
    ret = read_file_to_buf(SRC_LOADER_H, FILE_BUF_SIZE);
    if (ret <= 0) { api->kprintf(0x4F, "%s", "Error: Missing /sys/loader_h.bin\n"); goto end; }
    loader_sects = (ret + 511) / 512;
    if (api->ide_write_sectors(IDE_DRIVE, 2, loader_sects, file_buf) != 0) goto ioerr;
    api->kprintf(0x0A, "  Written LOADER (LBA 2, %d bytes)\n", ret);

    /* === Phase 2: ext2フォーマット === */
    /* 起動時の自動マウントが残っていると、フォーマット後も古い
     * スーパーブロック/GDT を保持した ctx で書き込むことになるため必ず外す */
    api->sys_umount("/hd0");
    api->kprintf(ATTR_YELLOW, "%s", "\n[2/3] Formatting Ext2 Filesystem...\n");
    ret = api->ext2_format(IDE_DRIVE, info.total_sectors - HDD_PARTITION_LBA);
    if (ret != 0) {
        api->kprintf(0x4F, "%s",
                     "Error: format failed - the drive is not bootable;"
                     " rerun install\n");
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

    /* HDD側ディレクトリ構造を作成。1 つでも作れなければ未完成なので中止する
     * (往復 1 の B1: 戻り値を捨てると /hd0/tmp だけ無い HDD で終了 0 になる)。
     * 親は子より先に並べること。 */
    api->kprintf(0x0A, "%s", "  Creating directories...\n");
    for (i = 0; i < (int)(sizeof(init_dirs) / sizeof(init_dirs[0])); i++) {
        if (api->sys_mkdir(init_dirs[i]) != 0) {
            api->kprintf(0x4F, "Error: Failed to create %s\n", init_dirs[i]);
            goto end;
        }
    }

    /* 圧縮カーネル → /hd0/boot/vmkernel.lz4 (128KB バッファで反復コピー)。
     * ローダ v3 がここを読む。長さが合わなければ失敗。 */
    copied = copy_file(SRC_KERNEL_LZ4, DST_KERNEL_LZ4);
    if (copied < 0 || (u32)copied != lz4_size) {
        api->kprintf(0x4F, "  [FAIL] %s -> %s (code: %d, want %u bytes)\n",
                     SRC_KERNEL_LZ4, DST_KERNEL_LZ4, copied, lz4_size);
        goto end;
    }
    api->kprintf(0x0A, "  vmkernel.lz4 -> /boot (%d bytes)\n", copied);

    /* FDDのサブディレクトリ構造をそのままHDDにコピー (宛先の名前は小文字) */
    ret = 0;

    /* /sys/ → /hd0/sys/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /sys/ ...\n");
    ret += copy_directory("/sys", "/sys", 0);

    /* /bin/ → /hd0/bin/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /bin/ ...\n");
    ret += copy_directory("/bin", "/bin", 0);

    /* /sbin/ → /hd0/sbin/ */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /sbin/ ...\n");
    ret += copy_directory("/sbin", "/sbin", 0);

    /* /etc/ → /hd0/etc/ (profile は写さない、settings.db は seed) */
    api->kprintf(ATTR_CYAN, "%s", "  Copying /etc/ ...\n");
    ret += copy_directory("/etc", "/etc", 0);

    if (ret != 0) {
        api->kprintf(0x4F, "  %d file(s) failed to copy.\n", ret);
        goto end;
    }

    /* ファイルシステム同期 */
    api->kprintf(ATTR_CYAN, "%s", "  Syncing filesystem...\n");
    ret = api->vfs_sync();
    if (ret != 0) {
        api->kprintf(0x4F, "  sync failed (code: %d)\n", ret);
        goto end;
    }

    api->kprintf(0x7E, "%s", "\n========================================\n");
    api->kprintf(ATTR_CYAN, "%s", " OS32 Installation complete!            \n");
    api->kprintf(ATTR_CYAN, "%s", " Remove the Floppy Disk and reboot.     \n");
    api->kprintf(0x7E, "%s", "========================================\n");
    rc = 0;
    goto end;

ioerr:
    api->kprintf(0x4F, "%s", "Error: IDE Write I/O Error\n");

end:
    api->mem_free(file_buf);
    file_buf = 0;
    if (rc != 0)
        api->kprintf(ATTR_RED, "%s", "\n[FAIL] Installation did not complete.\n");
    return rc;
}
