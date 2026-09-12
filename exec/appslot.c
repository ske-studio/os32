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
volatile u32 ring3_kbd_park_count = 0;
volatile u32 appslot_reclaim_count = 0;
volatile int appslot_last_reclaim_id = 0;
volatile u32 gfx_init_reject_count = 0;

/* 画面の所有者 (票 T8 D1)。初期値は WM。CUI 中は誰も取らないのでここに
 * 留まる。static にしないのは kernel.map から emu_read_mem で読むため
 * (fault_kill_count と同じ流儀)。 */
volatile int g_gfx_owner = GFX_OWNER_WM;

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
    g_gfx_owner = GFX_OWNER_WM;   /* 画面は WM のもの (票 T8 D1) */
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
 * (決裁 2026-09-11)。ここは判定だけで、claim も alloc もまだ行わない。
 *
 * 票 T8 D1 でこの条件を広げた: GUI からの起動 (gui=1) は生存アプリの有無に
 * 関わらず断る。--cpl0 は VRAM を直接触る (v86 / VDM) ので、GUI 中に走ると
 * 画面の所有者 (D1) の外側で画面を壊し、WM が復帰する手がかりを失う。 */
int appslot_cpl0_admit(int is_shell, int gui)
{
    if (is_shell) return 0;             /* シェル帯はアプリ帯を使わない */
    if (gui) return OS32_ERR_INVAL;     /* GUI からは常に不可 (T8 D1) */
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
    a->parked_from_kbd = 0;
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
    a->parked_from_kbd = 0;
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

/* ---- 第 2 の park 点: GUI 中の kbd 待ち (票 K7 D1) --------------------- */
/* park_check との違いは「いま gui_call(OP_WAIT) の中か」を要求しないこと
 * だけ。ここへ来るのは kbd_getchar / kbd_getkey の syscall の中で、WM の
 * コールバックではないため。GUI モードと CPL=3 フレームの有無 (票 §5 R1) は
 * drivers/kbd.c と exec/exec.c が見る — この表はハードウェアを知らない。 */
int appslot_park_kbd_check(void)
{
    AppSlot *a;

    if (g_cur < APP_ID_MIN || g_cur > APP_ID_MAX) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    a = appslot_get(g_cur);
    if (!a || a->state != APP_STATE_RUNNING) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    /* CUI の入れ子 exec_run の子は park できない (D4 と同じ理由: longjmp の
     * 行き先が親アプリの中の exec_run フレームになり WM へ戻れない)。
     * 呼び手は拒否されたら従来の hlt 待ちへ落ちる。 */
    if (!a->gui) {
        ring3_park_reject_count++;
        return OS32_ERR_INVAL;
    }
    return 0;
}

void appslot_park_kbd_commit(void)
{
    AppSlot *a = appslot_get(g_cur);
    if (!a) return;
    a->parked_from_kbd = 1;       /* 「kbd 待ち由来」の印 (K7 D1) */
    a->in_op_wait = 0;
    a->state = APP_STATE_WAIT_KEY;
    g_cur_op_is_wait = 0;
    g_cur = APP_ID_SHELL;
    res_owner_set(APP_ID_SHELL);
    ring3_kbd_park_count++;
}

int appslot_resume_check(int id)
{
    AppSlot *a;

    /* resume を呼ぶのは WM の top-level。走っているアプリの横取りは無い。 */
    if (g_cur != APP_ID_SHELL) return OS32_ERR_INVAL;
    if (res_owner_get() != APP_ID_SHELL) return OS32_ERR_INVAL;
    a = appslot_get(id);
    if (!a) return OS32_ERR_INVAL;                  /* 未知 / 畳まれた ID */
    /* 印の無いフレームは起こさない (C6)。ここが受入 G7 / K7 の I5 の合否
     * そのもので、park 点が 2 つになっても規則は 1 つ — 「その状態に対応する
     * 印が立っているフレームだけ」。 */
    if (a->state == APP_STATE_WAIT_KEY) {
        if (!a->parked_from_kbd) {
            ring3_resume_bad_frame_count++;
            return OS32_ERR_STALE;
        }
        return 0;
    }
    if (a->state != APP_STATE_PARKED) return OS32_ERR_INVAL;
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
    a->parked_from_wait = 0;      /* 印は 1 回きり (両方の park 点で) */
    a->parked_from_kbd = 0;
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
    /* 走っている本人は CTRL+STOP の経路で畳む (D4)。止めてある側は
     * OP_WAIT 由来 (PARKED) でも kbd 待ち (WAIT_KEY) でも畳める — 鍵待ちの
     * アプリを永久に畳めないと CTRL+STOP の逃げ道が無くなる (票 K7 D5)。 */
    if (a->state != APP_STATE_PARKED && a->state != APP_STATE_WAIT_KEY) {
        return OS32_ERR_STALE;
    }
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

/* ======================================================================== */
/*  appslot_abort_clear — CTRL+STOP の要求を降ろす (KAPI v45、決裁 A1)       */
/*                                                                          */
/*  IRQ1 は宛先を選べないので「いま走っているアプリ」に無条件で立てる        */
/*  (appslot_abort_request)。契約 T6 の宛先はフォーカス窓のアプリなので、    */
/*  別アプリが走っていたときは WM がこれで要求を降ろし、フォーカス窓の ID を */
/*  exec_kill で畳む。降ろさないと「意図しない 1 本が次の syscall で死ぬ」。 */
/*                                                                          */
/*  要求を負えるのは走っている 1 本だけなので対象は高々 1 本。ただし WM が   */
/*  top-level (owner 1) へ戻るのは park の後なので、そのときスロットの状態は */
/*  PARKED になっている — 状態では絞らず「要求を負っている ID」で探す。      */
/*  降ろすのは abort_req だけで、state / in_op_wait / parked_from_wait と    */
/*  他の ID のスロットには触らない。                                         */
/* ======================================================================== */
int appslot_abort_clear(void)
{
    int i;

    /* gui_register と同じ判定: シェル帯 (owner 1) からのみ。 */
    if (res_owner_get() != APP_ID_SHELL) return OS32_ERR_INVAL;

    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
        if (g_slot[i].state != APP_STATE_FREE) g_slot[i].abort_req = 0;
    }
    return 0;
}

int appslot_state(int id)
{
    AppSlot *a;
    if (id < APP_ID_MIN || id > APP_ID_MAX) return OS32_ERR_INVAL;
    a = &g_slot[id];
    if (a->state == APP_STATE_FREE) return 0;
    if (a->state == APP_STATE_RUNNING) return 1;
    /* 3 = kbd 待ち (票 K7 §5 指摘 C の値の追加)。2 の意味は動かさない。 */
    if (a->state == APP_STATE_WAIT_KEY) return APP_STATE_WAIT_KEY;
    return 2;
}

/* ======================================================================== */
/*  画面の所有者 (票 T8 D1 / D1a)                                            */
/*                                                                          */
/*  全画面 GFX は「1 枚の画面を丸ごと持っていく」操作なので、持ち主を        */
/*  カーネルが 1 つだけ覚える。取るのは gfx_init / gfx_init_200 の KAPI      */
/*  ラッパ (gfx/gfx_core.c) だけで、返すのは回収 (exec_reclaim_owned) だけ。 */
/*                                                                          */
/*  判定を純関数に切り出してあるのは、材料 (GUI 中か / 呼び手の ID / CPL /   */
/*  ヘッダの宣言ビット) が全部この表にあり、ホストでそのまま試験できるため   */
/*  (tools/tests/multiapp_impl_host.c ケース 20)。                           */
/* ======================================================================== */
int appslot_gfx_claim_check(int gui_mode, int caller, int cpl3, u32 hdr_flags)
{
    /* CUI 中 (con_sink 無効) は所有者を触らない。gshell は居らず、
     * 全画面は従来どおり誰でも取れる (票 D1a「CUI 中は何でも通す」)。 */
    if (!gui_mode) return 0;
    /* WM 自身 (シェル帯) と、表に無い ID は素通し。復帰の gfx_init は
     * ここを通る。 */
    if (caller < APP_ID_MIN || caller > APP_ID_MAX) return 0;
    /* CPL=0 の子 (--cpl0) は所有者を取らない。GUI からの起動は
     * appslot_cpl0_admit が既に断っているので、ここへは来ない。 */
    if (!cpl3) return 0;
    /* 宣言 (mkos32x --gfx) が無ければ画面を渡さない (D1a)。黙って
     * 画面を壊させるより、gfx_init を呼ばずに断る。 */
    if ((hdr_flags & OS32X_FLAG_GFX) == 0) return OS32_ERR_INVAL;
    return caller;
}

int appslot_gfx_claim(int gui_mode)
{
    int caller = res_owner_get();
    AppSlot *a = appslot_get(caller);
    int r = appslot_gfx_claim_check(gui_mode, caller,
                                    a ? a->cpl3 : 0,
                                    a ? a->hdr_flags : 0);
    if (r < 0) {
        gfx_init_reject_count++;
        return r;
    }
    if (r > 0) g_gfx_owner = r;
    return 0;
}

int appslot_gfx_owner(void) { return g_gfx_owner; }

void appslot_gfx_owner_exit(int id)
{
    if (g_gfx_owner == id) g_gfx_owner = GFX_OWNER_WM;
}

/* ======================================================================== */
/*  appslot_resume_mark_selftest — 「印の無い resume は拒否」の負例 (I5)     */
/*                                                                          */
/*  票 K7 の受入 I5 の半分。park 点が 2 つになったので、C6 の規則             */
/*  (「その状態に対応する印が立っているフレームだけ起こせる」) が            */
/*  PARKED と WAIT_KEY の**両方**に効いていることをブート時に踏む。          */
/*                                                                          */
/*  空きスロット (APP_ID_MAX) を一時的に借りる。kselftest_run は exec_init   */
/*  より前に走るので g_slot は全部 FREE だが、順序に頼らず借りた中身と        */
/*  cur / owner / カウンタを丸ごと保存して戻す。                             */
/* ======================================================================== */
u32 appslot_resume_mark_selftest(void)
{
    u32 bad = 0;
    int id = APP_ID_MAX;
    AppSlot saved;
    int saved_cur = g_cur;
    int saved_owner = res_owner_get();
    u32 saved_badframe = ring3_resume_bad_frame_count;
    u32 saved_switch = ring3_switch_count;

    saved = g_slot[id];
    g_cur = APP_ID_SHELL;
    res_owner_set(APP_ID_SHELL);
    slot_zero(&g_slot[id]);

    /* (0) 空きスロットは起こせない */
    g_slot[id].state = APP_STATE_FREE;
    if (appslot_resume_check(id) != OS32_ERR_INVAL) bad |= 1u << 0;

    /* (1) OP_WAIT 由来: 印が無ければ STALE、あれば 0 */
    g_slot[id].state = APP_STATE_PARKED;
    g_slot[id].parked_from_wait = 0;
    g_slot[id].parked_from_kbd = 0;
    if (appslot_resume_check(id) != OS32_ERR_STALE) bad |= 1u << 1;
    if (ring3_resume_bad_frame_count != saved_badframe + 1) bad |= 1u << 1;
    g_slot[id].parked_from_wait = 1;
    if (appslot_resume_check(id) != 0) bad |= 1u << 1;

    /* (2) kbd 待ち: kbd の印が要る。OP_WAIT の印では起こせない */
    g_slot[id].state = APP_STATE_WAIT_KEY;
    g_slot[id].parked_from_wait = 1;
    g_slot[id].parked_from_kbd = 0;
    if (appslot_resume_check(id) != OS32_ERR_STALE) bad |= 1u << 2;
    if (ring3_resume_bad_frame_count != saved_badframe + 2) bad |= 1u << 2;
    g_slot[id].parked_from_kbd = 1;
    if (appslot_resume_check(id) != 0) bad |= 1u << 2;

    /* (3) 鍵待ちは畳める (D5)、走っている本人は畳めない */
    if (appslot_kill_check(id) != 0) bad |= 1u << 3;
    g_slot[id].state = APP_STATE_RUNNING;
    if (appslot_kill_check(id) != OS32_ERR_STALE) bad |= 1u << 3;

    /* (4) exec_app_state は 3 を返す (既存の 0/1/2 は不変) */
    g_slot[id].state = APP_STATE_WAIT_KEY;
    if (appslot_state(id) != APP_STATE_WAIT_KEY) bad |= 1u << 4;
    g_slot[id].state = APP_STATE_RUNNING;
    if (appslot_state(id) != 1) bad |= 1u << 4;
    g_slot[id].state = APP_STATE_PARKED;
    if (appslot_state(id) != 2) bad |= 1u << 4;
    g_slot[id].state = APP_STATE_FREE;
    if (appslot_state(id) != 0) bad |= 1u << 4;

    /* 後始末: 借りたスロットも観測点も元に戻す (検査は 1 回も起こさない) */
    g_slot[id] = saved;
    g_cur = saved_cur;
    res_owner_set(saved_owner);
    ring3_resume_bad_frame_count = saved_badframe;
    if (ring3_switch_count != saved_switch) bad |= 1u << 5;
    return bad;
}

/* ======================================================================== */
/*  appslot_gfx_owner_selftest — 画面の所有者の遷移 (票 T8 D1 / D1a)         */
/*                                                                          */
/*  ブート時に踏むのは 2 つ:                                                 */
/*    (0) 遷移: 宣言のある CPL=3 アプリの gfx_init で所有者がその ID へ移り、 */
/*        回収 (appslot_gfx_owner_exit) で WM (1) へ戻る。CUI 中 (gui_mode   */
/*        = 0) は 1 のまま動かない。                                         */
/*    (1) 拒否: GUI 中に宣言の無い CPL=3 が呼んだら OS32_ERR_INVAL で、       */
/*        所有者は動かず gfx_init_reject_count だけが 1 増える。              */
/*                                                                          */
/*  空きスロット (APP_ID_MAX) を一時的に借りる。借りた中身・cur・owner・      */
/*  所有者・カウンタは丸ごと保存して戻す。                                   */
/* ======================================================================== */
u32 appslot_gfx_owner_selftest(void)
{
    u32 bad = 0;
    int id = APP_ID_MAX;
    AppSlot saved;
    int saved_cur = g_cur;
    int saved_owner = res_owner_get();
    int saved_gfx = g_gfx_owner;
    u32 saved_reject = gfx_init_reject_count;

    saved = g_slot[id];
    slot_zero(&g_slot[id]);
    g_slot[id].state = APP_STATE_RUNNING;
    g_slot[id].cpl3 = 1;
    g_slot[id].gui = 1;
    g_cur = id;
    res_owner_set(id);
    g_gfx_owner = GFX_OWNER_WM;

    /* (0) 遷移: 宣言ありなら取り、回収で WM へ戻る。CUI 中は動かない。 */
    g_slot[id].hdr_flags = OS32X_FLAG_GFX;
    if (appslot_gfx_claim(0) != 0) bad |= 1u << 0;
    if (g_gfx_owner != GFX_OWNER_WM) bad |= 1u << 0;   /* CUI は触らない */
    if (appslot_gfx_claim(1) != 0) bad |= 1u << 0;
    if (g_gfx_owner != id) bad |= 1u << 0;
    appslot_gfx_owner_exit(GFX_OWNER_WM);              /* 他人の回収では戻らない */
    if (g_gfx_owner != id) bad |= 1u << 0;
    appslot_gfx_owner_exit(id);
    if (g_gfx_owner != GFX_OWNER_WM) bad |= 1u << 0;

    /* (1) 拒否: GUI 中の宣言なしは ERR_INVAL。所有者は動かず、数だけ増える。 */
    g_slot[id].hdr_flags = 0;
    if (appslot_gfx_claim(1) != OS32_ERR_INVAL) bad |= 1u << 1;
    if (g_gfx_owner != GFX_OWNER_WM) bad |= 1u << 1;
    if (gfx_init_reject_count != saved_reject + 1) bad |= 1u << 1;
    if (appslot_gfx_claim(0) != 0) bad |= 1u << 1;     /* CUI 中は通す */
    if (gfx_init_reject_count != saved_reject + 1) bad |= 1u << 1;

    /* 後始末 */
    g_slot[id] = saved;
    g_cur = saved_cur;
    res_owner_set(saved_owner);
    g_gfx_owner = saved_gfx;
    gfx_init_reject_count = saved_reject;
    return bad;
}
