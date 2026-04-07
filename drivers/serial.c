/* ======================================================================== */
/*  SERIAL.C — PC-98 RS-232C シリアル通信ドライバ                          */
/*                                                                          */
/*  μPD8251A内蔵RS-232Cポートを直接制御                                    */
/*  IRQ4割り込みで受信データをリングバッファに格納                            */
/*                                                                          */
/*  出典: PC9800Bible §2-10                                                 */
/*  参照: FreeBSD sys/pc98/cbus/sio.c                                       */
/*    - pc98_i8251_reset(): 0x00×3 → 0x40 → mode → cmd                    */
/*    - pc98_set_baud_rate(): PIT #2 (0x75/0x77), I/Oウェイト(0x5f)         */
/*    - pc98_ttspeedtab(): 8MHz系 1996800 / 16 / speed                     */
/*    - IRQ4固定、ポート {0x30, 0x32, 0x32, 0x33, 0x35}                    */
/* ======================================================================== */

#include "serial.h"
#include "io.h"
#include "pc98.h"

/* 外部: irq_enable (idt.c で定義) */
extern void irq_enable(unsigned int irq);

/* ======== 初期化状態 ======== */
static int ser_initialized = 0;

/* ======== 受信リングバッファ ======== */
static volatile u8  ser_buf[SER_BUF_SIZE];
static volatile int ser_head = 0;
static volatile int ser_tail = 0;
static volatile int ser_count = 0;

/* ======================================================================== */
/*  PITモード値 (RS-232C通信速度設定用)                                    */
/*  PC9800Bible §2-3: カウンタ#2 = RS-232C通信速度                             */
/* ======================================================================== */
#define PIT_SER_MODE3  (PIT_SC_CNT2 | PIT_RL_LSBMSB | PIT_M_SQWAVE)  /* 0xB6 */
#define PIT_SER_MODE2  (PIT_SC_CNT2 | PIT_RL_LSBMSB | PIT_M_RATEGEN) /* 0xB4 */

/* ======================================================================== */
/*  serial_init — RS-232C初期化                                             */
/*                                                                          */
/*  FreeBSD pc98_i8251_reset() + pc98_set_baud_rate() 準拠                  */
/*  デフォルト: 8N1 (8bit, パリティなし, ストップビット1)                     */
/* ======================================================================== */
void serial_init(unsigned long baud)
{
    u16 count;
    u8  mode;

    /* ---- 割り込み禁止 (初期化中) ---- */
    outp(SER_MASK, 0x00);   /* 全割り込みマスク */

    /* ---- 8251A リセット (FreeBSD pc98_i8251_reset() 準拠) ---- */
    outp(SER_CMD, 0x00); io_wait();   /* ダミー ×3 */
    outp(SER_CMD, 0x00); io_wait();
    outp(SER_CMD, 0x00); io_wait();
    outp(SER_CMD, CMD_RESET); io_wait();   /* 内部リセット (0x40) */

    /* PC-98: BUZ OFF (ポート0x37 BSRモード)
     * PC9800Bible: 0x06=OFF, 0x07=ON だが NP21/Wでは極性逆
     * NP21/W: BSR_BUZ_ON (0x07) = BUZ OFF */
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);
    /* 8MHz系: 1996800 / 16 / baud */
    if (baud == 0) baud = 9600;
    count = (u16)(TIMER_CLK_8MHZ / 16UL / baud);
    if (count == 0) count = 1;

    /* PIT モード設定: カウンタ#2, LSB+MSB, Mode 3(方形波) */
    /* FreeBSD: count==3 のときだけ Mode 2 */
    if (count != 3)
        outp(SER_TIMER_MODE, PIT_SER_MODE3);
    else
        outp(SER_TIMER_MODE, PIT_SER_MODE2);

    io_wait();
    outp(SER_TIMER_CNT, count & 0xFF);
    io_wait();
    outp(SER_TIMER_CNT, (count >> 8) & 0xFF);

    /* ---- モードセット: 8N1, ×16分周 ---- */
    mode = MOD_CLKx16 | MOD_8BIT | MOD_STOP1;  /* 0x4E */
    outp(SER_CMD, mode); io_wait();

    outp(SER_CMD, CMD_TXE | CMD_DTR | CMD_RXE | CMD_RTS | CMD_ER);
    /* = 0x01 | 0x02 | 0x04 | 0x20 | 0x10 = 0x37 */

    /* ---- バッファクリア ---- */
    ser_head = 0;
    ser_tail = 0;
    ser_count = 0;

    /* ---- 受信割り込みを有効化 ---- */
    outp(SER_MASK, IEN_RX);

    /* ---- PIC IRQ4 有効化 ---- */
    irq_enable(4);

    /* ---- BUZ OFF 再確認 (PIT設定の副作用対策) ---- */
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);  /* NP21/W: BSR_BUZ_ON = BUZ OFF */

    ser_initialized = 1;
}

/* ======================================================================== */
/*  serial_irq_handler — IRQ4 割り込みハンドラ (Cレベル)                    */
/*                                                                          */
/*  PC-98ではRS-232Cの送受信が同一IRQ4を共有                                */
/*  ステータスを読んで受信か送信かを判定                                      */
/* ======================================================================== */
void serial_irq_handler(void)
{
    u8 sts;
    u8 data;
    int loop_count = 0; /* 無限ループ防止用のカウンタ */

    for (;;) {
        sts = (u8)inp(SER_CMD);
        
        /* エラーがあればリセット */
        if (sts & (STS_PE | STS_OE | STS_FE)) {
            outp(SER_CMD, CMD_TXE | CMD_DTR | CMD_RXE | CMD_RTS | CMD_ER);
        }

        if (!(sts & STS_RXRDY)) break;

        data = (u8)inp(SER_DATA);

        /* バッファに格納 */
        if (ser_count < SER_BUF_SIZE) {
            ser_buf[ser_tail] = data;
            ser_tail = (ser_tail + 1) % SER_BUF_SIZE;
            ser_count++;
        }

        /* 異常な割り込み嵐を防ぐため、1回のIRQで最大128バイト読んだら一旦抜ける */
        loop_count++;
        if (loop_count > 128) break;
    }

    outp(SER_MASK, 0x00);
    outp(SER_MASK, IEN_RX);
}

/* ======================================================================== */
/*  公開API                                                                */
/* ======================================================================== */

int serial_is_initialized(void)
{
    return ser_initialized;
}

int serial_has_data(void)
{
    return ser_count > 0;
}

/* ノンブロッキング受信 */
int serial_trygetchar(void)
{
    int ch;
    if (ser_count == 0) return -1;

    _disable();
    ch = ser_buf[ser_head];
    ser_head = (ser_head + 1) % SER_BUF_SIZE;
    ser_count--;
    _enable();

    return ch;
}

/* ブロッキング受信 */
int serial_getchar(void)
{
    int ch;
    while (ser_count == 0) {
        __asm__ volatile("hlt");
    }

    _disable();
    ch = ser_buf[ser_head];
    ser_head = (ser_head + 1) % SER_BUF_SIZE;
    ser_count--;
    _enable();

    return ch;
}

/* ポーリング送信 (TxRDY待ち + 割り込み待機)
 * TxRDYが即座にセットされない場合は hlt で待機して
 * CPU負荷を軽減する。NP21/Wのパイプバッファ溢れ対策。 */
void serial_putchar(char c)
{
    int spin;
    int retry;

    for (retry = 0; retry < 5; retry++) {
        /* まず短いスピン (高速パス) */
        for (spin = 0; spin < 100; spin++) {
            if (inp(SER_CMD) & STS_TXRDY) {
                outp(SER_DATA, (unsigned)(u8)c);
                return;
            }
        }
        /* TxRDYでないなら hlt で1割り込み分待つ */
        __asm__ volatile("hlt");
    }
    /* タイムアウト: 送信を諦める */
}

/* 文字列送信 (フロー制御付き)
 * 16バイトごとにio_waitを挿入し、
 * NP21/Wのパイプバッファが処理する時間を確保 */
void serial_puts(const char *str)
{
    int count = 0;
    while (*str) {
        serial_putchar(*str);
        str++;
        count++;
        if ((count & 0xF) == 0) {
            /* 16バイトごとに短いウェイト */
            io_wait();
            io_wait();
        }
    }
}
