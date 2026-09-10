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
 * 全部この 1 つの ID で呼ぶ。ハーネスは「その ID の分だけ消える」を数える。 */
static void ma_reclaim_res(int id)
{
    int k;
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

static int ma_fault(void) { return ma_exit(-1); }

static int ma_abort_request(void)
{
    return appslot_abort_request() ? 0 : OS32_ERR_INVAL;
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
