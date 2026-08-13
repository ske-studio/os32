/* ======================================================================== */
/*  IO.H — ベアメタルI/Oインライン定義                                      */
/*                                                                          */
/*  GCC __asm__ volatile によるI/Oポート操作・割り込み制御・                 */
/*  特権命令のインライン定義を一元管理する。                                 */
/* ======================================================================== */

#ifndef IO_H
#define IO_H

/* ---- I/Oポート操作 (8-bit) ---- */
static inline unsigned int inp(unsigned int port) {
    unsigned char ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outp(unsigned int port, unsigned int value) {
    __asm__ volatile("outb %b0, %w1" : : "a"((unsigned char)value), "Nd"(port));
}

/* ---- I/Oポート操作 (16-bit) ---- */
static inline unsigned int inpw(unsigned int port) {
    unsigned short ret;
    __asm__ volatile("inw %w1, %w0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outpw(unsigned int port, unsigned int value) {
    __asm__ volatile("outw %w0, %w1" : : "a"((unsigned short)value), "Nd"(port));
}

/* ---- I/Oポート操作 (REP INSW: バッファ読み込み) ---- */
static inline void insw_rep(unsigned int port, void *buf, unsigned int count) {
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(count)
                     : "d"(port)
                     : "memory");
}

/* ---- 割り込み制御 ---- */
static inline void _enable(void) {
    __asm__ volatile("sti" : : : "memory");
}

static inline void _disable(void) {
    __asm__ volatile("cli" : : : "memory");
}

/* ---- 割り込み状態の保存/復元 ---- */
/* 盲目的な cli/sti ペアの代替。呼び出し時点の EFLAGS を保存して CLI し、  */
/* irq_restore() で IF を元の状態 (有効/無効) に戻す。割り込み禁止区間が   */
/* ネストしても安全 (内側の restore が外側の禁止状態を壊さない)。          */
static inline unsigned int irq_save(void) {
    unsigned int flags;
    __asm__ volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(unsigned int flags) {
    __asm__ volatile("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

/* ---- 特権命令 ---- */
static inline void _lidt(void *ptr) {
    __asm__ volatile("lidt (%0)" : : "r"(ptr) : "memory");
}

/* ---- HLT命令 ---- */
static inline void _halt(void) {
    __asm__ volatile("hlt" : : : "memory");
}

/* ---- PC-98 I/Oウェイト (ポート0x5Fダミーアクセス, 約0.6µs) ---- */
/* PC9800Bible §4-4 準拠。各ドライバはこの関数を使用すること。       */
static inline void io_wait(void) {
    outp(0x5F, 0);
}

/* n 回ウェイト (約 0.6µs × n)。手書きの io_wait() 連打はこれを使う。 */
static inline void io_wait_n(int n) {
    while (n-- > 0) io_wait();
}

/* ---- リングバッファの取り出し (割り込み保護付き) ----
 * ISR がエンキューするリングバッファから 1 要素取り出す共通イディオム。
 * 要素型が u16/int などバッファごとに違うためマクロで提供する。
 * 呼び出し前に count > 0 を確認しておくこと。 */
#define RING_DEQUEUE(entry, buf, head, count, bufsize)      \
    do {                                                    \
        unsigned int ring_flags_ = irq_save();              \
        (entry) = (buf)[(head)];                            \
        (head) = ((head) + 1) % (bufsize);                  \
        (count)--;                                          \
        irq_restore(ring_flags_);                           \
    } while (0)

#endif /* IO_H */
