/* ======================================================================== */
/*  ATAPI.H — PC-98 IDE/ATAPI CD-ROM PIOドライバ                             */
/*                                                                          */
/*  IDEセカンダリバンク (I/O 0x430/0x432) に接続されたATAPI CD-ROMデバイスに  */
/*  PACKETコマンド (0xA0) を発行し、データ転送を行う。                       */
/*                                                                          */
/*  I/OポートはHDDドライバ (ide.h) と共有。バンク切替で選択する。            */
/*  セクタサイズは2048バイト (HDD=512バイトとは異なる)。                     */
/*                                                                          */
/*  参照: NP21/W (atapicmd.c, ideio.c), UNDOCUMENTED io_ide.md              */
/* ======================================================================== */

#ifndef ATAPI_H
#define ATAPI_H

#include "ide.h"

/* ======== CD-ROM セクタサイズ ======== */
#define ATAPI_SECTOR_SIZE    2048

/* ======== ATAPI / PACKET コマンド ======== */
#define ATAPI_CMD_PACKET         0xA0   /* PACKETコマンド (CDB送出) */
#define ATAPI_CMD_IDENTIFY_PKT   0xA1   /* IDENTIFY PACKET DEVICE */

/* ======== ATAPI シグネチャ (IDENTIFY時にCylLo/CylHiで返る) ======== */
#define ATAPI_SIG_CYL_LO    0x14
#define ATAPI_SIG_CYL_HI    0xEB

/* ======== SCSI CDB オペコード ======== */
#define SCSI_CMD_TEST_UNIT_READY  0x00
#define SCSI_CMD_REQUEST_SENSE    0x03
#define SCSI_CMD_INQUIRY          0x12
#define SCSI_CMD_READ_CAPACITY    0x25
#define SCSI_CMD_READ_10          0x28

/* ======== Interrupt Reason (Sector Count レジスタ) ビット ======== */
#define ATAPI_IR_CD    0x01   /* 1=コマンドパケット, 0=データ */
#define ATAPI_IR_IO    0x02   /* 1=デバイス→ホスト, 0=ホスト→デバイス */
#define ATAPI_IR_REL   0x04   /* バスリリース */

/* ======== エラーコード ======== */
#define ATAPI_OK           0
#define ATAPI_ERR_TIMEOUT -1
#define ATAPI_ERR_NO_DRIVE -2
#define ATAPI_ERR_IO      -3
#define ATAPI_ERR_NO_MEDIA -4

/* ======== CD-ROM 容量情報 ======== */
typedef struct {
    u32 total_sectors;   /* 総セクタ数 (2048B/セクタ) */
    u32 sector_size;     /* セクタサイズ (通常 2048) */
} AtapiCapacity;

/* ======== 公開API ======== */

/* ATAPI初期化: セカンダリバンクのCD-ROMを検出
 * 戻り値: 1=CD-ROM検出, 0=未検出 */
int atapi_init(void);

/* CD-ROM 存在チェック */
int atapi_present(void);

/* TEST UNIT READY: メディア挿入確認
 * 戻り値: ATAPI_OK=メディアあり, ATAPI_ERR_NO_MEDIA=なし */
int atapi_test_unit_ready(void);

/* READ CAPACITY: メディア容量取得 */
int atapi_read_capacity(AtapiCapacity *cap);

/* セクタ読み出し (2048バイト/セクタ, LBA指定)
 *   lba:   読み出し開始LBA
 *   count: 読み出しセクタ数
 *   buf:   データバッファ (count * 2048 バイト)
 * 戻り値: ATAPI_OK=成功 */
int atapi_read_sectors(u32 lba, u32 count, void *buf);

#endif /* ATAPI_H */
