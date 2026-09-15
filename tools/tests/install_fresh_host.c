/* ========================================================================= */
/*  INSTALL_FRESH_HOST.C — 票 S3I2-I (docs/archive/settings/TASK_S3I2.md §1)   */
/*                        のホスト TDD                                       */
/*                                                                           */
/*  実物は `userland/system/install.c` の **通常インストール経路** そのもの   */
/*  (`#define main` で取り込む)。模型は KAPI の贋物だけ:                      */
/*    - `ide_identify` は実型 (drivers/ide.h の IdeInfo と同じ 96 B) を書く   */
/*    - `ide_write_sectors` は (LBA, 本数) を記録するだけ                     */
/*    - `sys_ls` は **FAT のように大文字** の名前を返す (FF_USE_LFN 0)        */
/*    - 媒体側 (`/hd0` 以外) の名前引きは FAT のように大小文字を無視し、      */
/*      `/hd0` 側 (ext2) は区別する                                          */
/*    - read の負 / short write / mkdir / sys_ls / format / mount / sync /    */
/*      ide_write の失敗を注入できる                                         */
/*  ホストのファイルシステム・エミュレータ・配備には 1 バイトも触らない。      */
/*                                                                           */
/*  実行: python3 -B tools/tests/test_install_fresh.py                       */
/* ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stddef.h>

#include "os32api.h"

/* ---- kprintf の捕捉 ---------------------------------------------------- */
static char cap_buf[65536];
static int  cap_len;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n--- output ---\n%s", \
            __func__, __LINE__, #c, cap_buf); exit(1); } } while (0)
#define CHECK_STR(want) do { if (!strstr(cap_buf, (want))) { \
    fprintf(stderr, "FAIL %s:%d: message missing: %s\n--- output ---\n%s", \
            __func__, __LINE__, (want), cap_buf); exit(1); } } while (0)
#define CHECK_NOSTR(bad) do { if (strstr(cap_buf, (bad))) { \
    fprintf(stderr, "FAIL %s:%d: unexpected message: %s\n--- output ---\n%s", \
            __func__, __LINE__, (bad), cap_buf); exit(1); } } while (0)

/* ========================================================================= */
/*  贋の媒体 (FAT) と贋の HDD (ext2) を 1 つの表で持つ                        */
/* ========================================================================= */

#define FX_MAX 160
typedef struct {
    char           path[96];
    unsigned char *data;
    int            size;
    int            is_dir;
    int            used;
} FxFile;

static FxFile fx[FX_MAX];

static int fx_is_hdd(const char *p) { return !strncmp(p, "/hd0", 4); }

static int fx_same(const char *a, const char *b)
{
    /* ext2 (= /hd0 配下) は大小文字を区別、FAT の媒体側は区別しない */
    if (fx_is_hdd(a) || fx_is_hdd(b)) return !strcmp(a, b);
    return !strcasecmp(a, b);
}

static int fx_find(const char *path)
{
    int i;
    for (i = 0; i < FX_MAX; i++)
        if (fx[i].used && fx_same(fx[i].path, path)) return i;
    return -1;
}

static int fx_new(const char *path)
{
    int i;
    for (i = 0; i < FX_MAX; i++) {
        if (!fx[i].used) {
            memset(&fx[i], 0, sizeof(fx[i]));
            fx[i].used = 1;
            strcpy(fx[i].path, path);
            return i;
        }
    }
    fprintf(stderr, "fixture table full\n");
    exit(2);
}

/* 中身は位置で決まる。長さ一致だけでなく中身の一致も見られる。 */
static unsigned char fx_byte(int i) { return (unsigned char)((i * 7 + 3) & 0xFF); }

static void fx_put(const char *path, int size)
{
    int i = fx_find(path), k;
    if (i < 0) i = fx_new(path);
    free(fx[i].data);
    fx[i].data = (unsigned char *)malloc((size_t)(size > 0 ? size : 1));
    for (k = 0; k < size; k++) fx[i].data[k] = fx_byte(k);
    fx[i].size = size;
    fx[i].is_dir = 0;
}

static void fx_dir(const char *path)
{
    int i = fx_find(path);
    if (i < 0) i = fx_new(path);
    fx[i].is_dir = 1;
    fx[i].size = 0;
}

static void fx_rm(const char *path)
{
    int i = fx_find(path);
    if (i >= 0) { free(fx[i].data); fx[i].data = NULL; fx[i].used = 0; }
}

static int fx_exists(const char *path) { return fx_find(path) >= 0; }

static int fx_size(const char *path)
{
    int i = fx_find(path);
    return i < 0 ? -1 : fx[i].size;
}

/* 中身が「元の媒体の中身と同じ」か */
static int fx_content_ok(const char *path, int size)
{
    int i = fx_find(path), k;
    if (i < 0 || fx[i].size != size) return 0;
    for (k = 0; k < size; k++) if (fx[i].data[k] != fx_byte(k)) return 0;
    return 1;
}

static void fx_reset(void)
{
    int i;
    for (i = 0; i < FX_MAX; i++) { free(fx[i].data); fx[i].data = NULL; fx[i].used = 0; }
}

/* ---- 親ディレクトリ ----------------------------------------------------- */
static void fx_parent(const char *path, char *out)
{
    const char *slash = strrchr(path, '/');
    size_t n;
    if (!slash || slash == path) { strcpy(out, "/"); return; }
    n = (size_t)(slash - path);
    memcpy(out, path, n);
    out[n] = '\0';
}

/* ========================================================================= */
/*  記録と注入                                                               */
/* ========================================================================= */

#define REC_MAX 512
static char rec_open[REC_MAX][96];
static int  rec_open_n;
static char rec_stat[REC_MAX][96];
static int  rec_stat_n;
static char rec_mkdir[REC_MAX][96];
static int  rec_mkdir_n;
static struct { u32 lba; u32 cnt; } rec_wr[REC_MAX];
static int  rec_wr_n;

static int  inj_ide_write_fail_lba = -1;
static int  inj_format_fail;
static int  inj_mount_fail;
static const char *inj_mkdir_fail;
static const char *inj_ls_fail;
static int  inj_ls_fail_after;        /* 何件 callback を呼んでから負を返すか */
static const char *inj_read_neg;      /* この名前の read を負で返す */
static int  inj_read_neg_after;       /* 何回目の read から負にするか (0 = 最初) */
static const char *inj_write_short;   /* この宛先の write を 1 バイト減らす */
static int  inj_sync_fail;
static const char *inj_stat_big;      /* stat だけが大きい長さを名乗る名前 */
static int  inj_stat_big_extra;

static const char *key_script;        /* confirm_install に食わせる鍵 */

static void rec_reset(void)
{
    rec_open_n = rec_stat_n = rec_mkdir_n = rec_wr_n = 0;
    inj_ide_write_fail_lba = -1;
    inj_format_fail = 0;
    inj_mount_fail = 0;
    inj_sync_fail = 0;
    inj_mkdir_fail = NULL;
    inj_ls_fail = NULL;
    inj_ls_fail_after = 0;
    inj_read_neg = NULL;
    inj_write_short = NULL;
    inj_read_neg_after = 0;
    inj_stat_big = NULL;
    inj_stat_big_extra = 0;
    key_script = "y";
    cap_len = 0;
    cap_buf[0] = '\0';
}

static int rec_has(char list[][96], int n, const char *path)
{
    int i;
    for (i = 0; i < n; i++) if (!strcasecmp(list[i], path)) return 1;
    return 0;
}

/* 綴りまで一致するか (媒体側の名前をそのまま開いているかの確認) */
static int rec_has_exact(char list[][96], int n, const char *path)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(list[i], path)) return 1;
    return 0;
}

/* ========================================================================= */
/*  実物を取り込む                                                           */
/* ========================================================================= */

#define main install_main
#include "../../userland/system/install.c"
#undef main

/* drivers/ide.h の IdeInfo と同じ並び (i386 では 96 B)。install.c 側の型が
 * これより小さいと ide_identify が呼び手の領域外へ 2 バイト書く (票 §1 の B2)。
 * ホストは LP64 で u32 = unsigned long が 8 B なので「96」という数そのものは
 * 再現できない。ここでは **実型との一致** (大きさと全 offset) を見て、96 /
 * 92 という数は `--target` の i386-elf コンパイル時表明で固定する。 */
typedef struct {
    u32  total_sectors;
    u16  cylinders;
    u16  heads;
    u16  sectors;
    u32  size_mb;
    char model[41];
    char serial[21];
    char firmware[9];
    int  lba_supported;
    u16  phys_sector_size;
} IdeIdentifyWire;

/* ========================================================================= */
/*  KAPI の贋物                                                              */
/* ========================================================================= */

static void fake_kprintf(u8 attr, const char *fmt, ...)
{
    va_list ap;
    (void)attr;
    va_start(ap, fmt);
    cap_len += vsnprintf(cap_buf + cap_len, sizeof(cap_buf) - (size_t)cap_len, fmt, ap);
    va_end(ap);
    if (cap_len > (int)sizeof(cap_buf) - 1) cap_len = (int)sizeof(cap_buf) - 1;
}

static void *fake_mem_alloc(u32 n) { return malloc(n); }
static void  fake_mem_free(void *p) { free(p); }
static void  fake_ide_init(void) { }
static int   fake_ide_drive_present(int drv) { (void)drv; return 1; }

#define FAKE_TOTAL_SECTORS 409600u   /* 200MB, H=8 S=17 */

static int fake_ide_identify(int drv, void *out)
{
    IdeIdentifyWire w;
    (void)drv;
    /* 呼び手の型が実型と同じ大きさでなければ、この memcpy が領域外書込みに
     * なる。install.c の型をそのまま見て落とす (票 §1 の (7))。 */
    CHECK(sizeof(IdeInfo) == sizeof(IdeIdentifyWire));
    CHECK(offsetof(IdeInfo, phys_sector_size) ==
          offsetof(IdeIdentifyWire, phys_sector_size));
    memset(&w, 0, sizeof(w));
    w.total_sectors = FAKE_TOTAL_SECTORS;
    w.cylinders = 3011;
    w.heads = 8;
    w.sectors = 17;
    w.size_mb = 200;
    strcpy(w.model, "OS32 FAKE DISK");
    strcpy(w.serial, "S3I2");
    strcpy(w.firmware, "1.0");
    w.lba_supported = 1;
    w.phys_sector_size = 512;
    memcpy(out, &w, sizeof(w));
    return 0;
}

static int fake_ide_write_sectors(int drv, u32 lba, u32 cnt, const void *buf)
{
    (void)drv; (void)buf;
    if (inj_ide_write_fail_lba >= 0 && (u32)inj_ide_write_fail_lba == lba) return -1;
    if (rec_wr_n < REC_MAX) {
        rec_wr[rec_wr_n].lba = lba;
        rec_wr[rec_wr_n].cnt = cnt;
        rec_wr_n++;
    }
    return 0;
}

static int fake_ext2_format(int drv, u32 sectors)
{ (void)drv; (void)sectors; return inj_format_fail ? -1 : 0; }

static int fake_sys_mount(const char *pre, const char *dev, const char *fs)
{ (void)pre; (void)dev; (void)fs; return inj_mount_fail ? -1 : 0; }

static void fake_sys_umount(const char *pre) { (void)pre; }

static int fake_sys_mkdir(const char *path)
{
    if (rec_mkdir_n < REC_MAX) strcpy(rec_mkdir[rec_mkdir_n++], path);
    if (inj_mkdir_fail && !strcmp(inj_mkdir_fail, path)) return -1;
    if (fx_find(path) >= 0) return -1;   /* 既存 */
    fx_dir(path);
    return 0;
}

static int fake_sys_ls(const char *path, void *cb, void *ctx)
{
    DirCallback fn = (DirCallback)cb;
    DirEntry_Ext e;
    char parent[96];
    int i;
    int sent = 0;
    int failing = (inj_ls_fail && fx_same(inj_ls_fail, path));
    int d = fx_find(path);

    /* 列挙の途中で落ちる形 (S3I2-K 後の fatfs_vfs_list): 何件か callback を
     * 呼んでから負を返す。呼び手が件数だけを見ていると気付けない。 */
    if (failing && inj_ls_fail_after <= 0) return -5;
    if (d < 0 || !fx[d].is_dir) return -2;

    for (i = 0; i < FX_MAX; i++) {
        const char *name;
        if (!fx[i].used || i == d) continue;
        fx_parent(fx[i].path, parent);
        if (!fx_same(parent, path)) continue;
        if (failing && sent >= inj_ls_fail_after) return -5;
        name = strrchr(fx[i].path, '/') + 1;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, name, sizeof(e.name) - 1);
        e.size = (u32)fx[i].size;
        e.type = (u8)(fx[i].is_dir ? OS32_FILE_TYPE_DIR : 1);
        fn(&e, ctx);
        sent++;
    }
    if (failing) return -5;
    return 0;
}

/* ---- FD 表 -------------------------------------------------------------- */
#define FD_MAX 16
static struct { int used; int fi; int pos; int wr; int reads; } fds[FD_MAX];

static int fake_sys_open(const char *path, int mode)
{
    int i, fi;
    if (rec_open_n < REC_MAX) strcpy(rec_open[rec_open_n++], path);
    fi = fx_find(path);
    if (mode & KAPI_O_CREAT) {
        if (fi < 0) fi = fx_new(path);
        if (mode & KAPI_O_TRUNC) {
            free(fx[fi].data); fx[fi].data = NULL; fx[fi].size = 0;
        }
    } else if (fi < 0) {
        return -1;
    }
    if (fx[fi].is_dir) return -1;
    for (i = 0; i < FD_MAX; i++) {
        if (!fds[i].used) {
            fds[i].used = 1; fds[i].fi = fi; fds[i].pos = 0; fds[i].reads = 0;
            fds[i].wr = (mode & (KAPI_O_WRONLY | KAPI_O_RDWR)) ? 1 : 0;
            return i + 3;
        }
    }
    return -1;
}

static void fake_sys_close(int fd)
{ if (fd >= 3 && fd - 3 < FD_MAX) fds[fd - 3].used = 0; }

static int fake_sys_read(int fd, void *buf, u32 size)
{
    int i = fd - 3, n;
    if (i < 0 || i >= FD_MAX || !fds[i].used) return -1;
    if (inj_read_neg && fx_same(fx[fds[i].fi].path, inj_read_neg) &&
        fds[i].reads >= inj_read_neg_after) { fds[i].reads++; return -7; }
    fds[i].reads++;
    n = fx[fds[i].fi].size - fds[i].pos;
    if (n > (int)size) n = (int)size;
    if (n <= 0) return 0;
    memcpy(buf, fx[fds[i].fi].data + fds[i].pos, (size_t)n);
    fds[i].pos += n;
    return n;
}

static int fake_sys_write(int fd, const void *buf, u32 size)
{
    int i = fd - 3, want = (int)size, need;
    FxFile *f;
    if (i < 0 || i >= FD_MAX || !fds[i].used || !fds[i].wr) return -1;
    f = &fx[fds[i].fi];
    if (inj_write_short && !strcmp(f->path, inj_write_short) && want > 0) want--;
    need = fds[i].pos + want;
    if (need > f->size || !f->data) {
        f->data = (unsigned char *)realloc(f->data, (size_t)(need > 0 ? need : 1));
        f->size = need;
    }
    memcpy(f->data + fds[i].pos, buf, (size_t)want);
    fds[i].pos += want;
    return want;
}

static int fake_sys_stat(const char *path, OS32_Stat *st)
{
    int i;
    if (rec_stat_n < REC_MAX) strcpy(rec_stat[rec_stat_n++], path);
    i = fx_find(path);
    if (i < 0) return -2;
    memset(st, 0, sizeof(*st));
    st->st_size = (u32)fx[i].size;
    if (inj_stat_big && fx_same(fx[i].path, inj_stat_big))
        st->st_size += (u32)inj_stat_big_extra;
    st->st_mode = (u16)(fx[i].is_dir ? OS_S_IFDIR : OS_S_IFREG);
    st->st_nlink = 1;
    return 0;
}

static int fake_vfs_sync(void) { return inj_sync_fail ? -1 : 0; }

static int fake_kbd_trygetchar(void)
{
    if (key_script && *key_script) return (int)*key_script++;
    return 0;
}
static int fake_serial_trygetchar(void) { return 0; }

/* ========================================================================= */
/*  固定具の組み立てと実行                                                   */
/* ========================================================================= */

#define LZ4_SIZE   470000       /* 128KB バッファで 4 回に分かれる */
#define LOADER_LEN 8192
#define SHELL_LEN  1000

static KernelAPI api;

static void api_init(void)
{
    memset(&api, 0, sizeof(api));
    api.kprintf = fake_kprintf;
    api.mem_alloc = fake_mem_alloc;
    api.mem_free = fake_mem_free;
    api.ide_init = fake_ide_init;
    api.ide_drive_present = fake_ide_drive_present;
    api.ide_identify = fake_ide_identify;
    api.ide_write_sectors = fake_ide_write_sectors;
    api.ext2_format = fake_ext2_format;
    api.sys_mount = fake_sys_mount;
    api.sys_umount = fake_sys_umount;
    api.sys_mkdir = fake_sys_mkdir;
    api.sys_ls = fake_sys_ls;
    api.sys_open = fake_sys_open;
    api.sys_close = fake_sys_close;
    api.sys_read = fake_sys_read;
    api.sys_write = fake_sys_write;
    api.sys_stat = fake_sys_stat;
    api.vfs_sync = fake_vfs_sync;
    api.kbd_trygetchar = fake_kbd_trygetchar;
    api.serial_trygetchar = fake_serial_trygetchar;
}

/* FDD イメージの中身 (FAT は大文字で返す) */
static void media_fixture(void)
{
    int i;
    for (i = 0; i < FD_MAX; i++) fds[i].used = 0;
    fx_reset();
    fx_dir("/");
    fx_put("/VMKRNL.LZ4", LZ4_SIZE);
    fx_dir("/SYS");
    fx_put("/SYS/BOOT_HDD.BIN", 512);
    fx_put("/SYS/LOADER_H.BIN", LOADER_LEN);
    fx_put("/SYS/SHELL.BIN", SHELL_LEN);
    fx_dir("/BIN");
    fx_put("/BIN/LS.BIN", 300);
    fx_dir("/SBIN");
    fx_put("/SBIN/INIT.BIN", 400);
    fx_dir("/ETC");
    fx_put("/ETC/SETTINGS.DB", 2048);
    fx_put("/ETC/PROFILE", 60);
}

static void setup(void)
{
    api_init();
    rec_reset();
    media_fixture();
}

static int run(void)
{
    return install_main(1, NULL, &api);
}

/* ========================================================================= */
/*  試験                                                                     */
/* ========================================================================= */

/* (1) /kernel.bin を読まない、(6b) 生カーネルの書込みが無い */
static void case_nokernel(void)
{
    int i, loader_sects = (LOADER_LEN + 511) / 512;
    setup();
    CHECK(run() == 0);
    CHECK(!rec_has(rec_open, rec_open_n, "/kernel.bin"));
    CHECK(!rec_has(rec_stat, rec_stat_n, "/kernel.bin"));
    CHECK_NOSTR("kernel.bin");
    CHECK_NOSTR("Written KERNEL");
    /* 生の書込みは IPL(0,1) / PT(1,1) / ローダ(2,n) の 3 回だけ。
     * LBA 6 はローダの 2..17 に含まれるので「6 に書かない」とはしない。 */
    CHECK(rec_wr_n == 3);
    CHECK(rec_wr[0].lba == 0 && rec_wr[0].cnt == 1);
    CHECK(rec_wr[1].lba == 1 && rec_wr[1].cnt == 1);
    CHECK(rec_wr[2].lba == 2 && rec_wr[2].cnt == (u32)loader_sects);
    for (i = 0; i < rec_wr_n; i++)
        CHECK(rec_wr[i].lba <= 2);   /* ローダより後ろへの生書きは無い */
    CHECK_STR("OS32 HDD Installer v4.1");
    CHECK_STR("[1/3]");
    CHECK_STR("[2/3]");
    CHECK_STR("[3/3]");
}

/* (3) /hd0/boot/vmkernel.lz4 の長さ一致 */
static void case_vmkernel(void)
{
    setup();
    CHECK(run() == 0);
    CHECK(rec_has(rec_mkdir, rec_mkdir_n, "/hd0/boot"));
    CHECK(fx_exists("/hd0/boot/vmkernel.lz4"));
    CHECK(fx_size("/hd0/boot/vmkernel.lz4") == LZ4_SIZE);
    CHECK(fx_content_ok("/hd0/boot/vmkernel.lz4", LZ4_SIZE));
    CHECK_STR("vmkernel.lz4 -> /boot (470000 bytes)");
    CHECK_STR("Installation complete");
}

/* (4) 宛先が小文字、(5) profile は写らない */
static void case_lower(void)
{
    setup();
    fx_dir("/ETC/RC.D");
    fx_put("/ETC/RC.D/BOOT.SH", 12);
    CHECK(run() == 0);
    CHECK(fx_exists("/hd0/sys/shell.bin"));
    CHECK(fx_exists("/hd0/bin/ls.bin"));
    CHECK(fx_exists("/hd0/sbin/init.bin"));
    CHECK(fx_exists("/hd0/etc/settings.db"));
    CHECK(fx_content_ok("/hd0/etc/settings.db", 2048));
    CHECK(!fx_exists("/hd0/etc/SETTINGS.DB"));
    CHECK(!fx_exists("/hd0/sys/SHELL.BIN"));
    /* 再帰しても各成分が小文字 */
    CHECK(rec_has(rec_mkdir, rec_mkdir_n, "/hd0/etc/rc.d"));
    CHECK(fx_exists("/hd0/etc/rc.d/boot.sh"));
    CHECK(!fx_exists("/hd0/etc/rc.d/BOOT.SH"));
    /* profile は写さない (FDD 用の PATH が HDD に残ると /usr/bin が消える) */
    CHECK(!fx_exists("/hd0/etc/profile"));
    CHECK(!fx_exists("/hd0/etc/PROFILE"));
}

/* (2) 事前検査: 欠損・空で 1 バイトも書かない */
static void case_precheck(void)
{
    setup();
    fx_rm("/VMKRNL.LZ4");
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0);
    CHECK(!fx_exists("/hd0/boot/vmkernel.lz4"));
    CHECK_STR("VMKRNL.LZ4");
    CHECK_STR("[FAIL]");

    setup();
    fx_put("/VMKRNL.LZ4", 0);
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0);

    setup();
    fx_rm("/SYS/BOOT_HDD.BIN");
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0);
    CHECK_STR("boot_hdd.bin");

    setup();
    fx_rm("/SYS/LOADER_H.BIN");
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0);
    CHECK_STR("loader_h.bin");

    /* 承認前なので確認も出ない */
    setup();
    fx_rm("/VMKRNL.LZ4");
    CHECK(run() == 1);
    CHECK_NOSTR("ERASED");
}

/* 承認しない: 何も書かず 0 (回復モードの作法と同じ) */
static void case_decline(void)
{
    setup();
    key_script = "N";
    CHECK(run() == 0);
    CHECK(rec_wr_n == 0);
    CHECK(!fx_exists("/hd0/boot/vmkernel.lz4"));
    CHECK_STR("Installation aborted");
    CHECK_NOSTR("Installation complete");
}

/* (6) ブート段 / format / mount の失敗で 1 */
static void case_boot_fail(void)
{
    int lba;
    for (lba = 0; lba <= 2; lba++) {
        setup();
        inj_ide_write_fail_lba = lba;
        CHECK(run() == 1);
        CHECK_STR("[FAIL]");
        CHECK_NOSTR("Installation complete");
    }
    setup();
    inj_format_fail = 1;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    setup();
    inj_mount_fail = 1;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
}

/* (6) コピーの失敗が main の終了コードまで届く */
static void case_copy_fail(void)
{
    /* read の負 (EOF と区別する): 最初の read で負 = 旧コードなら 0 バイトで成功 */
    setup();
    inj_read_neg = "/BIN/LS.BIN";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");

    /* 途中の read が負 (lz4 の 2 塊目) */
    setup();
    inj_read_neg = "/VMKRNL.LZ4";
    inj_read_neg_after = 1;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    /* short write */
    setup();
    inj_write_short = "/hd0/sys/shell.bin";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    /* lz4 の short write = 長さ不一致 */
    setup();
    inj_write_short = "/hd0/boot/vmkernel.lz4";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    /* mkdir の失敗 (再帰先) */
    setup();
    fx_dir("/ETC/RC.D");
    fx_put("/ETC/RC.D/BOOT.SH", 12);
    inj_mkdir_fail = "/hd0/etc/rc.d";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    /* /hd0/boot の mkdir が失敗 */
    setup();
    inj_mkdir_fail = "/hd0/boot";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");

    /* sys_ls の負 (列挙の頭から失敗、S3I2-K で FAT が返すようになる) */
    setup();
    inj_ls_fail = "/bin";
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");

    /* sys_ls の負 (**途中**: 何件か callback を呼んでから負)。件数だけを
     * 見ていると「1 件写せたから成功」に見えてしまう形。 */
    setup();
    fx_put("/SYS/SHLIB.BIN", 120);
    inj_ls_fail = "/sys";
    inj_ls_fail_after = 2;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");

    /* 最後の 1 件を渡した後で負 (全件渡ってから落ちる形) */
    setup();
    inj_ls_fail = "/etc";
    inj_ls_fail_after = 99;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");
}

/* B1 (往復 1): 初期ディレクトリ作成の失敗も終了 1 まで届く。
 * どれか 1 つでも作れなければ、その先のコピーと sync が通っても未完成。 */
static void case_mkdir_init(void)
{
    static const char *const dirs[12] = {
        "/hd0/boot", "/hd0/sys", "/hd0/bin", "/hd0/sbin", "/hd0/etc",
        "/hd0/usr", "/hd0/usr/bin", "/hd0/usr/man", "/hd0/data",
        "/hd0/home", "/hd0/home/user", "/hd0/tmp"
    };
    int i;
    for (i = 0; i < 12; i++) {
        setup();
        inj_mkdir_fail = dirs[i];
        if (run() != 1) {
            fprintf(stderr, "FAIL %s: mkdir %s failed but install returned 0\n"
                    "--- output ---\n%s", __func__, dirs[i], cap_buf);
            exit(1);
        }
        CHECK_STR("[FAIL]");
        CHECK_NOSTR("Installation complete");
    }
    /* 全部作れる正常系では 12 個すべてを作る */
    setup();
    CHECK(run() == 0);
    for (i = 0; i < 12; i++)
        CHECK(rec_has(rec_mkdir, rec_mkdir_n, dirs[i]));
}

/* 列挙の境界 (MAX_FILES = 64) と再帰の深さ (4 まで) */
static void case_bounds(void)
{
    char name[64];
    int i;

    /* ちょうど 64 件は取りこぼさない */
    setup();
    fx_rm("/BIN/LS.BIN");
    for (i = 0; i < 64; i++) {
        sprintf(name, "/BIN/F%02d.BIN", i);
        fx_put(name, 8);
    }
    CHECK(run() == 0);
    CHECK(fx_exists("/hd0/bin/f00.bin"));
    CHECK(fx_exists("/hd0/bin/f63.bin"));

    /* 65 件は取りこぼすので失敗 */
    setup();
    fx_rm("/BIN/LS.BIN");
    for (i = 0; i < 65; i++) {
        sprintf(name, "/BIN/F%02d.BIN", i);
        fx_put(name, 8);
    }
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");

    /* 深さ 4 (=/etc/d1/d2/d3/d4) までは写る */
    setup();
    fx_dir("/ETC/D1"); fx_dir("/ETC/D1/D2");
    fx_dir("/ETC/D1/D2/D3"); fx_dir("/ETC/D1/D2/D3/D4");
    fx_put("/ETC/D1/D2/D3/D4/DEEP.TXT", 5);
    CHECK(run() == 0);
    CHECK(fx_exists("/hd0/etc/d1/d2/d3/d4/deep.txt"));

    /* 深さ 5 は黙って写し漏らさず失敗にする */
    setup();
    fx_dir("/ETC/D1"); fx_dir("/ETC/D1/D2");
    fx_dir("/ETC/D1/D2/D3"); fx_dir("/ETC/D1/D2/D3/D4");
    fx_dir("/ETC/D1/D2/D3/D4/D5");
    fx_put("/ETC/D1/D2/D3/D4/D5/DEEP.TXT", 5);
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");
}

/* 注入なしの長さ不一致 (stat だけが大きい = 正常 EOF で短く終わる) と、
 * ソース名の綴りが媒体のまま保たれること */
static void case_srcname(void)
{
    setup();
    CHECK(run() == 0);
    /* 開いたのは媒体の綴りそのもの。小文字版は開いていない。 */
    CHECK(rec_has_exact(rec_open, rec_open_n, "/VMKRNL.LZ4"));
    CHECK(rec_has_exact(rec_open, rec_open_n, "/sys/SHELL.BIN"));
    CHECK(!rec_has_exact(rec_open, rec_open_n, "/sys/shell.bin"));
    CHECK(rec_has_exact(rec_open, rec_open_n, "/etc/SETTINGS.DB"));
    CHECK(!rec_has_exact(rec_open, rec_open_n, "/etc/settings.db"));
    /* 宛先は小文字 */
    CHECK(rec_has_exact(rec_open, rec_open_n, "/hd0/sys/shell.bin"));

    /* 読みは正常に EOF で終わるのに長さが足りない (媒体の申告より短い) */
    setup();
    inj_stat_big = "/VMKRNL.LZ4";
    inj_stat_big_extra = 4096;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");
}

/* (6) vfs_sync の失敗 */
static void case_sync_fail(void)
{
    setup();
    inj_sync_fail = 1;
    CHECK(run() == 1);
    CHECK_STR("[FAIL]");
    CHECK_NOSTR("Installation complete");
}

/* (7) IdeInfo は 96 B の実型 */
static void case_idetype(void)
{
    setup();
    CHECK(sizeof(IdeInfo) == sizeof(IdeIdentifyWire));
    CHECK(offsetof(IdeInfo, total_sectors) == offsetof(IdeIdentifyWire, total_sectors));
    CHECK(offsetof(IdeInfo, cylinders) == offsetof(IdeIdentifyWire, cylinders));
    CHECK(offsetof(IdeInfo, heads) == offsetof(IdeIdentifyWire, heads));
    CHECK(offsetof(IdeInfo, sectors) == offsetof(IdeIdentifyWire, sectors));
    CHECK(offsetof(IdeInfo, size_mb) == offsetof(IdeIdentifyWire, size_mb));
    CHECK(offsetof(IdeInfo, model) == offsetof(IdeIdentifyWire, model));
    CHECK(offsetof(IdeInfo, serial) == offsetof(IdeIdentifyWire, serial));
    CHECK(offsetof(IdeInfo, firmware) == offsetof(IdeIdentifyWire, firmware));
    CHECK(offsetof(IdeInfo, lba_supported) == offsetof(IdeIdentifyWire, lba_supported));
    CHECK(offsetof(IdeInfo, phys_sector_size) ==
          offsetof(IdeIdentifyWire, phys_sector_size));
    /* 末尾が構造体の最後であること (後ろに別の物を足していない) */
    CHECK(offsetof(IdeInfo, phys_sector_size) + sizeof(u16) <= sizeof(IdeInfo));
    /* 実型で受けていることを通常経路でも確認 (贋 identify が 96 B 書く) */
    CHECK(run() == 0);
    CHECK_STR("OS32 FAKE DISK");
    CHECK_STR("200 MB");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "nokernel")) case_nokernel();
    else if (!strcmp(argv[1], "vmkernel")) case_vmkernel();
    else if (!strcmp(argv[1], "lower")) case_lower();
    else if (!strcmp(argv[1], "precheck")) case_precheck();
    else if (!strcmp(argv[1], "decline")) case_decline();
    else if (!strcmp(argv[1], "boot_fail")) case_boot_fail();
    else if (!strcmp(argv[1], "copy_fail")) case_copy_fail();
    else if (!strcmp(argv[1], "mkdir_init")) case_mkdir_init();
    else if (!strcmp(argv[1], "bounds")) case_bounds();
    else if (!strcmp(argv[1], "srcname")) case_srcname();
    else if (!strcmp(argv[1], "sync_fail")) case_sync_fail();
    else if (!strcmp(argv[1], "idetype")) case_idetype();
    else return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
