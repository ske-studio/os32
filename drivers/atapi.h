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

/* ======== 1 回の READ(10) で読むセクタ数の上限 ========
 * atapi_read_sectors() は連続する count セクタを、この数ずつの READ(10) に
 * 分けて出す (1 セクタずつ出していた頃は、実機の cdinst が 20KB/s を切った)。
 * **実機の CD ドライブとの相性で困ったら、ここを 1 にすれば旧来の読み方に戻る**。
 * 16 = 32KB。PIO の byte count limit (下) はこれより大きくてよい。
 * (ホスト試験は -D で 32 にした版も回す = 1 回の転送が 64KB を超える形) */
#ifndef ATAPI_READ_MAX_SECTORS
#define ATAPI_READ_MAX_SECTORS  16
#endif

/* PACKET の byte count limit (Cylinder Low/High) に書く上限。1 回の DRQ で
 * デバイスが渡してよいバイト数。ATA の規定で偶数、0xFFFF は使わない。
 * セクタの倍数 (31 × 2048) にしておくと、DRQ の区切りがセクタの途中に来ない */
#define ATAPI_PIO_BCL_MAX       0xF800U

/* 読みの失敗の診断の行 ([atapi] READ(10) ...) を出す上限 (起動から数えて) */
#define ATAPI_DIAG_MAX          8

/* ======== センスキー (エラーレジスタの bit7-4) ======== */
#define ATAPI_ERR_SENSE_SHIFT   4
#define ATAPI_SK_NOT_READY      0x02
#define ATAPI_SK_UNIT_ATTENTION 0x06   /* 媒体の交換・リセットの後の最初のコマンド */

/* ======== ATAPI / PACKET コマンド ======== */
#define ATAPI_CMD_PACKET         0xA0   /* PACKETコマンド (CDB送出) */
#define ATAPI_CMD_IDENTIFY_PKT   0xA1   /* IDENTIFY PACKET DEVICE */

/* ======== 装置の選択 (DRV_HEAD) ======== */
#define ATAPI_DRV_SLAVE     0x10   /* bit4 = 1: スレーブ (セカンダリの 2 台目) */

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

/* 使っている装置: 0 = セカンダリのマスター、1 = スレーブ。atapi_init は
 * 両方のシグネチャを見て、2 台あれば媒体の入っている方 (マスター優先) を選ぶ */
int atapi_drive_index(void);

/* TEST UNIT READY: メディア挿入確認
 * 戻り値: ATAPI_OK=メディアあり, ATAPI_ERR_NO_MEDIA=なし */
int atapi_test_unit_ready(void);

/* READ CAPACITY: メディア容量取得 */
int atapi_read_capacity(AtapiCapacity *cap);

/* セクタ読み出し (2048バイト/セクタ, LBA指定)
 *   lba:   読み出し開始LBA
 *   count: 読み出しセクタ数
 *   buf:   データバッファ (count * 2048 バイト)
 * 戻り値: ATAPI_OK=成功
 *
 * 連続する count セクタを ATAPI_READ_MAX_SECTORS ずつの READ(10) で読む。
 * 複数セクタの READ(10) が失敗したら、その範囲だけ 1 セクタずつ読み直す。
 * UNIT ATTENTION (媒体の交換の後の最初のコマンド) を受けたら媒体の世代を
 * 進めて同じコマンドを 1 回だけ出し直す。 */
int atapi_read_sectors(u32 lba, u32 count, void *buf);

/* 媒体の世代。UNIT ATTENTION / NOT READY を見るたびに進む。上の層 (iso9660)
 * は覚えた値と違えばキャッシュを捨てる。**NP21/W は READ(10) で UNIT
 * ATTENTION を返さない** (交換は TEST UNIT READY だけが報告する) ので、
 * エミュレータではこの経路は動かない */
u32 atapi_media_gen(void);

/* 読みの統計 (試験と起動時の行のため) */
typedef struct {
    u32 read10_cmds;      /* 出した READ(10) の数 (出し直しを含む) */
    u32 read10_sectors;   /* 成功した READ(10) で受け取ったセクタ数 */
    u32 multi_fail;       /* 複数セクタの READ(10) が失敗して 1 セクタずつへ落ちた数 */
    u32 single_retry;     /* 落ちた後に 1 セクタずつ出した READ(10) の数 */
    u32 unit_attention;   /* UNIT ATTENTION / NOT READY を見た数 */
} AtapiStats;

void atapi_get_stats(AtapiStats *out);

#endif /* ATAPI_H */
