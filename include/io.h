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

#endif /* IO_H */
