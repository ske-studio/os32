/* ========================================================================= */
/*  INSTALL_FRESH_HOST.C — 票 S3I2-I (docs/archive/settings/TASK_S3I2.md §1) */
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
#include "drivers/pc98pt.h"     /* 区画表を読み戻して確かめる (段 2) */
#include "userland/system/inst_disk.h"

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
static int  inj_readback_bad_lba = -1; /* この LBA の読み戻しを 1 バイト違える */
static int  inj_write_corrupt_lba = -1;/* この LBA へは 1 バイト違えて書く (媒体に残る) */
static int  inj_format_fail;
static const char *inj_ls_size_name;  /* 列挙がこの名前に名乗らせる長さ */
static u32  inj_ls_size;
static int  inj_umount_fail;
static int  inj_mount_stays;          /* umount しても hd0 のマウントが残る */
static int  inj_root_hd0;             /* ルート (/) が hd0 */
static int  hd0_mounts;               /* dev_mount_count(0) */
static int  hd0_at_hd0;               /* /hd0 にマウントされている */

/* ---- 贋の hd0: LBA 0〜31 だけを持つ (区画表・IPL・ローダの帯) ---- */
#define DISK_MODEL_SECTS 32
static unsigned char disk[DISK_MODEL_SECTS][512];
static HddGeom geom;

/* 起きた順の記録 ("F" format_at, "W<lba>" 書き, "M" mount, "U" umount) */
static char ev[4096];
static void ev_add(const char *e) { strcat(ev, e); strcat(ev, " "); }
static u32  fmt_start, fmt_len;
static int  fmt_calls;
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
    inj_readback_bad_lba = -1;
    inj_write_corrupt_lba = -1;
    inj_format_fail = 0;
    inj_ls_size_name = NULL;
    inj_ls_size = 0;
    inj_umount_fail = 0;
    inj_mount_stays = 0;
    inj_root_hd0 = 0;
    hd0_mounts = 0;
    hd0_at_hd0 = 0;
    memset(disk, 0, sizeof(disk));
    ev[0] = '\0';
    fmt_start = fmt_len = 0;
    fmt_calls = 0;
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

/* 幾何: 8/17 の 200MB NHD (NP21/W) か、16/63 の 8GB (実機 Ra266) */
static void geom_817(void)
{
    memset(&geom, 0, sizeof(geom));
    geom.bios_queried = 1; geom.bios_valid = 1;
    geom.bios_heads = 8; geom.bios_spt = 17; geom.bios_cyl = 3011;
    geom.bios_seclen = 512;
    geom.ata_def_cyl = 3011; geom.ata_def_heads = 8; geom.ata_def_spt = 17;
    geom.ata_cur_cyl = 3011; geom.ata_cur_heads = 8; geom.ata_cur_spt = 17;
    geom.ata_w49 = 0x0200; geom.ata_w53 = 1;
    geom.ata_total = 409600;
    geom.ata_present = 1; geom.addr_mode = HDD_AMODE_LBA28; geom.bios_da = 0x80;
}

static void geom_1663(void)
{
    memset(&geom, 0, sizeof(geom));
    geom.bios_queried = 1; geom.bios_valid = 1;
    geom.bios_heads = 16; geom.bios_spt = 63; geom.bios_cyl = 16382;
    geom.bios_seclen = 512;
    geom.ata_def_cyl = 16382; geom.ata_def_heads = 16; geom.ata_def_spt = 63;
    geom.ata_cur_cyl = 16382; geom.ata_cur_heads = 16; geom.ata_cur_spt = 63;
    geom.ata_w49 = 0x0200; geom.ata_w53 = 1;
    geom.ata_total = 16514063;
    geom.ata_present = 1; geom.addr_mode = HDD_AMODE_LBA28; geom.bios_da = 0x80;
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

/* 1 セクタずつの書き込み (段 2 の手順は ide_write_sector だけを使う) */
static int fake_ide_write_sector(int drv, u32 lba, const void *buf)
{
    char e[16];
    CHECK(drv == 0);
    if (inj_ide_write_fail_lba >= 0 && (u32)inj_ide_write_fail_lba == lba) return -1;
    if (rec_wr_n < REC_MAX) {
        rec_wr[rec_wr_n].lba = lba;
        rec_wr[rec_wr_n].cnt = 1;
        rec_wr_n++;
    }
    CHECK(lba < DISK_MODEL_SECTS);            /* 区画の中へは ext2_format_at だけ */
    memcpy(disk[lba], buf, 512);
    if (inj_write_corrupt_lba >= 0 && (u32)inj_write_corrupt_lba == lba)
        disk[lba][16] ^= 0x20;                  /* 区画表なら名前 'O' → 'o' */
    sprintf(e, "W%u", (unsigned)lba);
    ev_add(e);
    return 0;
}

/* 旧経路 (複数セクタ) は段 2 では使わない。呼ばれたら落とす */
static int fake_ide_write_sectors(int drv, u32 lba, u32 cnt, const void *buf)
{
    (void)drv; (void)lba; (void)cnt; (void)buf;
    CHECK(!"ide_write_sectors must not be used");
    return -1;
}

static int fake_ide_read_sector(int drv, u32 lba, void *buf)
{
    CHECK(drv == 0);
    CHECK(lba < DISK_MODEL_SECTS);
    memcpy(buf, disk[lba], 512);
    if (inj_readback_bad_lba >= 0 && (u32)inj_readback_bad_lba == lba && rec_wr_n > 0)
        ((unsigned char *)buf)[7] ^= 0x5A;
    return 0;
}

static int inj_geom_fail;
static int fake_hdd_geom_info(int drv, HddGeom *out)
{
    CHECK(drv == 0);
    if (inj_geom_fail) return -1;
    *out = geom;
    return 0;
}

/* 旧経路 (区画表から位置を決める format) は段 2 では使わない */
static int fake_ext2_format(int drv, u32 sectors)
{ (void)drv; (void)sectors; CHECK(!"ext2_format must not be used"); return -1; }

static int fake_ext2_format_at(int drv, u32 start, u32 len)
{
    CHECK(drv == 0);
    fmt_calls++;
    fmt_start = start;
    fmt_len = len;
    ev_add("F");
    return inj_format_fail ? -1 : 0;
}

static int fake_sys_mount(const char *pre, const char *dev, const char *fs)
{
    (void)fs;
    if (!strcmp(pre, "/hd0")) {
        CHECK(!strcmp(dev, "hd0"));
        ev_add("M");
        if (inj_mount_fail) return -1;
        hd0_mounts++;
        hd0_at_hd0 = 1;
    }
    return 0;
}

static void fake_sys_umount(const char *pre) { (void)pre; CHECK(!"use sys_umount_checked"); }

static int fake_sys_umount_checked(const char *pre)
{
    ev_add("U");
    if (inj_umount_fail) return -5;
    if (!strcmp(pre, "/hd0") && hd0_at_hd0) {
        hd0_at_hd0 = 0;
        if (!inj_mount_stays) hd0_mounts--;
        return 0;
    }
    return -2;
}

static int fake_sys_is_mounted(const char *pre)
{ return (!strcmp(pre, "/hd0") && hd0_at_hd0) ? 1 : 0; }

static int fake_dev_mount_count(int drv) { CHECK(drv == 0); return hd0_mounts; }

static const char *fake_vfs_devname(const char *pre)
{
    if (!strcmp(pre, "/")) return inj_root_hd0 ? "hd0" : "fd0";
    if (!strcmp(pre, "/hd0")) return hd0_at_hd0 ? "hd0" : NULL;
    return NULL;
}

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
        if (inj_ls_size_name && fx_same(fx[i].path, inj_ls_size_name))
            e.size = inj_ls_size;
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
    api.ide_write_sector = fake_ide_write_sector;
    api.ide_read_sector = fake_ide_read_sector;
    api.hdd_geom_info = fake_hdd_geom_info;
    api.ext2_format = fake_ext2_format;
    api.ext2_format_at = fake_ext2_format_at;
    api.sys_mount = fake_sys_mount;
    api.sys_umount = fake_sys_umount;
    api.sys_umount_checked = fake_sys_umount_checked;
    api.sys_is_mounted = fake_sys_is_mounted;
    api.dev_mount_count = fake_dev_mount_count;
    api.vfs_devname = fake_vfs_devname;
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
    inj_geom_fail = 0;
    geom_817();
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
    /* 生の書込みは (段 2) PT(1) → ローダ(2..) → IPL(0) の順に 1 セクタずつ。
     * LBA 6 はローダの 2..17 に含まれるので「6 に書かない」とはしない。 */
    CHECK(rec_wr_n == 2 + loader_sects);
    CHECK(rec_wr[0].lba == 1);
    for (i = 0; i < loader_sects; i++) CHECK(rec_wr[1 + i].lba == (u32)(2 + i));
    CHECK(rec_wr[rec_wr_n - 1].lba == 0);
    for (i = 0; i < rec_wr_n; i++)
        CHECK(rec_wr[i].lba < (u32)(2 + loader_sects));   /* ローダより後ろへの生書きは無い */
    CHECK_STR("OS32 HDD Installer v5.0");
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

/* ========================================================================= */
/*  段 2 (票 TASK_HDD_INSTALL 段 2 / §1-v3 N4・N6・N8・R3-1)                 */
/* ========================================================================= */

#define PLAN817_START 1632u
#define PLAN817_LEN   407864u   /* (409600 - 1632) を 136 に切り下げ */
#define PLAN1663_START 2016u
#define PLAN1663_LEN   524160u  /* 256MiB を 1008 に切り下げ */

/* 区画表 (標準配置) に OS32 の項目を置く */
static void pt_std(int idx, u32 start, u32 len, u32 heads, u32 spt)
{
    PC98PartEntry e;
    CHECK(pc98pt_make_os32(&e, start, len, heads, spt) == PC98PT_OK);
    CHECK(pc98pt_put(disk[1], idx, &e) == PC98PT_OK);
}

/* 2026-09-23 までの cdinst / install が書いた旧配置の項目 (cdinst.c の
 * write_partition_table と同じバイト列) */
static void pt_legacy(u32 start_cyl, u32 end_cyl, u32 heads, u32 spt)
{
    unsigned char *pt = disk[1];
    int i;
    memset(pt, 0, 32);
    pt[0] = 0x80; pt[1] = 0xE2;
    pt[6] = 0; pt[7] = 0;
    pt[8] = (unsigned char)(start_cyl & 0xFF); pt[9] = (unsigned char)(start_cyl >> 8);
    pt[10] = (unsigned char)(spt - 1); pt[11] = (unsigned char)(heads - 1);
    pt[12] = (unsigned char)(end_cyl & 0xFF); pt[13] = (unsigned char)(end_cyl >> 8);
    pt[16] = 'O'; pt[17] = 'S'; pt[18] = '3'; pt[19] = '2';
    for (i = 20; i < 32; i++) pt[i] = ' ';
}

static void ipl_sig(void) { disk[0][510] = 0x55; disk[0][511] = 0xAA; }

/* 書く前に断った: 1 セクタも書かず、format も umount もしない */
#define CHECK_NOTHING_WRITTEN() do { \
    CHECK(rec_wr_n == 0); CHECK(fmt_calls == 0); \
    CHECK(strchr(ev, 'U') == NULL); CHECK(strchr(ev, 'F') == NULL); \
    CHECK_STR("Nothing was written"); \
    CHECK_NOSTR("Installation complete"); } while (0)

/* 出来た hd0: 区画表 (標準配置、項目 1 つ)・IPL の幾何・ローダの中身・format の範囲 */
static void check_disk(u32 start, u32 len, u32 heads, u32 spt, u32 total)
{
    unsigned long st = 0, ln = 0;
    int idx = -1, k;
    CHECK(pc98pt_count_used(disk[1]) == 1);
    CHECK(pc98pt_find_os32(disk[1], heads, spt, total, &idx, &st, &ln) == PC98PT_OK);
    CHECK(idx == 0 && st == start && ln == len);
    CHECK(disk[1][PC98PT_OFF_NAME] == 'O' && disk[1][PC98PT_OFF_NAME + 3] == '2');
    /* IPL の [8]/[9] = 区画表の CHS の幾何 = BIOS 幾何 */
    CHECK(disk[0][INST_IPL_OFF_HEADS] == heads && disk[0][INST_IPL_OFF_SPT] == spt);
    CHECK(disk[0][510] == 0x55 && disk[0][511] == 0xAA);
    for (k = 0; k < 510; k++) {
        if (k == INST_IPL_OFF_HEADS || k == INST_IPL_OFF_SPT) continue;
        CHECK(disk[0][k] == fx_byte(k));
    }
    for (k = 0; k < LOADER_LEN; k++) CHECK(disk[2 + k / 512][k % 512] == fx_byte(k));
    CHECK(fmt_calls == 1 && fmt_start == start && fmt_len == len);
    /* 区画の開始 = 1632 以上の最初のシリンダ境界 */
    CHECK(start % (heads * spt) == 0 && start >= 1632 && start - (heads * spt) < 1632);
}

/* 手順の順序: [U] F W1 M W2..W17 W0 (format → 区画表 → マウント → ローダ → IPL) */
static void check_order(int with_umount)
{
    char want[256];
    int k;
    want[0] = '\0';
    if (with_umount) strcat(want, "U ");
    strcat(want, "F W1 M ");
    for (k = 2; k < 2 + LOADER_LEN / 512; k++) {
        char e[8];
        sprintf(e, "W%d ", k);
        strcat(want, e);
    }
    strcat(want, "W0 ");
    if (strcmp(ev, want) != 0) {
        fprintf(stderr, "FAIL order: got '%s' want '%s'\n", ev, want);
        exit(1);
    }
}

/* 8/17 の NHD: 開始 1632、IPL 8/17 */
static void case_geom817(void)
{
    setup();
    CHECK(run() == 0);
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);
    check_order(0);
    CHECK_STR("empty disk");
    CHECK_NOSTR("WILL BE LOST");
    CHECK_STR("Installation complete");

    /* BIOS は 8/17 に変換、ドライブ (IDENTIFY) は 16/63 を申告する: 区画表も
     * IPL も BIOS 幾何 (旧 install は IPL に IDENTIFY の幾何を書いた、F15) */
    setup();
    geom.ata_def_cyl = 16383; geom.ata_def_heads = 16; geom.ata_def_spt = 63;
    geom.ata_cur_cyl = 16383; geom.ata_cur_heads = 16; geom.ata_cur_spt = 63;
    CHECK(run() == 0);
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);
}

/* 16/63 の 8GB (実機 Ra266): 開始 2016、長さは 256MiB を 1008 で切り下げ、IPL 16/63。
 * 8/17 のまま書く旧 install なら IPL は 16/63 (IDENTIFY) で区画表は 8/17 だった (F15) */
static void case_geom1663(void)
{
    setup();
    geom_1663();
    CHECK(run() == 0);
    check_disk(PLAN1663_START, PLAN1663_LEN, 16, 63, 16514063);
    check_order(0);
    CHECK_STR("Installation complete");
}

/* モード: 再作成 (標準配置 / 旧配置 8/17) は通り、他は 1 セクタも書かずに断る */
static void case_modes(void)
{
    /* 再作成 (標準配置、hdprep の一時置き場 64MiB) — マウント中なら外してから */
    setup();
    pt_std(0, PLAN817_START, 136u * 900u, 8, 17);
    hd0_mounts = 1; hd0_at_hd0 = 1;
    CHECK(run() == 0);
    CHECK_STR("re-create the existing OS32 area");
    CHECK_STR("ALL FILES IN IT WILL BE LOST");
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);
    check_order(1);

    /* 再作成 16/63 (hdprep が作った 2016 の区画) */
    setup();
    geom_1663();
    pt_std(0, PLAN1663_START, 1008u * 100u, 16, 63);
    ipl_sig();
    CHECK(run() == 0);
    CHECK_STR("WILL BE LOST");
    check_disk(PLAN1663_START, PLAN1663_LEN, 16, 63, 16514063);

    /* 項目が表の 2 番目にあっても 1 つなら再作成。新しい表は項目 0 だけ */
    setup();
    pt_std(3, PLAN817_START, 136u * 900u, 8, 17);
    CHECK(run() == 0);
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);

    /* 旧配置 (旧 cdinst / install が 8/17 の NHD に書いた表) は作り直す */
    setup();
    pt_legacy(12, 409600u / 136u - 1u, 8, 17);
    ipl_sig();
    CHECK(run() == 0);
    CHECK_STR("old table layout");
    CHECK_STR("WILL BE LOST");
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);

    /* 旧配置でも 16/63 ではシリンダ 12 = LBA 12,096 ≠ 2016 → 断る */
    setup();
    geom_1663();
    pt_legacy(12, 2000, 16, 63);
    ipl_sig();
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("does not start where");

    /* 未知の区画 (sid 0x21、名前 MS-DOS) */
    setup();
    {
        PC98PartEntry e;
        CHECK(pc98pt_make_os32(&e, PLAN817_START, 136u * 100u, 8, 17) == PC98PT_OK);
        e.sys_id = 0x21;
        memcpy(e.name, "MS-DOS 6.20     ", 16);
        CHECK(pc98pt_put(disk[1], 0, &e) == PC98PT_OK);
    }
    ipl_sig();
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("OS32 did not create");

    /* sid は OS32 だが名前が違う */
    setup();
    pt_std(0, PLAN817_START, 136u * 100u, 8, 17);
    disk[1][PC98PT_OFF_NAME] = 'X';
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("OS32 did not create");

    /* 2 項目 (OS32 + もう 1 つ) */
    setup();
    pt_std(0, PLAN817_START, 136u * 100u, 8, 17);
    pt_std(1, PLAN817_START + 136u * 100u, 136u * 100u, 8, 17);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("two or more partitions");

    /* OS32 の項目だが開始が期待値でない (シリンダ 13) */
    setup();
    pt_std(0, PLAN817_START + 136u, 136u * 100u, 8, 17);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("does not start where");

    /* 区画項目が無いのに LBA 0 に 55AA */
    setup();
    ipl_sig();
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("55AA");

    /* OS32 の項目が壊れている (どちらの配置でも範囲にならない) */
    setup();
    disk[1][0] = 0x80; disk[1][1] = 0xE2;
    disk[1][8] = 200;       /* 標準: 開始セクタ 200 >= 17、旧: 開始シリンダ 200 > 終了 0 */
    memcpy(disk[1] + 16, "OS32            ", 16);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("entry is broken");
}

/* 事前検査の各失敗: 1 セクタも書かない (段 2-11、N6、N8) */
static void case_preflight(void)
{
    /* 大きさの境界: ローダ 8192 と vmkernel 508KiB ちょうどは通る */
    setup();
    fx_put("/VMKRNL.LZ4", 508 * 1024);
    CHECK(run() == 0);
    CHECK(fx_size("/hd0/boot/vmkernel.lz4") == 508 * 1024);

    setup();
    fx_put("/VMKRNL.LZ4", 508 * 1024 + 1);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("vmkernel.lz4 is empty or larger than 508 KiB");

    setup();
    fx_put("/SYS/LOADER_H.BIN", 8193);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than 8192");

    setup();
    fx_put("/SYS/BOOT_HDD.BIN", 513);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than 512");

    /* 展開先の容量: FD の 1 本が 300MB を名乗る (列挙の長さで数える) */
    setup();
    inj_ls_size_name = "/BIN/LS.BIN";
    inj_ls_size = 300u * 1024u * 1024u;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than ext2 can hold");     /* 1 ファイルの上限 (P2-4) が先 */

    /* 容量: 13MB の区画 (1632 + 200 シリンダ) に 30MB を名乗る 1 本 */
    setup();
    geom.ata_total = 1632u + 136u * 200u;
    inj_ls_size_name = "/BIN/LS.BIN";
    inj_ls_size = 30u * 1024u * 1024u;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("do not fit");

    /* 列挙の失敗は書く前に分かる (容量を数えられない) */
    setup();
    inj_ls_fail = "/sbin";
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();

    /* BIOS 幾何が無い (FD ローダが問い合わせていない) */
    setup();
    geom.bios_valid = 0;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("no BIOS geometry");

    /* I/O が既定の CHS しか無い */
    setup();
    geom.addr_mode = HDD_AMODE_CHS_DEF;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();

    /* ディスクが小さすぎる */
    setup();
    geom.ata_total = 5000;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("too small");

    /* ルートが hd0 (HDD 起動中の自分自身) */
    setup();
    inj_root_hd0 = 1;
    hd0_mounts = 1;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("root file system");

    /* 別の場所にマウントされている (/hd0 ではない) → 承認の前に断る */
    setup();
    hd0_mounts = 1; hd0_at_hd0 = 0;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("mounted somewhere other than /hd0");
    CHECK_NOSTR("will be unmounted");

    /* /hd0 と別の場所の両方 (2 つ) → 断る */
    setup();
    hd0_mounts = 2; hd0_at_hd0 = 1;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("mounted somewhere other than /hd0");

    /* 必須のファイル (/sys/shell.bin) が無い・空・ディレクトリ → 何も書かない
     * (Codex 往復 1 P1-3: 以前は shell の無い HDD が「完了」になった) */
    setup();
    fx_rm("/SYS/SHELL.BIN");
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("Missing /sys/shell.bin");
    setup();
    fx_put("/SYS/SHELL.BIN", 0);
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("Empty /sys/shell.bin");
    setup();
    fx_rm("/SYS/SHELL.BIN");
    fx_dir("/SYS/SHELL.BIN");
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("not a regular file");
    setup();
    fx_rm("/SYS/LOADER_H.BIN");
    fx_dir("/SYS/LOADER_H.BIN");
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("not a regular file");

    /* hdd_geom_info が失敗 → 分かる文言で断る */
    setup();
    inj_geom_fail = 1;
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("did not return the geometry of hd0");

    /* umount_checked が失敗 (sync の失敗など) */
    setup();
    hd0_mounts = 1; hd0_at_hd0 = 1;
    inj_umount_fail = 1;
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0 && fmt_calls == 0);
    CHECK_STR("umount /hd0 failed");
    CHECK_NOSTR("INCOMPLETE");

    /* umount したのにまだ残っている */
    setup();
    hd0_mounts = 1; hd0_at_hd0 = 1;
    inj_mount_stays = 1;
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0 && fmt_calls == 0);
    CHECK_STR("still mounted");

    /* 承認しなければ umount もしない */
    setup();
    hd0_mounts = 1; hd0_at_hd0 = 1;
    key_script = "n";
    CHECK(run() == 0);
    CHECK(rec_wr_n == 0 && fmt_calls == 0 && ev[0] == '\0');
}

/* 書いた後の失敗: INCOMPLETE と出して止まり、完了とは言わない (R3-1) */
static void case_incomplete(void)
{
    /* format の失敗 → 区画表は書かない (次の実行は空のディスクとして通る) */
    setup();
    inj_format_fail = 1;
    CHECK(run() == 1);
    CHECK(rec_wr_n == 0);
    CHECK_STR("INCOMPLETE");
    CHECK_NOSTR("Installation complete");

    /* 区画表の読み戻しが違う → マウントもローダ / IPL も書かない */
    setup();
    inj_readback_bad_lba = 1;
    CHECK(run() == 1);
    CHECK(rec_wr_n == 1 && rec_wr[0].lba == 1);
    CHECK(strchr(ev, 'M') == NULL);
    CHECK_STR("INCOMPLETE: partition table");
    CHECK_NOSTR("Installation complete");

    /* 通常のマウントの失敗 → ローダ / IPL を書かない */
    setup();
    inj_mount_fail = 1;
    CHECK(run() == 1);
    CHECK(rec_wr_n == 1 && rec_wr[0].lba == 1);
    CHECK(disk[0][510] == 0);
    CHECK_STR("INCOMPLETE: mount /hd0");
    CHECK_NOSTR("Installation complete");

    /* ローダの読み戻しが違う → IPL を書かない */
    setup();
    inj_readback_bad_lba = 5;
    CHECK(run() == 1);
    CHECK(disk[0][510] == 0);
    CHECK_STR("INCOMPLETE: loader");

    /* IPL の読み戻しが違う → 展開しない */
    setup();
    inj_readback_bad_lba = 0;
    CHECK(run() == 1);
    CHECK(!fx_exists("/hd0/boot/vmkernel.lz4"));
    CHECK_STR("INCOMPLETE: IPL");

    /* 展開 (コピー) と sync の失敗も INCOMPLETE (完了とは言わない) */
    setup();
    inj_read_neg = "/BIN/LS.BIN";
    CHECK(run() == 1);
    CHECK_STR("INCOMPLETE");
    CHECK_NOSTR("Installation complete");

    setup();
    inj_sync_fail = 1;
    CHECK(run() == 1);
    CHECK_STR("INCOMPLETE: sync failed");
    CHECK_NOSTR("Installation complete");
    /* INCOMPLETE は 1 回だけ出す、再起動してから入れ直すよう案内する */
    CHECK(strstr(strstr(cap_buf, "INCOMPLETE") + 1, "INCOMPLETE") == NULL);
    CHECK_STR("REBOOT, then run the installer again");

    setup();
    inj_readback_bad_lba = 5;
    CHECK(run() == 1);
    CHECK(strstr(strstr(cap_buf, "INCOMPLETE") + 1, "INCOMPLETE") == NULL);
    CHECK_STR("REBOOT");
}

/* 途中で止まった hd0 は次の実行で入れ直せる (空 / 再作成のどちらかになる) */
static void case_rerun(void)
{
    static unsigned char keep[DISK_MODEL_SECTS][512];

    /* マウントの失敗の後 (区画表だけ書いた) → 再作成モードで通る */
    setup();
    inj_mount_fail = 1;
    CHECK(run() == 1);
    memcpy(keep, disk, sizeof(keep));
    setup();
    memcpy(disk, keep, sizeof(keep));
    CHECK(run() == 0);
    CHECK_STR("re-create the existing OS32 area");
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);

    /* format の失敗の後 → 空のディスクとして通る */
    setup();
    inj_format_fail = 1;
    CHECK(run() == 1);
    memcpy(keep, disk, sizeof(keep));
    setup();
    memcpy(disk, keep, sizeof(keep));
    CHECK(run() == 0);
    CHECK_STR("empty disk");

    /* 区画表が媒体の上で壊れた (書いたものと違う) → INCOMPLETE と、ゲストでは
     * 直せない旨とホスト側の手当てを出す。次の実行は 1 セクタも書かずに断り、
     * 同じ案内を出す */
    setup();
    inj_write_corrupt_lba = 1;
    CHECK(run() == 1);
    CHECK_STR("INCOMPLETE: partition table");
    CHECK_STR("cannot be repaired");
    CHECK_STR("nhd-init");
    memcpy(keep, disk, sizeof(keep));
    setup();
    memcpy(disk, keep, sizeof(keep));
    CHECK(run() == 1);
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("OS32 did not create");
    CHECK_STR("nhd-init");

    /* 完了した hd0 をもう一度入れ直す (再作成) */
    setup();
    CHECK(run() == 0);
    memcpy(keep, disk, sizeof(keep));
    setup();
    memcpy(disk, keep, sizeof(keep));
    CHECK(run() == 0);
    CHECK_STR("WILL BE LOST");
    check_disk(PLAN817_START, PLAN817_LEN, 8, 17, 409600);
}

/* 段 fdset (tools/tests/test_packages.py case 9): argv[2] の一覧 (1 行
 * "<FD 上のパス> <大きさ>") を FAT の媒体として並べて install を通し、
 * /hd0 に出来たファイルを "<パス> <大きさ>" で argv[3] へ書く。
 * 中身の一致は大きさと fx_content_ok で見る (贋の中身は位置で決まる)。 */
static void case_fdset(const char *list, const char *out)
{
    FILE *f;
    char line[160];
    int i;

    api_init();
    rec_reset();
    geom_817();
    for (i = 0; i < FD_MAX; i++) fds[i].used = 0;
    fx_reset();
    fx_dir("/");
    f = fopen(list, "r");
    CHECK(f != NULL);
    while (fgets(line, sizeof(line), f)) {
        char path[96];
        int size, k;
        if (sscanf(line, "%95s %d", path, &size) != 2) continue;
        for (k = 0; path[k]; k++)             /* FAT は大文字で返す */
            if (path[k] >= 'a' && path[k] <= 'z') path[k] = (char)(path[k] - 32);
        for (k = 1; path[k]; k++) {           /* 親ディレクトリを作る */
            if (path[k] == '/') {
                path[k] = '\0';
                if (!fx_exists(path)) fx_dir(path);
                path[k] = '/';
            }
        }
        fx_put(path, size);
    }
    fclose(f);
    CHECK(run() == 0);
    f = fopen(out, "w");
    CHECK(f != NULL);
    for (i = 0; i < FX_MAX; i++) {
        if (!fx[i].used || fx[i].is_dir || !fx_is_hdd(fx[i].path)) continue;
        CHECK(fx_content_ok(fx[i].path, fx[i].size));
        fprintf(f, "%s %d\n", fx[i].path + 4, fx[i].size);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 2 && !(argc == 4 && !strcmp(argv[1], "fdset"))) return 2;
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
    else if (!strcmp(argv[1], "geom817")) case_geom817();
    else if (!strcmp(argv[1], "geom1663")) case_geom1663();
    else if (!strcmp(argv[1], "modes")) case_modes();
    else if (!strcmp(argv[1], "preflight")) case_preflight();
    else if (!strcmp(argv[1], "incomplete")) case_incomplete();
    else if (!strcmp(argv[1], "rerun")) case_rerun();
    else if (!strcmp(argv[1], "fdset") && argc >= 4) case_fdset(argv[2], argv[3]);
    else return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
