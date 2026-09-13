/* ======================================================================== */
/*  EXEC.H — 簡易プログラムローダー                                         */
/*                                                                          */
/*  ext2上のフラットバイナリを拡張メモリにロードして実行する。               */
/*  外部プログラムはKernelAPI構造体を通じてカーネル関数を呼び出す。          */
/*                                                                          */
/*  呼び出し規約:                                                           */
/*    カーネル: System V i386 ABI                                            */
/*    外部プログラム: System V i386 ABI                                      */
/*    KernelAPIの関数ポインタ: __cdecl ラッパー経由                          */
/*    ExecEntry (外部プログラムのmain): __cdecl                             */
/*                                                                          */
/*  メモリ配置:                                                             */
/*    KAPI_ADDR : KernelAPI テーブル (関数ポインタ群)                        */
/*    0x400000  : プログラムロード領域 (最大1MB)                             */
/* ======================================================================== */

#ifndef __EXEC_H
#define __EXEC_H

/* KernelAPI構造体・OS32Header・基本型は共有ヘッダから取得 */
#include "os32_kapi_shared.h"
#include "memmap.h"

/* ネスト実行の上限は exec/appslot.h の ID の池 (APP_ID_MIN..APP_ID_MAX)。
 * 同時に生きられる非シェル ID は APP_MAX_APPS = 4 で、GUI アプリと CUI の
 * 入れ子 exec_run が **1 つの池を共有する** — 決裁 D9-2。
 * 旧 MAX_EXEC_NEST は K5b で参照が消えたため削除した (2026-09-11)。 */

/* プログラムのロード先 (固定) */
#define EXEC_LOAD_ADDR    MEM_EXEC_LOAD_ADDR
/* ======== API ======== */
void exec_init(void);

/* 従来の起動。子が終わるまで呼び出し元を塞ぐ (CUI の入れ子はこれ)。 */
int exec_run(const char *cmdline);

/* ---- アプリ 4 本の同時実行 (KAPI v44、票 docs/tasks/gui/v13) ----
 * 詳細は TASK_K5_multiapp.md の D4 / D8。呼べるのは owner 1 (シェル帯) だけ。 */

/* 塞がない起動。>0 = app_id (最初の OP_WAIT で park した) / 0 = park より前に
 * 終了した / <0 = 起動しなかった (INVAL / FULL / NOMEM / NOT_FOUND / INVALID)。*/
i32 exec_start(const char *cmdline);

/* park してあるアプリを 1 本だけ起こす。wait_ret は OP_WAIT の戻り値。
 * app_id = また park した / 0 = 終了した / <0 = 起こせなかった。
 * 起こせるのは印のあるフレームだけ (OS32_ERR_STALE): OP_WAIT 由来なら
 * parked_from_wait、kbd 待ち (WAIT_KEY) 由来なら parked_from_kbd、
 * ポーリングの譲り (WAIT_POLL) 由来なら parked_from_poll。
 * kbd 待ちの側は wait_ret を**使わず**、注入リングの 1 バイトを EAX に
 * 入れる。リングが空なら起こさず OS32_ERR_AGAIN (票 K7 §5 の指摘 B)。
 * ポーリングの側も wait_ret を使わないが、空でも **EAX = -1 で起こす**
 * (1 周だけの譲りなので、次の周に必ず戻す。票 T8 §7 D8)。 */
i32 exec_resume(i32 app_id, i32 wait_ret);

/* 走っているアプリを OP_WAIT の中で止め、WM へ戻す。成立すれば **戻らない**。
 * 呼べない文脈では OS32_ERR_INVAL を返して普通に戻る。 */
i32 exec_park(void);

/* 第 2 の park 点 (票 K7 D1): GUI 中に kbd が空のとき、走っている CPL=3 の
 * アプリを WAIT_KEY で止めて WM へ戻す。drivers/kbd.c から呼ぶ。
 * 成立すれば **戻らない**。0 = 止められなかった (呼び手は hlt 待ちへ)。 */
int exec_park_kbd(void);

/* 第 3 の park 点 (票 T8 §7 D8): GUI 中に注入リングが空で、前回の譲りから
 * PIT tick が進んでいるとき、走っている CPL=3 アプリを WAIT_POLL で止めて
 * **1 周だけ** WM へ譲る。drivers/kbd.c の kbd_trygetchar / kbd_trygetkey
 * から、now_tick に tick_count を渡して呼ぶ。成立すれば **戻らない**。
 * 0 = 譲れなかった (呼び手はそのまま -1 を返す)。 */
int exec_park_poll(u32 now_tick);

/* 第 4 の park 点 (票 T9 D5、KAPI v49 sys_yield): 明示的な譲り。GUI 中は
 * tick の間引き無しで **必ず** WAIT_POLL へ park し、印 parked_from_yield を
 * 立てる (起こすとき注入リングを読まず EAX = 0)。成立すれば **戻らない**。
 * park できない文脈 (CUI / CPL=0 / syscall の外 / 入れ子の子) では `hlt` を
 * 1 回して 0 を返す。 */
i32 exec_sys_yield(void);

/* 止めてあるアプリを起こさずに畳む。0 / OS32_ERR_INVAL / OS32_ERR_STALE。
 * 票 T9 D8: **id とその子孫** (起動要求表の child を末尾まで辿ったもの) を
 * **末尾から** 畳む。CTRL+STOP のように 1 本だけ止めたいときは、WM が
 * launch_child() で末尾を解決してその ID を渡す。 */
i32 exec_kill(i32 app_id);

/* 0 = 空き / 1 = 走っている / 2 = park 中 (OP_WAIT) / 3 = kbd 待ち /
 * 4 = ポーリングの譲り / OS32_ERR_INVAL。3 は K7 の、4 は T8 D8 の追加で、
 * 既存の 0〜3 の意味は動かない。 */
i32 exec_app_state(i32 app_id);

/* CTRL+STOP (IRQ1 が走っているアプリに立てた要求) を降ろす (KAPI v45、A1)。
 * 0 = 降ろした / 要求が無かった、OS32_ERR_INVAL = owner 1 以外。 */
i32 exec_abort_clear(void);

/* KAPI sys_getcwd の実体 (票 T9 §12 R1)。CPL=3 の呼び手には
 * トランポリンページ内の写しを、CPL=0 の呼び手には fs/vfs.c の static cwd を
 * 返す。カーネル帯には USER ビットが無いので、写さずに返すと CPL=3 側が
 * 読んだ瞬間に #PF → fault kill になる。sdk/kapi.json の target をこれに
 * 差し替えてあるだけで、スロット・引数・戻り型は不変 ([ABI2])。 */
const char *vfs_cwd_user(void);

/* 上の写し場の番地とページ属性をブート時に踏む (票 T9 §12 R1)。
 * kselftest_run() は exec_init() より前に走るので、この項だけ
 * kselftest_run_post_exec() から呼ぶ。0 = 全部通った。 */
u32 exec_tramp_user_selftest(void);

/* 現在のネスト深度 (0=外部プログラム未実行) */
extern volatile int exec_nest_level;

#endif /* __EXEC_H */
