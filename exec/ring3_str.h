/* ======================================================================== */
/*  RING3_STR.H — KAPI が CPL=3 へ **返す** 文字列の置き場                   */
/*                                                                          */
/*  票: docs/tasks/gui/v13/TASK_T9_sh.md §12 R1 (Codex 網羅レビュー 往復 7)  */
/*                                                                          */
/*  KAPI の引数として来るポインタはディスパッチャが範囲検証する              */
/*  (`ring3_ptr_ok`) が、**戻り値のポインタ** は誰も見ていなかった。         */
/*  `sys_getcwd` は `fs/vfs.c` の static `cwd` (カーネル帯 = PDE0 の PTE) を */
/*  そのまま返す。`kernel/paging.c` はカーネル帯を `phys | PAGE_RW`          */
/*  (USER なし) で張るので、CPL=3 の呼び手がその番地を読むと #PF → fault     */
/*  kill になる。RO+USER になっているのは **KAPI トランポリンページの 1 枚**  */
/*  だけ (`exec/exec.c` の `ring3_trampoline_init`)。                        */
/*                                                                          */
/*  直し方は「そのページの空きへ写して、写したポインタを返す」。ABI も       */
/*  KAPI の本数も変わらない (`sdk/kapi.json` の `sys_getcwd` の target を    */
/*  `vfs_cwd` → `vfs_cwd_user` に差し替えるだけ)。                           */
/*                                                                          */
/*  ページの中身 (`ring3_trampoline_init` が組む順):                         */
/*                                                                          */
/*    +0                      : ユーザ可視の KernelAPI 表 (magic/version/    */
/*                              関数ポインタ KAPI_FUNC_COUNT 本/データ 2 個) */
/*    +RING3_USTR_STUB_OFF    : int 0x80 スタブ 8 B × KAPI_FUNC_COUNT       */
/*    +RING3_USTR_OFF         : ここ (写し場 RING3_USTR_CAP バイト)          */
/*                                                                          */
/*  写しは **呼ばれるたびに上書き**する。呼び手は次の KAPI 呼び出しより前に   */
/*  読み切ること (仕様。docs/KAPI_SPEC.md の sys_getcwd の行に注記)。        */
/* ======================================================================== */

#ifndef __RING3_STR_H
#define __RING3_STR_H

#include "types.h"
#include "os32_kapi_shared.h"   /* KernelAPI / KAPI_FUNC_COUNT / OS32_MAX_PATH */

/* 表の直後 (4B 整列) からスタブ。`ring3_trampoline_init` の stub_base と
 * **同じ式**でなければならない。 */
#define RING3_USTR_STUB_OFF  ((u32)((sizeof(KernelAPI) + 3u) & ~3u))
/* スタブの終わり = 写し場の先頭 (4B 整列)。 */
#define RING3_USTR_OFF       ((u32)((RING3_USTR_STUB_OFF + \
                                     (u32)KAPI_FUNC_COUNT * 8u + 3u) & ~3u))
/* 写し場の大きさ。cwd が入れば足りる (いま返すのは sys_getcwd だけ)。 */
#define RING3_USTR_CAP       ((u32)OS32_MAX_PATH)

/* カーネル帯の文字列を CPL=3 の呼び手へ返してよい形に直す。
 *   in_syscall : `ring3_in_syscall` (1 = int 0x80 ディスパッチ中 = CPL=3 由来)
 *   scratch    : 写し先 (トランポリンページ内)。0 なら写さない
 *   cap        : 写し先のバイト数 (NUL 込み)
 *   src        : カーネル帯の文字列 (0 可)
 * 戻り値: CPL=3 由来なら scratch、そうでなければ src (CPL=0 の常駐シェルは
 * カーネル帯をそのまま読めるので、写す意味も余地もない)。
 * **純関数に近い** (書くのは scratch だけ) のでホストでそのまま試験できる
 * — tools/tests/ring3_str_host.c。 */
const char *ring3_user_str(int in_syscall, char *scratch, u32 cap,
                           const char *src);

#endif /* __RING3_STR_H */
