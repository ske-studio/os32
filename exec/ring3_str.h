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

/* ======================================================================== */
/*  CPL=3 へ**書き込む**前の判定 (票 TASK_HAL_WIRING、Codex 往復 10)         */
/*                                                                          */
/*  OS32 は **CR0.WP = 0** で走る (arch/x86/arch_cpu.h。kernel/shlib.c が     */
/*  「カーネルからは読み取り専用ページにも書ける」ことに依存している)。      */
/*  そのため CPL=0 の KAPI ラッパは、アプリが渡した読み取り専用 USER ページ   */
/*  — 具体的には**共有ライブラリの .text/.rodata**、全アプリで同じ物理 —     */
/*  にも #PF を起こさずに書けてしまう。既存の早期検査 (`kapi_argptr` →       */
/*  `ring3_ptr_ok`) は**帯しか見ない**ので、ここは素通りする。               */
/*  (`exec/exec.c` の `ring3_ptr_ok` にあった「.text への書き込みは PTE が    */
/*   RO なのでハードウェアの #PF で捕まる」という注記は CR0.WP = 0 では誤り) */
/*                                                                          */
/*  判定の**純粋な部分**だけをここに置く。実際にどの番地がどう張られている    */
/*  かを引くのは exec/exec.c の `ring3_user_range_writable`。                */
/* ======================================================================== */

/* PTE の下位 12bit から「CPL=3 が書けるページか」を決める表。
 * present + RW + USER の**3 つとも**立っていなければ書いてはいけない。
 * (i386 の実効権限は PDE と PTE の論理積だが、呼び手が両方を AND してから
 *  渡す約束にしてここは 1 語だけ見る。) */
int ring3_pte_writable_ok(u32 pte_flags);

/* PDE (下位 12bit で足りる) から「この PDE の下を CPL=3 が書けるか」。
 *   - present + RW + USER が**3 つとも**要る。i386 の実効権限は PDE と PTE の
 *     論理積なので、PTE が RW + USER でも PDE が supervisor / RO なら
 *     アプリは書けない (Approve 後の注意 4)。
 *   - **PS (4MB ページ) は拒否** — その PDE の下に PT は無い。いまの OS32 に
 *     4MB PDE を作る経路は 1 つも無いので、見えたら表の読み違い (アプリ CR3 の
 *     まま歩いた等) を疑うべき状態。 */
int ring3_pde_walkable_ok(u32 pde_flags);

/* [p, p + len) と [base, end) が重なるか。len = 0 は重ならない。
 * **加算の桁あふれでも素通しにしない** (p + len が巻き戻ると、帯の外を
 * 指すポインタが「帯に重ならない」と答えてしまう)。 */
int ring3_range_overlaps(u32 p, u32 len, u32 base, u32 end);

#endif /* __RING3_STR_H */
