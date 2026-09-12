/* ========================================================================
 *  multiapp_impl_host.c — K5b-K の「実装」を K5a の模型と同じ検査で回す
 *
 *  対象票: docs/tasks/gui/v13/TASK_K5B_kernel.md (ホスト試験の 3 本目)
 *  実行:   python3 -B tools/tests/test_multiapp_impl.py
 *  記録:   tools/tests/k5b_kernel_tdd.md
 *
 *  K5a の tools/tests/multiapp_model_host.c は「設計の模型」で、状態機械を
 *  その場に手書きしていた。こちらは **実物の AppSlot 管理コード**
 *  (exec/appslot.c) をそのままコンパイルして、同じ 84 検査を回す。
 *  番号と検査名は模型と 1 対 1 で対応させてあるので、落ちた検査名で
 *  「模型のどの規則が実装で崩れたか」がそのまま分かる。
 *
 *  模型との差 (意図したもの、報告済み):
 *    (a) 戻り値は模型の MA_ERR_* ではなく **実物の OS32_ERR_* / EXEC_ERR_***。
 *        検査名は同じで、期待値だけ実物の定数に読み替えてある。
 *    (b) ケース 6 (SHM スロット) と 12〜16 (同時 ready の選択規則) は
 *        設計 D6 / D11-5 が **gshell (WM) の領分**と決めた部分なので、
 *        カーネルには無い。ハーネス側に模型と同じ規則を置き、その下で
 *        park / resume だけを実物に通す (= 規則と機構の噛み合わせを見る)。
 *        W レーンはこの規則を Rust へ写す。
 *    (c) ケース 17 は票 K5b の追加要求 = 「印なし resume の拒否」(C6) の負例。
 *
 *  C89 ([C1])。libc は使わない (-nostdlib で直接走る)。
 * ======================================================================== */

#include "types.h"

/* ---- 実物のカーネルコード (ハードウェアには一切触らない部分) ---------- */
extern void res_owner_set(int owner);
extern int  res_owner_get(void);
static int host_owner = 1;
static int host_owner_sets = 0;
void res_owner_set(int owner) { host_owner = owner; host_owner_sets++; }
int  res_owner_get(void)      { return host_owner; }

#include "appslot.c"

/* K7: 注入リングは **実物** (kernel/kbd_inject.c) をそのまま取り込む。
 * 権限の照合相手だけハーネスが持つ (con_sink はここでは要らない —
 * 「読み手だけが注げる」は tools/tests/kbd_inject_host.c が本物の
 * kernel/con_sink.c と組んで見ている)。 */
static int host_reader = 2;
int con_sink_reader_get(void) { return host_reader; }
#include "kbd_inject.c"

/* ---- 試験ハーネス ----------------------------------------------------- */

#define MA_SLOT_MAX   4              /* = include/memmap.h の GUI_SLOT_MAX */

#define MA_RES_FD     0
#define MA_RES_PIPE   1
#define MA_RES_WINDOW 2
#define MA_RES_TIMER  3
#define MA_RES_SHM    4
#define MA_RES_KINDS  5

#define MA_OP_WAIT   1
#define MA_OP_POLL   2
#define MA_OP_COMMIT 3

/* D11 の 2 つの上限 (WM 側の私有状態。模型と同じ値・同じ式)。 */
#define MA_INPUT_STREAK_MAX  APP_MAX_APPS
#define MA_STARVE_BOUND \
    ((2 * APP_MAX_APPS - 2) * (MA_INPUT_STREAK_MAX + 1))

/* 実物では pgalloc / gshell が持つもの。ハーネスが代わりに持つ。 */
typedef struct {
    int  slot[APP_SLOT_COUNT];        /* SHM スロット (gshell の alloc_slot) */
    int  res[APP_SLOT_COUNT][MA_RES_KINDS];
    int  input_ready[APP_SLOT_COUNT];
    int  derived_ready[APP_SLOT_COUNT];
    int  turn_used[APP_SLOT_COUNT];
    u32  free_pages;
    int  cpl0_children;               /* exec/exec.c の g_cpl0_children */
    int  last_run;
    int  focus;
    int  input_streak;
    int  exit_status;
} MaHost;

static MaHost H;

static void ma_init(u32 free_pages)
{
    int i, k;
    appslot_init();
    ring3_switch_count = 0;
    ring3_transition_count = 0;
    ring3_park_reject_count = 0;
    ring3_resume_bad_frame_count = 0;
    appslot_reclaim_count = 0;
    appslot_last_reclaim_id = 0;
    for (i = 0; i < APP_SLOT_COUNT; i++) {
        H.slot[i] = -1;
        H.input_ready[i] = 0;
        H.derived_ready[i] = 0;
        H.turn_used[i] = 0;
        for (k = 0; k < MA_RES_KINDS; k++) H.res[i][k] = 0;
    }
    H.free_pages = free_pages;
    H.cpl0_children = 0;
    H.last_run = 0;
    H.focus = 0;
    H.input_streak = 0;
    H.exit_status = 0;
}

/* 模型の pd_switches = 「アプリ PD を CR3 に載せた回数」。実物は受入 G7 の
 * ために 2 本に割ってあるので (D0)、模型と突き合わせるときは足す。 */
static u32 ma_pd_switches(void)
{
    return ring3_switch_count + ring3_transition_count;
}

/* gshell の alloc_slot (wm.rs:641) と同じ規則。小さい順、ID と 1 対 1。 */
static int ma_alloc_slot(void)
{
    int s, i, taken;
    for (s = 0; s < MA_SLOT_MAX; s++) {
        taken = 0;
        for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
            if (appslot_get(i) != 0 && H.slot[i] == s) taken = 1;
        }
        if (!taken) return s;
    }
    return -1;
}

static int ma_start(u32 pages, int gui)
{
    int id = appslot_start_admit(gui, pages, H.free_pages);
    int slot = -1;
    if (id < 0) return id;
    if (gui) {
        slot = ma_alloc_slot();
        if (slot < 0) return OS32_ERR_FULL;
    }
    appslot_start_commit(id, gui, pages);
    H.free_pages -= pages;
    H.slot[id] = slot;
    H.turn_used[id] = 1;
    H.last_run = id;
    H.input_streak = 0;
    return id;
}

/* exec_launch が起動時にスロットへ書く 2 つ (ctx->cpl3 = 1 / ctx->hdr_flags =
 * hdr->flags) を、立てたあとに写す。票 T8 の判定材料はこの 2 つだけ。 */
static int ma_start_gfx(u32 pages, int gui, u32 hdr_flags)
{
    int id = ma_start(pages, gui);
    AppSlot *a;
    if (id < 0) return id;
    a = appslot_at(id);
    a->cpl3 = 1;
    a->hdr_flags = hdr_flags;
    return id;
}

/* ---- --cpl0 の子 (exec/exec.c の exec_cpl0_claim / exec_cpl0_release) ----
 * 実物は帯 [MEM_EXEC_LOAD_ADDR, mem_end) を **丸ごと** pgalloc_mark_used し、
 * 最後の 1 本が終わったときに丸ごと free する。効くのは「丸ごと」という点
 * だけなので、ハーネスでは枚数を 1 つの定数で代表させる。 */
#define MA_CPL0_BAND_PAGES 512

static void ma_cpl0_claim(void)
{
    if (H.cpl0_children++ > 0) return;
    H.free_pages -= MA_CPL0_BAND_PAGES;
}

static void ma_cpl0_release(void)
{
    if (H.cpl0_children <= 0) return;
    if (--H.cpl0_children > 0) return;
    H.free_pages += MA_CPL0_BAND_PAGES;
}

/* exec_launch の CPL=0 経路をその**順番のまま**なぞる (決裁 2026-09-11):
 *   池の admit → want_ring3 の判定 → cpl0 の admit → claim → commit
 * 拒否は claim より前でなければならない (claim も alloc もしないこと)。 */
/* gui は exec_launch の gui_arg (0 = CUI の exec_run / 1 = GUI の exec_start)。
 * 票 T8 D1 で「GUI からの --cpl0 は常に拒否」が入ったので、拒否の戻り値を
 * 潰さずそのまま返す (以前は一律 OS32_ERR_FULL に丸めていた)。 */
static int ma_start_cpl0_gui(int is_shell, int gui)
{
    int id;
    int rc;
    if (is_shell) {
        /* exec_launch の is_shell 経路: 池も枚数勘定も帯の claim も通らない */
        rc = appslot_cpl0_admit(1, gui);
        if (rc < 0) return rc;
        appslot_shell_commit();
        return APP_ID_SHELL;
    }
    id = appslot_start_admit(gui, 0, 0);     /* 池だけ。状態は変えない */
    if (id < 0) return id;
    if (appslot_launch_is_app(0, OS32X_FLAG_FORCE_CPL0) != 0)
        return OS32_ERR_INVAL;               /* --cpl0 はアプリ帯を使わない */
    rc = appslot_cpl0_admit(0, gui);
    if (rc < 0) return rc;
    ma_cpl0_claim();
    appslot_start_commit(id, gui, 0);
    H.turn_used[id] = 1;
    H.last_run = id;
    return id;
}

static int ma_start_cpl0(int is_shell) { return ma_start_cpl0_gui(is_shell, 0); }

static void ma_gui_call(int op) { appslot_gui_op_enter(op == MA_OP_WAIT); }
static void ma_gui_return(void) { appslot_gui_op_leave(); }

static int ma_park(void)
{
    int rc = appslot_park_check();
    if (rc < 0) return rc;
    appslot_park_commit();
    return 0;
}

static int ma_resume(int id)
{
    int rc = appslot_resume_check(id);
    if (rc < 0) return rc;
    appslot_resume_commit(id);
    H.turn_used[id] = 1;
    H.last_run = id;
    H.input_streak = 0;
    return 0;
}

/* ---- K7: 第 2 の park 点 (GUI 中の kbd 待ち) --------------------------- */
/* 表 (appslot.c) は実物。CR3 / フレーム / longjmp を持つ exec/exec.c は
 * ホストに持ち込めないので、exec_park_kbd / exec_resume の **kbd 分岐だけ**
 * をここに写す (ケース 12〜16 が gshell の選択規則を写しているのと同じ扱い)。
 * 写した部分は 3 行 — 印を見て注入リングから 1 バイト取り、EAX に入れ、
 * 空なら OS32_ERR_AGAIN で起こさない (票 §5 の指摘 B)。 */
static int ma_park_kbd(void)
{
    int rc = appslot_park_kbd_check();
    if (rc < 0) return rc;
    appslot_park_kbd_commit();
    return 0;
}

static int ma_resume_kbd(int id)
{
    AppSlot *a;
    u8 ch;
    int rc = appslot_resume_check(id);
    if (rc < 0) return rc;
    a = appslot_get(id);
    if (a->parked_from_kbd) {
        ch = 0;
        if (!kbd_inject_take(&ch)) return OS32_ERR_AGAIN;   /* 印は残す */
        a->frame[APP_FRAME_EAX] = (u32)ch;
    } else {
        a->frame[APP_FRAME_EAX] = 0;
    }
    appslot_resume_commit(id);
    H.turn_used[id] = 1;
    H.last_run = id;
    return 0;
}

static int ma_res_add(int kind, int n)
{
    int owner = res_owner_get();
    if (kind < 0 || kind >= MA_RES_KINDS) return OS32_ERR_INVAL;
    if (owner < APP_ID_MIN || owner > APP_ID_MAX) return OS32_ERR_INVAL;
    H.res[owner][kind] += n;
    return 0;
}

/* 実物の回収の並び (exec_exit): fd_redirect_reset_owned / vfs_close_owned /
 * pipe_free_owned / shm_free_owned / db_cleanup_owned / gui_owner_exit —
 * 全部この 1 つの ID で呼ぶ。ハーネスは「その ID の分だけ消える」を数える。
 * 票 T8 D1 で (11) 画面の所有者が並びの末尾に加わった。こちらは実物を呼ぶ。 */
static void ma_reclaim_res(int id)
{
    int k;
    appslot_gfx_owner_exit(id);
    for (k = 0; k < MA_RES_KINDS; k++) H.res[id][k] = 0;
    H.slot[id] = -1;
    H.input_ready[id] = 0;
    H.derived_ready[id] = 0;
    H.turn_used[id] = 0;
}

static int ma_exit(int status)
{
    int id = appslot_cur();
    int target;
    if (id < APP_ID_MIN || id > APP_ID_MAX) return OS32_ERR_INVAL;
    H.exit_status = status;
    target = appslot_return_target(id);
    ma_reclaim_res(id);
    H.free_pages += appslot_reclaim(id);
    appslot_switch_to(target);
    return 0;
}

/* exec_exit の !cpl3 経路 (exec/exec.c: `if (!a->cpl3) exec_cpl0_release();`) */
static int ma_exit_cpl0(void)
{
    int rc = ma_exit(0);
    if (rc == 0) ma_cpl0_release();
    return rc;
}

static int ma_fault(void) { return ma_exit(-1); }

static int ma_abort_request(void)
{
    return appslot_abort_request() ? 0 : OS32_ERR_INVAL;
}

/* KAPI v45 exec_abort_clear の実体 (決裁 A1)。owner は呼ぶ側の文脈のまま。 */
static int ma_abort_clear(void)
{
    return appslot_abort_clear();
}

static int ma_abort_check(void)
{
    AppSlot *a = appslot_get(appslot_cur());
    if (!a) return OS32_ERR_INVAL;
    if (!a->abort_req) return 0;
    return ma_exit(-2);
}

static int ma_kill(int id)
{
    int rc = appslot_kill_check(id);
    if (rc < 0) return rc;
    ma_reclaim_res(id);
    H.free_pages += appslot_reclaim(id);
    return 0;
}

/* ---- D11: 同時 ready の選択規則 (WM 側。D11-5 でカーネルには置かない) --- */
static void ma_set_ready(int id, int input, int derived)
{
    if (id < APP_ID_MIN || id > APP_ID_MAX) return;
    H.input_ready[id] = input;
    H.derived_ready[id] = derived;
}

static int ma_round_remaining(void)
{
    int i, n = 0;
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
        AppSlot *a = appslot_get(i);
        if (a && a->state == APP_STATE_PARKED && !H.turn_used[i] &&
            (H.input_ready[i] || H.derived_ready[i])) n++;
    }
    return n;
}

static int ma_pick_group(int want_input)
{
    int start, n, i, id;
    start = (H.last_run >= APP_ID_MIN && H.last_run <= APP_ID_MAX) ?
            (H.last_run - APP_ID_MIN + 1) : 0;
    for (n = 0; n < APP_MAX_APPS; n++) {
        AppSlot *a;
        i = (start + n) % APP_MAX_APPS;
        id = APP_ID_MIN + i;
        a = appslot_get(id);
        if (!a || a->state != APP_STATE_PARKED || H.turn_used[id]) continue;
        if (want_input) {
            if (H.input_ready[id]) return id;
        } else {
            if (!H.input_ready[id] && H.derived_ready[id]) return id;
        }
    }
    return 0;
}

static int ma_pick(void)
{
    int f, k, i;
    if (appslot_cur() != APP_ID_SHELL) return 0;
    if (ma_round_remaining() == 0) {
        for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) H.turn_used[i] = 0;
        if (ma_round_remaining() == 0) return 0;
    }
    f = H.focus;
    if (f >= APP_ID_MIN && f <= APP_ID_MAX) {
        AppSlot *a = appslot_get(f);
        if (a && a->state == APP_STATE_PARKED && !H.turn_used[f] &&
            H.input_ready[f]) return f;
    }
    k = ma_pick_group(1);
    if (k) return k;
    return ma_pick_group(0);
}

static int ma_should_park(void)
{
    int cur = appslot_cur();
    int i, other_ready = 0;
    AppSlot *a;
    if (cur < APP_ID_MIN || cur > APP_ID_MAX) return 0;
    a = appslot_get(cur);
    if (!a || a->state != APP_STATE_RUNNING) return 0;
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) {
        AppSlot *o = appslot_get(i);
        if (o && o->state == APP_STATE_PARKED &&
            (H.input_ready[i] || H.derived_ready[i])) other_ready = 1;
    }
    if (!other_ready) return 0;
    if (H.input_ready[cur] && H.input_streak < MA_INPUT_STREAK_MAX) {
        H.input_streak++;
        return 0;
    }
    return 1;
}

/* ---- 出力 ------------------------------------------------------------- */
static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}

static void report(const char *text)
{
    u32 len = 0;
    while (text[len]) len++;
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(text), "d"(len)
                     : "memory");
}

static int failures;
static int checks;

static void check(int cond, const char *name)
{
    checks++;
    if (cond) {
        report("  ok   ");
    } else {
        report("  FAIL ");
        failures++;
    }
    report(name);
    report("\n");
}

static void fill_four(u32 pages_each)
{
    int i;
    for (i = 0; i < 4; i++) {
        ma_start(pages_each, 1);
        ma_gui_call(MA_OP_WAIT);
        ma_park();
    }
}

/* ---- 1. 5 本目は ERR_FULL、既存 4 本は無事 (票 G3) ---- */
static void case_fifth_refused(void)
{
    int ids[4], i, r;
    ma_init(4096);
    for (i = 0; i < 4; i++) {
        ids[i] = ma_start(100, 1);
        ma_gui_call(MA_OP_WAIT);
        ma_park();
    }
    check(ids[0] == 2 && ids[1] == 3 && ids[2] == 4 && ids[3] == 5,
          "1a ID は 2..5 が小さい順に配られる");
    r = ma_start(100, 1);
    check(r == OS32_ERR_FULL, "1b 5 本目は ERR_FULL");
    check(appslot_live() == 4, "1c 5 本目の拒否で既存 4 本は減らない");
    check(H.free_pages == 4096 - 400,
          "1d 拒否された 5 本目はページを 1 枚も取らない");
    check(appslot_cur() == APP_ID_SHELL && res_owner_get() == APP_ID_SHELL,
          "1e 拒否のあと WM top-level のまま");
}

/* ---- 2. 終了は 1 本分だけ回収する (票 G2) ---- */
static void case_exit_reclaims_one(void)
{
    ma_init(4096);
    fill_four(100);
    ma_resume(2);
    ma_res_add(MA_RES_FD, 3);
    ma_res_add(MA_RES_WINDOW, 2);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    ma_resume(3);
    ma_res_add(MA_RES_FD, 5);
    ma_res_add(MA_RES_TIMER, 1);
    ma_exit(0);

    check(appslot_live() == 3, "2a 終了で生きている本数が 1 本だけ減る");
    check(appslot_reclaim_count == 1 && appslot_last_reclaim_id == 3,
          "2b 回収は終了した ID に対して 1 回だけ");
    check(appslot_last_reclaim_id == 3,
          "2c gui_owner_exit は終了した ID で呼ばれる");
    check(H.free_pages == 4096 - 300, "2d ページは 1 本分だけ返る");
    check(appslot_get(2) != 0 && H.res[2][MA_RES_FD] == 3 &&
          H.res[2][MA_RES_WINDOW] == 2,
          "2e 他のアプリの資源は 1 つも触られない");
    check(appslot_get(3) == 0, "2f 終了した ID は空く");
    check(appslot_cur() == APP_ID_SHELL, "2g 終了後は WM top-level へ戻る");
}

/* ---- 3. 切替は OP_WAIT の中でだけ (票 G7 / 契約 T2a) ---- */
static void case_switch_only_in_op_wait(void)
{
    u32 before;
    ma_init(4096);
    ma_start(100, 1);
    check(ma_park() < 0, "3a gui_call の外では park できない");
    ma_gui_call(MA_OP_POLL);
    check(ma_park() < 0, "3b OP_POLL の中では park できない");
    ma_gui_return();
    ma_gui_call(MA_OP_COMMIT);
    check(ma_park() < 0, "3c OP_COMMIT の中では park できない");
    ma_gui_return();
    before = ma_pd_switches();
    check(ma_pd_switches() == before, "3d park に失敗した間 CR3 は動かない");
    ma_gui_call(MA_OP_WAIT);
    check(ma_park() == 0, "3e OP_WAIT の中でだけ park できる");
    check(res_owner_get() == APP_ID_SHELL, "3f park で owner は 1 (WM) に戻る");
    check(ma_resume(2) == 0, "3g top-level から resume できる");
    check(ma_pd_switches() == before + 1,
          "3h CR3 が動くのは resume の 1 回だけ");
}

/* ---- 4. 走っているアプリの横取りは起きない ---- */
static void case_no_preemption(void)
{
    ma_init(4096);
    fill_four(100);
    ma_resume(2);
    check(ma_resume(3) == OS32_ERR_INVAL,
          "4a アプリが走っている間は別のアプリを起こせない");
    check(appslot_cur() == 2, "4b 横取りが弾かれても走っているのは同じ 1 本");
    check(ma_kill(3) == OS32_ERR_INVAL,
          "4c アプリが走っている間は WM の kill も走らない");
}

/* ---- 5. fault は他のアプリの資源に触らない (票 G5) ---- */
static void case_fault_isolated(void)
{
    int i;
    ma_init(4096);
    fill_four(100);
    for (i = 2; i <= 5; i++) {
        ma_resume(i);
        ma_res_add(MA_RES_FD, i);
        ma_res_add(MA_RES_SHM, 1);
        ma_gui_call(MA_OP_WAIT);
        ma_park();
    }
    ma_resume(3);
    check(ma_fault() == 0, "5a 走っているアプリは fault で畳める");
    check(appslot_live() == 3, "5b fault で減るのは 1 本だけ");
    check(appslot_get(2) != 0 && H.res[2][MA_RES_FD] == 2 &&
          H.res[2][MA_RES_SHM] == 1,
          "5c fault の前後で ID 2 の資源は不変");
    check(appslot_get(4) != 0 && H.res[4][MA_RES_FD] == 4 &&
          H.res[4][MA_RES_SHM] == 1,
          "5d fault の前後で ID 4 の資源は不変");
    check(H.free_pages == 4096 - 300, "5e 返るページは 1 本分だけ");
}

/* ---- 6. スロットと ID の対応は一意で、park/resume で動かない ---- */
static void case_slot_bijection(void)
{
    int i, j, s[4], id;
    ma_init(4096);
    fill_four(100);
    for (i = 0; i < 4; i++) s[i] = H.slot[APP_ID_MIN + i];
    check(s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3,
          "6a スロットは 0..3 が小さい順に配られる");
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            if (s[i] == s[j]) { check(0, "6b スロットが重複しない"); return; }
        }
    }
    check(1, "6b スロットが重複しない");
    ma_resume(4);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    check(H.slot[4] == 2, "6c park/resume でスロットは動かない");
    ma_resume(3);
    ma_exit(0);
    id = ma_start(100, 1);
    check(id == 3 && H.slot[3] == 1,
          "6d 空いた ID とスロットが次の起動へ回る");
}

/* ---- 7. メモリが足りなければ拒否する (D5、スワップしない) ---- */
static void case_nomem_refuses(void)
{
    int r;
    ma_init(250);
    ma_start(100, 1);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    ma_start(100, 1);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    r = ma_start(100, 1);
    check(r == EXEC_ERR_NOMEM, "7a 入らない要求は ERR_NOMEM で拒否");
    check(appslot_live() == 2, "7b 拒否で既存のアプリは減らない");
    check(H.free_pages == 50, "7c 拒否は空きページを 1 枚も動かさない");
    check(ma_start(50, 1) == 4, "7d 入る大きさなら 3 本目も起動できる");
}

/* ---- 8. CUI 互換: 入れ子 exec_run では 段 = ID ---- */
static void case_cui_nesting(void)
{
    int a, b, c;
    ma_init(4096);
    a = ma_start(10, 0);
    b = ma_start(10, 0);
    c = ma_start(10, 0);
    check(a == 2 && b == 3 && c == 4,
          "8a 入れ子 exec の ID は段の深さと同じ (2,3,4)");
    check(H.slot[2] == -1 && H.slot[4] == -1,
          "8b CUI の子は GUI スロットを取らない");
    check(res_owner_get() == 4, "8c 走っているのは最も深い段");
    ma_exit(0);
    check(appslot_cur() == 3 && res_owner_get() == 3,
          "8d 終了で 1 段だけ親へ戻る");
    ma_exit(0);
    check(appslot_cur() == 2 && res_owner_get() == 2, "8e さらに 1 段戻る");
    ma_exit(0);
    check(appslot_cur() == APP_ID_SHELL && res_owner_get() == APP_ID_SHELL,
          "8f 最後はシェル (ID 1) へ戻る");
    check(appslot_live() == 0 && H.free_pages == 4096,
          "8g 3 段ぶんのページが全部返る");
    fill_four(10);
    check(ma_start(10, 0) == OS32_ERR_FULL,
          "8h GUI 4 本のときは CUI の入れ子も ERR_FULL");
}

/* ---- 9. CTRL+STOP は走っているアプリ宛にしか立たない (票 G4) ---- */
static void case_abort_targets_running(void)
{
    ma_init(4096);
    fill_four(100);
    ma_resume(3);
    ma_abort_request();
    check(appslot_get(2) != 0 && appslot_get(2)->abort_req == 0,
          "9a 止めてあるアプリに CTRL+STOP は立たない");
    check(appslot_get(3) != 0 && appslot_get(3)->abort_req == 1,
          "9b 走っているアプリにだけ立つ");
    check(ma_abort_check() == 0, "9c 次の安全地点で畳まれる");
    check(appslot_live() == 3 && appslot_get(3) == 0,
          "9d 畳まれるのはその 1 本だけ");
    check(appslot_cur() == APP_ID_SHELL, "9e 畳んだあと WM top-level へ戻る");
    check(ma_kill(4) == 0, "9f 止めてあるアプリは kill で畳める");
    check(appslot_live() == 2, "9g kill も 1 本だけ");
}

/* ---- 10. 起動失敗はどの状態も動かさない ---- */
static void case_failed_start_is_clean(void)
{
    u32 sw;
    ma_init(4096);
    fill_four(100);
    sw = ma_pd_switches();
    check(ma_start(100, 1) == OS32_ERR_FULL, "10a 5 本目は起動しない");
    check(ma_pd_switches() == sw, "10b 起動失敗では CR3 を載せ替えない");
    check(res_owner_get() == APP_ID_SHELL, "10c 起動失敗で owner は 1 のまま");
    check(appslot_reclaim_count == 0, "10d 起動失敗では回収を回さない");
}

/* ---- 11. GUI の起動は WM top-level からだけ (契約 S2) ---- */
static void case_gui_start_only_from_toplevel(void)
{
    ma_init(4096);
    ma_start(100, 1);
    check(ma_start(100, 1) == OS32_ERR_INVAL,
          "11a 走っているアプリの中から GUI アプリは起動できない");
    check(appslot_live() == 1, "11b 弾かれた起動は本数を増やさない");
    check(H.free_pages == 4096 - 100, "11c 弾かれた起動はページを取らない");
    check(appslot_cur() == 2, "11d 弾かれても走っているのは元のアプリのまま");
    check(ma_start(10, 0) == 3, "11e CUI の入れ子 exec_run は従来どおり通る");
}

/* ---- 12. 同時 ready の選択規則 (D11) ---- */
static void case_pick_rule(void)
{
    ma_init(4096);
    fill_four(100);

    ma_set_ready(3, 1, 0);
    ma_set_ready(5, 1, 0);
    H.focus = 5;
    H.last_run = 2;
    check(ma_pick() == 5, "12a 入力群にフォーカスが居ればフォーカスを選ぶ");

    H.focus = 2;
    H.last_run = 3;
    check(ma_pick() == 5, "12b 入力群はラウンドロビン (last_run の次から)");

    ma_set_ready(3, 0, 0);
    ma_set_ready(5, 0, 0);
    ma_set_ready(2, 0, 1);
    ma_set_ready(4, 0, 1);
    H.focus = 2;
    H.last_run = 2;
    check(ma_pick() == 4, "12c 導出群もラウンドロビン");
    H.focus = 4;
    H.last_run = 4;
    check(ma_pick() == 2, "12d 導出群ではフォーカスを優先しない");

    ma_set_ready(5, 1, 0);
    H.focus = 2;
    H.last_run = 4;
    check(ma_pick() == 5, "12e 入力群は導出群より先");

    ma_set_ready(2, 0, 0);
    ma_set_ready(4, 0, 0);
    ma_set_ready(5, 0, 0);
    check(ma_pick() == 0, "12f ready が 1 本も無ければ誰も起こさない");
}

/* ---- 13. 走っているアプリが park すべきか (D11 (1)) ---- */
static void case_park_decision(void)
{
    ma_init(4096);
    fill_four(100);
    ma_resume(2);
    ma_gui_call(MA_OP_WAIT);

    ma_set_ready(2, 1, 0);
    ma_set_ready(4, 1, 0);
    check(ma_should_park() == 0, "13a 自分に入力があれば park しない");

    ma_set_ready(2, 0, 1);
    ma_set_ready(4, 0, 0);
    check(ma_should_park() == 0,
          "13b 他に ready が居なければ park しない (1 本のときの回帰ゼロ)");

    ma_set_ready(4, 1, 0);
    check(ma_should_park() == 1, "13c 他に入力があれば導出だけの自分は譲る");

    ma_set_ready(4, 0, 1);
    check(ma_should_park() == 1, "13d 他も導出だけなら巡回のため譲る");

    ma_set_ready(2, 0, 0);
    check(ma_should_park() == 1, "13e 自分が ready でなければ譲る");
}

/* ---- 14. 飢餓が起きない (導出群だけの 4 本が 1 周で全員走る) ---- */
static void case_no_starvation(void)
{
    int seen[APP_MAX_APPS];
    int order[4];
    int i, k;
    ma_init(4096);
    fill_four(100);
    for (i = 0; i < APP_MAX_APPS; i++) seen[i] = 0;
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) ma_set_ready(i, 0, 1);
    H.focus = 2;
    H.last_run = 0;

    for (k = 0; k < 4; k++) {
        int id = ma_pick();
        order[k] = id;
        if (id < APP_ID_MIN || id > APP_ID_MAX) {
            check(0, "14a 4 周とも 1 本選べる");
            return;
        }
        seen[id - APP_ID_MIN]++;
        ma_resume(id);
        ma_gui_call(MA_OP_WAIT);
        ma_park();
    }
    check(1, "14a 4 周とも 1 本選べる");
    check(order[0] == 2 && order[1] == 3 && order[2] == 4 && order[3] == 5,
          "14b 巡回の順は ID 昇順 (2,3,4,5)");
    check(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1,
          "14c 4 周で全員がちょうど 1 回ずつ走る (飢餓なし)");
    check(ma_pick() == 2, "14d 1 周したら先頭へ戻る");
}

/* ---- 15. 自作入力で park を回避できないこと (レビュー反例 1) ---- */
#define MA_STARVE_LIMIT 200
static void case_self_input_cannot_starve(void)
{
    int i, b_ran = 0, b_at = 0, parks = 0;
    ma_init(4096);
    ma_start(100, 1); ma_gui_call(MA_OP_WAIT); ma_park();
    ma_start(100, 1); ma_gui_call(MA_OP_WAIT); ma_park();
    ma_set_ready(3, 0, 1);
    H.focus = 2;
    ma_resume(2);

    for (i = 0; i < MA_STARVE_LIMIT; i++) {
        ma_gui_call(MA_OP_WAIT);
        ma_set_ready(2, 1, 0);
        if (ma_should_park()) {
            int k;
            ma_park();
            parks++;
            k = ma_pick();
            if (k <= 0) {
                check(0, "15a 自作入力を続けても park は必ず起きる");
                return;
            }
            ma_resume(k);
        } else {
            ma_gui_return();
        }
        if (appslot_cur() == 3 && !b_ran) { b_ran = 1; b_at = i + 1; }
    }
    check(parks > 0, "15a 自作入力を続けても park は必ず起きる");
    check(b_ran, "15b Paint 待ちの B が走る (飢餓しない)");
    check(b_ran && b_at == (2 * 2 - 2) * (MA_INPUT_STREAK_MAX + 1),
          "15c B は N=2 の上限 (10 回) にちょうど届く");
    check(b_ran && b_at <= MA_STARVE_BOUND,
          "15d B は定数の上限 (MA_STARVE_BOUND) の内側");
}

/* ---- 16. ラウンドをまたぐ待ちの上限 (レビュー反例 2) ---- */
static int run_cross_round(int focus_id)
{
    int i, n, k;
    ma_init(4096);
    for (i = 0; i < 4; i++) {
        ma_start(100, 1);
        ma_gui_call(MA_OP_WAIT);
        ma_park();
    }
    for (i = APP_ID_MIN; i <= APP_ID_MAX; i++) ma_set_ready(i, 1, 0);
    H.focus = APP_ID_MIN;
    k = ma_pick();
    if (k != APP_ID_MIN) return -1;
    ma_resume(k);
    ma_set_ready(APP_ID_MIN, 0, 1);
    H.focus = focus_id;
    for (n = 0; n < 200; n++) {
        ma_gui_call(MA_OP_WAIT);
        if (ma_should_park()) {
            ma_park();
            k = ma_pick();
            if (k <= 0) return -2;
            ma_resume(k);
            if (k == APP_ID_MIN) return n;
        } else {
            ma_gui_return();
        }
    }
    return 0;
}

static void case_cross_round_bound(void)
{
    int a2, a3, a5;
    a2 = run_cross_round(APP_ID_MIN);
    a3 = run_cross_round(APP_ID_MIN + 1);
    a5 = run_cross_round(APP_ID_MAX);
    check(a2 > 0, "16a 導出群の A は必ず走る (無限待ちにならない)");
    check(a2 <= MA_STARVE_BOUND, "16b A は再導出した上限以内に走る");
    check(a2 == MA_STARVE_BOUND,
          "16c 上限は緩くない (この構成でちょうど上限に届く)");
    check(a3 > 0 && a3 <= MA_STARVE_BOUND,
          "16d フォーカスが B でも上限を超えない");
    check(a5 > 0 && a5 <= MA_STARVE_BOUND,
          "16e フォーカスが D でも上限を超えない");
}

/* ---- 17. 印なし resume の拒否と、G7 の 4 カウンタ (票 K5b の追加) ----
 *  D0 の C5/C6: park は「OP_WAIT 由来」の印を立て、resume はその印のある
 *  フレームだけを起こす。印を人為的に落として負例を作る。 */
static void case_resume_needs_wait_mark(void)
{
    AppSlot *a;
    u32 bad0, sw0, rej0, tr0;

    ma_init(4096);
    fill_four(100);
    sw0 = ring3_switch_count;
    bad0 = ring3_resume_bad_frame_count;
    rej0 = ring3_park_reject_count;
    tr0 = ring3_transition_count;

    a = appslot_get(4);
    a->parked_from_wait = 0;              /* 印の無いフレームに見せる */
    check(ma_resume(4) == OS32_ERR_STALE,
          "17a 印の無いフレームは OS32_ERR_STALE で拒否される");
    check(ring3_resume_bad_frame_count == bad0 + 1,
          "17b 拒否のたび ring3_resume_bad_frame_count が増える");
    check(ring3_switch_count == sw0,
          "17c 拒否は ring3_switch_count を増やさない");
    check(appslot_get(4)->state == APP_STATE_PARKED && appslot_cur() == 1,
          "17d 拒否されたアプリは park のまま、WM は top-level のまま");

    a->parked_from_wait = 1;              /* 印を戻せば起きる */
    check(ma_resume(4) == 0, "17e 印のあるフレームは起こせる");
    check(ring3_switch_count == sw0 + 1 &&
          ring3_resume_bad_frame_count == bad0 + 1,
          "17f 成功は switch_count だけを増やす");
    check(appslot_get(4)->parked_from_wait == 0,
          "17g resume した時点で印は消える (二度は起こせない)");

    /* OP_WAIT 以外からの park は弾かれ、park_reject_count だけが増える */
    ma_gui_call(MA_OP_POLL);
    check(ma_park() == OS32_ERR_INVAL && ma_park() == OS32_ERR_INVAL,
          "17h OP_WAIT 以外の op からの park は 2 回とも弾かれる");
    check(ring3_park_reject_count == rej0 + 2,
          "17i 弾いた回数が ring3_park_reject_count に載る");
    ma_gui_return();
    check(ring3_transition_count == tr0,
          "17j park/resume は transition_count を動かさない (別勘定)");

    /* CUI の入れ子 exec_run の子は park できない: longjmp の行き先が
     * 「走っている親アプリの中の exec_run フレーム」になり WM へ戻れない。
     * 親は子の終了まで塞がっているので譲る相手も居ない (D4)。 */
    {
        /* ここまでで ID 4 が走っている (17e で resume した)。 */
        u32 rej1 = ring3_park_reject_count;
        check(ma_start(10, 0) == OS32_ERR_FULL,
              "17k GUI 4 本のときは CUI の子も立たない (池は 1 つ)");
        ma_exit(0);                       /* 走っている ID 4 を畳んで枠を空ける */
        check(appslot_cur() == APP_ID_SHELL && ma_start(10, 0) == 4,
              "17l 空いた枠に CUI の子 (gui=0) が立つ");
        ma_gui_call(MA_OP_WAIT);
        check(ma_park() == OS32_ERR_INVAL,
              "17m CUI の入れ子の子は OP_WAIT の中でも park できない");
        check(ring3_park_reject_count == rej1 + 1,
              "17n その拒否も park_reject_count に載る");
        ma_gui_return();
    }
}

/* ---- 18. シェル帯 (ID 1) は per-app 経路に巻き込まれない ----
 * 2026-09-11 の差し戻し (実機初回起動で「FATAL: shell.bin load failed」) で
 * 立てた境界。shell.bin も gshell.bin も exec ネスト段 0 = ID 1 で、
 * 0x300000 の MEM_SHELL_* 帯に identity で載る CPL=0 プログラム。
 * pgalloc の空きがどれだけ少なくても、per-app 物理・ID の池・枚数勘定の
 * どれにも掛からずに起動できなければならない (K5a 設計 D7「変えないもの」)。
 * 判定の実体は exec/appslot.c の appslot_launch_is_app() で、
 * exec_launch の want_ring3 はこれをそのまま使う。 */
static void case_shell_never_takes_app_band(void)
{
    AppSlot *sh;

    /* (a) 帯の判定 — シェルは flags に関わらずアプリ帯を使わない */
    check(appslot_launch_is_app(1, 0) == 0,
          "18a shell.bin (ネスト段 0) はアプリ帯を使わない");
    check(appslot_launch_is_app(1, OS32X_FLAG_FORCE_CPL0) == 0,
          "18b gshell.bin も同じ経路 — --cpl0 の有無で変わらない");
    check(appslot_launch_is_app(0, 0) == 1,
          "18c 子プログラムは従来どおりアプリ帯 (CPL=3)");
    check(appslot_launch_is_app(0, OS32X_FLAG_FORCE_CPL0) == 0,
          "18d --cpl0 の子は従来どおり identity (アプリ帯ではない)");

    /* (b) 物理が尽きていてもシェルは立つ。同じ空きでアプリは弾かれる。 */
    ma_init(1);          /* 空き 1 枚。0 は admit の「勘定しない」合図なので使わない */
    check(ma_start(64, 1) == EXEC_ERR_NOMEM,
          "18e 空き 1 枚では GUI アプリの起動は ERR_NOMEM");
    appslot_shell_commit();          /* exec_launch の is_shell 経路はこれだけ */
    sh = appslot_get(APP_ID_SHELL);
    check(sh != 0 && sh->state == APP_STATE_RUNNING &&
          appslot_cur() == APP_ID_SHELL,
          "18f 同じ空きでもシェルは起動する (枚数勘定を通らない)");
    check(sh->depth == 1 && sh->cpl3 == 0,
          "18g シェルは段 1 の CPL=0 (アプリ PD を持たない)");
    check(H.free_pages == 1,
          "18h シェルの起動は空きページを 1 枚も動かさない");
    check(appslot_live() == 0,
          "18i シェルは非シェル ID の池を消費しない");
    check(appslot_alloc_id() == APP_ID_MIN,
          "18j 池は手つかず — 次のアプリは ID 2 から");
    check(res_owner_get() == APP_ID_SHELL,
          "18k 資源の所有者はシェル帯 (owner 1) のまま");

    /* (c) gshell ⇔ CUI shell の載せ替え (sys_switch_shell / K4 の起動ループ)
     *     を繰り返しても、同じ ID 1 に留まり池も空きも動かない。 */
    appslot_shell_commit();
    appslot_shell_commit();
    check(appslot_cur() == APP_ID_SHELL && appslot_live() == 0 &&
          appslot_get(APP_ID_SHELL)->depth == 1,
          "18l 載せ替えを繰り返しても ID 1 / 段 1 のまま");
    check(H.free_pages == 1 && appslot_alloc_id() == APP_ID_MIN,
          "18m 載せ替えは空きも池も動かさない");
}

/* ---- 19. CPL=3 アプリが生きている間は --cpl0 の子を立てない ----
 * 申し送り A1 / 決裁 2026-09-11。--cpl0 の子は exec_cpl0_claim() で
 * アプリ帯 [0x500000, mem_end) を identity で丸ごと pgalloc_mark_used し、
 * exec_cpl0_release() で丸ごと free する。K5b-K 以後は CPL=3 アプリの
 * per-app 物理も同じ pgalloc から取るので、park 中のアプリが 1 本でも居る
 * ところへ --cpl0 の子を立てると、生きているアプリの物理を上書きし、
 * その子の終了で他人のページを解放する。枚数で刻む機構は増やさず、
 * **生存アプリが 1 本でも居たら exec_run の段階で拒否**する。
 * 拒否は exec_cpl0_claim より前 — claim も alloc も 1 つも行わない。 */
static void case_cpl0_child_needs_no_live_apps(void)
{
    u32 free0;
    int owner0;
    int r;

    /* (a) 生存アプリなし = 従来どおり立つ。帯を丸ごと claim する。 */
    ma_init(4096);
    check(appslot_cpl0_admit(0, 0) == 0,
          "19a 生存アプリが 0 本なら --cpl0 の子は通る");
    check(ma_start_cpl0(0) == APP_ID_MIN,
          "19b 生存アプリなしの --cpl0 の子は従来どおり ID 2 で立つ");
    check(H.cpl0_children == 1 && H.free_pages == 4096 - MA_CPL0_BAND_PAGES,
          "19c 立った --cpl0 の子は帯を丸ごと claim する");
    check(ma_exit_cpl0() == 0 && H.cpl0_children == 0 &&
          H.free_pages == 4096,
          "19d 終了で帯は丸ごと返り、池も空く");
    check(appslot_live() == 0 && appslot_cur() == APP_ID_SHELL,
          "19e --cpl0 の子は 1 本も残さない");

    /* (b) 走行中のアプリが 1 本 = 拒否。何も動かない。 */
    ma_init(4096);
    check(ma_start(100, 1) == 2, "19f 下ごしらえ: GUI アプリ 1 本が走る");
    free0 = H.free_pages;
    owner0 = res_owner_get();
    r = ma_start_cpl0(0);
    check(r == OS32_ERR_FULL,
          "19g 走行中のアプリが 1 本でも居れば --cpl0 の子は ERR_FULL");
    check(appslot_live() == 1 && appslot_get(2) != 0 &&
          appslot_get(2)->state == APP_STATE_RUNNING &&
          appslot_get(3) == 0,
          "19h 拒否で AppSlot は 1 つも変わらない");
    check(H.free_pages == free0 && H.cpl0_children == 0,
          "19i 拒否は claim も alloc もしない (帯を押さえない)");
    check(res_owner_get() == owner0 && appslot_cur() == 2,
          "19j 拒否で資源の所有者も現在の ID も動かない");
    check(appslot_alloc_id() == 3,
          "19k 拒否は池を消費しない (次の空きは 3 のまま)");

    /* (c) park 中のアプリが 1 本 = 同じく拒否 (走行中かどうかは問わない)。 */
    ma_init(4096);
    ma_start(100, 1);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    check(appslot_cur() == APP_ID_SHELL && appslot_live() == 1,
          "19l 下ごしらえ: GUI アプリ 1 本が park 中で WM top-level");
    free0 = H.free_pages;
    /* && で短絡させない (RED でも後ろが素通りしてしまう) */
    r = appslot_cpl0_admit(0, 0);
    check(r == OS32_ERR_FULL,
          "19m park 中のアプリが 1 本でも居れば判定で弾かれる");
    r = ma_start_cpl0(0);
    check(r == OS32_ERR_FULL,
          "19n park 中のアプリが 1 本でも居れば --cpl0 の子は ERR_FULL");
    check(appslot_live() == 1 && appslot_get(2) != 0 &&
          appslot_get(2)->state == APP_STATE_PARKED &&
          appslot_get(2)->parked_from_wait == 1 && appslot_get(3) == 0,
          "19o park 中のアプリは印ごと無傷で、池も空いたまま");
    check(H.free_pages == free0 && H.cpl0_children == 0,
          "19p park 中でも拒否は帯を押さえない");

    /* (d) 4 本 (満杯) でも同じ拒否。池が尽きる前に判定でも弾かれる。 */
    ma_init(4096);
    fill_four(100);
    free0 = H.free_pages;
    r = appslot_cpl0_admit(0, 0);
    check(appslot_live() == 4 && r == OS32_ERR_FULL,
          "19q 4 本生きていれば --cpl0 の判定でも弾かれる");
    /* 4 本のときは池も尽きているので ma_start_cpl0 の戻りは同じ
     * OS32_ERR_FULL。判定そのものは 1 つ上の 19q が押さえている。 */
    r = ma_start_cpl0(0);
    check(r == OS32_ERR_FULL && H.free_pages == free0 &&
          H.cpl0_children == 0,
          "19r 4 本のときの拒否も帯を押さえない");

    /* (e) 全部畳めば元どおり立つ。「閉じてから使え」が成り立つこと。 */
    ma_resume(2); ma_exit(0);
    ma_resume(3); ma_exit(0);
    ma_resume(4); ma_exit(0);
    ma_resume(5); ma_exit(0);
    check(appslot_live() == 0, "19s 下ごしらえ: GUI アプリを全部閉じた");
    check(ma_start_cpl0(0) == APP_ID_MIN,
          "19t アプリを全部閉じれば --cpl0 の子は立つ");
    check(H.cpl0_children == 1, "19u そのときは帯を claim する");
    ma_exit_cpl0();

    /* (f) シェル (exec ネスト段 0) は対象外 — CUI shell / gshell の載せ替えは
     *     アプリが生きていても通らなければならない (K5a 設計 D7)。 */
    ma_init(4096);
    ma_start(100, 1);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    free0 = H.free_pages;
    check(appslot_cpl0_admit(1, 0) == 0,
          "19v シェルは --cpl0 の判定の対象外 (アプリが生きていても通る)");
    check(ma_start_cpl0(1) == APP_ID_SHELL && H.cpl0_children == 0 &&
          H.free_pages == free0,
          "19w シェルの起動は帯も枚数も動かさない");
    check(appslot_live() == 1 && appslot_get(2) != 0,
          "19x シェルの載せ替えで park 中のアプリは消えない");

    /* (g) CUI の入れ子 exec_run (--cpl0 でない子) は従来どおり通る。 */
    ma_init(4096);
    check(ma_start(100, 0) == 2, "19y --cpl0 でない CUI の子は従来どおり立つ");
    check(ma_start(100, 0) == 3,
          "19z その入れ子 (アプリ帯の CPL=3) も従来どおり立つ");
}

/* ---- 20. CTRL+STOP の要求を降ろす (KAPI v45 exec_abort_clear、決裁 A1) ----
 *
 * IRQ1 は宛先を選べず「いま走っているアプリ」に立てるが、契約 T6 の宛先は
 * フォーカス窓のアプリ。WM は本人の要求をこれで降ろしてからフォーカス窓の
 * ID を exec_kill で畳む。降ろせないと本人が次の安全地点で畳まれ、
 * 「意図しない 1 本が死ぬ」(票 K5b-W の A1 が書いた残る穴)。
 */
static void case_abort_clear(void)
{
    int owner0;
    int r;

    /* (a) owner 1 以外からは降ろせない (gui_register と同じ判定)。
     *     && で短絡させない (RED でも後ろが素通りしてしまう)。 */
    ma_init(4096);
    fill_four(100);
    ma_resume(3);
    ma_abort_request();
    check(appslot_get(3) != 0 && appslot_get(3)->abort_req == 1,
          "20a 下ごしらえ: 走っている 3 に CTRL+STOP が立つ");
    owner0 = res_owner_get();
    r = ma_abort_clear();
    check(owner0 == 3, "20b 下ごしらえ: owner は走っているアプリ");
    check(r == OS32_ERR_INVAL, "20c owner 1 以外からは OS32_ERR_INVAL");
    check(appslot_get(3) != 0 && appslot_get(3)->abort_req == 1,
          "20d 弾かれた呼び出しは要求を降ろさない");

    /* (b) park で WM top-level (owner 1) へ戻れば降ろせる。要求は park して
     *     も残るので、状態 (RUNNING/PARKED) では対象を絞らない。 */
    ma_gui_call(MA_OP_WAIT);
    r = ma_park();
    check(r == 0 && res_owner_get() == APP_ID_SHELL,
          "20e 下ごしらえ: park して WM top-level へ戻る");
    check(appslot_get(3) != 0 && appslot_get(3)->abort_req == 1,
          "20f 要求は park をまたいで残る");
    r = ma_abort_clear();
    check(r == 0, "20g owner 1 からは 0");
    check(appslot_get(3) != 0 && appslot_get(3)->abort_req == 0,
          "20h 要求が降りている");

    /* (c) 降ろすだけ — 他の ID にも、対象の ID の他の欄にも触らない。 */
    check(appslot_live() == 4 && appslot_reclaim_count == 0,
          "20i 降ろすだけで 1 本も畳まない");
    check(appslot_get(2) != 0 && appslot_get(2)->state == APP_STATE_PARKED &&
          appslot_get(2)->parked_from_wait == 1,
          "20j 別の ID は印ごと無傷");
    check(appslot_get(5) != 0 && appslot_get(5)->state == APP_STATE_PARKED &&
          appslot_get(5)->parked_from_wait == 1,
          "20k 別の ID は印ごと無傷 (末尾も)");
    check(appslot_get(3)->state == APP_STATE_PARKED &&
          appslot_get(3)->parked_from_wait == 1,
          "20l 対象の ID も abort_req 以外は動かない");

    /* (d) 降ろした後は次の安全地点で畳まれない (A1 の残る穴がふさがる)。 */
    r = ma_resume(3);
    check(r == 0, "20m 下ごしらえ: 3 を起こし直す");
    r = ma_abort_check();
    check(r == 0, "20n 降ろした後の安全地点は畳まない");
    check(appslot_live() == 4 && appslot_get(3) != 0 &&
          appslot_reclaim_count == 0,
          "20o 意図しない 1 本が死なない");

    /* (e) 要求が無ければ何も起きない (二度目も 0)。 */
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    r = ma_abort_clear();
    check(r == 0, "20p 要求が無くても 0");
    check(appslot_live() == 4 && appslot_reclaim_count == 0,
          "20q 要求が無いときは 1 本も動かさない");

    /* (f) 走っているアプリが 1 本も居ないときも 0 で、何も起きない。 */
    ma_init(4096);
    r = ma_abort_clear();
    check(r == 0, "20r アプリが 1 本も居なければ 0");
    check(appslot_live() == 0 && appslot_cur() == APP_ID_SHELL &&
          appslot_reclaim_count == 0,
          "20s 居なければ何も起きない");
}

/* ---- 19. GUI 中の kbd 待ち = 第 2 の park 点 (票 K7 D1 / D5 / §5 B) ----
 * K5b までの park 点は gui_call(OP_WAIT) の 1 つだけだった。K7 で
 * kbd_getchar / kbd_getkey が 2 つ目になり、状態 WAIT_KEY と印
 * parked_from_kbd が増える。ここで固定するのは 4 つ:
 *   (a) park すると WAIT_KEY + 印 + cur はシェル帯 (exec_app_state は 3)
 *   (b) 注入が空なら resume は OS32_ERR_AGAIN で、印も状態もそのまま
 *       (WM はその周を譲ってもう一度試せる)
 *   (c) 注入があれば resume が通り、**1 バイトだけ** EAX に入る
 *   (d) 印の取り違え (OP_WAIT の印で WAIT_KEY を起こす) は STALE、
 *       鍵待ちのアプリは exec_kill で畳める (D5) */
static void case_wait_key(void)
{
    int id;
    AppSlot *a;
    u32 sw0, bad0, rej0, park0;

    ma_init(4096);
    kbd_inject_discard();
    host_reader = 2;
    id = ma_start(100, 1);
    check(id == APP_ID_MIN, "19a GUI アプリが 1 本走る");
    sw0 = ring3_switch_count;
    bad0 = ring3_resume_bad_frame_count;
    rej0 = ring3_park_reject_count;
    park0 = ring3_kbd_park_count;

    /* (a) OP_WAIT の中でなくても park できる — 呼び手は kbd の syscall */
    check(ma_park_kbd() == 0, "19b OP_WAIT の外でも kbd 待ちなら park できる");
    check(ring3_kbd_park_count == park0 + 1,
          "19c ring3_kbd_park_count が増える");
    check(ring3_park_reject_count == rej0, "19d 正常な park は弾き数に載らない");
    a = appslot_get(id);
    check(a->state == APP_STATE_WAIT_KEY && a->parked_from_kbd == 1,
          "19e WAIT_KEY + kbd 由来の印");
    check(a->parked_from_wait == 0, "19f OP_WAIT の印は立たない");
    check(appslot_cur() == APP_ID_SHELL && res_owner_get() == APP_ID_SHELL,
          "19g cur と owner はシェル帯 (WM top-level) へ戻る");
    check(appslot_state(id) == APP_STATE_WAIT_KEY,
          "19h exec_app_state は 3 を返す");

    /* (b) 注入が空なら起こせない。印も状態も動かない */
    check(kbd_inject_pending() == 0, "19i 注入リングは空");
    check(ma_resume_kbd(id) == OS32_ERR_AGAIN,
          "19j 空の resume は OS32_ERR_AGAIN");
    check(ring3_switch_count == sw0, "19k 拒否は switch_count を増やさない");
    check(ring3_resume_bad_frame_count == bad0,
          "19l 空は「印なし」ではないので bad_frame_count も増えない");
    check(appslot_get(id)->state == APP_STATE_WAIT_KEY &&
          appslot_get(id)->parked_from_kbd == 1,
          "19m 印も状態もそのまま (次の周でもう一度試せる)");

    /* (c) 注入があれば 1 バイトだけ EAX に入る */
    res_owner_set(2);                      /* 端末アプリ (読み手) から注ぐ */
    check(kbd_inject((const u8 *)"ab", 2) == 2, "19n 端末アプリが 2 バイト注ぐ");
    res_owner_set(APP_ID_SHELL);           /* resume を呼ぶのは WM */
    check(ma_resume_kbd(id) == 0, "19o 注入があれば起こせる");
    check(appslot_get(id)->frame[APP_FRAME_EAX] == (u32)'a',
          "19p EAX には最初の 1 バイトだけが入る");
    check(kbd_inject_pending() == 1, "19q 残りは 1 バイト (まとめて渡さない)");
    check(ring3_switch_count == sw0 + 1, "19r 成功は switch_count を 1 増やす");
    check(appslot_get(id)->parked_from_kbd == 0,
          "19s 起こした時点で印は消える");

    /* (d) 印の取り違えと kill */
    check(ma_park_kbd() == 0, "19t もう一度 kbd 待ちで park できる");
    a = appslot_get(id);
    a->parked_from_kbd = 0;
    a->parked_from_wait = 1;               /* OP_WAIT の印だけに見せる */
    check(ma_resume_kbd(id) == OS32_ERR_STALE,
          "19u WAIT_KEY を OP_WAIT の印では起こせない");
    check(ring3_resume_bad_frame_count == bad0 + 1,
          "19v 印の取り違えは bad_frame_count に載る");
    check(appslot_kill_check(id) == 0,
          "19w 鍵待ちのアプリは exec_kill で畳める (D5)");
    a->parked_from_kbd = 1;
    check(ma_resume_kbd(id) == 0, "19x 印を戻せば起こせる (残りの 'b')");
    check(appslot_get(id)->frame[APP_FRAME_EAX] == (u32)'b',
          "19y 2 バイト目が次の resume で届く");
    check(kbd_inject_pending() == 0, "19z 注入リングは空に戻る");

    /* 走っている本人は kill できない / CUI の入れ子の子は park できない */
    check(appslot_kill_check(id) == OS32_ERR_INVAL,
          "19A 走っている間は exec_kill を呼べない (畳むのは CTRL+STOP)");
    {
        u32 rej1 = ring3_park_reject_count;
        int child = ma_start(10, 0);
        check(child > 0, "19B CUI の入れ子の子が立つ");
        check(ma_park_kbd() == OS32_ERR_INVAL,
              "19C CUI の入れ子の子は kbd 待ちでも park できない");
        check(ring3_park_reject_count == rej1 + 1,
              "19D その拒否は park_reject_count に載る");
        ma_exit(0);
    }
    ma_exit(0);
    check(appslot_cur() == APP_ID_SHELL, "19E 畳んだら WM top-level へ戻る");
    check(ma_park_kbd() == OS32_ERR_INVAL,
          "19F シェル帯 (WM top-level) からの park は弾かれる");
    kbd_inject_discard();
}


/* ======================================================================== */
/*  ケース 20 — 画面の所有者 (票 T8 D1 / D1a / D3、2026-09-12)               */
/*                                                                          */
/*  全画面 GFX プログラムが gshell 配下で走ると、画面の持ち主が 1 本に決まり  */
/*  ((a) 遷移)、宣言していないプログラムは画面を取れず ((b) 拒否)、VRAM を    */
/*  直接触る --cpl0 は GUI から起動できない ((c))。3 つとも実物の            */
/*  exec/appslot.c を叩く。                                                  */
/* ======================================================================== */
static void case_gfx_screen_owner(void)
{
    int id, other;
    u32 rej0;

    /* --- (a) 遷移: gfx_init で取り、回収で WM へ戻る ------------------- */
    ma_init(4096);
    check(appslot_gfx_owner() == GFX_OWNER_WM,
          "20a 起動直後の画面の所有者は WM (1)");

    id = ma_start_gfx(100, 1, OS32X_FLAG_GFX);
    check(id == APP_ID_MIN, "20b 下ごしらえ: 宣言付きの GFX アプリが 1 本立つ");
    check(appslot_gfx_owner() == GFX_OWNER_WM,
          "20c 起動しただけでは画面は WM のまま (gfx_init を呼んでいない)");

    check(appslot_gfx_claim(0) == 0 && appslot_gfx_owner() == GFX_OWNER_WM,
          "20d CUI 中 (con_sink 無効) は所有者を触らない");

    check(appslot_gfx_claim(1) == 0, "20e GUI 中の gfx_init は通る");
    check(appslot_gfx_owner() == id,
          "20f 画面の所有者は gfx_init を呼んだアプリへ移る");

    check(appslot_gfx_claim(1) == 0 && appslot_gfx_owner() == id,
          "20g 同じアプリが二度呼んでも所有者は変わらない");

    appslot_gfx_owner_exit(GFX_OWNER_WM);
    check(appslot_gfx_owner() == id,
          "20h 他人 (WM) の回収では所有者は戻らない");

    check(ma_exit(0) == 0, "20i アプリが終了する");
    check(appslot_gfx_owner() == GFX_OWNER_WM,
          "20j 所有者の回収で画面は WM へ戻る (D1)");

    /* kill (CTRL+STOP / exec_kill) でも同じ経路を通る。 */
    ma_init(4096);
    id = ma_start_gfx(100, 1, OS32X_FLAG_GFX);
    appslot_gfx_claim(1);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    check(appslot_gfx_owner() == id,
          "20k park しただけでは画面はアプリのまま (WM は上書きしない)");
    check(ma_kill(id) == 0 && appslot_gfx_owner() == GFX_OWNER_WM,
          "20l exec_kill で畳んでも画面は WM へ戻る");

    /* 2 本目が取った画面は、1 本目の回収では戻らない。 */
    ma_init(4096);
    id = ma_start_gfx(100, 1, OS32X_FLAG_GFX);
    ma_gui_call(MA_OP_WAIT);
    ma_park();
    other = ma_start_gfx(100, 1, OS32X_FLAG_GFX);
    check(other == APP_ID_MIN + 1, "20m 下ごしらえ: 2 本目の GFX アプリ");
    check(appslot_gfx_claim(1) == 0 && appslot_gfx_owner() == other,
          "20n 2 本目が gfx_init を呼べば画面は 2 本目のもの");
    ma_exit(0);
    check(appslot_gfx_owner() == GFX_OWNER_WM, "20o 2 本目の回収で WM へ戻る");
    check(ma_resume(id) == 0, "20p 1 本目を起こす");
    ma_exit(0);
    check(appslot_gfx_owner() == GFX_OWNER_WM,
          "20q 画面を持っていない 1 本目の回収では所有者は動かない");

    /* --- (b) 宣言なしの拒否 (D1a) --------------------------------------- */
    ma_init(4096);
    rej0 = gfx_init_reject_count;
    id = ma_start_gfx(100, 1, 0);        /* app.conf に gfx 列が無いプログラム */
    check(id == APP_ID_MIN, "20r 下ごしらえ: 宣言の無いアプリが 1 本立つ");
    check(appslot_gfx_claim(1) == OS32_ERR_INVAL,
          "20s GUI 中に宣言の無い CPL=3 が gfx_init を呼べば ERR_INVAL");
    check(appslot_gfx_owner() == GFX_OWNER_WM,
          "20t 拒否で画面は WM のまま (gfx_init は呼ばれない)");
    check(gfx_init_reject_count == rej0 + 1,
          "20u 拒否は gfx_init_reject_count に載る");
    check(appslot_gfx_claim(0) == 0 && appslot_gfx_owner() == GFX_OWNER_WM,
          "20v CUI 中は宣言が無くても従来どおり通る");
    check(gfx_init_reject_count == rej0 + 1,
          "20w CUI 中の素通しは拒否として数えない");

    /* CPL=0 の子 (--cpl0) は宣言の有無に関わらず所有者を取らない。 */
    appslot_at(id)->cpl3 = 0;
    appslot_at(id)->hdr_flags = OS32X_FLAG_GFX;
    check(appslot_gfx_claim(1) == 0 && appslot_gfx_owner() == GFX_OWNER_WM,
          "20x CPL=0 の子は画面の所有者にならない");
    ma_exit(0);

    /* WM 自身 (シェル帯、owner 1) の復帰の gfx_init は素通し。 */
    check(appslot_cur() == APP_ID_SHELL, "20y WM top-level へ戻っている");
    check(appslot_gfx_claim(1) == 0 && appslot_gfx_owner() == GFX_OWNER_WM,
          "20z WM の復帰の gfx_init は所有者を動かさない");

    /* --- (c) --cpl0 は GUI から起動できない (D1) ------------------------ */
    ma_init(4096);
    check(appslot_cpl0_admit(0, 1) == OS32_ERR_INVAL,
          "20A 生存アプリが 0 本でも GUI からの --cpl0 は ERR_INVAL");
    check(ma_start_cpl0_gui(0, 1) == OS32_ERR_INVAL,
          "20B exec_start 経路の --cpl0 は起動しない");
    check(appslot_live() == 0 && H.cpl0_children == 0 &&
          H.free_pages == 4096,
          "20C 拒否は池も帯も 1 つも動かさない");
    check(appslot_alloc_id() == APP_ID_MIN,
          "20D 拒否は池を消費しない");
    check(ma_start_cpl0_gui(0, 0) == APP_ID_MIN,
          "20E CUI (exec_run) 経路は従来どおり通る");
    check(H.cpl0_children == 1, "20F CUI からは帯を claim する");
    ma_exit_cpl0();

    /* シェル (ネスト段 0) の載せ替えは gui に関わらず対象外。 */
    check(appslot_cpl0_admit(1, 1) == 0,
          "20G シェル帯の載せ替えは GUI 判定の対象外");
}

int main(void)
{
    failures = 0;
    checks = 0;
    report("multiapp impl (K5b-K, exec/appslot.c)\n");
    case_fifth_refused();
    case_exit_reclaims_one();
    case_switch_only_in_op_wait();
    case_no_preemption();
    case_fault_isolated();
    case_slot_bijection();
    case_nomem_refuses();
    case_cui_nesting();
    case_abort_targets_running();
    case_failed_start_is_clean();
    case_gui_start_only_from_toplevel();
    case_pick_rule();
    case_park_decision();
    case_no_starvation();
    case_self_input_cannot_starve();
    case_cross_round_bound();
    case_resume_needs_wait_mark();
    case_shell_never_takes_app_band();
    case_cpl0_child_needs_no_live_apps();
    case_abort_clear();
    case_wait_key();
    case_gfx_screen_owner();
    if (checks < 84) {
        report("TOO FEW CHECKS (K5a の 84 検査を下回った)\n");
        die(1);
    }
    if (failures) {
        report("FAILURES\n");
        die(1);
    }
    report("ALL PASS\n");
    die(0);
    return 0;
}

void _start(void)
{
    main();
}
