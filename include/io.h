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

/* ======================================================================== */
/*  割り込み制御と CPU 停止の原始命令                                        */
/*                                                                          */
/*  カーネル側の C ソース (kernel/ drivers/ exec/ fs/ kapi/ lib/ net/ gfx/)  */
/*  は hlt / cli / sti を直接 asm で書かない。ここの原始命令だけを使う。      */
/*  番人は tools/check_arch_asm.py (make check)。                            */
/*                                                                          */
/*  各関数の註は「何を保証するか」の契約であって x86 の説明ではない。         */
/*  別アーキテクチャへ移すときは、命令列ではなく **契約** を再現すること。    */
/* ======================================================================== */

/* 割り込みを許可する。
 * 契約: 戻った時点で割り込みは受理される状態。呼び出し前の状態は問わない
 *       (すでに許可されていても無害)。
 * 注意: 「許可して眠る」を意図するなら _idle() を使うこと。_enable() の
 *       直後に _halt() を並べてはいけない (_idle() の註を参照)。 */
static inline void _enable(void) {
    __asm__ volatile("sti" : : : "memory");
}

/* 割り込みを禁止する。
 * 契約: 戻った時点で割り込みは受理されない。**元の状態は覚えない**ので、
 *       禁止区間が入れ子になり得る場所では irq_save()/irq_restore() を使う。 */
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

/* ---- CPU 停止 ---- */

/* 次の割り込みまで CPU を止める。
 * 契約: 割り込みが **すでに許可されている** ことを呼び手が保証する。IF=0 の
 *       文脈で呼ぶと永久に起きない (drivers/serial.c の panic 経路が
 *       スピン待ちにしてあるのはこの理由)。
 *       割り込みを 1 つ処理して戻る、が期待する使い方。ただし NMI や
 *       他の外部事象でも戻り得るので、待ち条件は必ずループで再検査する。 */
static inline void _halt(void) {
    __asm__ volatile("hlt" : : : "memory");
}

/* 割り込みを許可して、次の割り込みまで CPU を止める。**不可分**。
 * 契約: 「許可する」と「眠る」のあいだに割り込みが入って取りこぼす窓が
 *       無いこと。割り込み禁止区間の中で条件を確かめ、成立していなければ
 *       そのまま眠る — という書き方はこの不可分性だけに依存できる。
 *
 * ★ _enable(); _halt(); に分けてはいけない ★
 *   x86 の sti は直後の 1 命令のあいだ割り込みを遅らせるので
 *   "sti; hlt" は分割不能だが、C の 2 文に分けると最適化や将来の挿入で
 *   両者のあいだが開き、そこへ来た割り込みを処理したあとに hlt へ入って
 *   「起こすはずだった割り込みを使い切ったまま眠る」ことが起こり得る。
 *   移植先でも、この 2 つは 1 つの原始命令として実装すること
 *   (ARMv7/v8 なら cpsie i / msr daifclr + wfi を同じ規則で並べる。
 *    条件判定 → wfi の窓を閉じるのは wfi 自身の wake-up event 保持)。
 *
 * "memory" clobber も契約の一部。眠るまえに (cli 区間の中で) 読んだ変数は
 * 割り込みハンドラが書き換え得るので、起きたあとに必ず再読させる。
 * net/link.c の LINK_IDLE() はこれに依存している。落としてはいけない。 */
static inline void _idle(void) {
    __asm__ volatile("sti\n\thlt" : : : "memory");
}

/* 割り込みを禁じて CPU を止める。「ここで終わり」を表す。
 * 契約: 通常の割り込みでは戻らない。ただし NMI / SMI / デバッグ例外では
 *       戻り得るので、**呼び手は必ず for (;;) で囲む**。
 *       panic / 到達不能点の行き止まりに使う (アイドル待ちではない)。 */
static inline void _stop(void) {
    __asm__ volatile("cli\n\thlt" : : : "memory");
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
