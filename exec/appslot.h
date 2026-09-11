/* ======================================================================== */
/*  APPSLOT.H — アプリ ID の表と状態遷移 (K5b-K、票 TASK_K5_multiapp.md D0〜D4)*/
/*                                                                          */
/*  「同時に生きているアプリは 4 本まで、走るのは常に 1 本」を成り立たせる    */
/*  背骨。exec_ctx_stack の「ネスト段のスタック」を **ID で引く表** に置き    */
/*  換えたもの (I14) で、次の 3 つを 1 か所に集めてある:                     */
/*                                                                          */
/*    1. ID の池 (1 = シェル帯 / 2〜5 = アプリ)。空きは必ず小さい方から      */
/*       配るので、CUI の入れ子 exec_run では従来どおり **段 = ID** になる。 */
/*    2. 状態遷移 (FREE / RUNNING / PARKED) と、その遷移が許される文脈。      */
/*       park は `gui_call(OP_WAIT)` の中からだけ、resume は「OP_WAIT で     */
/*       park された印のあるフレーム」に対してだけ (D0 の C5/C6)。           */
/*    3. 受入 G7 のカウンタ (C1〜C3/C6)。KAPI にはしない — kernel.map の      */
/*       番地を emu_read_mem で読む (fault_kill_count と同じ形)。            */
/*                                                                          */
/*  ここにはハードウェア (CR3 / setjmp / pgalloc / ページテーブル) を         */
/*  1 つも置かない。実体は exec/exec.c が持ち、この表は「誰が居て、どの状態で、*/
/*  次に何をしてよいか」だけを決める。だからホストでそのまま試験できる       */
/*  (tools/tests/multiapp_impl_host.c が K5a の模型と同じ 84 検査を回す)。   */
/* ======================================================================== */

#ifndef __APPSLOT_H
#define __APPSLOT_H

#include "types.h"
#include "paging.h"        /* struct addrspace (値で持つ。関数は呼ばない) */
#include "ksetjmp.h"       /* KSETJMP_BUF_LEN */

/* ---- ID の池 (D3) ---------------------------------------------------- */
/* 1 = シェル帯 (CUI シェル / gshell)。kernel/gui.h の GUI_SHELL_OWNER と同値。
 * 一致は exec/appslot.c の STATIC_ASSERT が固定する。 */
#define APP_ID_SHELL     1
#define APP_MAX_APPS     4                       /* 同時に生きる非シェル ID */
#define APP_ID_MIN       2
#define APP_ID_MAX       (APP_ID_MIN + APP_MAX_APPS - 1)   /* 5 */
#define APP_SLOT_COUNT   (APP_ID_MAX + 1)        /* 添字 = ID。0 は使わない */

/* ---- 状態 ------------------------------------------------------------ */
#define APP_STATE_FREE     0
#define APP_STATE_RUNNING  1
#define APP_STATE_PARKED   2

/* int80_stub が積むフレームの語数 ([0..7]=pushad, [8]=EIP [9]=CS
 * [10]=EFLAGS [11]=userESP [12]=userSS)。ring3_entry.asm と同期。 */
#define APP_FRAME_WORDS    13
/* pushad の EAX スロット = 戻り値 (resume の wait_ret を書く先) */
#define APP_FRAME_EAX      7

typedef struct {
    int  state;               /* APP_STATE_* */
    int  parent;              /* 起動した側の ID (GUI アプリは常に 1) */
    int  depth;               /* 従来の exec ネスト段 (シェル = 1) */
    int  gui;                 /* 塞がない起動 (exec_start) で立った */
    int  cpl3;                /* CPL=3 で走っている (AS を持つ) */
    int  in_op_wait;          /* いま gui_call(OP_WAIT) の中に居る (C4) */
    int  abort_req;           /* CTRL+STOP 要求 (この ID 宛) */
    int  parked_from_wait;    /* park したフレームの「OP_WAIT 由来」の印 (C5) */

    u32  jmpbuf[KSETJMP_BUF_LEN];   /* この ID の呼び出し元へ帰る点 */
    u32  frame[APP_FRAME_WORDS];    /* park した CPL=3 フレーム (D2 の (b)) */

    /* 旧 ExecContext の中身 (レイアウトは 1 バイトも動かさない — I12/I13) */
    u32  guard_a;             /* sbrk ガード (= exec_heap の直下) */
    u32  guard_b;             /* スタックガード */
    u32  sbrk_heap_limit;
    u32  exec_heap_base;
    u32  exec_heap_size;
    u32  exec_heap_used;
    u32  load_addr;
    u32  stack_top;

    /* CPL=3 アプリ固有 (I10: 切替のたびに差し替える「現在のアプリ」の値) */
    u32  band_top;
    u32  band_pdes;
    struct addrspace as;
    u32  pages;               /* この ID が握っている物理ページ数 (D5) */
} AppSlot;

/* ---- 受入 G7 のカウンタ (D8 の C1/C2/C3/C6) --------------------------- */
/* KAPI にしない。fault_kill_count / ring3_abort_count と同じくカーネル
 * シンボルとして公開し、PM の V4 検証が emu_read_mem で読む。 */
extern volatile u32 ring3_switch_count;            /* C1 resume 成功回数 */
extern volatile u32 ring3_transition_count;        /* C2 start/終了の CR3 遷移 */
extern volatile u32 ring3_park_reject_count;       /* C3 OP_WAIT 外の park */
extern volatile u32 ring3_resume_bad_frame_count;  /* C6 印無しフレームの resume */
/* 回収の回数と直前の対象 (試験と診断用。G2/G5 の「1 本分だけ」を数える) */
extern volatile u32 appslot_reclaim_count;
extern volatile int appslot_last_reclaim_id;

/* ---- 表の操作 -------------------------------------------------------- */

/* 表を空にし、シェル帯 (ID 1) を走っている状態にする。exec_init から 1 回。 */
void appslot_init(void);

/* 生きているスロット (FREE なら 0)。id が範囲外でも 0。 */
AppSlot *appslot_get(int id);
/* FREE も含めて添字で引く (起動途中の書き込み用)。範囲外なら 0。 */
AppSlot *appslot_at(int id);

/* いま走っている ID (1 = シェル帯 / WM top-level)。 */
int appslot_cur(void);
/* 生きている非シェル ID の本数。 */
int appslot_live(void);
/* 空き ID を小さい方から 1 つ (無ければ 0)。まだ確保はしない。 */
int appslot_alloc_id(void);

/* ---- 起動 (D4) -------------------------------------------------------- */
/* この起動が **アプリ帯 (CPL=3、per-app 物理、0x500000〜)** を使うか。
 * 1 = アプリ帯 / 0 = そうでない (シェル帯の常駐 CPL=0、または --cpl0 の子)。
 *
 * is_shell (= exec ネスト段 0) は shell.bin でも gshell.bin でも **必ず 0**。
 * 常駐シェルは 0x300000 の MEM_SHELL_* 帯に identity で載る CPL=0 プログラム
 * で、per-app 物理化・ID の池・枚数勘定のどれにも掛からない (K5a 設計 D7
 * 「変えないもの」)。2026-09-11 の差し戻しで、この境界をホストで押さえる
 * ようにした (tools/tests/multiapp_impl_host.c ケース 18)。
 * hdr_flags は OS32X ヘッダの flags (OS32X_FLAG_FORCE_CPL0 を見る)。 */
int appslot_launch_is_app(int is_shell, u32 hdr_flags);

/* --cpl0 の子 (アプリ帯を identity で丸ごと押さえる CPL=0 の子) を起動して
 * よいか。**状態は 1 つも変えない**。
 *
 * 決裁 2026-09-11 (申し送り A1): --cpl0 の子は exec_cpl0_claim() で
 * [MEM_EXEC_LOAD_ADDR, mem_end) を丸ごと pgalloc_mark_used し、終了時に
 * 丸ごと free する。K5b-K 以後は CPL=3 アプリの per-app 物理も同じ pgalloc
 * から取るので、生きているアプリ (走行中 / park 中) が 1 本でも居ると、
 * その物理を上書きし、終了時に他人のページを解放してしまう。
 * 枚数で刻む機構は増やさず、**生存アプリが 1 本でも居たら拒否**する
 * (特権が要る例外用途なので、GUI のアプリを閉じてから使えば足りる)。
 *
 * 戻り値: 0 = 起動してよい / OS32_ERR_FULL = 生存アプリが居るので不可。
 * シェル (exec ネスト段 0) はそもそもアプリ帯を使わないので対象外 —
 * 呼び出し側が appslot_launch_is_app() と同じく is_shell を渡す。 */
int appslot_cpl0_admit(int is_shell);

/* 起動してよいかを判定する。**状態は 1 つも変えない**。
 *   gui=1 (exec_start): WM の top-level からだけ (契約 S2)
 *   gui=0 (exec_run):   走っているアプリからも通る (決裁 D9-8)
 * 戻り値: >0 = 使ってよい ID / OS32_ERR_INVAL (S2 違反) /
 *         OS32_ERR_FULL (池が尽きた) / EXEC_ERR_NOMEM (物理が足りない)。
 * pages は要求する物理ページ数、free_pages は今の空き (呼び出し側が pgalloc
 * に聞いて渡す)。free_pages == 0 は「勘定しない」の合図 (CPL=0 の子)。 */
int appslot_start_admit(int gui, u32 pages, u32 free_pages);

/* admit した ID を実際に走らせる (iret の直前)。ring3_transition_count++。 */
void appslot_start_commit(int id, int gui, u32 pages);

/* シェル帯 (ID 1) を走らせる。親は無く、段は必ず 1 (K4 のシェル起動ループが
 * shell.bin / gshell.bin を載せ替えるたびに呼ばれる)。 */
void appslot_shell_commit(void);

/* ---- gui_call の文脈 (C4) --------------------------------------------- */
/* gui_call がハンドラを呼ぶ間だけ現在の op を控える。is_wait=1 で OP_WAIT。 */
void appslot_gui_op_enter(int is_wait);
void appslot_gui_op_leave(void);

/* ---- park / resume (D0 / D2) ----------------------------------------- */
/* park してよいか。ダメなら ring3_park_reject_count++ して負を返す。
 * 条件: 走っているのがアプリ (2〜5) で、いま gui_call(OP_WAIT) の中。 */
int appslot_park_check(void);
/* park を成立させる (フレームは呼び出し側が slot->frame へ写してから呼ぶ)。
 * 印 parked_from_wait を立て、PARKED にして cur をシェル帯へ戻す。 */
void appslot_park_commit(void);

/* resume してよいか。WM top-level からだけ、印のあるフレームだけ。
 * 印が無ければ ring3_resume_bad_frame_count++ して OS32_ERR_STALE。 */
int appslot_resume_check(int id);
/* resume を成立させる (CR3 を載せる直前)。印を消し ring3_switch_count++。 */
void appslot_resume_commit(int id);

/* ---- 終了・kill (D4) -------------------------------------------------- */
/* この ID のスロットを空にする。戻り値は握っていた物理ページ数。
 * 資源 (FD / pipe / shm / db / GUI 窓) の回収は呼び出し側が ID で行う。 */
u32 appslot_reclaim(int id);
/* 畳んだ後に戻る先の ID (親が生きていれば親、居なければシェル帯)。 */
int appslot_return_target(int id);
/* cur を id へ移す (回収の後に親へ戻すとき)。ring3_transition_count++。 */
void appslot_switch_to(int id);

/* kill してよいか (top-level から、止めてあるアプリだけ)。 */
int appslot_kill_check(int id);

/* CTRL+STOP: 走っているアプリにだけ要求を立てる (D4)。1=立った。 */
int appslot_abort_request(void);

/* CTRL+STOP の要求を降ろす (KAPI v45 exec_abort_clear の実体、決裁 A1)。
 * 呼べるのは owner 1 (シェル帯 = WM) だけ — それ以外は OS32_ERR_INVAL。
 * IRQ1 は「いま走っているアプリ」に無条件で立てるが、契約 T6 の宛先は
 * **フォーカス窓のアプリ**。別アプリが走っていたら WM がここで降ろし、
 * フォーカス窓の ID を exec_kill で畳む。対象は要求を負っている高々 1 本
 * (立てられるのは走っている 1 本だけなので) で、他の ID の状態は動かさない。
 * 戻り値: 0 = 降ろした / 要求が無かった、OS32_ERR_INVAL = owner 1 でない。 */
int appslot_abort_clear(void);

/* KAPI exec_app_state の実体: 0=空き / 1=走っている / 2=park 中 / 負=不正。 */
int appslot_state(int id);

#endif /* __APPSLOT_H */
