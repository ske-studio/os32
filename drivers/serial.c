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
#include "serial_plan.h"   /* 速度と FIFO の純粋な判定 (ホストで試験する) */
#include "io.h"
#include "pc98.h"
#include "kprintf.h"

/* drivers/ はカーネルヘッダ (kernel/paging.h) を見ないので extern で引く
 * (kbd.c / ide.c / lgy98.c と同じ作法)。BIOS ワークエリアを読むのは
 * master の番地空間にいるあいだだけにしたい。 */
extern u32 paging_current_cr3(void);
extern u32 paging_kernel_pd_phys(void);

/* 外部: irq_enable / irq_disable (idt.c で定義)。drivers/ は -Ikernel を
 * 持たないので、kbd.c / ide.c と同じ扱いでここに宣言する。 */
extern void irq_enable(unsigned int irq);
extern void irq_disable(unsigned int irq);

/* 外部: 校正済みマイクロ秒ディレイ (kernel/cpu_calibrate.h)。
 * drivers/ はカーネルヘッダを見ない作法なので extern で引く
 * (paging_* と同じ理由。ne2000.c は kernel/ を -I しているが、ここは
 * 宣言 1 行で足りる)。 */
extern void cpu_delay_us(u32 us);

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
static struct serial_setup s_setup = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ======================================================================== */
/*  いま使っているポートとビットマスク                                      */
/*                                                                          */
/*  **互換 (0030h/0032h) と FIFO (0130h/0132h) はビット位置まで違う。**      */
/*  取り違えを 1 か所に閉じ込めるために、serial_plan.c の選択子で引いた値を  */
/*  ここに持ち、送受信はこれだけを見る。初期値は従来の互換モード。          */
/* ======================================================================== */
static unsigned int s_port_data  = SER_DATA;
static unsigned int s_port_cmd   = SER_CMD;
static u8 s_mask_txrdy = STS_TXRDY;
static u8 s_mask_rxrdy = STS_RXRDY;
static u8 s_mask_err   = (u8)(STS_PE | STS_OE | STS_FE);
/* `0035h` に書く割り込み許可。**ISR 末尾の再許可もこれを使う** — 直値だと
 * 切替でマスクを変えた瞬間に ISR が踏み潰す (Codex レビュー blocker 1)。 */
static u8 s_mask_ien   = IEN_RX;

/* TxRDY を待つ予算 [µs]。serial_init で速度から決める (serial_plan.h の式)。
 * 初期化前でも putchar が呼ばれうる (パニック経路) ので既定を入れておく。 */
static u32 s_tx_budget_us = (u32)SER_TX_BUDGET_MAX_US;

/* FIFO 搭載判定はリセットまで変わらないので 1 回だけ行う。 */
static u8 s_fifo_probed = 0;

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

/* ======================================================================== */
/*  FIFO 搭載判定 — 0136h を 2 回読んで bit6 の反転を見る                    */
/*                                                                          */
/*  資料 io_rs.md 304〜323 行。判定そのものは serial_plan.c にあり、ここは   */
/*  読むだけ。**リセットまで変わらないので 1 回しか行わない** (0136h の      */
/*  読みは NP21/W では pic_resetirq(4) の副作用を持つ)。                    */
/* ======================================================================== */
static void serial_probe_fifo(void)
{
    u8 a, b;

    if (s_fifo_probed) {
        return;
    }
    a = (u8)inp(SER_FIFO_IIR);
    io_wait();
    b = (u8)inp(SER_FIFO_IIR);
    s_setup.has_fifo = (u8)(serial_fifo_detected(a, b) ? 1 : 0);
    s_fifo_probed = 1;
}

/* ======================================================================== */
/*  serial_init_ex — 初期化の本体 (互換 / V･FAST 共通)                      */
/*                                                                          */
/*  want_vfast が 0 なら従来とまったく同じ経路を通る。1 のときだけ          */
/*  0138h / 013Ah を叩く。戻り 0 = V･FAST に入った / -1 = 互換。            */
/* ======================================================================== */
static int serial_init_ex(unsigned long baud, int want_vfast)
{
    struct serial_plan_out plan;
    u8  mode;
    unsigned long clk;
    unsigned int irqf;
    unsigned long keep;
    int rc;

    /* ==================================================================== */
    /*  切替のあいだは IRQ4 を止める                                        */
    /*                                                                      */
    /*  **`0035h` に 0 を書くだけでは排他にならない。** NP21/W の            */
    /*  `rs232c_callback` は `0035h` を見ずに `pic_setirq(4)` を上げるし、   */
    /*  こちらの ISR 末尾も無条件で `0035h` を再許可する。止めないと:        */
    /*    - `ser_head=0; ser_tail=0;` の直後に ISR が添字 0 へ書いて tail=1、 */
    /*      そのあと初期化側が `ser_count=0` を書く → 読み手は head=0 から   */
    /*      **古い値を 1 バイト返す**                                        */
    /*    - `s_port_*` / `s_mask_*` を 1 本ずつ差し替えている最中に ISR が   */
    /*      走ると、**新しいポートを古いビット位置で読む**                   */
    /*  PIC のマスクと IF の両方を落とす。IF は元の状態へ戻す                */
    /*  (`irq_save`/`irq_restore`) — 起動時は IF=0 で呼ばれることがある。    */
    /*                                                                      */
    /*  ⚠ **この区間で `kprintf` を呼んではいけない。** rshell 中の          */
    /*  `console.c` は kprintf をシリアルへも流し、`serial_putchar` は予算を */
    /*  使い切ると `_halt()` する。IF=0 の `hlt` は二度と起きない。          */
    /*  報告は区間を出てから行う。                                          */
    /* ==================================================================== */
    irqf = irq_save();
    irq_disable(4);

    /* ---- FIFO 搭載判定 (1 回だけ。0136h を読むだけで何も書かない) ---- */
    serial_probe_fifo();

    /* クロックは 0000:0501h から判定したもの。まだ判定していなければ従来値。 */
    clk = s_timer_clk ? s_timer_clk : TIMER_CLK_1997;
    if (baud == 0) baud = SER_BAUD_DEFAULT;
    serial_plan(baud, (int)s_setup.has_fifo, clk, want_vfast, &plan);

    /* ==================================================================== */
    /*  出せない速度は **適用しない** (現状維持)                             */
    /*                                                                      */
    /*  以前は「WARN を出して実効値を適用」だった。それだと FIFO 非搭載機で  */
    /*  `serial 115200` を打ったとき、2.4576MHz 系では count=1 = 153600bps が */
    /*  そのまま入る。ホストは 115200 へ移ってしまうので、**戻すための       */
    /*  `serial 9600` すら届かなくなる** (Codex レビュー blocker 2)。        */
    /*  ここまでハードウェアには 1 バイトも書いていないので、そのまま戻れば  */
    /*  いまの設定が生き残る。                                              */
    /*                                                                      */
    /*  起動時の既定 (SYS_SERIAL_BAUD = 9600) は 1.9968MHz / 2.4576MHz の    */
    /*  どちらでも exact なので、この分岐を通らない。                        */
    /* ==================================================================== */
    if (plan.mode == SER_MODE_COMPAT && !plan.exact) {
        keep = s_setup.actual;
        if (ser_initialized) {
            irq_enable(4);   /* いまの設定を生かしたままにする */
        }
        irq_restore(irqf);
        if (ser_initialized) {
            kprintf(0x0E,
                    "[ser] refuse %ubps: 8253 では %ubps になる (現状維持 %ubps)\n",
                    (u32)baud, (u32)plan.actual, (u32)keep);
        } else {
            kprintf(0x0E,
                    "[ser] refuse %ubps: 8253 では %ubps になる (未初期化のまま)\n",
                    (u32)baud, (u32)plan.actual);
        }
        return SER_INIT_REFUSED;
    }

    /* ---- ここから実際に書く ---- */
    outp(SER_MASK, 0x00);   /* 全割り込みマスク */

    /* ---- いまのモードから抜ける ----
     * V･FAST / FIFO から互換へ戻すときは **8251 を触る前に** 013Ah bit7 と
     * 0138h を落とす。落とさないと以後のコマンド書きが 0032h と 0132h の
     * どちらに効くのか決まらない。
     * FIFO 非搭載機では 0130h〜013Ah は存在しないので触らない。 */
    if (s_setup.has_fifo && plan.mode != SER_MODE_VFAST) {
        outp(SER_VFAST_REG, SER_VFAST_OFF); io_wait();
        outp(SER_FIFO_FCR, SER_FCR_OFF);    io_wait();
    }

    /* ---- ポートとビットマスクを新しいモードに合わせる ---- */
    s_port_data  = serial_data_port(plan.mode);
    s_port_cmd   = serial_cmd_port(plan.mode);
    s_mask_txrdy = serial_txrdy_mask(plan.mode);
    s_mask_rxrdy = serial_rxrdy_mask(plan.mode);
    s_mask_err   = serial_err_mask(plan.mode);

    /* ---- V･FAST に入る ----
     * 1. 0138h に FCR0|FCR1|FCR2 = FIFO モード + 送受信 FIFO リセット
     * 2. 013Ah に bit7 | 分周
     * 以後データは 0130h、ステータス/コマンドは 0132h。 */
    if (plan.mode == SER_MODE_VFAST) {
        outp(SER_FIFO_FCR,
             SER_FCR_ENABLE | SER_FCR_RX_RST | SER_FCR_TX_RST); io_wait();
        outp(SER_VFAST_REG,
             (unsigned)(SER_VFAST_ENABLE | plan.div)); io_wait();
    }

    /* ---- 8251A リセット (FreeBSD pc98_i8251_reset() 準拠) ----
     * FIFO は 8251 の**前段**に入るだけで 1st CCU は 8251 のまま (資料の
     * 「RS-232C クロック」図) なので、手順は互換モードと同じ。違うのは
     * 書き先が 0132h になることだけ。**資料は 0132h を [READ] としか
     * 書いていない**が、NP21/W は 0132h の out を 0032h と同じハンドラ
     * (rs232c_o32) に繋いでいる (rs232c_bind)。資料 0138h の関連欄も
     * 0030h / 0032h を挙げているので、これに従う。 */
    outp(s_port_cmd, 0x00); io_wait();   /* ダミー ×3 */
    outp(s_port_cmd, 0x00); io_wait();
    outp(s_port_cmd, 0x00); io_wait();
    outp(s_port_cmd, CMD_RESET); io_wait();   /* 内部リセット (0x40) */

    /* PC-98: BUZ OFF (ポート0x37 BSRモード)
     * PC9800Bible: 0x06=OFF, 0x07=ON だが NP21/Wでは極性逆
     * NP21/W: BSR_BUZ_ON (0x07) = BUZ OFF */
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);

    /* **要求どおりに出るかを記録して報告する** ([V4]: 黙ってずれたまま進まない)。
     * 互換モードの分周比は整数しか設定できないので、割り切れない速度は必ずずれる。
     * 例: 1.9968MHz で 38400 を頼むと count=3 になり実効 41600bps (+8.3%)。
     * UART の許容 (±3% 程度) を超えるので実機では通らない。
     * V･FAST は 8253 と無関係なので、表にある速度はちょうど出る。 */
    s_setup.want = baud;
    s_setup.clk = clk;
    s_setup.count = plan.count;
    s_setup.actual = plan.actual;
    s_setup.sysclk_8mhz = s_sysclk_8mhz;
    s_setup.exact = plan.exact;
    s_setup.mode = (u8)plan.mode;
    s_setup.vfast_div = plan.div;

    /* TxRDY を待つ予算は **実効速度** から決める (要求値ではない)。 */
    s_tx_budget_us = serial_tx_budget_us(s_setup.actual);

    /* ---- 8253 カウンタ#2 (互換モードだけ) ----
     * V･FAST 中は 013Ah bit7 がカウンタ#2 出力を無効にするので触らない
     * (資料 013Ah の解説)。互換へ戻すときはこの経路が必ず通るので、
     * 8253 は「互換に戻った時点で」正しい値に入る。 */
    if (plan.mode == SER_MODE_COMPAT) {
        /* PIT モード設定: カウンタ#2, LSB+MSB, Mode 3(方形波) */
        /* FreeBSD: count==3 のときだけ Mode 2 */
        if (plan.count != 3)
            outp(SER_TIMER_MODE, PIT_SER_MODE3);
        else
            outp(SER_TIMER_MODE, PIT_SER_MODE2);

        io_wait();
        outp(SER_TIMER_CNT, plan.count & 0xFF);
        io_wait();
        outp(SER_TIMER_CNT, (plan.count >> 8) & 0xFF);
    }

    /* ---- モードセット: 8N1, ×16分周 ---- */
    mode = MOD_CLKx16 | MOD_8BIT | MOD_STOP1;  /* 0x4E */
    outp(s_port_cmd, mode); io_wait();

    outp(s_port_cmd, CMD_TXE | CMD_DTR | CMD_RXE | CMD_RTS | CMD_ER);
    /* = 0x01 | 0x02 | 0x04 | 0x20 | 0x10 = 0x37 */

    /* ---- バッファクリア ---- */
    ser_head = 0;
    ser_tail = 0;
    ser_count = 0;

    /* ---- 受信割り込みを有効化 ----
     * **資料に FIFO モードでの割り込みマスクの記述は無い。** 0136h は
     * 「割り込み参照」で、許可/禁止のレジスタではない。NP21/W も 0035h
     * 以外でマスクしていないので、両モードとも従来どおり 0035h を使う。
     * ISR 末尾の再許可は s_mask_ien を書くので、ここで決めた値と食い違わない
     * (直値を書いていたころは、モードごとにマスクを変えた瞬間に ISR が
     * 1 回目の受信でそれを踏み潰す形になっていた)。 */
    s_mask_ien = IEN_RX;
    outp(SER_MASK, s_mask_ien);

    /* ---- PIC IRQ4 有効化 ---- */
    irq_enable(4);

    /* ---- BUZ OFF 再確認 (PIT設定の副作用対策) ---- */
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);  /* NP21/W: BSR_BUZ_ON = BUZ OFF */

    ser_initialized = 1;
    rc = (plan.mode == SER_MODE_VFAST) ? SER_INIT_VFAST : SER_INIT_COMPAT;

    /* ---- 危険区間はここまで。IF を元へ戻す ---- */
    irq_restore(irqf);

    /* **報告は区間の外で。** rshell 中はこの kprintf がシリアルへも流れ、
     * `serial_putchar` が予算を使い切ると `_halt()` する ([V4] の報告より
     * 先に、IF=0 の hlt で止まらないことが要る)。 */
    if (plan.mode == SER_MODE_VFAST) {
        kprintf(0x0A, "[ser] %ubps (V-FAST div %u, FIFO)\n",
                (u32)s_setup.actual, (u32)plan.div);
    } else {
        kprintf(0x0A, "[ser] %ubps (clk %uHz, count %u)%s\n",
                (u32)baud, (u32)clk, (u32)plan.count,
                s_setup.has_fifo ? " [FIFO available]" : "");
    }
    return rc;
}

void serial_init(unsigned long baud)
{
    /* **従来の経路のまま。** 票の決裁「起動時の既定 9600 は互換モード」。
     * V･FAST 中に呼べば 013Ah bit7=0 / 0138h=0 を書いて互換へ戻る。 */
    (void)serial_init_ex(baud, 0);
}

int serial_init_vfast(unsigned long baud)
{
    /* FIFO 非搭載 / 表に無い速度なら serial_plan が互換を返すので、
     * ここは「頼んだ」ことを渡すだけ。戻り -1 = 互換に落ちた。 */
    return serial_init_ex(baud, 1);
}

int serial_get_status(u32 *mode, u32 *baud, u32 *fifo)
{
    if (mode) *mode = (u32)s_setup.mode;
    if (baud) *baud = (u32)s_setup.actual;
    if (fifo) *fifo = (u32)s_setup.has_fifo;
    return ser_initialized ? 0 : -1;
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
    /* FIFO モードでは 1 回の割り込みで FIFO 1 杯 (16 バイト) を汲む。
     * 互換モードは従来どおり 128 バイトまで (1 バイトずつしか来ないので
     * 実際には 1〜2 周で抜ける)。 */
    int loop_max = (s_setup.mode == SER_MODE_VFAST)
                 ? SER_FIFO_DEPTH : SER_IRQ_DRAIN_MAX;

    for (;;) {
        /* **ポートもビットも s_* 経由。** 互換 0032h は bit1 が RxRDY、
         * FIFO 0132h は bit2 が RxRDY で、0x04 は互換では TxEMP にあたる。
         * 直に書くと「送信が空くたびに受信データを読む」形で静かに壊れる。 */
        sts = (u8)inp(s_port_cmd);

        /* エラーがあればリセット */
        if (sts & s_mask_err) {
            outp(s_port_cmd, CMD_TXE | CMD_DTR | CMD_RXE | CMD_RTS | CMD_ER);
        }

        if (!(sts & s_mask_rxrdy)) break;

        data = (u8)inp(s_port_data);

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

        /* 異常な割り込み嵐を防ぐため、上限まで読んだら一旦抜ける
         * (残っていれば次の割り込みで続きを汲む) */
        loop_count++;
        if (loop_count >= loop_max) break;
    }

    /* FIFO モードでは 0136h (割り込み参照) を読んで要因を落とす。
     * **資料は「取得を行う」としか書いていない**が、NP21/W の rs232c_i136 は
     * 読んだときに irqflag を畳んで pic_resetirq(4) を呼ぶ。読まないと同じ
     * 要因で割り込みが上がり続ける。互換モードにこのレジスタは無い。 */
    if (s_setup.mode == SER_MODE_VFAST) {
        (void)inp(SER_FIFO_IIR);
    }

    /* 再許可は **初期化が決めた値** を書く。直値 IEN_RX だと、将来モードごとに
     * マスクを変えたときに ISR が 1 回目の受信でそれを踏み潰す。 */
    outp(SER_MASK, 0x00);
    outp(SER_MASK, s_mask_ien);
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

/* ======================================================================== */
/*  serial_putchar — ポーリング送信 (予算つき)                              */
/*                                                                          */
/*  **直す前は 100 回スピンしてから `hlt` で次の 10ms tick まで寝ていた。**  */
/*  1 文字ごとに最悪 10ms 寝るので、9600 で 490B/s、38400 では 233B/s と     */
/*  回線より遅くなっていた (票 TASK_SERIAL_VFAST §0)。                      */
/*                                                                          */
/*  回線が 1 文字を押し出す時間は baud で決まるのだから、その時間だけ見て    */
/*  から寝ればよい。予算 = 1 文字時間 × 2 (serial_plan.h)。予算のあいだは    */
/*  `cpu_delay_us(SER_TX_POLL_US)` を挟んで TxRDY を見る。                  */
/*                                                                          */
/*  予算を超えたら従来どおり `hlt` で 1 割り込み分待つ (最大                 */
/*  SER_TX_HLT_RETRY 回) — 相手がフロー制御で止めているときに CPU を         */
/*  焼かないため。**ここは IF=1 でしか呼べない** (割り込み禁止区間からは     */
/*  serial_puts_polled を使う)。                                            */
/*                                                                          */
/*  cpu_calibrate() の前は cpu_delay_us が即座に返る (s_loops_per_tick=0)。  */
/*  そのときは予算 ÷ 刻み回だけ素のスピンになる — 従来の 100 回スピンと      */
/*  同程度で、害は無い。                                                    */
/* ======================================================================== */
void serial_putchar(char c)
{
    u32 waited;
    int retry;

    for (retry = 0; retry < SER_TX_HLT_RETRY; retry++) {
        waited = 0;
        for (;;) {
            if (inp(s_port_cmd) & s_mask_txrdy) {
                outp(s_port_data, (unsigned)(u8)c);
                return;
            }
            if (waited >= s_tx_budget_us) break;
            cpu_delay_us((u32)SER_TX_POLL_US);
            waited += (u32)SER_TX_POLL_US;
        }
        /* 予算を使い切った = 相手が読んでいない。次の割り込みまで寝る。 */
        _halt();
    }
    /* タイムアウト: 送信を諦める */
}

/* 文字列送信。
 * 16 バイトごとの `io_wait` は **NP21/W のパイプバッファ対策**として
 * 入ったもの (ホスト側の commng がデータを掃けるまでの間合い)。実機では
 * 要らないが、1 バイトあたり 0.6µs 程度なので 115200 (1 文字 87µs) でも
 * 影響は 1% 未満。残しておく。 */
void serial_puts(const char *str)
{
    int count = 0;
    while (*str) {
        serial_putchar(*str);
        str++;
        count++;
        if ((count & (SER_PUTS_WAIT_EVERY - 1)) == 0) {
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
    for (spin = 0; spin < SER_POLLED_SPIN_MAX; spin++) {
        /* 予算も cpu_delay_us も使わない (校正が壊れている可能性がある
         * 状況で呼ばれる)。ポートとビットだけモードに合わせる。 */
        if (inp(s_port_cmd) & s_mask_txrdy) {
            outp(s_port_data, (unsigned)(u8)c);
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
