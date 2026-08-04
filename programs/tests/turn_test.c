/* ======================================================================== */
/*  TURN_TEST.C — libos32turn テストプログラム                              */
/* ======================================================================== */

#include "os32api.h"
#include "libos32turn.h"
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
/*  テスト1: 基本進行と週（ラウンド）境界                                   */
/* ====================================================================== */
static void test_basic_flow(void)
{
    TurnState ts;
    TurnAdvance ta;
    int rc;

    header("Test 1: Basic Round-Robin Flow");

    /* 4人、7ターンで週境界、最大100ターン */
    turn_init(&ts, 4, 7, 100);
    check_eq("init count=1", turn_count(&ts), 1);
    check_eq("init round=1", turn_round(&ts), 1);
    check_eq("init current=0", turn_current(&ts), 0);
    check("init is boundary", turn_is_round_boundary(&ts));

    /* 1ターン目 -> 2ターン目への進行 (P0 -> P1) */
    rc = turn_advance(&ts, &ta);
    check_eq("advance rc=0 (not over)", rc, 0);
    check_eq("next current=1", ta.current, 1);
    check_eq("next current (query)=1", turn_current(&ts), 1);
    check_eq("next count=2", ta.turn_count, 2);
    check_eq("next round=1", ta.round_count, 1);
    check("not boundary", !turn_is_round_boundary(&ts));
    check_eq("crossed_round=0", ta.crossed_round, 0);

    /* 6ターン進めて累計8ターン目に入る (週境界チェック) */
    /* 1->2(1), 2->3(2), 3->4(3), 4->5(4), 5->6(5), 6->7(6), 7->8(7) */
    rc = turn_advance(&ts, &ta); /* P1->P2 (turn 3) */
    rc = turn_advance(&ts, &ta); /* P2->P3 (turn 4) */
    rc = turn_advance(&ts, &ta); /* P3->P0 (turn 5) */
    rc = turn_advance(&ts, &ta); /* P0->P1 (turn 6) */
    rc = turn_advance(&ts, &ta); /* P1->P2 (turn 7) */
    
    /* 7ターン目から8ターン目へ進む瞬間 */
    rc = turn_advance(&ts, &ta); /* P2->P3 (turn 8) */
    check_eq("advance turn 8 rc=0", rc, 0);
    check_eq("turn 8 count=8", ta.turn_count, 8);
    check_eq("turn 8 round=2", ta.round_count, 2);
    check_eq("crossed_round=1", ta.crossed_round, 1);
    check("turn 8 is boundary", turn_is_round_boundary(&ts));
}

/* ====================================================================== */
/*  テスト2: スキップ制御 (死亡・休み)                                     */
/* ====================================================================== */
static void test_skip_flow(void)
{
    TurnState ts;
    TurnAdvance ta;

    header("Test 2: Skip Flow");

    turn_init(&ts, 4, 7, 100);

    /* P2 を2回分スキップ登録 */
    turn_skip(&ts, 2, 2);

    /* P0 -> P1 */
    turn_advance(&ts, &ta);
    check_eq("P0 -> P1", ta.current, 1);

    /* P1 -> P2(スキップ1回目) -> P3 */
    turn_advance(&ts, &ta);
    check_eq("P1 -> P3 (P2 skipped, skip_left=1)", ta.current, 3);
    check_eq("P2 skip counter=1", ts.skip[2], 1);

    /* P3 -> P0 */
    turn_advance(&ts, &ta);
    check_eq("P3 -> P0", ta.current, 0);

    /* P0 -> P1 */
    turn_advance(&ts, &ta);
    check_eq("P0 -> P1", ta.current, 1);

    /* P1 -> P2(スキップ2回目) -> P3 */
    turn_advance(&ts, &ta);
    check_eq("P1 -> P3 (P2 skipped again, skip_left=0)", ta.current, 3);
    check_eq("P2 skip counter=0", ts.skip[2], 0);

    /* P3 -> P0 */
    turn_advance(&ts, &ta);
    check_eq("P3 -> P0", ta.current, 0);

    /* P0 -> P1 */
    turn_advance(&ts, &ta);

    /* P1 -> P2 (スキップ完了のため行動可能に) */
    turn_advance(&ts, &ta);
    check_eq("P1 -> P2 (P2 recovered)", ta.current, 2);
}

/* ====================================================================== */
/*  テスト3: 脱落と終了判定                                                 */
/* ====================================================================== */
static void test_over_flow(void)
{
    TurnState ts;
    TurnAdvance ta;
    int rc;

    header("Test 3: Deactivation and Over Flow");

    turn_init(&ts, 4, 7, 100);
    check_eq("alive=4", turn_alive_count(&ts), 4);
    check("not over", !turn_is_over(&ts));

    /* P2 脱落 */
    turn_set_active(&ts, 2, 0);
    check_eq("alive=3", turn_alive_count(&ts), 3);

    /* P0 -> P1 */
    turn_advance(&ts, &ta);
    /* P1 -> P2(脱落) -> P3 */
    turn_advance(&ts, &ta);
    check_eq("skip deactivated P2 -> P3", ta.current, 3);

    /* P1 脱落 */
    turn_set_active(&ts, 1, 0);
    /* P3 脱落 -> 生存1名 (P0) でゲーム終了 */
    turn_set_active(&ts, 3, 0);
    check_eq("alive=1", turn_alive_count(&ts), 1);
    check("game is over", turn_is_over(&ts));

    rc = turn_advance(&ts, &ta);
    check_eq("advance when over returns 1", rc, 1);
    check_eq("is_over=1", ta.is_over, 1);
}

/* ====================================================================== */
/*  テスト4: 最大ターン終了                                                 */
/* ====================================================================== */
static void test_max_turns(void)
{
    TurnState ts;
    TurnAdvance ta;

    header("Test 4: Max Turns Boundary");

    /* 最大3ターンに制限 */
    turn_init(&ts, 4, 7, 3);
    
    turn_advance(&ts, &ta); /* turn 1->2 */
    turn_advance(&ts, &ta); /* turn 2->3 */
    
    /* 3ターン目から4ターン目へ進もうとすると、最大ターン制限で終了 */
    turn_advance(&ts, &ta); /* turn 3->4 (max_turns=3 なので is_over になる) */
    check("over by max_turns", turn_is_over(&ts));
    check_eq("advance returns over", ta.is_over, 1);
}

/* ====================================================================== */
/*  メイン                                                                 */
/* ====================================================================== */
int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    g_total = 0;
    g_passed = 0;

    api->kprintf(ATTR_CYAN, "turn_test: libos32turn test suite\n");

    test_basic_flow();
    test_skip_flow();
    test_over_flow();
    test_max_turns();

    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n", g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All turn_test tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }

    return 0;
}
