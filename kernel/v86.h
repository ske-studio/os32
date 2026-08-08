/* ======================================================================== */
/*  V86.H — 仮想8086モード ランタイム                                       */
/*                                                                          */
/*  16bit ゲスト (PC-98 ネイティブゲーム / DOS) を OS32 上で走らせるための   */
/*  最小ランタイム。方式と根拠は docs/tasks/v86v2/00_approach_study.md。     */
/*                                                                          */
/*  カーネルに置くのは以下だけ:                                             */
/*    特権命令の発行 / #GP・#DB・#PF の一次受け / IRQ ISR 本体 /            */
/*    I/O トラップの一次デコード / ページテーブル直書き / V86 モード遷移     */
/*  HLE・セッション管理・整形はユーザ空間側に寄せる。                       */
/* ======================================================================== */

#ifndef __V86_H
#define __V86_H

#include "types.h"

/* EFLAGS */
#define EFLAGS_IF       0x000200UL
#define EFLAGS_IOPL3    0x003000UL      /* IOPL=3 */
#define EFLAGS_VM       0x020000UL      /* 仮想8086モード */

/* ゲストに渡す初期 EFLAGS。
 *
 * IOPL=3 にしているのが要点。386 には VME が無いため IOPL<3 では
 * INT n / CLI / STI / PUSHF / POPF / IRET が全て #GP に落ちる。
 * IOPL=3 ならこれらは素通りし、I/O だけが TSS の I/O 許可ビットマップで
 * 個別に制御される。実測ではこれで #GP レートが 5〜6 倍下がる
 * (docs/tasks/v86v2/00_approach_study.md §3.1)。
 *
 * 代償はゲストの CLI が実 IF を落とすこと。暴走した場合の脱出手段が
 * 別途必要になる。
 *
 * IF は立てない状態で渡す。ゲストが自分で STI するまで割り込みは来ない。
 * これは実機の起動直後と同じ挙動で、ゲストが IVT を整える前に割り込みが
 * 飛び込むのを防ぐ。IRQ スタブ側は Phase 1 で V86 安全化済み。 */
#define V86_EFLAGS_INIT (EFLAGS_VM | EFLAGS_IOPL3 | 0x2)

/* V86 突入時のコンテキスト。v86_entry.asm が iretd で積む順序と
 * 1:1 で対応するので、フィールドの並べ替え禁止。 */
struct v86_context {
    u32 eip;
    u32 cs;
    u32 eflags;
    u32 esp;
    u32 ss;
    u32 es;
    u32 ds;
    u32 fs;
    u32 gs;
};

/* ------------------------------------------------------------------------ */
/*  #GP スタックフレームのレイアウト                                        */
/*                                                                          */
/*  V86 から #GP が入ると、CPU は TSS の SS0:ESP0 に切り替えてから          */
/*  GS/FS/DS/ES/SS/ESP/EFLAGS/CS/EIP と error_code を積む。                 */
/*  スタブがさらに PUSHAD するので、ハンドラが受け取る u32 配列は:          */
/*                                                                          */
/*    [0..7]  PUSHAD (低位から EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX)             */
/*    [8]     error_code                                                    */
/*    [9..11] EIP, CS, EFLAGS                                               */
/*    [12,13] ゲスト ESP, SS                                                */
/*    [14..17] ES, DS, FS, GS                                               */
/* ------------------------------------------------------------------------ */
#define V86F_EDI        0
#define V86F_ESI        1
#define V86F_EBP        2
#define V86F_ESP_DUMMY  3
#define V86F_EBX        4
#define V86F_EDX        5
#define V86F_ECX        6
#define V86F_EAX        7
#define V86F_ERRCODE    8
#define V86F_EIP        9
#define V86F_CS         10
#define V86F_EFLAGS     11
#define V86F_ESP        12
#define V86F_SS         13
#define V86F_ES         14
#define V86F_DS         15
#define V86F_FS         16
#define V86F_GS         17

/* ------------------------------------------------------------------------ */
/*  IRQ (割り込み) フレームのレイアウト                                     */
/*                                                                          */
/*  ハードウェア割り込みはエラーコードを積まないので、#GP フレームより      */
/*  全体が 1 ワードぶん手前にずれる。ここを取り違えると EFLAGS のつもりで    */
/*  CS を読むことになり、症状が出るのは注入した後になるので厄介。           */
/* ------------------------------------------------------------------------ */
#define V86I_EIP        8
#define V86I_CS         9
#define V86I_EFLAGS     10
#define V86I_ESP        11
#define V86I_SS         12
#define V86I_ES         13
#define V86I_DS         14
#define V86I_FS         15
#define V86I_GS         16

/* PC-98 のサウンドボード (PC-9801-26K/86) は IRQ12。
 * ゲストから見ると INT 14h になる (IRQ8-15 → INT 10h-17h)。 */
#define V86_IRQ_SOUND       12
#define V86_INT_SOUND       0x14

/* セッション終了理由 */
enum v86_exit_reason {
    V86_EXIT_NONE = 0,
    V86_EXIT_HLT,           /* ゲストが HLT を実行 */
    V86_EXIT_TRAP_PORT,     /* 脱出用ポートへの OUT */
    V86_EXIT_UNKNOWN_OP,    /* 未対応命令 */
    V86_EXIT_TIMEOUT,
    V86_EXIT_HOTKEY,
    V86_EXIT_FAULT          /* #PF など */
};

/* ======== API ======== */

/* V86 モードへ遷移する (v86_entry.asm)。
 * 呼び出し前に TSS.ESP0 を現在のカーネルスタックに合わせること。
 * 正常には戻らず、セッション終了は longjmp 経由になる。 */
void v86_enter(const struct v86_context *ctx);

/* #GP の V86 経路から呼ばれる一次ハンドラ (kernel/v86.c)。
 * 0 を返すと V86 へ復帰、非 0 でセッション終了。 */
int v86_gp_handler(u32 *frame);

/* #GP 以外の例外を V86 中に食らったときの記録。isr_stub.asm から呼ぶ。
 * 記録したらセッションは畳む (呼び出し側が v86_exit_to_kernel する)。 */
void v86_fault_handler(u32 *frame, u32 vector);

/* セッションが終了した理由 */
enum v86_exit_reason v86_get_exit_reason(void);

/* デバッグ用: 直近の #GP のゲスト CS:IP と先頭オペコード */
u32 v86_last_gp_cs(void);
u32 v86_last_gp_ip(void);
u32 v86_gp_count(void);
u32 v86_last_gp_eflags(void);

/* 1 セッション実行して終了理由を返す */
int v86_run(const struct v86_context *ctx);

/* V86 セッションが実行中か (IRQ スタブから参照する) */
int v86_is_active(void);

/* IRQ スタブから呼ばれる割り込み反射。frame は PUSHAD 後のフレーム先頭。
 * ゲストの IVT を引いてスタックに FLAGS/CS/IP を積み、ISR へ飛ばす。 */
/* 実 IRQ をゲストに反射する。第 2 引数は **IRQ 番号** (ベクタではない)。
 * ベクタとマスクの判定は仮想 PIC (v86_pic.h) が行う。 */
void v86_reflect_irq(u32 *frame, u32 irq);

/* #GP フレーム版の割り込み注入。ゲストの INT n のうち HLE しないものを
 * ゲスト自身の IVT (多くは BIOS ROM) へ流すために使う。 */
void v86_inject_int(u32 *frame, u32 vector);

/* 反射した割り込みの回数 (検証用) */
u32 v86_irq_reflect_count(void);

/* セッションのタイムアウト (100Hz タイマの tick 数)。
 * 暴走したゲストを無限に走らせないための保険。 */
#define V86_TICK_LIMIT      (30 * 100)      /* 30 秒 */

/* #GP の上限。
 *
 * タイマ由来のタイムアウトはゲストが STI していることが前提で、IOPL=3 では
 * ゲストが CLI したまま回り続けると一切効かない。実際 Ys の IPL を起動したら
 * 拒否ポートの読みが期待値を返さずポーリングループに入り、#GP を出し続けた
 * まま止まらなくなった。
 *
 * ゲストの割り込み状態に依存しない歯止めとして、セッション中の #GP 総数にも
 * 上限を設ける。実測値はゲームプレイ中で 560/s、最も重い場面でも 1,334/s
 * なので、100 万回は正常動作を妨げない一方、暴走は数秒で止まる。 */
/* 1 回の #GP が拒否ポートの読みだけなら数マイクロ秒だが、INT 1Bh のように
 * 実ディスク I/O を伴うと数ミリ秒かかる。100 万回にすると後者で何分も
 * 帰ってこなくなるので、実用的に「数十秒で必ず戻る」値にしておく。
 * 実測: IPL 段階の INT 1Bh は数十回。ゲーム本編のロードでも数千回程度。 */
#define V86_GP_LIMIT        50000UL

/* タイマ IRQ の反射経路から呼ぶ。上限超過なら非 0。 */
int v86_tick_and_check_timeout(void);

/* Phase 1 スモークテスト: 最小の 16bit コードを V86 で実行し、
 * カーネルが生存したまま戻れることを確認する。戻り値は v86_exit_reason。
 * 検証が済んだら削除する一時コード。 */
int v86_smoke_test(void);

/* ディスクイメージからゲストをブートする。IPL を 1FC0:0000 に読み、
 * そこへ制御を渡す。戻り値は v86_exit_reason。 */
int v86_boot(const char *path);

/* ディスクテスト: イメージを attach し、ゲストに INT 1Bh READ をさせて
 * 読めた中身を loop_dev の直読と突き合わせる。0=一致、負=失敗。 */
int v86_disk_test(const char *path);

/* スモークテストが使う低位メモリ (memmap.h の「空き 20KB」内) */
#define V86_TEST_CODE_ADDR   0x8A000UL
#define V86_TEST_STACK_ADDR  0x8B000UL
#define V86_TEST_MAGIC_ADDR  0x8C000UL
#define V86_TEST_MAGIC       0x1234U
#define V86_TEST_BUF_ADDR    0x8D000UL   /* ディスクテストの転送先 */
#define V86_DISK_SECLEN      256U        /* PC-98 2HD ブートトラック */

/* PC-98 の IPL は 1FC0:0000 に読まれ、そこから実行が始まる。
 * ブートスタックは BIOS が用意する低位メモリ (実測で SS:SP=0000:0286)。 */
#define V86_IPL_SEG          0x1FC0U
#define V86_IPL_ADDR         0x1FC00UL
#define V86_BOOT_SS          0x0000U
#define V86_BOOT_SP          0x0290U

#endif /* __V86_H */
