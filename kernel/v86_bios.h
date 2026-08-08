/* ======================================================================== */
/*  V86_BIOS.H — ゲスト BIOS の HLE (High Level Emulation)                  */
/*                                                                          */
/*  V86 の `INT n` は IOPL に関係なくプロテクトモードの IDT を引く。         */
/*  OS32 の IDT ゲートは DPL=0、ゲストは CPL=3 なので                        */
/*  「softint かつ gate.DPL < CPL」の規則で必ず #GP になる。                 */
/*  つまりトランポリンを仕込まなくても BIOS コールは横取りできる。          */
/*                                                                          */
/*      ゲスト: INT 1Bh                                                     */
/*        → #GP (IDT ゲートの DPL チェック)                                  */
/*        → カーネルが CD ib をデコードしてベクタを得る                      */
/*        → HLE 対象なら レジスタと CF をフレームに直接書いて IP を進める     */
/*        → 対象外ならゲスト自身の IVT (多くは BIOS ROM) へ流す              */
/*                                                                          */
/*  INT は配送前に落ちているのでゲストスタックには何も積まれていない。      */
/*  したがって戻り値はスタックではなくフレームの EFLAGS に書く。            */
/* ======================================================================== */

#ifndef __V86_BIOS_H
#define __V86_BIOS_H

#include "types.h"

/* ゲストに申告するコンベンショナルメモリ量。
 * バッキング RAM のリマップ範囲 (0x00000-0x8EFFF) に合わせる。 */
#define V86_GUEST_MEM_KB    572

/* ======== API ======== */

/* ゲストの IVT / BDA を用意し、HLE 対象ベクタにスタブを仕込む。
 * v86_mem_setup() がバッキング RAM を張った後に呼ぶこと。 */
void v86_bios_setup(void);

/* リマップ前の実機 IVT / BDA を退避する。v86_mem_setup() の先頭で呼ぶ。 */
void v86_bios_save_real(void);

/* このベクタを HLE するか (しないならゲストの IVT へ流す) */
int  v86_bios_is_hle(u32 vector);

/* BIOS コールを処理する。frame は #GP フレーム。 */
void v86_bios_dispatch(u32 *frame, u32 vector);

/* 処理した BIOS コールの回数と直近のベクタ (検証用) */
u32  v86_bios_call_count(void);
u32  v86_bios_last_vector(void);

#endif /* __V86_BIOS_H */
