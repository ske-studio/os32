/* ======================================================================== */
/*  DISK.H — フロッピーディスクI/O (PC-98 1MB 2HD)                         */
/*                                                                          */
/*  FDCドライバ (fdc.c) 経由でセクタ読み書きを行う。                        */
/*  BIOSトランポリンは使用しない。                                          */
/* ======================================================================== */

#ifndef DISK_H
#define DISK_H

#include "fdc.h"    /* FDCドライバAPI + u8/u16/u32型 */

/* ======== ディスクパラメータ (fdc.hの定義を再エクスポート) ======== */
#define DISK_CYL       FDC_CYLINDERS
#define DISK_HEAD      FDC_HEADS
#define DISK_SPT       FDC_SPT
#define DISK_SECT_SZ   FDC_SECTOR_SIZE
#define DISK_TOTAL_SEC FDC_TOTAL_SECTORS

/* CHS版 disk_read_chs / disk_write_chs は disk.c 内部のstaticに移行済み。
 * 外部からは LBA版のみ使用すること。 */

/* LBA→CHS変換 */
void disk_lba_to_chs(int lba, int *cyl, int *head, int *sect);

/* CHS→LBA変換 */
int disk_chs_to_lba(int cyl, int head, int sect);

/* LBA単位のセクタ読み書き */
int disk_read_lba(int drv, int lba, int count, void *buf);
int disk_write_lba(int drv, int lba, int count, const void *buf);

#endif /* DISK_H */
