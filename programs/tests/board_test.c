/* ======================================================================== */
/*  BOARD_TEST.C — libos32board テストプログラム                             */
/*                                                                          */
/*  KernelAPI kprintf でボードエンジンの全APIをテストする。                  */
/* ======================================================================== */

#include "os32api.h"
#include "libos32board.h"
#include "libos32db.h"
#include <string.h>

extern KernelAPI *kapi;
#define api kapi

static int g_total;
static int g_passed;

static void check(const char *label, int cond)
{
    g_total++;
    if (cond) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s\n", label);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s\n", label);
    }
}

static void check_eq(const char *label, int got, int expect)
{
    g_total++;
    if (got == expect) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s = %d\n", label, got);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s: got %d, expect %d\n",
                     label, got, expect);
    }
}

static void header(const char *title)
{
    api->kprintf(ATTR_CYAN, "\n=== %s ===\n", title);
}

/* ====================================================================== */
/*  テスト1: DB無し初期化                                                   */
/* ====================================================================== */

static void test_init_no_db(void)
{
    int rc;
    header("Test 1: Init (no DB)");

    rc = board_init(NULL);
    check_eq("board_init(NULL)", rc, 0);
    check_eq("mass_count=0", board_mass_count(), 0);
    check_eq("area_count=0", board_area_count(), 0);
    board_shutdown();
}

/* ====================================================================== */
/*  テスト2: DBロード                                                       */
/* ====================================================================== */

static void test_db_load(void)
{
    int rc;
    const BoardMass *m;

    header("Test 2: DB Load");

    rc = board_init("/host/assets/board.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] board_init returned %d "
            "(is board.db deployed?)\n", rc);
        return;
    }
    check("board_init(board.db)", rc == 0);

    /* マス数確認 */
    check_eq("mass_count=12", board_mass_count(), 12);

    /* マス0 (スタート) */
    m = board_get_mass(0);
    check("mass 0 exists", m != (const BoardMass *)0);
    if (m) {
        check_eq("mass0 type=4(start)", (int)m->type, 4);
        check_eq("mass0 area=0", (int)m->area, 0);
        check_eq("mass0 connect=1", (int)m->connect_count, 1);
        check_eq("mass0 connect[0]=1", (int)m->connect[0], 1);
    }

    /* マス2 (分岐) */
    m = board_get_mass(2);
    check("mass 2 exists", m != (const BoardMass *)0);
    if (m) {
        check_eq("mass2 type=1(event)", (int)m->type, 1);
        /* 接続: 1, 3, 5 (双方向で3本) */
        check_eq("mass2 connect=3", (int)m->connect_count, 3);
        check("mass2 is branch", board_has_branch(2));
    }

    /* マス4 (ゴール) */
    check_eq("mass4 type=3(goal)", (int)board_get_type(4), 3);

    /* 存在しないマス */
    m = board_get_mass(99);
    check("mass 99 = NULL", m == (const BoardMass *)0);

    /* 区画数確認 */
    check_eq("area_count=2", board_area_count(), 2);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト3: 接続・分岐                                                     */
/* ====================================================================== */

static void test_connections(void)
{
    u16 conns[8];
    int count;

    header("Test 3: Connections");

    board_init("/host/assets/board.db");

    /* マス1の接続取得 */
    count = board_get_connections(1, conns, 8);
    check_eq("mass1 conns=2", count, 2);

    /* 分岐マスの確認 */
    check("mass0 no branch", !board_has_branch(0));
    check("mass2 has branch", board_has_branch(2));
    check("mass4 no branch", !board_has_branch(4));

    /* 種別検索 */
    {
        u16 goals[4];
        int n = board_find_by_type(3, goals, 4);
        check_eq("type=3(goal) count=2", n, 2);
    }

    board_shutdown();
}

/* ====================================================================== */
/*  テスト4: 同マス判定                                                     */
/* ====================================================================== */

static void test_colocated(void)
{
    u16 positions[4];
    u8 indices[4];
    int found;

    header("Test 4: Colocated");

    board_init(NULL);

    positions[0] = 5;
    positions[1] = 3;
    positions[2] = 5;
    positions[3] = 7;

    found = board_check_colocated(5, positions, 4, indices, 4);
    check_eq("colocated count=2", found, 2);
    if (found >= 2) {
        check_eq("index0=0", (int)indices[0], 0);
        check_eq("index1=2", (int)indices[1], 2);
    }

    /* 誰もいないマス */
    found = board_check_colocated(99, positions, 4, indices, 4);
    check_eq("nobody = 0", found, 0);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト5: フラグ操作                                                     */
/* ====================================================================== */

static void test_flags(void)
{
    header("Test 5: Flags");

    board_init("/host/assets/board.db");

    /* 初期状態: フラグなし */
    check("mass1 no blocked", !board_has_flag(1, BOARD_FLAG_BLOCKED));

    /* 封鎖設定 */
    board_set_flag(1, BOARD_FLAG_BLOCKED);
    check("mass1 blocked", board_has_flag(1, BOARD_FLAG_BLOCKED));

    /* 解除 */
    board_clear_flag(1, BOARD_FLAG_BLOCKED);
    check("mass1 unblocked", !board_has_flag(1, BOARD_FLAG_BLOCKED));

    /* 複数フラグ */
    board_set_flag(3, BOARD_FLAG_BLOCKED);
    board_set_flag(3, BOARD_FLAG_HIDDEN);
    check("mass3 blocked+hidden",
          board_has_flag(3, BOARD_FLAG_BLOCKED) &&
          board_has_flag(3, BOARD_FLAG_HIDDEN));

    board_clear_flag(3, BOARD_FLAG_BLOCKED);
    check("mass3 only hidden",
          !board_has_flag(3, BOARD_FLAG_BLOCKED) &&
          board_has_flag(3, BOARD_FLAG_HIDDEN));

    board_shutdown();
}

/* ====================================================================== */
/*  テスト6: 罠管理                                                         */
/* ====================================================================== */

static void test_traps(void)
{
    header("Test 6: Traps");

    board_init("/host/assets/board.db");

    /* 初期状態: 罠なし */
    check_eq("mass1 trap_owner=0xFF", (int)board_get_trap_owner(1), 0xFF);
    check("mass1 no trap flag", !board_has_flag(1, BOARD_FLAG_TRAP));

    /* 罠設置 */
    board_set_trap(1, 2);
    check("mass1 trap set", board_has_flag(1, BOARD_FLAG_TRAP));
    check_eq("mass1 trap_owner=2", (int)board_get_trap_owner(1), 2);

    /* 罠解除 */
    board_clear_trap(1);
    check("mass1 trap cleared", !board_has_flag(1, BOARD_FLAG_TRAP));
    check_eq("mass1 trap_owner=0xFF", (int)board_get_trap_owner(1), 0xFF);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト7: 移動シミュレーション (walk)                                    */
/* ====================================================================== */

static void test_walk(void)
{
    u16 dest;
    int remaining;

    header("Test 7: Walk");

    board_init("/host/assets/board.db");

    /* マス0から1歩 → マス1 */
    dest = board_walk(0, 1, &remaining);
    check_eq("walk(0,1) = 1", (int)dest, 1);
    check_eq("remaining = 0", remaining, 0);

    /* マス0から2歩 → マス2で分岐停止 (残り0) */
    dest = board_walk(0, 2, &remaining);
    /* マス0→マス1(1歩消費)、マス1はconnect_count=2→分岐なので停止 */
    /* 実際にはマス1のconnect_countは2 (0と2に接続) なので分岐で停止 */
    api->kprintf(ATTR_WHITE, "    walk(0,2): dest=%d, remaining=%d\n",
                 (int)dest, remaining);
    /* マス1で分岐停止、remaining=1 */
    check_eq("walk(0,2) stops at 1", (int)dest, 1);
    check_eq("remaining = 1", remaining, 1);

    /* マス3から1歩 → マス3はconnect_count=2(マス2とマス4)→分岐停止 */
    dest = board_walk(3, 1, &remaining);
    check_eq("walk(3,1) stays", (int)dest, 3);
    check_eq("remaining=1", remaining, 1);

    /* マス8から3歩 (8→9、9はconnect_count=1(一方通行で10のみ)→直線) */
    dest = board_walk(8, 3, &remaining);
    api->kprintf(ATTR_WHITE, "    walk(8,3): dest=%d, remaining=%d\n",
                 (int)dest, remaining);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト8: 先読み (peek_path)                                             */
/* ====================================================================== */

static void test_peek_path(void)
{
    u16 path[8];
    int count;

    header("Test 8: Peek Path");

    board_init("/host/assets/board.db");

    /* マス0の方向0 (→マス1) から先読み */
    count = board_peek_path(0, 0, path, 8);
    api->kprintf(ATTR_WHITE, "    peek(0,dir0): count=%d\n", count);
    if (count >= 1) {
        check_eq("peek[0]=1", (int)path[0], 1);
    }
    /* マス1は分岐なので停止 */
    check_eq("peek stops at branch", count, 1);

    /* マス9 方向0から先読み (9→10→11で停止) */
    /* マス9は一方通行(from_id=9, to_id=10, bidir=0) */
    /* マス9のconnect_count: 8<->9双方向 + 9->10一方向 = 2つ */
    /* → connect_count=2 → 分岐 → peek_pathは最初の方向のみ */
    count = board_peek_path(9, 0, path, 8);
    api->kprintf(ATTR_WHITE, "    peek(9,dir0): count=%d", count);
    if (count > 0) {
        int i;
        for (i = 0; i < count; i++) {
            api->kprintf(ATTR_WHITE, " [%d]=%d", i, (int)path[i]);
        }
    }
    api->kprintf(ATTR_WHITE, "\n");

    board_shutdown();
}

/* ====================================================================== */
/*  テスト9: 最短距離 (BFS)                                                 */
/* ====================================================================== */

static void test_distance(void)
{
    int d;

    header("Test 9: BFS Distance");

    board_init("/host/assets/board.db");

    /* 自分自身 */
    d = board_distance(0, 0);
    check_eq("dist(0,0) = 0", d, 0);

    /* 隣接 */
    d = board_distance(0, 1);
    check_eq("dist(0,1) = 1", d, 1);

    /* 0 -> 4 (0->1->2->3->4) */
    d = board_distance(0, 4);
    check_eq("dist(0,4) = 4", d, 4);

    /* 0 -> 7 (0->1->2->5->6->7) */
    d = board_distance(0, 7);
    check_eq("dist(0,7) = 5", d, 5);

    /* 0 -> 11 (0と8-11は接続なし → -1) */
    d = board_distance(0, 11);
    check_eq("dist(0,11) = -1", d, -1);

    /* 8 -> 11 (8->9->10->11) */
    d = board_distance(8, 11);
    check_eq("dist(8,11) = 3", d, 3);

    /* 一方通行: 11 -> 9 (11->10 OK, 10->9は不可) */
    /* 10->9の接続は一方通行(9->10のみ)なので存在しない */
    /* 11->10->... 9へのパスはない → -1 */
    d = board_distance(11, 9);
    api->kprintf(ATTR_WHITE, "    dist(11,9) = %d\n", d);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト10: 区画管理                                                      */
/* ====================================================================== */

static void test_areas(void)
{
    int rc;

    header("Test 10: Area Management");

    board_init("/host/assets/board.db");

    /* 区画0: 初期解放 */
    check("area0 unlocked", board_is_area_unlocked(0));

    /* 区画1: ロック */
    check("area1 locked", !board_is_area_unlocked(1));

    /* 区画1を解放 */
    rc = board_unlock_area(1);
    check_eq("unlock area1 = 1", rc, 1);
    check("area1 now unlocked", board_is_area_unlocked(1));

    /* 二重解放 */
    rc = board_unlock_area(1);
    check_eq("double unlock = 0", rc, 0);

    /* ロック */
    board_lock_area(1);
    check("area1 locked again", !board_is_area_unlocked(1));

    /* 存在しない区画 */
    rc = board_unlock_area(99);
    check_eq("unlock unknown = -1", rc, -1);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト11: リセット                                                      */
/* ====================================================================== */

static void test_reset(void)
{
    header("Test 11: Reset");

    board_init("/host/assets/board.db");

    /* フラグを変更 */
    board_set_flag(1, BOARD_FLAG_BLOCKED);
    board_set_trap(3, 1);
    board_unlock_area(1);

    check("before reset: mass1 blocked",
          board_has_flag(1, BOARD_FLAG_BLOCKED));
    check("before reset: area1 unlocked",
          board_is_area_unlocked(1));

    /* リセット */
    board_reset();

    check("after reset: mass1 not blocked",
          !board_has_flag(1, BOARD_FLAG_BLOCKED));
    check("after reset: mass3 no trap",
          !board_has_flag(3, BOARD_FLAG_TRAP));
    check("after reset: area1 locked",
          !board_is_area_unlocked(1));

    /* マスデータは保持されている */
    check_eq("mass_count still 12", board_mass_count(), 12);

    board_shutdown();
}

/* ====================================================================== */
/*  テスト12: 動的マス操作                                                  */
/* ====================================================================== */

static void test_dynamic(void)
{
    BoardMass new_mass;
    int new_id;
    int rc;
    const BoardMass *m;

    header("Test 12: Dynamic Mass Ops");

    board_init("/host/assets/board.db");

    /* マス追加 */
    memset(&new_mass, 0, sizeof(new_mass));
    new_mass.id = 100;
    new_mass.type = 1;
    new_mass.x = 250;
    new_mass.y = 0;
    new_mass.connect_count = 0;

    new_id = board_add_mass(&new_mass);
    check_eq("add mass id=100", new_id, 100);
    check_eq("mass_count=13", board_mass_count(), 13);

    m = board_get_mass(100);
    check("new mass exists", m != (const BoardMass *)0);
    if (m) {
        check_eq("new mass type=1", (int)m->type, 1);
    }

    /* 接続追加: 4 -> 100 */
    rc = board_add_connection(4, 100);
    check_eq("add_conn 4->100 = 1", rc, 1);

    /* 重複接続 */
    rc = board_add_connection(4, 100);
    check_eq("dup conn = 0", rc, 0);

    /* 接続確認 */
    {
        u16 conns[8];
        int n = board_get_connections(4, conns, 8);
        api->kprintf(ATTR_WHITE, "    mass4 conns=%d\n", n);
    }

    /* 接続削除 */
    board_remove_connection(4, 100);
    {
        u16 conns[8];
        int n;
        int i;
        int found = 0;
        n = board_get_connections(4, conns, 8);
        for (i = 0; i < n; i++) {
            if (conns[i] == 100) found = 1;
        }
        check("conn removed", !found);
    }

    /* BFS: 0 -> 100 (接続削除済みなので到達不能) */
    {
        int d = board_distance(0, 100);
        check_eq("dist(0,100) after remove = -1", d, -1);
    }

    /* 再接続して到達可能にする */
    board_add_connection(4, 100);
    {
        int d = board_distance(0, 100);
        check_eq("dist(0,100) reconnected = 5", d, 5);
    }

    board_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "board_test: libos32board test suite\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* Phase 1 テスト */
    test_init_no_db();
    test_db_load();
    test_connections();
    test_colocated();
    test_flags();
    test_traps();
    test_walk();
    test_peek_path();

    /* Phase 2 テスト */
    test_distance();
    test_areas();
    test_reset();
    test_dynamic();

    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n",
                 g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }
    return 0;
}
