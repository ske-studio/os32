/* ======================================================================== */
/*  DISK.C — フロッピーディスクI/O実装                                     */
/*                                                                          */
/*  FDCドライバ (fdc.c) 経由でセクタ読み書きを行う。                        */
/*  BIOSトランポリン (INT 1Bh) は使用しない。                               */
/*  PC9800Bible §2-9 準拠                                                   */
/* ======================================================================== */

#include "disk.h"

/* ======================================================================== */
/*  CHS ↔ LBA 変換                                                         */
/* ======================================================================== */

void disk_lba_to_chs(int lba, int *cyl, int *head, int *sect)
{
    *sect = (lba % DISK_SPT) + 1;       /* セクタは1始まり */
    *head = (lba / DISK_SPT) % DISK_HEAD;
    *cyl  = lba / (DISK_SPT * DISK_HEAD);
}

int disk_chs_to_lba(int cyl, int head, int sect)
{
    return (cyl * DISK_HEAD + head) * DISK_SPT + (sect - 1);
}

/* ======================================================================== */
/*  セクタ読込 (FDCドライバ経由)                                            */
/* ======================================================================== */

int disk_read(int drv, int cyl, int head, int sect, int count, void *buf)
{
    int i, ret;
    u8 *p = (u8 *)buf;

    for (i = 0; i < count; i++) {
        ret = fdc_read_sector(drv, cyl, head, sect, p);
        if (ret != 0) return ret;
        p += FDC_SECTOR_SIZE;

        /* 次のセクタに進む */
        sect++;
        if (sect > DISK_SPT) {
            sect = 1;
            head++;
            if (head >= DISK_HEAD) {
                head = 0;
                cyl++;
            }
        }
    }
    return 0;
}

/* ======================================================================== */
/*  セクタ書込 (FDCドライバ経由)                                            */
/* ======================================================================== */

int disk_write(int drv, int cyl, int head, int sect, int count, const void *buf)
{
    int i, ret;
    const u8 *p = (const u8 *)buf;

    for (i = 0; i < count; i++) {
        ret = fdc_write_sector(drv, cyl, head, sect, p);
        if (ret != 0) return ret;
        p += FDC_SECTOR_SIZE;

        /* 次のセクタに進む */
        sect++;
        if (sect > DISK_SPT) {
            sect = 1;
            head++;
            if (head >= DISK_HEAD) {
                head = 0;
                cyl++;
            }
        }
    }
    return 0;
}

/* ======================================================================== */
/*  LBA単位の読み書き (CHS変換付き)                                        */
/* ======================================================================== */

int disk_read_lba(int drv, int lba, int count, void *buf)
{
    int cyl, head, sect;
    disk_lba_to_chs(lba, &cyl, &head, &sect);
    return disk_read(drv, cyl, head, sect, count, buf);
}

int disk_write_lba(int drv, int lba, int count, const void *buf)
{
    int cyl, head, sect;
    disk_lba_to_chs(lba, &cyl, &head, &sect);
    return disk_write(drv, cyl, head, sect, count, buf);
}
