/* ======================================================================== */
/*  FDC.H — PC-98 フロッピーディスクコントローラ直接制御ドライバ            */
/*                                                                          */
/*  µPD765A (1MB FDC) + µPD8237A (DMA ch2) を                               */
/*  32ビットプロテクトモードから I/Oポート経由で直接制御する。               */
/*  BIOS呼び出し (INT 1Bh) やリアルモード遷移は一切不要。                   */
/*                                                                          */
/*  出典: PC9800Bible §2-9, §1-5, §4-3 / OSDev Wiki FDC                    */
/* ======================================================================== */

#ifndef FDC_H
#define FDC_H

#include "types.h"   /* u8, u16, u32 型定義 */

/* ======================================================================== */
/*  µPD765A FDC I/Oポート (PC-98 1MB FDD)                                  */
/* ======================================================================== */
#define FDC_MSR     0x90    /* R:  メインステータスレジスタ */
#define FDC_FIFO    0x92    /* RW: データレジスタ (コマンド/リザルト/データ) */
#define FDC_CTRL    0x94    /* W:  コントロールレジスタ */
                            /* R:  リードスイッチ (FDDタイプ) */

/* --- MSRビットフラグ --- */
#define MSR_RQM     0x80    /* Request for Master: FDC準備完了 */
#define MSR_DIO     0x40    /* Data I/O direction: 1=FDC→CPU, 0=CPU→FDC */
#define MSR_NDMA    0x20    /* Non-DMA実行フェーズ中 */
#define MSR_BUSY    0x10    /* FDCビジー (コマンド実行中) */
#define MSR_ACTD    0x08    /* ドライブD アクティブ */
#define MSR_ACTC    0x04    /* ドライブC アクティブ */
#define MSR_ACTB    0x02    /* ドライブB アクティブ */
#define MSR_ACTA    0x01    /* ドライブA アクティブ */

/* --- コントロールレジスタ (0x94) ビット --- */
/* PC-98固有: FDDコントローラのリセットやモーター制御 */
#define CTRL_MTON   0x08    /* モーターON */
#define CTRL_DMAE   0x10    /* DMA有効 */
#define CTRL_RST    0x80    /* FDCリセット */

/* ======================================================================== */
/*  µPD765A コマンドコード                                                  */
/* ======================================================================== */
#define FDC_CMD_SPECIFY         0x03
#define FDC_CMD_SENSE_DRIVE     0x04
#define FDC_CMD_WRITE_DATA      0x05    /* + MF + MT */
#define FDC_CMD_READ_DATA       0x06    /* + MF + MT */
#define FDC_CMD_RECALIBRATE     0x07
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_READ_ID         0x0A    /* + MF */
#define FDC_CMD_FORMAT_TRACK    0x0D    /* + MF */
#define FDC_CMD_SEEK            0x0F

/* コマンドオプションビット */
#define FDC_OPT_MT    0x80    /* マルチトラック */
#define FDC_OPT_MF    0x40    /* MFM (倍密度) — 常にセット */
#define FDC_OPT_SK    0x20    /* Skip deleted sectors */

/* ======================================================================== */
/*  DMA (µPD8237A) チャネル2 I/Oポート (PC-98)                             */
/* ======================================================================== */
#define DMA_CH2_ADDR   0x09    /* チャネル2 アドレス (Low/High) */
#define DMA_CH2_COUNT  0x0B    /* チャネル2 ワードカウント (Low/High) */
#define DMA_CH2_BANK   0x23    /* チャネル2 バンクレジスタ */
#define DMA_MASK_REG   0x15    /* シングルマスクレジスタ */
#define DMA_MODE_REG   0x17    /* モードレジスタ */
#define DMA_FLIPFLOP   0x19    /* バイトポインタ・フリップフロップ・クリア */

/* DMAモード値 */
#define DMA_MODE_READ  0x46    /* ch2, single, addr++, read (FDC→メモリ) */
#define DMA_MODE_WRITE 0x4A    /* ch2, single, addr++, write (メモリ→FDC) */
#define DMA_MASK_CH2   0x06    /* ch2をマスク (無効化) */
#define DMA_UNMASK_CH2 0x02    /* ch2をアンマスク (有効化) */

/* ======================================================================== */
/*  割り込み                                                                */
/* ======================================================================== */
/* PC-98: 2HD FDD = スレーブ IR11 → PIC_SLAVE_OFFSET + 3 = 0x2B */
#define FDC_IRQ     11    /* スレーブPIC IR11 */

/* ======================================================================== */
/*  ディスクパラメータ (PC-98 2HD 1MB MFM)                                 */
/* ======================================================================== */
#define FDC_CYLINDERS    77     /* シリンダ数 (0-76) */
#define FDC_HEADS        2      /* ヘッド数 */
#define FDC_SPT          8      /* セクタ/トラック */
#define FDC_SECTOR_SIZE  1024   /* バイト/セクタ (N=3) */
#define FDC_SECTOR_N     3      /* セクタ長コード (3=1024) */
#define FDC_GAP3         0x74   /* Read/Write ギャップ長 (MFM 1024byte/sec) */
#define FDC_TOTAL_SECTORS (FDC_CYLINDERS * FDC_HEADS * FDC_SPT)  /* 1232 */
#define FDC_DATARATE     0      /* 500Kbps (CCR値) */
#define FDC_TIMEOUT_LOOP 10000  /* BSY等待ちのためのループカウンタ上限 */

/* ======================================================================== */
/*  FDCドライバAPI                                                          */
/* ======================================================================== */

/* FDC初期化: リセット → Specify → Recalibrate */
int fdc_init(void);

/* セクタ読み込み
 *   drv:   ドライブ (0-3)
 *   cyl:   シリンダ (0-76)
 *   head:  ヘッド (0-1)
 *   sect:  セクタ (1-8)
 *   buf:   データバッファ (>= 1024バイト)
 * 戻り値: 0=成功
 */
int fdc_read_sector(int drv, int cyl, int head, int sect, void *buf);

/* セクタ書き込み */
int fdc_write_sector(int drv, int cyl, int head, int sect, const void *buf);

/* IRQ11完了フラグ (isr_handlers.cからセット) */
extern volatile u32 fdc_irq_fired;

#endif /* FDC_H */
