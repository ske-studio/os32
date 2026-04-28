/* ======================================================================== */
/*  DISKIO.C — FatFs ディスクI/Oブリッジ (OS32カーネル用)                    */
/*                                                                          */
/*  FatFsモジュールとOS32のディスクドライバ (FDC/IDE) を接続する。           */
/*                                                                          */
/*  物理ドライブマッピング:                                                 */
/*    pdrv=0 : FDD (µPD765A FDC, 1024B/sector, disk.c経由)                 */
/*    pdrv=1 : HDD (ATA/IDE PIO, 512B/sector, ide.c経由)                   */
/*                                                                          */
/*  出典: FatFs R0.15 Application Note (elm-chan.org)                       */
/* ======================================================================== */

#include "ff.h"
#include "diskio.h"

/* disk.h は CHS版 disk_read/disk_write を宣言しており、
 * FatFSの diskio.h の disk_read/disk_write と名前が衝突する。
 * LBA関数のみ forward宣言して回避。 */
extern int disk_read_lba(int drv, int lba, int count, void *buf);
extern int disk_write_lba(int drv, int lba, int count, const void *buf);

#include "ide.h"
#include "rtc.h"
#include "fdc.h"
#include "kstring.h"

/* 物理ドライブ番号 */
#define DRV_FDD   0
#define DRV_HDD   1

/* ドライブ状態 */
static DSTATUS fdd_status = STA_NOINIT;
static DSTATUS hdd_status = STA_NOINIT;

/* FDDドライブ番号 (OS32側: 通常0) */
static int fdd_drive = 0;
/* HDDドライブ番号 (OS32側: 通常0) */
static int hdd_drive = 0;


/* ======================================================================== */
/*  disk_status — ドライブ状態取得                                          */
/* ======================================================================== */

DSTATUS disk_status(BYTE pdrv)
{
    switch (pdrv) {
    case DRV_FDD:
        return fdd_status;
    case DRV_HDD:
        return hdd_status;
    }
    return STA_NOINIT;
}


/* ======================================================================== */
/*  disk_initialize — ドライブ初期化                                        */
/* ======================================================================== */

DSTATUS disk_initialize(BYTE pdrv)
{
    switch (pdrv) {
    case DRV_FDD:
        /* FDCはカーネル起動時に fdc_init() で初期化済み */
        fdd_status = 0;
        return fdd_status;

    case DRV_HDD:
        /* IDEはカーネル起動時に ide_init() で初期化済み */
        if (ide_drive_present(hdd_drive)) {
            hdd_status = 0;
        } else {
            hdd_status = STA_NOINIT;
        }
        return hdd_status;
    }
    return STA_NOINIT;
}


/* ======================================================================== */
/*  disk_read — セクタ読み込み                                              */
/* ======================================================================== */

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    int rc;
    UINT i;

    switch (pdrv) {
    case DRV_FDD:
        if (fdd_status & STA_NOINIT) return RES_NOTRDY;
        /* FDD: disk_read_lba() は1セクタずつ読む */
        for (i = 0; i < count; i++) {
            rc = disk_read_lba(fdd_drive, (int)(sector + i), 1,
                               buff + i * FDC_SECTOR_SIZE);
            if (rc != 0) return RES_ERROR;
        }
        return RES_OK;

    case DRV_HDD:
        if (hdd_status & STA_NOINIT) return RES_NOTRDY;
        /* IDE: ide_read_sectors() で複数セクタ一括読み込み */
        rc = ide_read_sectors(hdd_drive, (u32)sector, (u32)count, buff);
        if (rc != 0) return RES_ERROR;
        return RES_OK;
    }
    return RES_PARERR;
}


/* ======================================================================== */
/*  disk_write — セクタ書き込み                                             */
/* ======================================================================== */

#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    int rc;
    UINT i;

    switch (pdrv) {
    case DRV_FDD:
        if (fdd_status & STA_NOINIT) return RES_NOTRDY;
        /* FDD: disk_write_lba() は1セクタずつ書く */
        for (i = 0; i < count; i++) {
            rc = disk_write_lba(fdd_drive, (int)(sector + i), 1,
                                buff + i * FDC_SECTOR_SIZE);
            if (rc != 0) return RES_ERROR;
        }
        return RES_OK;

    case DRV_HDD:
        if (hdd_status & STA_NOINIT) return RES_NOTRDY;
        /* IDE: ide_write_sectors() で複数セクタ一括書き込み */
        rc = ide_write_sectors(hdd_drive, (u32)sector, (u32)count, buff);
        if (rc != 0) return RES_ERROR;
        return RES_OK;
    }
    return RES_PARERR;
}

#endif /* FF_FS_READONLY == 0 */


/* ======================================================================== */
/*  disk_ioctl — デバイス制御                                               */
/* ======================================================================== */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    IdeInfo ide_info;

    switch (pdrv) {
    case DRV_FDD:
        if (fdd_status & STA_NOINIT) return RES_NOTRDY;
        switch (cmd) {
        case CTRL_SYNC:
            /* PIO転送: バッファなし → 即完了 */
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = (LBA_t)FDC_TOTAL_SECTORS;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = (WORD)FDC_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1; /* 消去ブロック = 1セクタ */
            return RES_OK;
        }
        return RES_PARERR;

    case DRV_HDD:
        if (hdd_status & STA_NOINIT) return RES_NOTRDY;
        switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            if (ide_get_info(hdd_drive, &ide_info) != 0) return RES_ERROR;
            *(LBA_t *)buff = (LBA_t)ide_info.total_sectors;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;
        }
        return RES_PARERR;
    }
    return RES_PARERR;
}


/* ======================================================================== */
/*  get_fattime — FAT時刻取得 (RTCから)                                     */
/* ======================================================================== */

DWORD get_fattime(void)
{
    RTC_Time t;
    DWORD fattime;
    int year;

    rtc_read(&t);

    /* RTC_Time.year は 00-99 (2桁) → 2000年代として扱う */
    year = (int)t.year;
    if (year < 80) year += 2000;  /* 00-79 → 2000-2079 */
    else           year += 1900;  /* 80-99 → 1980-1999 */

    /* FAT時刻フォーマット:
     *   bit[31:25] = 年 (1980年起点, 0-127)
     *   bit[24:21] = 月 (1-12)
     *   bit[20:16] = 日 (1-31)
     *   bit[15:11] = 時 (0-23)
     *   bit[10:5]  = 分 (0-59)
     *   bit[4:0]   = 秒/2 (0-29)
     */
    fattime = ((DWORD)(year - 1980) << 25)
            | ((DWORD)t.month << 21)
            | ((DWORD)t.day << 16)
            | ((DWORD)t.hour << 11)
            | ((DWORD)t.min << 5)
            | ((DWORD)(t.sec / 2));

    return fattime;
}
