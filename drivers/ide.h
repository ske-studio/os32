/* ======================================================================== */
/*  IDE.H — PC-98 IDE/ATA PIOドライバ (CHS専用)                              */
/*                                                                          */
/*  PC-98のIDE I/Oポートマッピング:                                          */
/*    0x640: Data (16-bit)        0x642: Features/Error                      */
/*    0x644: Sector Count         0x646: Sector Number / LBA[7:0]            */
/*    0x648: Cylinder Low / LBA[15:8]  0x64A: Cylinder High / LBA[23:16]    */
/*    0x64C: Drive/Head           0x64E: Command/Status                     */
/*    0x74C: Device Control / Alt Status                                     */
/*                                                                          */
/*  バンク切替: 0x430/0x432 でプライマリ/セカンダリ選択                      */
/*                                                                          */
/*  PC-98ではLBAアドレッシングは使用しない (UNDOCUMENTED io_ide.md)。         */
/*  本ドライバはCHSモード専用。APIはLBA値で受け取り内部でCHS変換を行う。     */
/*                                                                          */
/*  出典: NP21/W (ideio.c), DOSBox-X (ide.cpp), PC9800Bible §2-9,           */
/*        UNDOCUMENTED 9801/9821 Vol.2 io_ide.md                             */
/* ======================================================================== */

#ifndef IDE_H
#define IDE_H

#include "types.h"

/* IDE I/Oポート (PC-98) */
#define IDE_DATA       0x640   /* データレジスタ (16-bit) */
#define IDE_ERROR      0x642   /* エラーレジスタ (R) */
#define IDE_FEATURES   0x642   /* フィーチャーレジスタ (W) */
#define IDE_SECT_CNT   0x644   /* セクタカウント */
#define IDE_SECT_NUM   0x646   /* セクタ番号 / LBA[7:0] */
#define IDE_CYL_LO     0x648   /* シリンダ下位 / LBA[15:8] */
#define IDE_CYL_HI     0x64A   /* シリンダ上位 / LBA[23:16] */
#define IDE_DRV_HEAD   0x64C   /* ドライブ/ヘッド */
#define IDE_STATUS     0x64E   /* ステータス (R) */
#define IDE_COMMAND    0x64E   /* コマンド (W) */
#define IDE_ALT_STATUS 0x74C   /* 代替ステータス (R) */
#define IDE_DEV_CTRL   0x74C   /* デバイスコントロール (W) */

/* バンクレジスタ (プライマリ/セカンダリ切替) */
#define IDE_BANK0      0x430
#define IDE_BANK1      0x432

/* ステータスビット */
#define IDE_ST_BSY     0x80   /* ビジー */
#define IDE_ST_DRDY    0x40   /* ドライブレディ */
#define IDE_ST_DF      0x20   /* デバイスフォルト */
#define IDE_ST_DSC     0x10   /* シーク完了 */
#define IDE_ST_DRQ     0x08   /* データ要求 */
#define IDE_ST_CORR    0x04   /* 訂正済みデータ */
#define IDE_ST_IDX     0x02   /* インデックス */
#define IDE_ST_ERR     0x01   /* エラー */

/* ATAコマンド */
#define IDE_CMD_READ       0x20   /* READ SECTOR(S) */
#define IDE_CMD_WRITE      0x30   /* WRITE SECTOR(S) */
#define IDE_CMD_IDENTIFY   0xEC   /* IDENTIFY DEVICE */

/* ドライブ選択 — PC-98ではCHS専用 (UNDOCUMENTED: bit6は常に0) */
#define IDE_DRV_SEL_CHS    0xA0   /* CHSモード: bit5=1, bit6=0, bit7=1 */

#define IDE_TIMEOUT_LOOP   1000000 /* BSY/DRQ待ちループカウント (長い) */
#define IDE_TIMEOUT_BSY    500000  /* 一般的なBSY等のループカウント */
#define IDE_NIEN           0x02   /* nIEN: 割り込み無効 */

/* ====================================================================== */
/*  IDE ハードウェアタイミング仕様                                          */
/* ====================================================================== */
#define IDE_WAIT_NS             400    /* ドライブ選択等の必要ウェイト時間 (ns) */
#define IDE_BUS_CYCLE_NS        100    /* I/Oバスサイクル時間概算 (ns) */
#define IDE_SEL_SETTLE          (IDE_WAIT_NS / IDE_BUS_CYCLE_NS) /* 4回 */

/* エラー定義 */
#define IDE_OK              0
#define IDE_ERR_TIMEOUT    -1
#define IDE_ERR_NO_DRIVE   -2
#define IDE_ERR_IO         -3

/* IDENTIFY DEVICE情報 */
typedef struct {
    u32 total_sectors;   /* LBA総セクタ数 */
    u16 cylinders;       /* シリンダ数 */
    u16 heads;           /* ヘッド数 */
    u16 sectors;         /* セクタ数/トラック */
    u32 size_mb;         /* サイズ(MB) */
    char model[41];      /* モデル名 */
    char serial[21];     /* シリアル番号 */
    char firmware[9];    /* ファームウェアリビジョン */
    int  lba_supported;  /* LBAサポート有無 */
} IdeInfo;

/* IDE初期化 (ドライブ検出) */
int ide_init(void);

/* ドライブ情報取得 */
int ide_identify(int drive, IdeInfo *info);

/* セクタ読み込み (LBA指定→内部CHS変換) — 512バイト/セクタ */
int ide_read_sector(int drive, u32 lba, void *buf);

/* 複数セクタ読み込み */
int ide_read_sectors(int drive, u32 lba, u32 count, void *buf);

/* セクタ書き込み (LBA指定→内部CHS変換) */
int ide_write_sector(int drive, u32 lba, const void *buf);

/* 複数セクタ書き込み */
int ide_write_sectors(int drive, u32 lba, u32 count, const void *buf);

/* ドライブ存在チェック */
int ide_drive_present(int drive);

/* キャッシュされたドライブ情報の取得 */
int ide_get_info(int drive, IdeInfo *info);

#endif /* IDE_H */
