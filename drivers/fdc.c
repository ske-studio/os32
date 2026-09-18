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
#include "kstring.h"

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
    /* スタティックフラグで前回の状態を記憶し、初回のみスピンアップ待ちを行う。
     * NP21/W の 0x94 リードはリードスイッチを返すため MTON 状態は判別不能。 */
    static int motor_running = 0;

    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);
    if (!motor_running) {
        /* 初回のみ 約300ms スピンアップ待ち (100Hzタイマで30tick) */
        u32 start = tick_count;
        while ((tick_count - start) < 30) { /* 何もしない */ }
        motor_running = 1;
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
    if (fdc_wait_irq(FDC_IRQ_TIMEOUT_TICKS) != 0) {
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
    if (fdc_wait_irq(FDC_IRQ_TIMEOUT_TICKS) != 0) return -2;

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
    if (fdc_wait_irq(FDC_IRQ_TIMEOUT_TICKS) != 0) return -2;

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
/*  セクタ読み込み (ジオメトリ指定版)                                        */
/* ======================================================================== */
int fdc_read_sector_geom(int drv, int cyl, int head, int sect,
                         const struct fdc_geom *g, void *buf)
{
    u8 results[7];
    int n, retry;
    u32 phys = (u32)dma_buffer;
    u16 bps = g->bps;

    for (retry = 0; retry < 3; retry++) {
        /* 1. シーク */
        if (fdc_seek(drv, cyl, head) != 0) continue;

        /* 2. DMAセットアップ (FDC→メモリ = read) */
        dma_setup(phys, bps, 0);

        /* 3. Read Data コマンド送信 */
        fdc_irq_fired = 0;
        if (fdc_send_byte(FDC_OPT_MF | FDC_CMD_READ_DATA) != 0) continue;
        if (fdc_send_byte((u8)((head << 2) | drv)) != 0) continue;
        if (fdc_send_byte((u8)cyl) != 0) continue;      /* C */
        if (fdc_send_byte((u8)head) != 0) continue;     /* H */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* R (1始まり) */
        if (fdc_send_byte(g->sec_n) != 0) continue;     /* N */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* EOT (最終セクタ) */
        if (fdc_send_byte(g->gap3) != 0) continue;      /* GPL */
        if (fdc_send_byte(FDC_DTL_UNUSED) != 0) continue;     /* DTL */

        /* 4. IRQ待ち (データ転送完了) */
        if (fdc_wait_irq(FDC_IRQ_TIMEOUT_TICKS) != 0) continue;

        /* 5. リザルト読み出し (7バイト) */
        n = fdc_read_results(results, 7);
        if (n < 7) continue;

        /* 6. エラーチェック: ST0のbit6-7が00なら成功 */
        if ((results[0] & ST0_IC_MASK) == 0) {
            /* DMAバッファからユーザーバッファにコピー */
            kmemcpy((u8 *)buf, dma_buffer, (u32)bps);
            return 0;
        }
    }

    return -1; /* 3回リトライ失敗 */
}

/* ======================================================================== */
/*  セクタ書き込み (ジオメトリ指定版)                                        */
/* ======================================================================== */
int fdc_write_sector_geom(int drv, int cyl, int head, int sect,
                          const struct fdc_geom *g, const void *buf)
{
    u8 results[7];
    int n, retry;
    u32 phys = (u32)dma_buffer;
    u16 bps = g->bps;

    /* ユーザーバッファからDMAバッファにコピー */
    kmemcpy(dma_buffer, (const u8 *)buf, (u32)bps);

    for (retry = 0; retry < 3; retry++) {
        /* 1. シーク */
        if (fdc_seek(drv, cyl, head) != 0) continue;

        /* 2. DMAセットアップ (メモリ→FDC = write) */
        dma_setup(phys, bps, 1);

        /* 3. Write Data コマンド送信 */
        fdc_irq_fired = 0;
        if (fdc_send_byte(FDC_OPT_MF | FDC_CMD_WRITE_DATA) != 0) continue;
        if (fdc_send_byte((u8)((head << 2) | drv)) != 0) continue;
        if (fdc_send_byte((u8)cyl) != 0) continue;      /* C */
        if (fdc_send_byte((u8)head) != 0) continue;     /* H */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* R */
        if (fdc_send_byte(g->sec_n) != 0) continue;     /* N */
        if (fdc_send_byte((u8)sect) != 0) continue;     /* EOT */
        if (fdc_send_byte(g->gap3) != 0) continue;      /* GPL */
        if (fdc_send_byte(FDC_DTL_UNUSED) != 0) continue;     /* DTL */

        /* 4. IRQ待ち */
        if (fdc_wait_irq(FDC_IRQ_TIMEOUT_TICKS) != 0) continue;

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
/*  セクタ読み込み (ドライブの現在ジオメトリを使う — 既存API互換)            */
/* ======================================================================== */
int fdc_read_sector(int drv, int cyl, int head, int sect, void *buf)
{
    return fdc_read_sector_geom(drv, cyl, head, sect, fdc_get_geom(drv), buf);
}

/* ======================================================================== */
/*  セクタ書き込み (ドライブの現在ジオメトリを使う — 既存API互換)            */
/* ======================================================================== */
int fdc_write_sector(int drv, int cyl, int head, int sect, const void *buf)
{
    return fdc_write_sector_geom(drv, cyl, head, sect, fdc_get_geom(drv), buf);
}

/* ======================================================================== */
/*  fdc_init — FDC初期化                                                    */
/* ======================================================================== */
int fdc_init(void)
{
    int ret;

    /* 前回の取りこぼし IRQ をクリア (冪等化対策) */
    fdc_irq_fired = 0;

    /* モーターON (既にONの場合はスピンアップ待ちをスキップ) */
    fdc_motor_on();

    /* FDCリセット + Specify */
    ret = fdc_reset();
    if (ret != 0) return ret;

    /* Recalibrate (ヘッドをシリンダ0に移動)
     * 1回目失敗: リセット直後の安定化待ち(100ms)後にリトライ */
    ret = fdc_recalibrate(0);
    if (ret != 0) {
        u32 start = tick_count;
        while ((tick_count - start) < 10) { /* 100ms ウェイト */ }
        ret = fdc_recalibrate(0);
    }

    /* ドライブ1は未接続時にタイムアウトするためエラーは無視する */
    fdc_recalibrate(1);

    return ret;
}

/* ======================================================================== */
/*  既知メディアジオメトリ定義 (fdc.h で extern 宣言済み)                    */
/* ======================================================================== */
const struct fdc_geom fdc_geom_2hd = {
    77, 2, 8, 3, 1024, 0x74, 0x90
};
const struct fdc_geom fdc_geom_2dd_640 = {
    80, 2, 8, 2, 512, 0x2A, 0x10  /* GAP3=0x2A: MFM 512B/sec 標準値 */
};
const struct fdc_geom fdc_geom_2dd_720 = {
    80, 2, 9, 2, 512, 0x2A, 0x10
};
const struct fdc_geom fdc_geom_2d_256 = {
    77, 2, 16, 1, 256, 0x0E, 0x90  /* GAP3=0x0E: MFM 256B/sec, DAUA=0x90 */
};
/* 1.44MB (PC/AT 標準の 2HD)。PC-98 では DA/UA 0x30 系の
 * 「1.44MB 対応両用インタフェース」でアクセスする (PC9800Bible 表 2-34)。
 * GAP3=0x1B は NP21/W の src/bios/fdfmt.h fdfmt144[] の MFM R/W GPL 値。 */
const struct fdc_geom fdc_geom_144 = {
    80, 2, 18, 2, 512, 0x1B, 0x30
};

/* ======================================================================== */
/*  ドライブごとの現在ジオメトリ                                            */
/*                                                                          */
/*  既定は 2HD 1232KB — 既存構成 (1232KB の FD / NHD 起動) を変えない。      */
/* ======================================================================== */
static const struct fdc_geom *s_geom[2] = {
    &fdc_geom_2hd, &fdc_geom_2hd
};

/* このドライバが 04BEh で 1.44MB モードへ切り替えたか (ドライブごと)。
 * **触っていないポートを戻そうとしない**ため。2HD だけの機種で 04BEh を
 * 書くと、00BEh のデコードイメージが出る機種では 1MB/640KB I/F の切替に
 * 化ける恐れがある (io_fdd.md の注意)。 */
static int s_3mode_on[2] = { 0, 0 };

/* ======================================================================== */
/*  3モードFD I/F (I/O 04BEh) — 1.44MB アクセスモードの切り替え             */
/* ======================================================================== */
int fdc_set_3mode(int drv, int on)
{
    u8 sel, v;

    if (drv < 0 || drv > 1) {
        return -1;
    }
    sel = (u8)((u8)drv << FDC_3M_DRV_SHIFT);

    /* ドライブ指定 + モード指定を 1 回で書く (bit4=1 で bit0 が有効)。 */
    outp(FDC_IO_3MODE,
         (u8)(sel | FDC_3M_APPLY | (on ? FDC_3M_MODE_144 : 0)));

    /* 読む前にドライブを指定し直す (bit4=0 = 無動作)。資料の指示どおり。 */
    outp(FDC_IO_3MODE, sel);
    v = (u8)inp(FDC_IO_3MODE);

    /* **FFh 判定で搭載を見ない。** 要求した値になったかだけを見る。 */
    if (on) {
        if (!(v & FDC_3M_CUR_144)) {
            return -1;
        }
    } else {
        if (v & FDC_3M_CUR_144) {
            return -1;
        }
    }
    s_3mode_on[drv] = on ? 1 : 0;
    return 0;
}

const struct fdc_geom *fdc_get_geom(int drv)
{
    if (drv < 0 || drv > 1) {
        return &fdc_geom_2hd;
    }
    return s_geom[drv];
}

int fdc_set_media(int drv, fdc_media_t media)
{
    const struct fdc_geom *g;

    if (drv < 0 || drv > 1) {
        return -1;
    }
    switch (media) {
    case FDC_MEDIA_2HD_1232: g = &fdc_geom_2hd;      break;
    case FDC_MEDIA_2DD_640:  g = &fdc_geom_2dd_640;  break;
    case FDC_MEDIA_2DD_720:  g = &fdc_geom_2dd_720;  break;
    case FDC_MEDIA_2D_256:   g = &fdc_geom_2d_256;   break;
    case FDC_MEDIA_2HD_1440: g = &fdc_geom_144;      break;
    default:                 return -1;
    }
    s_geom[drv] = g;

    /* アクセスモードをメディアに合わせる。**1.44MB に入るときと、自分で
     * 入れたものを戻すときだけ** 04BEh に触る (上の s_3mode_on の注記)。 */
    if (media == FDC_MEDIA_2HD_1440) {
        fdc_set_3mode(drv, 1);
    } else if (s_3mode_on[drv]) {
        fdc_set_3mode(drv, 0);
    }
    return 0;
}

int fdc_set_media_by_daua(int drv, u32 daua)
{
    /* DA/UA の上位ニブルが装置種別。0x30 / 0xB0 系が 1.44MB
     * (undocumented/memsys.md の 0000:0584h DISK_BOOT)。
     * 知らない値では 2HD のままにする — 起動経路を勝手に変えない。 */
    switch (daua & 0xF0) {
    case 0x30:
    case 0xB0:
        return fdc_set_media(drv, FDC_MEDIA_2HD_1440);
    default:
        return 0;
    }
}

