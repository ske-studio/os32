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
#include "kprintf.h"

/* drivers/ はカーネルヘッダ (kernel/paging.h) を見ないので extern で引く
 * (kbd.c / ide.c / lgy98.c と同じ作法)。BIOS ワークエリアを読むのは
 * master の番地空間にいるあいだだけにしたい。 */
extern u32 paging_current_cr3(void);
extern u32 paging_kernel_pd_phys(void);

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
/* ======================================================================== */
/*  システムクロックの判定 (0000:0501h bit7)                                */
/*                                                                          */
/*  0 = まだ判定していない → serial_init は従来どおり 1.9968MHz を使う。     */
/* ======================================================================== */
static unsigned long s_timer_clk = 0;
static u8 s_sysclk_8mhz = 0;
static struct serial_setup s_setup = { 0, 0, 0, 0, 0, 0 };

void serial_detect_clock(void)
{
    /* アドレスを volatile 経由にして定数畳み込みを止める (backend_pegc.c と
     * 同じ理由。直に書くと GCC が -Warray-bounds で誤診断する)。 */
    volatile u32 a = BIOS_WORK_SYSCLK;
    u8 v;

    /* master の番地空間でなければ読まない (BIOS ワークエリアは低位物理)。 */
    if (paging_current_cr3() != paging_kernel_pd_phys()) {
        return;
    }
    v = *(volatile u8 *)a;
    s_sysclk_8mhz = (u8)((v & BIOS_SYSCLK_8MHZ) ? 1 : 0);
    s_timer_clk = s_sysclk_8mhz ? TIMER_CLK_1997 : TIMER_CLK_2458;
}

const struct serial_setup *serial_get_setup(void)
{
    return &s_setup;
}

/* ======================================================================== */
/*  I/O 0434h — 拡張RS-232C制御 (極性は機種依存。serial.h の注記を読むこと)  */
/* ======================================================================== */
int serial_get_ext_ctrl(void)
{
    return (int)(u8)inp(SER_EXT_CTRL);
}

int serial_set_div4(int bit_value)
{
    u8 v;

    /* **bit0 (ポート切り離し) を保つ。** 読んでから bit6 だけ差し替える。 */
    v = (u8)inp(SER_EXT_CTRL);
    if (bit_value) {
        v = (u8)(v | SER_EXT_DIV4);
    } else {
        v = (u8)(v & ~SER_EXT_DIV4);
    }
    outp(SER_EXT_CTRL, v);
    io_wait();
    return (int)(u8)inp(SER_EXT_CTRL);
}

void serial_init(unsigned long baud)
{
    u16 count;
    u8  mode;
    unsigned long clk;

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
    /* クロックは 0000:0501h から判定したもの。まだ判定していなければ従来値。 */
    clk = s_timer_clk ? s_timer_clk : TIMER_CLK_1997;
    if (baud == 0) baud = 9600;
    count = (u16)(clk / 16UL / baud);
    if (count == 0) count = 1;

    /* **要求どおりに出るかを記録して報告する** ([V4]: 黙ってずれたまま進まない)。
     * 分周比は整数しか設定できないので、割り切れない速度は必ずずれる。
     * 例: 1.9968MHz で 38400 を頼むと count=3 になり実効 41600bps (+8.3%)。
     * UART の許容 (±3% 程度) を超えるので実機では通らない。 */
    s_setup.want = baud;
    s_setup.clk = clk;
    s_setup.count = count;
    s_setup.actual = clk / 16UL / (unsigned long)count;
    s_setup.sysclk_8mhz = s_sysclk_8mhz;
    s_setup.exact = (u8)((s_setup.actual == baud) ? 1 : 0);
    if (s_setup.exact) {
        kprintf(0x0A, "[ser] %ubps (clk %uHz, count %u)\n",
                (u32)baud, (u32)clk, (u32)count);
    } else {
        kprintf(0x0E,
                "[ser] WARN %ubps は出せない: 実効 %ubps (clk %uHz, count %u)\n",
                (u32)baud, (u32)s_setup.actual, (u32)clk, (u32)count);
    }

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
/* 受信バッファ溢れで捨てたバイト数 (kernel.map 経由で観測する)。
 * static にするとホストから読めないので意図的にグローバル。 */
u32 ser_overflow_n = 0;

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

        /* バッファに格納。
         * 満杯なら捨てるしかないが、黙って捨てると「rshell の応答が
         * たまに欠ける」の原因が分からなくなるので回数を残す
         * (ISR 内なので kprintf は使えない — 出力先が自分自身)。 */
        if (ser_count < SER_BUF_SIZE) {
            ser_buf[ser_tail] = data;
            ser_tail = (ser_tail + 1) % SER_BUF_SIZE;
            ser_count++;
        } else {
            ser_overflow_n++;
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

    RING_DEQUEUE(ch, ser_buf, ser_head, ser_count, SER_BUF_SIZE);

    return ch;
}

/* 取り出さずに先頭だけ覗く (継承バグ: script_exec の ESC 監視)。
 * 無ければ -1。serial_trygetchar と違ってリングは 1 バイトも動かさないので、
 * 「ESC かどうかだけ見て、ESC でなければ次の読み手へ残す」が書ける。
 * IRQ4 は tail 側にしか触らないので、ser_count > 0 なら先頭は動かない。 */
int serial_peekchar(void)
{
    if (ser_count == 0) return -1;
    return (int)ser_buf[ser_head];
}

/* ブロッキング受信 */
int serial_getchar(void)
{
    int ch;
    while (ser_count == 0) {
        _halt();
    }

    RING_DEQUEUE(ch, ser_buf, ser_head, ser_count, SER_BUF_SIZE);

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
        _halt();
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

/* ======================================================================== */
/*  ポーリング専用送信 (パニック/例外時用)                                   */
/*  割り込みが無効化されている状態で hlt を使用するとフリーズするため、      */
/*  タイムアウトまでスピンのみで待機する。                                   */
/* ======================================================================== */
static void serial_putchar_polled(char c)
{
    int spin;
    for (spin = 0; spin < 50000; spin++) {
        if (inp(SER_CMD) & STS_TXRDY) {
            outp(SER_DATA, (unsigned)(u8)c);
            return;
        }
    }
}

void serial_puts_polled(const char *str)
{
    int count = 0;
    while (*str) {
        serial_putchar_polled(*str);
        if (*str == '\n') {
            serial_putchar_polled('\r');
        }
        str++;
        count++;
        if ((count & 0xF) == 0) {
            io_wait();
            io_wait();
        }
    }
}

void serial_put_hex32_polled(u32 val)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 7; i >= 0; i--) {
        buf[i+2] = hex[val & 0xF];
        val >>= 4;
    }
    buf[10] = '\0';
    serial_puts_polled(buf);
}
