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

/* ディスク BIOS が使う loop_dev スロット。
 * 当面は「DA/UA 0x90 (2HD FDD#1) → slot 0」の固定対応で足りる。 */
#define V86_DISK_SLOT       0

/* INT 1Bh のコマンド (AH の下位ニブル)。
 * AH の上位ビットはリトライ有無などの修飾なので、判定は下位だけ見る。
 * 実測: Ys の IPL は AH=0xD6 で来る (= READ DATA + 修飾)。 */
#define V86_DISK_SEEK       0x00
#define V86_DISK_VERIFY     0x02
#define V86_DISK_INIT       0x03
#define V86_DISK_SENSE      0x04
#define V86_DISK_WRITE      0x05
#define V86_DISK_READ       0x06
#define V86_DISK_RECAL      0x07

/* ======== API ======== */

/* セッションで使うディスクイメージを結び付ける。
 * path が NULL ならディスク無しで動く。0=成功、負=失敗。 */
int  v86_bios_attach_disk(const char *path);
void v86_bios_detach_disk(void);

/* ディスクが結び付いているか (検証用) */
int  v86_bios_has_disk(void);

/* ゲストの IVT / BDA を用意し、HLE 対象ベクタにスタブを仕込む。
 * v86_mem_setup() がバッキング RAM を張った後に呼ぶこと。 */
void v86_bios_setup(void);

/* セッション終了時に実機の IVT/BDA を書き戻す */
void v86_bios_restore_real(void);

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
