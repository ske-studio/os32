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
#include "kprintf.h"

/* ST0 のビット定義と判定は drivers/fdc_decide.h / .c にある
 * (I/O を引かずにホストで試験するため — tools/tests/fdc_seek_tdd.md)。 */
#define FDC_DTL_UNUSED  0xFF    /* DTL未使用時の値 */

/* 外部: tick_count (idt.c) */
extern volatile u32 tick_count;

/* IRQ11完了フラグ */
volatile u32 fdc_irq_fired = 0;

/* 最後に SEEK / RECALIBRATE で見た ST0。診断行でしか使わない。
 * R/W が seek フェーズで失敗したとき、READ/WRITE の results[] は空のままな
 * ので、**こちらを出さないと「なぜシークが失敗したか」が出ない**
 * (NR = 媒体無しなのか、本当に動かなかったのかが読めない)。 */
static u8 s_last_seek_st0 = 0;

/* fdc_seek / fdc_recalibrate_st0 の内部戻り値。
 * NR (媒体もドライブも無い) だけは **リトライも回復もしない** ので
 * 他の失敗と分ける。 */
#define FDC_RC_NOT_READY  (-5)

/* DMA バッファ。
 * 大きさは 1 セクタ分 (最大 1024B) で、**1024B 境界に揃える** ([HW2])。
 * 揃えておけば 64KB 境界をまたぎようがない。以前は揃え指定が無く、
 * またいでいないことが**リンク順のたまたま**に依存していた
 * (2026-09-22 時点の番地は 0x1555e0 で、たまたま無事だった)。 */
static u8 dma_buffer[FDC_SECTOR_SIZE] __attribute__((aligned(FDC_DMA_ALIGN)));

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

/* リザルトフェーズ: 最大10バイトのリザルトを読み出し。
 * **1 バイトごとに空回りの上限を掛ける** — CB=1 のまま RQM/DIO が揃わない
 * FDC に当たると、縛りが無ければ i-- のリトライで永久に回る。
 * 戻り値は実際に読めたバイト数 (諦めた時点の i)。 */
static int fdc_read_results(u8 *st, int max)
{
    int i, spin;

    spin = 0;
    for (i = 0; i < max; i++) {
        u8 msr = (u8)inp(FDC_MSR);
        /* CMD BSY=0 なら終了 */
        if ((msr & MSR_BUSY) == 0) break;
        /* RQM=1, DIO=1 なら読み出し */
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
            st[i] = (u8)inp(FDC_FIFO);
            spin = 0;       /* 上限はバイトごとに数え直す */
        } else {
            fdc_delay();
            if (++spin >= FDC_MSR_SETTLE_LOOP) break;  /* 出てこない */
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
/*  リザルトフェーズがまだ続いているか                                      */
/*                                                                          */
/*  CB が立ったまま FDC→CPU 方向に RQM が立てば、次のリザルトバイトがある。  */
/*  CB が落ちていればコマンドは終わっていて、もうバイトは来ない。           */
/*  直前のバイトを読んだ直後は RQM が落ちているので少しだけ待つ。           */
/* ======================================================================== */
static int fdc_result_pending(void)
{
    int i;

    for (i = 0; i < FDC_MSR_SETTLE_LOOP; i++) {
        u8 msr = (u8)inp(FDC_MSR);
        if ((msr & MSR_BUSY) == 0) return 0;   /* コマンド終了 */
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) return 1;
        fdc_delay();
    }
    return 0;
}

/* ======================================================================== */
/*  Sense Interrupt Status (SIS)                                            */
/*                                                                          */
/*  ST0 を 1 バイト読み、**pending が無いときの invalid 応答 (ST0 = 80h)     */
/*  では PCN を読まずに戻る**。µPD765A の invalid command のリザルトは       */
/*  1 バイトで、2 バイト目を待つと来ないバイトを FDC_TIMEOUT_LOOP 回        */
/*  空転してから諦めることになる。排水ループは pending が尽きるまで回すので */
/*  **毎回 1 度は必ずこの空振りを踏む**。                                   */
/*  NP21/W も同じ 1 バイト応答を返す (src/io/fdc.c の FDC_SenceintStatus は  */
/*  pending 無しで fdc.buf[0] = FDCRLT_IC1 (0x80) / fdc.bufcnt = 1)。        */
/*                                                                          */
/*  戻り値: 0 = ST0 を読めた (pending の有無は *st0 で見る) / -1 = 失敗      */
/* ======================================================================== */
static int fdc_sense_interrupt(u8 *st0, u8 *cyl)
{
    int r0, r1;

    *st0 = 0;
    *cyl = 0;
    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != 0) return -1;

    r0 = fdc_read_byte();
    if (r0 < 0) return -1;
    *st0 = (u8)r0;

    /* 1 バイト応答なら PCN は来ない。MSR でも裏を取る — ST0 の読み方を
     * 間違えても、CB が落ちていれば読みに行かない。 */
    if (fdc_sis_result_bytes(*st0) < FDC_SIS_LEN_NORMAL) return 0;
    if (!fdc_result_pending()) return 0;

    r1 = fdc_read_byte();
    if (r1 < 0) return -1;
    *cyl = (u8)r1;
    return 0;
}

/* ======================================================================== */
/*  未回収の割り込みを排水する                                              */
/*                                                                          */
/*  SEEK / RECALIBRATE を出す前に必ず呼ぶ。遅れて来た seek-end 割り込みを    */
/*  SIS で読み出さないまま次のコマンドを出すと、µPD765A の INT 線が上がり    */
/*  っぱなしになり、**エッジトリガの PIC に次のエッジが来ない**。以後の      */
/*  fdc_wait_irq が全部タイムアウトし、3 回リトライしても読めず f_mount が   */
/*  落ちる — 実機の root panic はこの連鎖だった。                           */
/*                                                                          */
/*  回数は FDC_SIS_DRAIN_MAX で縛る (壊れた FDC で無限ループにしない)。      */
/*  戻り値: 排水した件数。                                                  */
/* ======================================================================== */
static int fdc_drain_interrupts(void)
{
    u8 st0, cyl;
    int i;

    for (i = 0; i < FDC_SIS_DRAIN_MAX; i++) {
        if (fdc_sense_interrupt(&st0, &cyl) != 0) break;
        /* ST0 = 80h = もう pending は無い。 */
        if (fdc_sis_result_bytes(st0) < FDC_SIS_LEN_NORMAL) break;
    }
    fdc_irq_fired = 0;
    return i;
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
/*  シーク完了待ち (SEEK / RECALIBRATE 共通)                                */
/*                                                                          */
/*  **期限 (start + FDC_SEEK_TIMEOUT_TICKS) までループする。**              */
/*  IRQ 待ちをループの前に 1 回だけ置くと、別ドライブの通知 (例: drv1 の     */
/*  Ready 変化) で IRQ が上がったときに、それを読み捨てた直後の SIS が       */
/*  80h を返して **自ドライブの正常なシークを途中で失敗にする**。           */
/*  なので:                                                                 */
/*    - 別ドライブの通知なら捨てて待ちを続ける                              */
/*    - 80h (pending 無し) も期限内なら待ちを続ける                         */
/*    - 自ドライブの結果が出たらそれを返す                                  */
/*  上限は**時間で**縛る (tick_count は単調に進むので無限ループにならない)。 */
/*                                                                          */
/*  期限を過ぎたら最後にもう一度 SIS を出す — 実機では遅れて上がった INT の  */
/*  エッジを PIC が取りこぼすことがあり、そのとき ST0 の SE が立っていれば   */
/*  シーク自体は終わっている。80h なら本当に終わっていないので              */
/*  FDC_SEEK_PENDING を返す。                                               */
/*                                                                          */
/*  want_cyl >= 0 で PCN を照合する。RECALIBRATE は -1 を渡す。             */
/*  戻り値は fdc_classify_seek_end() の FDC_SEEK_*。                        */
/*  診断用に最後に見た ST0 を s_last_seek_st0 へ残す。                      */
/* ======================================================================== */
static int fdc_wait_seek_end(int drv, u8 *st0, u8 *cyl, int want_cyl)
{
    u32 start = tick_count;
    u8 want_ds = (u8)(drv & FDC_ST0_DS_MASK);
    int i, rc;

    *st0 = 0;
    *cyl = 0;

    for (;;) {
        u32 elapsed = tick_count - start;

        if (elapsed >= FDC_SEEK_TIMEOUT_TICKS) break;   /* 期限切れ */
        /* 残り時間だけ待つ。来なければ期限切れとして下の救済へ。 */
        if (fdc_wait_irq(FDC_SEEK_TIMEOUT_TICKS - elapsed) != 0) break;

        if (fdc_sense_interrupt(st0, cyl) != 0) return FDC_SEEK_FAIL;
        s_last_seek_st0 = *st0;
        rc = fdc_classify_seek_end(*st0, *cyl, want_cyl);

        /* まだ何も出ていない / 別ドライブの通知 — どちらも待ちを続ける。 */
        if (rc == FDC_SEEK_PENDING) continue;
        if ((u8)(*st0 & FDC_ST0_DS_MASK) != want_ds) continue;
        return rc;
    }

    /* 期限切れ。エッジの取りこぼしだけなら救う。 */
    for (i = 0; i < FDC_SIS_DRAIN_MAX; i++) {
        if (fdc_sense_interrupt(st0, cyl) != 0) return FDC_SEEK_FAIL;
        s_last_seek_st0 = *st0;
        rc = fdc_classify_seek_end(*st0, *cyl, want_cyl);
        if (rc == FDC_SEEK_PENDING) return rc;
        if ((u8)(*st0 & FDC_ST0_DS_MASK) != want_ds) continue;
        return rc;
    }
    return FDC_SEEK_FAIL;
}

/* ======================================================================== */
/*  FDCリセット                                                             */
/* ======================================================================== */
static int fdc_reset(void)
{
    /* FDCをリセット */
    outp(FDC_CTRL, CTRL_RST);
    fdc_delay();
    fdc_delay();
    fdc_delay();
    fdc_delay();

    /* リセット解除 + DMA有効 + モーターON */
    fdc_irq_fired = 0;
    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);

    /* リセット完了IRQ待ち。来ない機種・エミュレータがあるので、
     * タイムアウトしても止めずに SIS の排水へ進む。 */
    (void)fdc_wait_irq(FDC_RESET_TIMEOUT_TICKS);

    /* リセット後は 4 ドライブ分の完了通知が溜まる。ST0 = 80h で尽きる。 */
    (void)fdc_drain_interrupts();

    /* Specifyコマンド: SRT=8ms, HLT=10ms, HUT=max, DMA有効 (値は fdc.h)。 */
    if (fdc_send_byte(FDC_CMD_SPECIFY) != 0) return -1;
    if (fdc_send_byte(FDC_SPECIFY_SRT_HUT) != 0) return -1;
    if (fdc_send_byte(FDC_SPECIFY_HLT_DMA) != 0) return -1;

    return 0;
}

/* ======================================================================== */
/*  実行フェーズごと打ち切る (転送の後始末)                                 */
/*                                                                          */
/*  **DMA ch2 を先にマスクするのが肝**。READ が実行フェーズ中にタイム       */
/*  アウトしたまま -1 を返すと、ch2 はアンマスクのまま残る。次に WRITE が    */
/*  dma_buffer へ書くべき中身を写した後で、遅れて来た旧 READ の DMA が       */
/*  **そこを上書きし得る**。WRITE は最初の SEEK が失敗しても fdc_recover で  */
/*  先へ進むので、気づかないまま**旧データをディスクに書いて成功を返す**。  */
/*                                                                          */
/*  IRQ は待たない — 最短で戻す。積み残しは SIS の排水で回収する。          */
/* ======================================================================== */
static void fdc_abort_transfer(void)
{
    /* 1. まず DMA を止める。以後 dma_setup が「マスク→設定→アンマスク」
     *    するので、ここではマスクしたままにしておいてよい。 */
    outp(DMA_MASK_REG, DMA_MASK_CH2);

    /* 2. FDC をリセットして実行フェーズを畳む。 */
    outp(FDC_CTRL, CTRL_RST);
    fdc_delay();
    fdc_delay();
    fdc_delay();
    fdc_delay();
    fdc_irq_fired = 0;
    outp(FDC_CTRL, CTRL_MTON | CTRL_DMAE);

    /* 3. Specify はリセットで消えるので入れ直す。 */
    (void)fdc_send_byte(FDC_CMD_SPECIFY);
    (void)fdc_send_byte(FDC_SPECIFY_SRT_HUT);
    (void)fdc_send_byte(FDC_SPECIFY_HLT_DMA);

    /* 4. 積み残しの割り込みを回収する (IRQ 待ちはしない)。 */
    (void)fdc_drain_interrupts();
}

/* ======================================================================== */
/*  Recalibrate (ヘッドをシリンダ0に移動)                                   */
/* ======================================================================== */
/* 成功条件は「SE が立ち、EC が立っていない」こと。PCN は照合しない。
 * 戻り値: 0 = 成功 / -1 = コマンド送信失敗 / -2 = 未完了 (ST0 = 80h)
 *         -3 = EC が取れなかった / -4 = その他の失敗
 *         FDC_RC_NOT_READY (-5) = NR (媒体もドライブも無い。回復しない)
 * 最後に見た ST0 を *out_st0 に返す (診断用。NULL 可)。 */
static int fdc_recalibrate_st0(int drv, u8 *out_st0)
{
    u8 st0, cyl;
    int rc, attempt;

    st0 = 0;
    rc = FDC_SEEK_FAIL;

    for (attempt = 0; attempt < FDC_RECAL_ATTEMPTS; attempt++) {
        /* 前のコマンドの取りこぼしを片付けてから出す。 */
        (void)fdc_drain_interrupts();

        fdc_irq_fired = 0;
        if (fdc_send_byte(FDC_CMD_RECALIBRATE) != 0) {
            if (out_st0) *out_st0 = st0;
            return -1;
        }
        if (fdc_send_byte((u8)drv) != 0) {
            if (out_st0) *out_st0 = st0;
            return -1;
        }

        rc = fdc_wait_seek_end(drv, &st0, &cyl, -1);
        if (rc == FDC_SEEK_OK) {
            if (out_st0) *out_st0 = st0;
            return 0;
        }
        if (rc != FDC_SEEK_RETRY_EC) break;
        /* EC = 77 ステップでトラック 0 に届かなかった。80 シリンダ媒体で
         * ヘッドが 77 より奥に居ると起きる。もう一度出せば残りを踏む。 */
    }

    if (out_st0) *out_st0 = st0;
    if (rc == FDC_SEEK_NOT_READY) return FDC_RC_NOT_READY;
    if (rc == FDC_SEEK_PENDING)  return -2;
    if (rc == FDC_SEEK_RETRY_EC) return -3;
    return -4;
}

static int fdc_recalibrate(int drv)
{
    return fdc_recalibrate_st0(drv, (u8 *)0);
}

/* ======================================================================== */
/*  リトライのあいだの回復                                                  */
/*                                                                          */
/*  DMA マスク → FDC リセット (0x94 bit7) → Specify → SIS 排水 →           */
/*  recalibrate。排水と recalibrate は fdc_recalibrate_st0() の中で行う。   */
/*  1 回目の転送が失敗した時点で INT 線が上がりっぱなしになっている可能性   */
/*  があり、そのまま 2 回目を出しても必ず同じ形で失敗する。                 */
/*                                                                          */
/*  **先頭で DMA ch2 をマスクする** — 直前の試行が実行フェーズで落ちて      */
/*  いると ch2 がアンマスクのまま残っている。これで、最終失敗が seek        */
/*  フェーズ (dma_setup を通らない) で起きた場合も ch2 は必ず閉じている。   */
/* ======================================================================== */
static int fdc_recover(int drv)
{
    int rc;

    outp(DMA_MASK_REG, DMA_MASK_CH2);
    rc = fdc_reset();
    if (rc != 0) return rc;
    return fdc_recalibrate(drv);
}

/* ======================================================================== */
/*  Seek (指定シリンダに移動)                                               */
/* ======================================================================== */
static int fdc_seek(int drv, int cyl, int head)
{
    u8 st0, result_cyl;
    int rc;

    /* 未回収の割り込みを片付けてから出す (上の fdc_drain_interrupts の注記)。 */
    (void)fdc_drain_interrupts();

    fdc_irq_fired = 0;
    if (fdc_send_byte(FDC_CMD_SEEK) != 0) return -1;
    if (fdc_send_byte((u8)((head << 2) | drv)) != 0) return -1;
    if (fdc_send_byte((u8)cyl) != 0) return -1;

    /* 完了待ち。PCN が要求シリンダと一致することまで見る。 */
    rc = fdc_wait_seek_end(drv, &st0, &result_cyl, cyl);
    /* NR = 媒体もドライブも無い。**リトライも回復もしない** ので分けて返す。 */
    if (rc == FDC_SEEK_NOT_READY) return FDC_RC_NOT_READY;
    if (rc == FDC_SEEK_PENDING) return -2;
    if (rc != FDC_SEEK_OK) return -4;

    /* ヘッド安定待ち: 約15ms */
    {
        u32 start = tick_count;
        while ((tick_count - start) < 2) { /* 何もしない */ }
    }

    return 0;
}

/* ======================================================================== */
/*  最終失敗の 1 行 ([V4])                                                  */
/*                                                                          */
/*  リトライごとには出さない — 画面が流れて元の失敗が見えなくなる。        */
/*  リザルトフェーズまで届いていない (seek / cmd で落ちた) ときは           */
/*  results[] がまだ空なので、**最後の SEEK / RECALIBRATE の ST0** を出す。 */
/*  NR (媒体もドライブも無い) なら印を付ける — いちばん多い失敗なので       */
/*  「なぜ読めなかったか」が 1 行で分かるようにする。                      */
/* ======================================================================== */
static void fdc_report_fail(const char *op, int drv, int cyl, int head,
                            int sect, const char *phase, int have_results,
                            const u8 *results)
{
    u8 st0, st1, st2;
    const char *mark;

    if (!have_results) {
        st0 = s_last_seek_st0;
        st1 = 0;
        st2 = 0;
    } else {
        st0 = results[0];
        st1 = results[1];
        st2 = results[2];
    }
    mark = (st0 & FDC_ST0_NR) ? " (NR)" : "";

    kprintf(0x07,
            "[fdc] %s fail drv=%d chs=%d/%d/%d phase=%s st0=%02x st1=%02x st2=%02x%s\n",
            op, drv, cyl, head, sect, phase, st0, st1, st2, mark);
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
    const char *phase = "seek";
    int have_results = 0;   /* results[] が埋まったか (診断の出し分け) */
    int dma_armed = 0;      /* DMA ch2 をアンマスクしたまま抜けていないか */

    kmemset(results, 0, sizeof(results));

    for (retry = 0; retry < FDC_RW_RETRIES; retry++) {
        int seek_rc;

        /* 0. 前の試行が失敗している。**次を出す前に回復する** —
         *    リセットと recalibrate を挟まないと、上がりっぱなしの INT 線の
         *    まま同じ形で失敗し続ける。最後の失敗の後には呼ばない。
         *    fdc_recover は先頭で ch2 をマスクするので dma_armed も下りる。
         *    診断は**最後の試行だけ**を映すので results[] も消す。 */
        if (retry > 0) {
            (void)fdc_recover(drv);
            dma_armed = 0;
            have_results = 0;
            kmemset(results, 0, sizeof(results));
        }

        /* 1. シーク */
        phase = "seek";
        seek_rc = fdc_seek(drv, cyl, head);
        /* NR = 媒体もドライブも無い。**リトライも回復もしない** —
         * 回復で直るものではないし、HDD 起動時の /fd0 サブマウント試行が
         * 空のドライブに対して毎回 3 回踏むと起動が数秒伸びる。 */
        if (seek_rc == FDC_RC_NOT_READY) break;
        if (seek_rc != 0) continue;

        /* 2. DMAセットアップ (FDC→メモリ = read)。
         *    ここで ch2 がアンマスクされる。以後どこで抜けても
         *    **閉じてからでないと戻れない** (下の fdc_abort_transfer)。 */
        dma_setup(phys, bps, 0);
        dma_armed = 1;
        phase = "cmd";

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
        phase = "irq";
        if (fdc_wait_irq(FDC_RW_TIMEOUT_TICKS) != 0) continue;

        /* 5. リザルト読み出し (7バイト) */
        phase = "result";
        n = fdc_read_results(results, 7);
        have_results = (n > 0);
        if (n < 7) continue;

        /* 6. エラーチェック: ST0のbit6-7が00なら成功 */
        if ((results[0] & FDC_ST0_IC_MASK) == FDC_ST0_IC_NORMAL) {
            /* DMAバッファからユーザーバッファにコピー */
            kmemcpy((u8 *)buf, dma_buffer, (u32)bps);
            return 0;
        }
    }

    /* 実行フェーズで落ちたまま戻らない。**DMA ch2 を閉じる** —
     * 開いたままだと、次の WRITE が dma_buffer を用意した後に遅れて来た
     * 旧 READ の DMA がそれを上書きし、古い内容が書かれてしまう。
     * 判断は phase の文字列ではなく dma_armed で行う — コマンド送信が
     * 途中で失敗した場合も ch2 は既に開いている。 */
    if (dma_armed) fdc_abort_transfer();

    fdc_report_fail("read", drv, cyl, head, sect, phase, have_results, results);
    return -1;
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
    const char *phase = "seek";
    int have_results = 0;   /* results[] が埋まったか (診断の出し分け) */
    int dma_armed = 0;      /* DMA ch2 をアンマスクしたまま抜けていないか */

    kmemset(results, 0, sizeof(results));

    /* ユーザーバッファからDMAバッファにコピー */
    kmemcpy(dma_buffer, (const u8 *)buf, (u32)bps);

    for (retry = 0; retry < FDC_RW_RETRIES; retry++) {
        int seek_rc;

        /* 0. 前の試行が失敗している。次を出す前に回復する (read 側と同じ)。 */
        if (retry > 0) {
            (void)fdc_recover(drv);
            dma_armed = 0;
            have_results = 0;
            kmemset(results, 0, sizeof(results));
        }

        /* 1. シーク */
        phase = "seek";
        seek_rc = fdc_seek(drv, cyl, head);
        if (seek_rc == FDC_RC_NOT_READY) break;   /* read 側と同じ理由 */
        if (seek_rc != 0) continue;

        /* 2. DMAセットアップ (メモリ→FDC = write)。read 側と同じ (ch2 が開く)。 */
        dma_setup(phys, bps, 1);
        dma_armed = 1;
        phase = "cmd";

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
        phase = "irq";
        if (fdc_wait_irq(FDC_RW_TIMEOUT_TICKS) != 0) continue;

        /* 5. リザルト読み出し */
        phase = "result";
        n = fdc_read_results(results, 7);
        have_results = (n > 0);
        if (n < 7) continue;

        /* 6. エラーチェック */
        if ((results[0] & FDC_ST0_IC_MASK) == FDC_ST0_IC_NORMAL) {
            return 0;
        }
    }

    if (dma_armed) fdc_abort_transfer();

    fdc_report_fail("write", drv, cyl, head, sect, phase, have_results, results);
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
    u8 st0 = 0;

    /* [HW2] DMA バッファが 64KB 境界をまたいでいないこと。
     * FDC_DMA_ALIGN の揃え指定があればまたぎようがないが、揃えが外れても
     * **黙って別の番地を壊さない** よう確かめて言う。 */
    if ((((u32)dma_buffer & FDC_DMA_BANK_MASK) + FDC_SECTOR_SIZE)
            > FDC_DMA_BANK_SIZE) {
        kprintf(0x07, "[fdc] dma buffer crosses 64KB boundary at %08x\n",
                (u32)dma_buffer);
    }

    /* I/O 0439h bit2 = 「1MB 以上への DMA アクセス禁止」で、**ノーマル
     * モードの起動時設定は 1**。dma_buffer はカーネル BSS (1MB 超) なので、
     * 立ったままだと READ DATA が正常終了してもデータが届かない。
     * 落とすのは dma_setup を通る前に 1 度でよい。
     *
     * bit7 (プリンタ I/F 選択) を壊さないよう **必ず RMW**。
     * **読みが FFh でも書く** — 実機は未使用ビットが 1 で読めれば正当に
     * FFh を返し得るので、そこを避けると DMA 禁止が残る (fdc.h の注記)。 */
    {
        u8 v = (u8)inp(SYSPORT_DMA_CTRL);

        if (v & SYSPORT_DMA_MASK_1MB) {
            u8 after;
            outp(SYSPORT_DMA_CTRL, (u8)(v & ~SYSPORT_DMA_MASK_1MB));
            after = (u8)inp(SYSPORT_DMA_CTRL);
            /* 読み戻しで落ちない機種でも起動は止めない ([V4]: 印を残す)。
             * NP21/W は 0439h に in ハンドラが無く常に FFh を返すので、
             * ここは `ff -> ff` と出る (書き込み自体は無害)。 */
            kprintf(0x07, "[fdc] dma>1MB: 0439h %02x -> %02x\n", v, after);
        }
    }

    /* 前回の取りこぼし IRQ をクリア (冪等化対策) */
    fdc_irq_fired = 0;

    /* モーターON (既にONの場合はスピンアップ待ちをスキップ) */
    fdc_motor_on();

    /* FDCリセット + Specify */
    ret = fdc_reset();
    if (ret != 0) return ret;

    /* Recalibrate (ヘッドをシリンダ0に移動)。
     * fdc_recalibrate_st0 は中で SIS 排水 → RECALIBRATE を
     * FDC_RECAL_ATTEMPTS 回 (EC のときだけ) 繰り返す。
     * ここでの 1 回目失敗はリセット直後の安定化待ち (100ms) 後にもう一度。 */
    ret = fdc_recalibrate_st0(0, &st0);
    if (ret != 0) {
        u32 start = tick_count;
        while ((tick_count - start) < 10) { /* 100ms ウェイト */ }
        ret = fdc_recalibrate_st0(0, &st0);
    }
    if (ret != 0) {
        /* [V4] 失敗はそのまま言う。ここで諦めると MOUNT の root panic に
         * なるが、画面には「なぜ」が出ていなかった。 */
        kprintf(0x07, "[fdc] recalibrate drv=%d rc=%d st0=%02x\n", 0, ret, st0);
    }

    /* ドライブ1は未接続時にタイムアウトするためエラーは無視する。
     * **黙って捨てない** — 失敗した完了通知は次の排水で回収される。 */
    (void)fdc_recalibrate(1);

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

