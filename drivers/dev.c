/* ======================================================================== */
/*  DEV.C — デバイス抽象化層 実装                                           */
/*                                                                          */
/*  初期登録デバイス:                                                        */
/*    fdd0 — FDDユニット0 (PC-98 2HD 1MB, DA/UA=0x90)                      */
/*    con  — コンソール (キーボード入力 + テキストVRAM出力)                  */
/* ======================================================================== */

#include "dev.h"
#include "disk.h"
#include "ide.h"

/* ======== デバイステーブル ======== */
static Device *dev_table[MAX_DEVICES];
static int     dev_num = 0;

/* ======== ユーティリティ ======== */
static int dev_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ======================================================================== */
/*  FDD0 ドライバ                                                          */
/* ======================================================================== */

static int fdd0_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return disk_read_lba(0, lba, count, buf);
}

static int fdd0_write(Device *self, int lba, int count, const void *buf)
{
    (void)self;
    return disk_write_lba(0, lba, count, buf);
}

static int fdd1_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return disk_read_lba(1, lba, count, buf);
}

static int fdd1_write(Device *self, int lba, int count, const void *buf)
{
    (void)self;
    return disk_write_lba(1, lba, count, buf);
}

static Device fdd0_dev = {
    "fd0",
    DEV_BLOCK,
    DISK_SECT_SZ,           /* セクタサイズ */
    DISK_TOTAL_SEC,         /* 総セクタ数 */
    fdd0_read,
    fdd0_write,
    0, 0,                   /* chr_read, chr_write */
    0,                      /* ioctl */
    0                       /* priv */
};

static Device fdd1_dev = {
    "fd1",
    DEV_BLOCK,
    DISK_SECT_SZ,           /* セクタサイズ */
    DISK_TOTAL_SEC,         /* 総セクタ数 */
    fdd1_read,
    fdd1_write,
    0, 0,                   /* chr_read, chr_write */
    0,                      /* ioctl */
    0                       /* priv */
};

/* ======================================================================== */
/*  HDD ドライバ (IDE)                                                      */
/* ======================================================================== */

static int hd0_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return ide_read_sectors(0, lba, count, buf);
}

static int hd0_write(Device *self, int lba, int count, const void *buf)
{
    (void)self;
    return ide_write_sectors(0, lba, count, buf);
}

static int hd1_read(Device *self, int lba, int count, void *buf)
{
    (void)self;
    return ide_read_sectors(1, lba, count, buf);
}

static int hd1_write(Device *self, int lba, int count, const void *buf)
{
    (void)self;
    return ide_write_sectors(1, lba, count, buf);
}

static Device hd0_dev = {
    "hd0",
    DEV_BLOCK,
    512,                    /* セクタサイズ */
    0,                      /* 総セクタ数 (初期化時に設定) */
    hd0_read,
    hd0_write,
    0, 0, 0, 0
};

static Device hd1_dev = {
    "hd1",
    DEV_BLOCK,
    512,
    0,
    hd1_read,
    hd1_write,
    0, 0, 0, 0
};

/* HDDを登録するユーティリティ */
void dev_register_hdd(int drive)
{
    IdeInfo info;
    if (ide_get_info(drive, &info) == IDE_OK) {
        if (drive == 0) {
            hd0_dev.total_sects = info.total_sectors;
            dev_register(&hd0_dev);
        } else if (drive == 1) {
            hd1_dev.total_sects = info.total_sectors;
            dev_register(&hd1_dev);
        }
    }
}

/* ======================================================================== */
/*  公開API                                                                 */
/* ======================================================================== */

void dev_init(void)
{
    int i;
    for (i = 0; i < MAX_DEVICES; i++) {
        dev_table[i] = 0;
    }
    dev_num = 0;

    /* 標準デバイス登録 */
    dev_register(&fdd0_dev);
    dev_register(&fdd1_dev);
}

int dev_register(Device *dev)
{
    if (dev_num >= MAX_DEVICES) return -1;
    dev_table[dev_num] = dev;
    dev_num++;
    return 0;
}

Device *dev_find(const char *name)
{
    int i;
    for (i = 0; i < dev_num; i++) {
        if (dev_table[i] && dev_streq(dev_table[i]->name, name)) {
            return dev_table[i];
        }
    }
    return 0;
}

Device *dev_get(int index)
{
    if (index < 0 || index >= dev_num) return 0;
    return dev_table[index];
}

int dev_count(void)
{
    return dev_num;
}

int dev_get_names(const char **names, int max)
{
    int i, n = 0;
    for (i = 0; i < dev_num && n < max; i++) {
        if (dev_table[i]) {
            names[n++] = dev_table[i]->name;
        }
    }
    return n;
}

int dev_api_get_info(int idx, char *name, int name_max, int *type, u32 *total_sects)
{
    Device *d = dev_get(idx);
    if (!d) return -1;
    {
        int i = 0;
        while (d->name[i] && i < name_max - 1) {
            name[i] = d->name[i];
            i++;
        }
        name[i] = '\0';
    }
    if (type) *type = d->type;
    if (total_sects) *total_sects = d->total_sects;
    return 0;
}
