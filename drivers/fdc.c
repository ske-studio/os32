/* ======================================================================== */
/*  FDC.C — PC-98 フロッピーディスクコントローラ直接制御ドライバ            */
/*                                                                          */
/*  µPD765A FDC を I/Oポート経由で直接制御する。                            */
/*  データ転送は DMA (µPD8237A ch2) を使用する。                            */
/*  BIOS (INT 1Bh) やリアルモード遷移は一切不要。                          */
/*                                                                          */
/*  出典: PC9800Bible §2-9, §1-5, §4-3 / OSDev Wiki FDC                    */
/* ======================================================================== */

#include "fdc.h"
#include "io.h"

/* ST0ビットマスク */
#define ST0_SEEK_END    0x20    /* Seek End ビット */
#define ST0_IC_MASK     0xC0    /* Interrupt Code マスク */
#define FDC_DTL_UNUSED  0xFF    /* DTL未使用時の値 */

/* 外部: tick_count (idt.c) */
extern volatile u32 tick_count;

/* IRQ11完了フラグ */
volatile u32 fdc_irq_fired = 0;

/* DMAバッファ (1MB未満のBSS領域に配置される) */
/* DMAバンク(64KB)をまたがないように1セクタ分 */
static u8 dma_buffer[FDC_SECTOR_SIZE];

/* ======================================================================== */
/*  内部ユーティリティ                                                      */
/* ======================================================================== */

/* 簡易メモリコピー */
static void fdc_memcpy(u8 *dst, const u8 *src, int n)
{
    int i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

/* 短い遅延 (I/Oポート読み出しで数µs) */
static void fdc_delay(void)
{
    io_wait();
    io_wait();
}

/* ======================================================================== */
/*  FDCコマンド送受信                                                       */
/* ======================================================================== */

/* MSRのRQM=1かつDIO=0 (CPU→FDC方向)を待ってからコマンドバイトを送信 */
static int fdc_send_byte(u8 val)
{
    int timeout;
    for (timeout = 0; timeout < FDC_TIMEOUT_LOOP; timeout++) {
        u8 msr = (u8)inp(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            outp(FDC_FIFO, val);
            return 0; /* 成功 */
        }
        fdc_delay();
    }
    return -1; /* タイムアウト */
}

/* MSRのRQM=1かつDIO=1 (FDC→CPU方向)を待ってからリザルトバイトを読み出し */
static int fdc_read_byte(void)
{
    int timeout;
    for (timeout = 0; timeout < FDC_TIMEOUT_LOOP; timeout++) {
        u8 msr = (u8)inp(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
            return (int)inp(FDC_FIFO);
        }
        fdc_delay();
    }
    return -1; /* タイムアウト */
}

/* リザルトフェーズ: 最大10バイトのリザルトを読み出し */
static int fdc_read_results(u8 *st, int max)
{
    int i;
    for (i = 0; i < max; i++) {
        u8 msr = (u8)inp(FDC_MSR);
        /* CMD BSY=0 なら終了 */
        if ((msr & MSR_BUSY) == 0) break;
        /* RQM=1, DIO=1 なら読み出し */
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
            st[i] = (u8)inp(FDC_FIFO);
        } else {
            fdc_delay();
            i--; /* リトライ */
        }
    }
    return i;
}

/* ======================================================================== */
/*  IRQ11 待ち                                                              */
/* ======================================================================== */
static int fdc_wait_irq(u32 timeout_ticks)
{
    u32 start = tick_count;
    while (!fdc_irq_fired) {
        if ((tick_count - start) > timeout_ticks) {
            return -1; /* タイムアウト */
        }
    }
    fdc_irq_fired = 0;
    return 0;
}

/* ======================================================================== */
/*  Sense Interrupt コマンド                                                */
/* ======================================================================== */
static int fdc_sense_interrupt(u8 *st0, u8 *cyl)
{
    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != 0) return -1;
    {
        int r0 = fdc_read_byte();
        int r1 = fdc_read_byte();
        if (r0 < 0 || r1 < 0) return -1;
        *st0 = (u8)r0;
        *cyl = (u8)r1;
    }
    return 0;
}

/* ======================================================================== */
/*  DMAセットアップ (µPD8237A チャネル2)                                    */
/* ======================================================================== */
static void dma_setup(u32 phys_addr, u16 byte_count, int is_write)
{
    u8 bank = (u8)((phys_addr >> 16) & 0xFF);
    u16 addr = (u16)(phys_addr & 0xFFFF);
    u16 count = byte_count - 1;  /* ワードカウント = バイト数 - 1 */

    /* チャネル2をマスク (転送停止) */
    outp(DMA_MASK_REG, DMA_MASK_CH2);

    /* バイトポインタ・フリップフロップをクリア */
    outp(DMA_FLIPFLOP, 0);

    /* モード設定 */
    if (is_write) {
        outp(DMA_MODE_REG, DMA_MODE_WRITE);  /* メモリ→FDC */
    } else {
        outp(DMA_MODE_REG, DMA_MODE_READ);   /* FDC→メモリ */
    }

    /* アドレス設定 (Low→High) */
    outp(DMA_CH2_ADDR, addr & 0xFF);
    outp(DMA_CH2_ADDR, (addr >> 8) & 0xFF);

    /* バンク設定 */
    outp(DMA_CH2_BANK, bank);

    /* ワードカウント設定 (Low→High) */
    outp(DMA_CH2_COUNT, count & 0xFF);
    outp(DMA_CH2_COUNT, (count >> 8) & 0xFF);

    /* チャネル2をアンマスク (転送許可) */
    outp(DMA_MASK_REG, DMA_UNMASK_CH2);
}

/* ======================================================================== */
/*  モーター制御                                                            */
/* ======================================================================== */
static void fdc_motor_on(void)
{
    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);
    /* モータースピンアップ待ち: 約300ms (100Hzタイマで30tick) */
    {
        u32 start = tick_count;
        while ((tick_count - start) < 30) { /* 何もしない */ }
    }
}

static void fdc_motor_off(void)
{
    outp(FDC_CTRL, 0);
}

/* ======================================================================== */
/*  FDCリセット                                                             */
/* ======================================================================== */
static int fdc_reset(void)
{
    u8 st0, cyl;
    int i;

    /* FDCをリセット */
    outp(FDC_CTRL, CTRL_RST);
    fdc_delay();
    fdc_delay();
    fdc_delay();
    fdc_delay();

    /* リセット解除 + DMA有効 + モーターON */
    fdc_irq_fired = 0;
    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);

    /* リセット完了IRQ待ち */
    if (fdc_wait_irq(300) != 0) {
        /* タイムアウト: エミュレータによってはIRQが来ない場合あり */
        /* Sense Interruptで続行を試みる */
    }

    /* Sense Interrupt × 4回 (リセット後は4ドライブ分必要) */
    for (i = 0; i < 4; i++) {
        if (fdc_sense_interrupt(&st0, &cyl) != 0) break;
    }

    /* Specifyコマンド: SRT=8ms, HLT=10ms, HUT=max, DMA有効 */
    /* SRT_value = 16 - (8 * 500000 / 500000) = 8 */
    /* HLT_value = 10 * 500000 / 1000000 = 5 */
    /* HUT_value = 0 (最大) */
    /* NDMA = 0 (DMAモード) */
    if (fdc_send_byte(FDC_CMD_SPECIFY) != 0) return -1;
    if (fdc_send_byte(0x80) != 0) return -1;  /* SRT=8, HUT=0 */
    if (fdc_send_byte(0x0A) != 0) return -1;  /* HLT=5, NDMA=0 */

    return 0;
}

/* ======================================================================== */
/*  Recalibrate (ヘッドをシリンダ0に移動)                                   */
/* ======================================================================== */
static int fdc_recalibrate(int drv)
{
    u8 st0, cyl;

    fdc_irq_fired = 0;
    if (fdc_send_byte(FDC_CMD_RECALIBRATE) != 0) return -1;
    if (fdc_send_byte((u8)drv) != 0) return -1;

    /* 完了IRQ待ち (最大3秒) */
    if (fdc_wait_irq(300) != 0) return -2;

    /* Sense Interrupt */
    if (fdc_sense_interrupt(&st0, &cyl) != 0) return -3;

    /* st0のbit5 (Seek End)がセットされているか確認 */
    if ((st0 & ST0_SEEK_END) == 0) {
        /* 失敗: リトライ */
        return -4;
    }

    return 0;
}

/* ======================================================================== */
/*  Seek (指定シリンダに移動)                                               */
/* ======================================================================== */
static int fdc_seek(int drv, int cyl, int head)
{
    u8 st0, result_cyl;

    fdc_irq_fired = 0;
    if (fdc_send_byte(FDC_CMD_SEEK) != 0) return -1;
    if (fdc_send_byte((u8)((head << 2) | drv)) != 0) return -1;
    if (fdc_send_byte((u8)cyl) != 0) return -1;

    /* 完了IRQ待ち */
    if (fdc_wait_irq(300) != 0) return -2;

    /* Sense Interrupt */
    if (fdc_sense_interrupt(&st0, &result_cyl) != 0) return -3;

    /* 正しいシリンダに到達したか */
    if (result_cyl != (u8)cyl) return -4;

    /* ヘッド安定待ち: 約15ms */
    {
        u32 start = tick_count;
        while ((tick_count - start) < 2) { /* 何もしない */ }
    }

    return 0;
}

/* ======================================================================== */
/*  セクタ読み込み                                                          */
/* ======================================================================== */
int fdc_read_sector(int drv, int cyl, int head, int sect, void *buf)
{
    u8 results[7];
    int n, retry;
    u32 phys = (u32)dma_buffer;

    for (retry = 0; retry < 3; retry++) {
        /* 1. シーク */
        if (fdc_seek(drv, cyl, head) != 0) continue;

        /* 2. DMAセットアップ (FDC→メモリ = read) */
        dma_setup(phys, FDC_SECTOR_SIZE, 0);

        /* 3. Read Data コマンド送信 */
        fdc_irq_fired = 0;
        if (fdc_send_byte(FDC_OPT_MF | FDC_CMD_READ_DATA) != 0) continue;
        if (fdc_send_byte((u8)((head << 2) | drv)) != 0) continue;
        if (fdc_send_byte((u8)cyl) != 0) continue;      /* C */
        if (fdc_send_byte((u8)head) != 0) continue;     /* H */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* R (1始まり) */
        if (fdc_send_byte(FDC_SECTOR_N) != 0) continue; /* N = 3 (1024) */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* EOT (最終セクタ) */
        if (fdc_send_byte(FDC_GAP3) != 0) continue;     /* GPL */
        if (fdc_send_byte(FDC_DTL_UNUSED) != 0) continue;     /* DTL (未使用) */

        /* 4. IRQ待ち (データ転送完了) */
        if (fdc_wait_irq(300) != 0) continue;

        /* 5. リザルト読み出し (7バイト) */
        n = fdc_read_results(results, 7);
        if (n < 7) continue;

        /* 6. エラーチェック: ST0のbit6-7が00なら成功 */
        if ((results[0] & ST0_IC_MASK) == 0) {
            /* DMAバッファからユーザーバッファにコピー */
            fdc_memcpy((u8 *)buf, dma_buffer, FDC_SECTOR_SIZE);
            return 0;
        }
    }

    return -1; /* 3回リトライ失敗 */
}

/* ======================================================================== */
/*  セクタ書き込み                                                          */
/* ======================================================================== */
int fdc_write_sector(int drv, int cyl, int head, int sect, const void *buf)
{
    u8 results[7];
    int n, retry;
    u32 phys = (u32)dma_buffer;

    /* ユーザーバッファからDMAバッファにコピー */
    fdc_memcpy(dma_buffer, (const u8 *)buf, FDC_SECTOR_SIZE);

    for (retry = 0; retry < 3; retry++) {
        /* 1. シーク */
        if (fdc_seek(drv, cyl, head) != 0) continue;

        /* 2. DMAセットアップ (メモリ→FDC = write) */
        dma_setup(phys, FDC_SECTOR_SIZE, 1);

        /* 3. Write Data コマンド送信 */
        fdc_irq_fired = 0;
        if (fdc_send_byte(FDC_OPT_MF | FDC_CMD_WRITE_DATA) != 0) continue;
        if (fdc_send_byte((u8)((head << 2) | drv)) != 0) continue;
        if (fdc_send_byte((u8)cyl) != 0) continue;      /* C */
        if (fdc_send_byte((u8)head) != 0) continue;     /* H */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* R */
        if (fdc_send_byte(FDC_SECTOR_N) != 0) continue; /* N = 3 */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* EOT */
        if (fdc_send_byte(FDC_GAP3) != 0) continue;     /* GPL */
        if (fdc_send_byte(FDC_DTL_UNUSED) != 0) continue;     /* DTL */

        /* 4. IRQ待ち */
        if (fdc_wait_irq(300) != 0) continue;

        /* 5. リザルト読み出し */
        n = fdc_read_results(results, 7);
        if (n < 7) continue;

        /* 6. エラーチェック */
        if ((results[0] & ST0_IC_MASK) == 0) {
            return 0;
        }
    }

    return -1;
}

/* ======================================================================== */
/*  fdc_init — FDC初期化                                                    */
/* ======================================================================== */
int fdc_init(void)
{
    int ret;

    /* モーターON */
    fdc_motor_on();

    /* FDCリセット + Specify */
    ret = fdc_reset();
    if (ret != 0) return ret;

    /* Recalibrate (ヘッドをシリンダ0に移動) */
    ret = fdc_recalibrate(0);
    if (ret != 0) {
        /* 1回目失敗の場合リトライ */
        ret = fdc_recalibrate(0);
    }
    /* fd1は接続されていないとタイムアウトするので一応試みる程度 */
    fdc_recalibrate(1);

    return ret;
}
