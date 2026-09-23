/* ========================================================================
 *  tools/tests/pcm_hostshim/io.h — ホスト試験用の io.h 差し替え
 *
 *  include/io.h は inp / outp / irq_save を**特権命令のインライン asm**で
 *  定義するので、ホストでそのまま走らせると落ちる。この版を -I で先に置いて
 *  差し替え、ポート操作を試験側の模型へ回す
 *  (tools/tests/hostdrv_hostshim/io.h と同じ作法)。
 *
 *  **アクセスを数える**のがここの主目的: 票 E1 の「入口ガードでは装置に
 *  1 度も触らない (Index 書きも 0 回)」は、数えないと確かめられない。
 *
 *  [C1] C89 / GNU89。
 * ======================================================================== */

#ifndef IO_H
#define IO_H

/* 試験側 (tools/tests/pcm_cs4231_host.c) が定義する */
unsigned int pcm_shim_inp(unsigned int port);
void pcm_shim_outp(unsigned int port, unsigned int value);

static inline unsigned int inp(unsigned int port) { return pcm_shim_inp(port); }
static inline void outp(unsigned int port, unsigned int value)
{ pcm_shim_outp(port, value); }

static inline unsigned int inpw(unsigned int port) { return pcm_shim_inp(port); }
static inline void outpw(unsigned int port, unsigned int value)
{ pcm_shim_outp(port, value); }

static inline void io_wait(void) { }
static inline void io_wait_n(int n) { (void)n; }

/* 割り込み禁止は**数えるだけ**。入れ子の釣り合いを試験が確かめる。 */
extern int pcm_shim_irq_depth;
static inline unsigned int irq_save(void) { pcm_shim_irq_depth++; return 0x200u; }
static inline void irq_restore(unsigned int flags)
{ (void)flags; pcm_shim_irq_depth--; }
static inline void _enable(void) { }
static inline void _disable(void) { }
static inline int _irq_enabled(void) { return pcm_shim_irq_depth == 0; }
static inline void _halt(void) { }
static inline void _idle(void) { }
static inline void _stop(void) { }

#endif /* IO_H */
