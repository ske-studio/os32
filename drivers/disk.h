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

/* ======== ディスクAPI ======== */

/* セクタ読み込み
 *   drv:   ドライブ番号(0=fd0, 1=fd1)
 *   cyl:   シリンダ番号 (0-76)
 *   head:  ヘッド番号 (0-1)
 *   sect:  セクタ番号 (1-8)
 *   count: 読み込みセクタ数
 *   buf:   データバッファ
 * 戻り値: 0=成功, それ以外=エラー
 */
int disk_read(int drv, int cyl, int head, int sect, int count, void *buf);

/* セクタ書き込み */
int disk_write(int drv, int cyl, int head, int sect, int count, const void *buf);

/* LBA→CHS変換 */
void disk_lba_to_chs(int lba, int *cyl, int *head, int *sect);

/* CHS→LBA変換 */
int disk_chs_to_lba(int cyl, int head, int sect);

/* LBA単位のセクタ読み書き */
int disk_read_lba(int drv, int lba, int count, void *buf);
int disk_write_lba(int drv, int lba, int count, const void *buf);

#endif /* DISK_H */
