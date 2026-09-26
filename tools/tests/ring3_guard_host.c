/* ========================================================================
 *  ring3_guard_host.c — WM の文脈では KAPI の出力検査を効かせない (実物で)
 *
 *  対象票: docs/tasks/memory/TASK_KAPI_OUTPUT_GUARD.md (追補 2026-09-26)
 *  実行:   python3 -B tools/tests/test_ring3_guard.py
 *  記録:   tools/tests/ring3_guard_tdd.md
 *
 *  症状: GUI で filer.bin を起動すると窓が出ずに静かに消え、fault_kill_count
 *  が +1。ブレークで捕まえると wrap_mouse_poll の出力検査
 *  (ring3_user_ranges_writable) が偽で ring3_fault_kill。ポインタは gshell
 *  自身のスタック (0x37ea88、シェル帯) だった。
 *
 *  原因: WM (gshell、CPL=0) は**アプリの syscall の中で**走る (契約 T8) ので、
 *  その間 ring3_in_syscall = 1 のまま。3 つの門 (書き側 / 読み側 / tramp_copy)
 *  は ring3_in_syscall だけで「アプリ由来」と決めていたので、WM 自身の
 *  ポインタがアプリの出力先として PTE を見られ、シェル帯には USER が無いので
 *  拒否 → アプリが kill された。
 *
 *  直し: 「カーネルが WM のコードへ入っている深さ」(ring3_wm_depth) を
 *  gui_call のハンドラ / ポンプ / owner_exit の前後で数え、門の判定は
 *  ring3_guard_active(in_syscall, wm_depth) (exec/ring3_str.c) に寄せる。
 *
 *  ここで実物のまま #include して見るのは 2 つ:
 *    (1) exec/ring3_str.c  — 判定表 (in_syscall × wm_depth)
 *    (2) kernel/gui.c      — 入口の配線: ハンドラの**中**では深さが 1 以上、
 *                            戻った後は元どおり。入れ子の gui_call でも同じ。
 *                            owner_exit も同じ印を立てる。
 *  kernel/gui.c の外部参照 (res_owner_get / appslot_gui_op_* / kbd_set_gui_mode
 *  / ime_set_render / kstrncpy / kmemset / ring3_wm_enter / ring3_wm_leave) は
 *  ここで贋物にする。ring3_wm_enter / leave の贋物は exec/exec.c の実装と
 *  同じ 2 行 (深さ +1 / 0 で止める -1)。
 *
 *  tools/tests/ring3_str_host.c と同じ様式 — ホスト ILP32 GNU89 で走らせ、
 *  libc は使わない (-nostdlib、Linux の int 0x80 で write/exit するだけ)。
 * ======================================================================== */

#include "types.h"

/* ---- カーネル帯の代わり ------------------------------------------------ */
char *kstrncpy(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    if (n == 0) return dst;
    while (i + 1 < n && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

void *kmemset(void *dst, int c, u32 n)
{
    u8 *p = (u8 *)dst;
    while (n--) *p++ = (u8)c;
    return dst;
}

/* fs/fd_redirect.c の res_owner_get — 試験が値を差し替える。 */
static int host_owner = 1;
int res_owner_get(void) { return host_owner; }

/* exec/appslot.c の OP_WAIT の控え — 呼ばれた回数だけ見る。 */
static int host_op_enter, host_op_leave;
void appslot_gui_op_enter(int is_wait) { (void)is_wait; host_op_enter++; }
void appslot_gui_op_leave(void) { host_op_leave++; }

void kbd_set_gui_mode(int on) { (void)on; }
void ime_set_render(void *r) { (void)r; }

/* exec/exec.c の ring3_wm_depth / enter / leave と同じ振る舞い。
 * 深さは試験が読む。 */
volatile int ring3_wm_depth = 0;
static int host_enter_calls, host_leave_calls;
void ring3_wm_enter(void) { host_enter_calls++; ring3_wm_depth++; }
void ring3_wm_leave(void) { host_leave_calls++; if (ring3_wm_depth > 0) ring3_wm_depth--; }

/* 実物 */
#include "ring3_str.c"
#include "gui.c"

/* ---- 最小の報告系 (libc 無し) ------------------------------------------ */

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

static void check(int cond, const char *name)
{
    report(cond ? "  ok   " : "  FAIL ");
    report(name);
    report("\n");
    if (!cond) failures++;
}

/* ========================================================================
 *  1. 判定表 — ring3_guard_active(in_syscall, wm_depth)
 * ======================================================================== */
static void case_table(void)
{
    report("1 ring3_guard_active の表\n");

    /* ディスパッチの外 (CPL=0 の直呼び) は深さに関係なく対象外 (従来どおり)。 */
    check(ring3_guard_active(0, 0) == 0, "1a CPL0 direct, depth 0 -> off");
    check(ring3_guard_active(0, 1) == 0, "1b CPL0 direct, depth 1 -> off");

    /* ディスパッチの中、WM に入っていない = アプリ由来 → ガード (従来どおり)。 */
    check(ring3_guard_active(1, 0) == 1, "1c in syscall, depth 0 -> ON");

    /* **これが直した点**: ディスパッチの中でも WM の文脈なら常駐側の直呼び扱い。 */
    check(ring3_guard_active(1, 1) == 0, "1d in syscall, in WM -> off");
    check(ring3_guard_active(1, 2) == 0, "1e in syscall, nested WM -> off");

    /* 壊れた深さ (負) は安全側 = ガード。 */
    check(ring3_guard_active(1, -1) == 1, "1f negative depth -> ON (safe side)");
}

/* ========================================================================
 *  2. gui_call の配線 — ハンドラの中では深さ 1 以上、戻ったら元どおり
 * ======================================================================== */
static int seen_depth_in_handler = -99;
static int seen_depth_nested = -99;
static int nest_once;
static u32 last_op;

static i32 host_handler(u32 op, u32 arg, int owner)
{
    (void)arg;
    (void)owner;
    last_op = op;
    seen_depth_in_handler = ring3_wm_depth;
    if (nest_once) {
        /* WM のハンドラの中から gui_call (入れ子)。深さは 2 になり、
         * 戻っても 1 のまま (このハンドラを抜けるまで WM の文脈)。 */
        nest_once = 0;
        (void)gui_call(GUI_OP_POLL, 0);
        seen_depth_nested = ring3_wm_depth;
    }
    return 7;
}

static i32 nested_probe(u32 op, u32 arg, int owner)
{
    (void)op; (void)arg; (void)owner;
    seen_depth_nested = ring3_wm_depth;
    return 0;
}

static void case_gui_call(void)
{
    i32 r;

    report("2 gui_call の前後で深さが対になる\n");

    /* 未登録なら NOSYS で、印も立てない。 */
    check(gui_call(GUI_OP_WAIT, 0) == OS32_ERR_NOSYS, "2a no WM -> NOSYS");
    check(host_enter_calls == 0 && ring3_wm_depth == 0, "2b no WM -> no mark");

    /* シェル帯 (owner 1) からの登録。 */
    host_owner = 1;
    check(gui_register((void *)host_handler, (void *)0) == 0, "2c register from owner 1");

    /* アプリ (owner 2) の gui_call: ハンドラの中は深さ 1、戻ると 0。 */
    host_owner = 2;
    r = gui_call(GUI_OP_WAIT, 5);
    check(r == 7, "2d handler result passes through");
    check(last_op == GUI_OP_WAIT, "2e op passes through");
    check(seen_depth_in_handler == 1, "2f depth is 1 inside the handler");
    check(ring3_wm_depth == 0, "2g depth is back to 0 after the handler");
    check(host_enter_calls == 1 && host_leave_calls == 1, "2h enter/leave paired once");
    /* 既存の OP_WAIT の控え (appslot) はそのまま対で呼ばれる。 */
    check(host_op_enter == 1 && host_op_leave == 1, "2i appslot op enter/leave untouched");

    /* 入れ子: ハンドラの中の gui_call で 2、戻って 1、抜けて 0。 */
    nest_once = 1;
    seen_depth_in_handler = -99;
    r = gui_call(GUI_OP_COMMIT, 0);
    check(r == 7, "2j nested call returns");
    check(seen_depth_nested == 1, "2k after nested gui_call depth is 1 (still in WM)");
    check(ring3_wm_depth == 0, "2l depth 0 after the outer handler");
    check(host_enter_calls == 3 && host_leave_calls == 3, "2m enter/leave paired for both levels");

    /* 入れ子の中の深さそのものは 2。 */
    host_owner = 1;
    check(gui_register((void *)nested_probe, (void *)0) == 0, "2n re-register probe");
    ring3_wm_enter();                       /* 外側の WM 文脈を装う */
    (void)gui_call(GUI_OP_POLL, 0);
    check(seen_depth_nested == 2, "2o depth is 2 inside a nested handler");
    ring3_wm_leave();
    check(ring3_wm_depth == 0, "2p back to 0");
}

/* ========================================================================
 *  3. owner_exit — exec_exit からの回収通知も WM の文脈として印を立てる
 * ======================================================================== */
static int exit_depth = -99;
static int exit_owner = -99;
static i32 exit_handler(u32 op, u32 arg, int owner)
{
    (void)arg;
    if (op == GUI_OP_OWNER_EXIT) {
        exit_depth = ring3_wm_depth;
        exit_owner = owner;
    }
    return 0;
}

static void case_owner_exit(void)
{
    int before_enter, before_leave;

    report("3 gui_owner_exit の前後で深さが対になる\n");

    host_owner = 1;
    check(gui_register((void *)exit_handler, (void *)0) == 0, "3a register");
    before_enter = host_enter_calls;
    before_leave = host_leave_calls;

    gui_owner_exit(3);                      /* アプリ (owner 3) の回収 */
    check(exit_depth == 1 && exit_owner == 3, "3b OWNER_EXIT runs with depth 1");
    check(ring3_wm_depth == 0, "3c depth back to 0");
    check(host_enter_calls == before_enter + 1 &&
          host_leave_calls == before_leave + 1, "3d enter/leave paired");

    /* WM 自身 (owner 1) の終了 = 登録解除。以後の gui_call は印を立てない。 */
    gui_owner_exit(GUI_SHELL_OWNER);
    before_enter = host_enter_calls;
    check(gui_call(GUI_OP_POLL, 0) == OS32_ERR_NOSYS, "3e WM gone -> NOSYS");
    check(host_enter_calls == before_enter && ring3_wm_depth == 0,
          "3f WM gone -> no mark");
    /* 未登録の owner_exit は何もしない (印も立てない)。 */
    gui_owner_exit(2);
    check(host_enter_calls == before_enter, "3g owner_exit without WM -> no mark");
}

int main(void)
{
    case_table();
    case_gui_call();
    case_owner_exit();
    if (failures) {
        report("ring3_guard_host: FAIL\n");
        die(1);
    }
    report("ring3_guard_host: all passed\n");
    return 0;
}

/* -nostdlib のエントリ。main の戻りで exit する。 */
void _start(void)
{
    int rc = main();
    die(rc);
}
