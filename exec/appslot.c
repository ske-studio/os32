/* ======================================================================== */
/*  APPSLOT.C — アプリ ID の表と状態遷移 (K5b-K)                             */
/*                                                                          */
/*  票 docs/tasks/gui/v13/TASK_K5_multiapp.md の §設計 D0〜D4 をそのまま      */
/*  写したもの。K5a のホスト模型 (tools/tests/multiapp_model_host.c) の       */
/*  遷移表が仕様で、この .c がその実装。同じ検査を                            */
/*  tools/tests/multiapp_impl_host.c が **この .c を直接コンパイルして** 回す。*/
/*                                                                          */
/*  ここに置かないもの (置くと模型がホストで動かなくなる):                    */
/*    - CR3 / setjmp / pgalloc / ページテーブル → exec/exec.c                */
/*    - 「次に誰を起こすか」の規則 → gshell (D11-5: カーネルは順番を決めない) */
/*    - SHM スロットの割当 → gshell の alloc_slot (D6)                       */
/* ======================================================================== */

#include "appslot.h"
#include "os32_kapi_shared.h"   /* OS32_ERR_* / EXEC_ERR_* */

/* res_owner_set/get は fs/fd_redirect.c。exec/ は -Ifs を持たないので
 * kernel/gui.c と同じ流儀で extern 宣言する。 */
extern void res_owner_set(int owner);
extern int  res_owner_get(void);

/* ID 1 = シェル帯は kernel/gui.h の GUI_SHELL_OWNER と同じ値でなければ
 * ならない (gui_register / sys_switch_shell の owner 判定がこれを見る)。
 * gui.h を include すると exec/ が -Ikernel に依存するので値で固定する。 */
STATIC_ASSERT(APP_ID_SHELL == 1, appslot_shell_id_is_gui_shell_owner);
STATIC_ASSERT(APP_ID_MAX < APP_SLOT_COUNT, appslot_table_holds_id_max);

volatile u32 ring3_switch_count = 0;
volatile u32 ring3_transition_count = 0;
volatile u32 ring3_park_reject_count = 0;
volatile u32 ring3_resume_bad_frame_count = 0;
volatile u32 appslot_reclaim_count = 0;
volatile int appslot_last_reclaim_id = 0;

static AppSlot g_slot[APP_SLOT_COUNT];
static int g_cur = APP_ID_SHELL;

/* gui_call がハンドラを呼ぶ間だけ立つ「いまの op は OP_WAIT か」(C4)。
 * park の可否と印の判定に使う。gui_call は入れ子にならない (契約 T1: WM から
 * アプリへのコールバック経路を作らない) ので 1 本で足りる。 */
static int g_cur_op_is_wait = 0;

static void slot_zero(AppSlot *a)
{
    u8 *b = (u8 *)a;
    u32 i;
    for (i = 0; i < (u32)sizeof(AppSlot); i++) b[i] = 0;
}

void appslot_init(void)
{
    int i;
    for (i = 0; i < APP_SLOT_COUNT; i++) {
        slot_zero(&g_slot[i]);
        g_slot[i].state = APP_STATE_FREE;
    }
    /* シェル帯 (ID 1) は常に居る。depth は従来の exec_nest_level = 1。 */
    g_slot[APP_ID_SHELL].state = APP_STATE_RUNNING;
    g_slot[APP_ID_SHELL].parent = 0;
    g_slot[APP_ID_SHELL].depth = 1;
    g_cur = APP_ID_SHELL;
    g_cur_op_is_wait = 0;
    res_owner_set(APP_ID_SHELL);
}

AppSlot *appslot_at(int id)
{
    if (id < 0 || id >= APP_SLOT_COUNT) return (AppSlot *)0;
    return &g_slot[id];
}

AppSlot *appslot_get(int id)
{
    AppSlot *a = appslot_at(id);
    if (!a || a->state == APP_STATE_FREE) return (AppSlot *)0;
    return a;
}

int appslot_cur(void) { return g_cur; }

int appslot_live(void)
{
    int i, n = 0;
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
        if (g_slot[i].state != APP_STATE_FREE) n++;
    }
    return n;
}

/* 空き ID は必ず**小さい方から**。これで CUI の入れ子 exec_run が
 * 2, 3, 4 と並び、「段 = ID」という従来の見え方がそのまま残る (D3)。 */
int appslot_alloc_id(void)
{
    int i;
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
        if (g_slot[i].state == APP_STATE_FREE) return i;
    }
    return 0;
}

/* ======================================================================== */
/*  起動 (D4)                                                               */
/* ======================================================================== */

/* アプリ帯を使うのは「シェルでない」かつ「--cpl0 でない」ものだけ。
 * ここが exec_launch の want_ring3 と ID の池の唯一の分かれ道 (宣言側の
 * 注記を参照)。判定材料はヘッダの flags だけで、物理の空きは見ない —
 * シェルの起動が pgalloc の空きに左右されてはいけないため。 */
int appslot_launch_is_app(int is_shell, u32 hdr_flags)
{
    if (is_shell) return 0;
    if (hdr_flags & OS32X_FLAG_FORCE_CPL0) return 0;
    return 1;
}

/* --cpl0 の子は帯を丸ごと押さえる (exec_cpl0_claim)。生きているアプリの
 * per-app 物理と正面衝突するので、1 本でも居たら起動そのものを断る
 * (決裁 2026-09-11)。ここは判定だけで、claim も alloc もまだ行わない。 */
int appslot_cpl0_admit(int is_shell)
{
    if (is_shell) return 0;             /* シェル帯はアプリ帯を使わない */
    if (appslot_live() > 0) return OS32_ERR_FULL;
    return 0;
}

int appslot_start_admit(int gui, u32 pages, u32 free_pages)
{
    int id;

    /* GUI アプリ (塞がない起動) は WM の top-level からだけ — 契約 S2。
     * CUI の入れ子 exec_run は従来どおり走っているアプリからも通る (D9-8)。 */
    if (gui && g_cur != APP_ID_SHELL) return OS32_ERR_INVAL;

    id = appslot_alloc_id();
    if (id == 0) return OS32_ERR_FULL;          /* 5 本目 (受入 G3) */

    /* 入らなければ拒否する。切り詰めない・スワップしない (D5)。
     * free_pages == 0 は「ここでは勘定しない」の合図 (CPL=0 の子は
     * pgalloc_mark_used で従来どおり固定帯を押さえる)。 */
    if (free_pages != 0 && pages > free_pages) return EXEC_ERR_NOMEM;

    return id;
}

void appslot_start_commit(int id, int gui, u32 pages)
{
    AppSlot *a = appslot_at(id);
    AppSlot *parent = appslot_at(g_cur);
    if (!a) return;
    a->state = APP_STATE_RUNNING;
    a->parent = g_cur;
    a->depth = (parent ? parent->depth : 1) + 1;
    a->gui = gui;
    a->pages = pages;
    a->in_op_wait = 0;
    a->abort_req = 0;
    a->parked_from_wait = 0;
    g_cur = id;
    res_owner_set(id);
    /* 起動の iret は「生存アプリの集合が変わる瞬間」で、生存アプリ間の
     * 実行切替ではない (D0)。受入 G7 が数える switch とは別勘定にする。 */
    ring3_transition_count++;
}

void appslot_shell_commit(void)
{
    AppSlot *a = &g_slot[APP_ID_SHELL];
    a->state = APP_STATE_RUNNING;
    a->parent = 0;
    a->depth = 1;
    a->gui = 0;
    a->cpl3 = 0;
    a->in_op_wait = 0;
    a->abort_req = 0;
    a->parked_from_wait = 0;
    g_cur = APP_ID_SHELL;
    g_cur_op_is_wait = 0;
    res_owner_set(APP_ID_SHELL);
}

/* ======================================================================== */
/*  gui_call の文脈 (C4)                                                    */
/* ======================================================================== */
void appslot_gui_op_enter(int is_wait)
{
    AppSlot *a = appslot_get(g_cur);
    g_cur_op_is_wait = is_wait ? 1 : 0;
    if (a) a->in_op_wait = g_cur_op_is_wait;
}

void appslot_gui_op_leave(void)
{
    AppSlot *a = appslot_get(g_cur);
    g_cur_op_is_wait = 0;
    if (a) a->in_op_wait = 0;
}

/* ======================================================================== */
/*  park / resume (D0 / D2)                                                 */
/* ======================================================================== */
int appslot_park_check(void)
{
    AppSlot *a;

    if (g_cur < APP_ID_MIN || g_cur > APP_ID_MAX) {
        /* 走っているアプリが居ない (WM top-level / シェル)。WM の規約違反。 */
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    a = appslot_get(g_cur);
    if (!a || a->state != APP_STATE_RUNNING) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    /* 塞がない起動 (exec_start) で立ったアプリだけが park できる。
     * CUI の入れ子 exec_run の子を park すると、longjmp の行き先が
     * 「走っている親アプリの中の exec_run フレーム」になり、WM top-level へ
     * 戻れない。親は子の終了まで塞がっているので譲る相手も居ない (D4)。 */
    if (!a->gui) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    /* 「OP_WAIT の中」でだけ譲る (契約 T2a、受入 G7)。他の op / X1 / X2 / X4
     * から呼ばれたら park させずに弾く — WM の行儀を信じない (D0)。 */
    if (!g_cur_op_is_wait || !a->in_op_wait) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    return 0;
}

void appslot_park_commit(void)
{
    AppSlot *a = appslot_get(g_cur);
    if (!a) return;
    a->parked_from_wait = 1;      /* 「OP_WAIT 由来」の印 (C5) */
    a->in_op_wait = 0;
    a->state = APP_STATE_PARKED;
    g_cur_op_is_wait = 0;
    g_cur = APP_ID_SHELL;
    res_owner_set(APP_ID_SHELL);
}

int appslot_resume_check(int id)
{
    AppSlot *a;

    /* resume を呼ぶのは WM の top-level。走っているアプリの横取りは無い。 */
    if (g_cur != APP_ID_SHELL) return OS32_ERR_INVAL;
    if (res_owner_get() != APP_ID_SHELL) return OS32_ERR_INVAL;
    a = appslot_get(id);
    if (!a) return OS32_ERR_INVAL;                  /* 未知 / 畳まれた ID */
    if (a->state != APP_STATE_PARKED) return OS32_ERR_INVAL;
    /* 印の無いフレームは起こさない (C6)。ここが受入 G7 の合否そのもの。 */
    if (!a->parked_from_wait) {
        ring3_resume_bad_frame_count++;
        return OS32_ERR_STALE;
    }
    return 0;
}

void appslot_resume_commit(int id)
{
    AppSlot *a = appslot_get(id);
    if (!a) return;
    a->parked_from_wait = 0;      /* 印は 1 回きり */
    a->in_op_wait = 0;
    a->state = APP_STATE_RUNNING;
    g_cur = id;
    g_cur_op_is_wait = 0;
    res_owner_set(id);
    /* 受入 G7 が数えるのはこれ = park してある生存アプリを起こした回数。 */
    ring3_switch_count++;
}

/* ======================================================================== */
/*  終了・kill (D4)                                                         */
/* ======================================================================== */
u32 appslot_reclaim(int id)
{
    AppSlot *a = appslot_get(id);
    u32 pages;
    if (!a || id == APP_ID_SHELL) return 0;
    pages = a->pages;
    slot_zero(a);
    a->state = APP_STATE_FREE;
    appslot_reclaim_count++;
    appslot_last_reclaim_id = id;
    return pages;
}

int appslot_return_target(int id)
{
    AppSlot *a = appslot_at(id);
    int parent = a ? a->parent : APP_ID_SHELL;
    if (parent >= APP_ID_MIN && parent <= APP_ID_MAX &&
        g_slot[parent].state != APP_STATE_FREE) return parent;
    return APP_ID_SHELL;
}

void appslot_switch_to(int id)
{
    AppSlot *a = appslot_at(id);
    if (!a) return;
    g_cur = id;
    g_cur_op_is_wait = 0;
    if (id != APP_ID_SHELL) a->state = APP_STATE_RUNNING;
    res_owner_set(id);
    ring3_transition_count++;
}

int appslot_kill_check(int id)
{
    AppSlot *a;
    if (g_cur != APP_ID_SHELL) return OS32_ERR_INVAL;
    if (res_owner_get() != APP_ID_SHELL) return OS32_ERR_INVAL;
    a = appslot_get(id);
    if (!a) return OS32_ERR_INVAL;
    /* 走っている本人は CTRL+STOP の経路で畳む (D4)。 */
    if (a->state != APP_STATE_PARKED) return OS32_ERR_STALE;
    return 0;
}

int appslot_abort_request(void)
{
    AppSlot *a = appslot_get(g_cur);
    /* IRQ1 が知っているのは「いま走っているアプリ」だけ。止めてあるアプリ
     * には届かない — そちらは exec_kill で畳む (D4)。 */
    if (!a || g_cur < APP_ID_MIN || a->state != APP_STATE_RUNNING) return 0;
    a->abort_req = 1;
    return 1;
}

int appslot_state(int id)
{
    AppSlot *a;
    if (id < APP_ID_MIN || id > APP_ID_MAX) return OS32_ERR_INVAL;
    a = &g_slot[id];
    if (a->state == APP_STATE_FREE) return 0;
    return (a->state == APP_STATE_RUNNING) ? 1 : 2;
}
