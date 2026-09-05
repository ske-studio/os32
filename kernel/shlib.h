/* ======================================================================== */
/*  SHLIB.H — 固定アドレス常駐の共有ライブラリ (GUI v1.1 K3)                 */
/*                                                                          */
/*  MEM_SHLIB_BASE (0x400000) に位置依存のライブラリを 1 つだけ常駐させる。   */
/*  ロードアドレスは 1 つ。再配置はしない (票 K3 の鉄則)。                    */
/*                                                                          */
/*  形式は OS32X (OS32X_FLAG_SHLIB) で、イメージの先頭ページが               */
/*  OS32ShlibHeader (sdk/include/os32/os32_kapi_shared.h、PM が凍結) の      */
/*  ジャンプ表になっている。K3 は版を照合せず、読んだ版をログに出すだけ:      */
/*  GUI_PROTO_VERSION との突き合わせはアプリ側スタブ (C3) の仕事。            */
/*                                                                          */
/*  メモリの張り方:                                                          */
/*    .text/.rodata  read-only + USER を master PD に張る。アプリ PD は       */
/*                   master の PDE/PT を写して作られるので全 PD で共有される。 */
/*    .data/.bss     同じ仮想番地に **アプリごとの物理ページ**。ring3 の      */
/*                   アドレス空間生成時 (shlib_addrspace_attach) に原本から   */
/*                   複製し、破棄時 (shlib_addrspace_detach) に解放する。     */
/*                                                                          */
/*  ライブラリが無ければ静かに「未ロード」。GUI 以外は従来どおり動く。        */
/* ======================================================================== */

#ifndef __SHLIB_H
#define __SHLIB_H

#include "types.h"
#include "paging.h"

/* 起動時に 1 回だけ呼ぶ (VFS 初期化後・シェル起動前、pgalloc_init 済み)。
 * 戻り値: 0=ロード成功, -1=未ロード (ファイルが無い / 形式不正)。
 * -1 でも起動は続行してよい (GUI を使わないプログラムには影響しない)。 */
int shlib_init(void);

/* ライブラリが常駐しているか (1=常駐)。 */
int shlib_loaded(void);

/* 常駐しているライブラリの版 (OS32ShlibHeader.version)。未ロードなら 0。 */
u32 shlib_version(void);

/* .text/.rodata の終端 (exclusive)。未ロードなら MEM_SHLIB_BASE。 */
u32 shlib_text_end(void);

/* ring3 アドレス空間にライブラリを張る (paging_addrspace_create の直後)。
 *   - .text/.rodata を read-only + USER で
 *   - .data/.bss は原本から複製した専用の物理ページを同じ仮想番地に
 * 未ロードなら何もせず 0。物理ページが取れなければ警告して -1
 * (アプリは起動するが、ライブラリを使うと #PF で kill される)。 */
int shlib_addrspace_attach(struct addrspace *as);

/* attach で複製した .data/.bss ページを解放する。
 * paging_addrspace_destroy の **前** に呼ぶこと。未 attach なら何もしない。 */
void shlib_addrspace_detach(struct addrspace *as);

#endif /* __SHLIB_H */
