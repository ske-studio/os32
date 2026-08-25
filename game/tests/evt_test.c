/* ======================================================================== */
/*  EVT_TEST.C — libos32event テストプログラム                              */
/*                                                                          */
/*  KernelAPI kprintf でイベントスケジューラの全APIをテストする。            */
/*  テスト項目:                                                             */
/*    1. DB無し初期化 + 手動定義                                            */
/*    2. PERIODICイベント発火                                                */
/*    3. RANDOMイベント発火 (カウンタベース確率上昇)                         */
/*    4. CONDITIONイベント発火 (コールバック)                                */
/*    5. アクティブイベント管理 (持続+キャンセル)                             */
/*    6. 排他グループ                                                       */
/*    7. 手動発火 (evt_trigger)                                              */
/*    8. 連鎖発火 (chain)                                                    */
/*    9. クールダウン                                                        */
/*   10. DBロードテスト                                                      */
/* ======================================================================== */

#include "os32api.h"
#include "libos32event.h"
#include "libos32ai.h"
#include "libos32math.h"
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
/*  手動イベント定義ヘルパー                                                */
/*                                                                          */
/*  evt_core.c の内部状態に直接定義を書き込む。                             */
/*  テスト専用: 通常はDBからロードする。                                    */
/* ====================================================================== */

extern EvtDef    g_evt_defs[];
extern u8        g_evt_def_count;
extern u16       g_evt_cooldowns[];

static void add_def(u16 id, u8 type, u8 weight, u16 min_turn,
                    u16 cooldown, u16 period, u8 duration,
                    u8 group, u16 chain_id, u8 chain_chance,
                    u8 scope)
{
    EvtDef *def;
    if (g_evt_def_count >= EVT_MAX_DEFS) return;
    def = &g_evt_defs[g_evt_def_count];
    def->id           = id;
    def->type         = type;
    def->weight       = weight;
    def->min_turn     = min_turn;
    def->cooldown     = cooldown;
    def->period       = period;
    def->duration     = duration;
    def->group        = group;
    def->chain_id     = chain_id;
    def->chain_chance = chain_chance;
    def->scope        = scope;
    g_evt_def_count++;
}

/* ====================================================================== */
/*  テスト1: DB無し初期化                                                   */
/* ====================================================================== */

static void test_init(void)
{
    int rc;

    header("Test 1: Init (no DB)");

    rc = evt_init(NULL);
    check_eq("evt_init(NULL)", rc, 0);
    check_eq("counter = 0", (int)evt_get_counter(), 0);
    check_eq("active_count = 0", evt_active_count(), 0);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト2: PERIODICイベント発火                                           */
/* ====================================================================== */

static void test_periodic(void)
{
    int fired;
    u16 ids[EVT_FIRED_MAX];

    header("Test 2: PERIODIC event");

    evt_init(NULL);
    rng_seed(42);

    /* 5ターンごとに発火する周期イベント (id=1, 瞬時) */
    add_def(1, EVT_TYPE_PERIODIC, 0, 0, 0, 5, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    /* ターン1: 発火しない (1%5 != 0) */
    fired = evt_tick(1, NULL);
    check_eq("turn=1: no fire", fired, 0);

    /* ターン5: 発火 */
    fired = evt_tick(5, NULL);
    check_eq("turn=5: fired 1", fired, 1);
    evt_get_fired(ids, EVT_FIRED_MAX);
    check_eq("turn=5: id=1", (int)ids[0], 1);

    /* ターン10: 発火 */
    fired = evt_tick(10, NULL);
    check_eq("turn=10: fired 1", fired, 1);

    /* ターン7: 発火しない */
    fired = evt_tick(7, NULL);
    check_eq("turn=7: no fire", fired, 0);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト3: RANDOMイベント発火                                             */
/* ====================================================================== */

static void test_random(void)
{
    int total_fired = 0;
    int i;

    header("Test 3: RANDOM event");

    evt_init(NULL);
    rng_seed(12345);

    /* RANDOMイベント (id=10, weight=5, min_turn=3, cooldown=5) */
    add_def(10, EVT_TYPE_RANDOM, 5, 3, 5, 0, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    /* 最初の3ターンは min_turn 未満で発火しない */
    for (i = 1; i <= 3; i++) {
        evt_tick((u16)i, NULL);
    }
    /* カウンタは3 (3ターン分インクリメント) */
    check_eq("counter after 3 turns", (int)evt_get_counter(), 3);

    /* 多数ターンを回して、いずれ発火することを確認 */
    for (i = 4; i <= 300; i++) {
        int f = evt_tick((u16)i, NULL);
        total_fired += f;
    }
    check("random event fired at least once", total_fired > 0);
    api->kprintf(ATTR_WHITE, "    total fires in 300 turns: %d\n", total_fired);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト4: CONDITIONイベント発火                                          */
/* ====================================================================== */

/* テスト用コンテキスト */
typedef struct {
    int hp_percent;
} TestContext;

/* 条件コールバック: HP30%以下で発火 */
static int cond_low_hp(u16 event_id, u16 turn, const void *ctx)
{
    const TestContext *tc = (const TestContext *)ctx;
    (void)event_id;
    (void)turn;
    if (tc == NULL) return 0;
    return (tc->hp_percent <= 30) ? 1 : 0;
}

static void test_condition(void)
{
    TestContext ctx;
    int fired;
    u16 ids[EVT_FIRED_MAX];

    header("Test 4: CONDITION event");

    evt_init(NULL);
    rng_seed(42);

    /* CONDITIONイベント (id=20, cooldown=3, 瞬時) */
    add_def(20, EVT_TYPE_CONDITION, 0, 0, 3, 0, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    evt_set_condition_callback(cond_low_hp);

    /* HP50%: 条件不成立 → 発火しない */
    ctx.hp_percent = 50;
    fired = evt_tick(1, &ctx);
    check_eq("HP50%: no fire", fired, 0);

    /* HP20%: 条件成立 → 発火 */
    ctx.hp_percent = 20;
    fired = evt_tick(2, &ctx);
    check_eq("HP20%: fired", fired, 1);
    evt_get_fired(ids, EVT_FIRED_MAX);
    check_eq("fired id=20", (int)ids[0], 20);

    /* クールダウン中: HP20%でも発火しない */
    fired = evt_tick(3, &ctx);  /* CD: 3→2 */
    check_eq("cooldown: no fire", fired, 0);

    /* クールダウン解消後: CD=3は3ターンのデクリメントで解消 */
    evt_tick(4, &ctx);  /* CD: 2→1 */
    fired = evt_tick(5, &ctx);  /* CD: 1→0 → 条件成立 → 発火 */
    check_eq("after cooldown: fired", fired, 1);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト5: アクティブイベント管理                                          */
/* ====================================================================== */

static void test_active(void)
{
    EvtActive list[EVT_MAX_ACTIVE];
    int count;

    header("Test 5: Active event management");

    evt_init(NULL);
    rng_seed(42);

    /* 持続イベント (id=30, duration=3) — 手動発火でテスト */
    add_def(30, EVT_TYPE_RANDOM, 5, 0, 100, 0, 3, 0, 0, 0, EVT_SCOPE_GLOBAL);

    /* 手動発火 */
    evt_reset();
    evt_trigger(30, 0xFF);
    check("is_active(30)", evt_is_active(30));
    check_eq("active_count", evt_active_count(), 1);

    count = evt_active_list(list, EVT_MAX_ACTIVE);
    check_eq("active_list count", count, 1);
    if (count > 0) {
        check_eq("list[0].id", (int)list[0].event_id, 30);
        check_eq("list[0].remaining", (int)list[0].remaining, 3);
    }

    /* tick: remaining 3→2 */
    evt_tick(1, NULL);
    count = evt_active_list(list, EVT_MAX_ACTIVE);
    if (count > 0) {
        check_eq("remaining after tick", (int)list[0].remaining, 2);
    }

    /* 手動キャンセル */
    evt_cancel(30);
    check_eq("after cancel: active_count", evt_active_count(), 0);
    check("!is_active(30)", !evt_is_active(30));

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト6: 排他グループ                                                   */
/* ====================================================================== */

static void test_exclusion(void)
{
    int rc;

    header("Test 6: Exclusion group");

    evt_init(NULL);
    rng_seed(42);

    /* 同じグループ1の2つの持続イベント — 手動trigger方式でテスト */
    add_def(40, EVT_TYPE_RANDOM, 5, 0, 100, 0, 5, 1, 0, 0, EVT_SCOPE_GLOBAL);
    add_def(41, EVT_TYPE_RANDOM, 5, 0, 100, 0, 5, 1, 0, 0, EVT_SCOPE_GLOBAL);

    /* id=40 を手動発火 → アクティブに */
    evt_reset();
    rc = evt_trigger(40, 0xFF);
    check_eq("trigger 40", rc, 0);
    check("40 is active", evt_is_active(40));

    /* id=41 は排他制約で発火できないはず */
    rc = evt_trigger(41, 0xFF);
    check_eq("trigger 41 blocked", rc, -1);
    check("41 not active (excluded)", !evt_is_active(41));

    /* id=40 をキャンセル → id=41 が発火可能に */
    evt_cancel(40);
    rc = evt_trigger(41, 0xFF);
    check_eq("trigger 41 after cancel", rc, 0);
    check("41 now active", evt_is_active(41));

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト7: 手動発火 (evt_trigger)                                         */
/* ====================================================================== */

static void test_trigger(void)
{
    int rc;
    u16 ids[EVT_FIRED_MAX];

    header("Test 7: Manual trigger");

    evt_init(NULL);
    rng_seed(42);

    /* 瞬時イベント (id=50, duration=0) */
    add_def(50, EVT_TYPE_RANDOM, 5, 0, 10, 0, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    /* 手動発火 */
    evt_reset();  /* 発火バッファクリア */
    rc = evt_trigger(50, 0xFF);
    check_eq("trigger rc=0", rc, 0);

    rc = evt_get_fired(ids, EVT_FIRED_MAX);
    check_eq("fired count", rc, 1);
    check_eq("fired id=50", (int)ids[0], 50);

    /* 瞬時なのでアクティブには入らない */
    check_eq("not active (instant)", evt_active_count(), 0);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト8: 連鎖発火 (chain)                                               */
/* ====================================================================== */

static void test_chain(void)
{
    int rc;
    u16 ids[EVT_FIRED_MAX];

    header("Test 8: Chain fire");

    evt_init(NULL);
    rng_seed(42);

    /* id=60 → 100% で id=61 に連鎖 */
    add_def(60, EVT_TYPE_RANDOM, 5, 0, 10, 0, 0, 0, 61, 100, EVT_SCOPE_GLOBAL);
    add_def(61, EVT_TYPE_RANDOM, 5, 0, 10, 0, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    evt_reset();
    rc = evt_trigger(60, 0xFF);
    check_eq("chain trigger rc=0", rc, 0);

    rc = evt_get_fired(ids, EVT_FIRED_MAX);
    check_eq("chain fired count = 2", rc, 2);
    check_eq("chain[0] = 60", (int)ids[0], 60);
    check_eq("chain[1] = 61", (int)ids[1], 61);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト9: クールダウン                                                   */
/* ====================================================================== */

static void test_cooldown(void)
{
    int fired;

    header("Test 9: Cooldown");

    evt_init(NULL);
    rng_seed(42);

    /* PERIODICイベント (id=70, period=1, cooldown=3) */
    add_def(70, EVT_TYPE_PERIODIC, 0, 0, 3, 1, 0, 0, 0, 0, EVT_SCOPE_GLOBAL);

    /* ターン1: 発火 → クールダウン3設定 */
    fired = evt_tick(1, NULL);
    check_eq("turn=1: fired", fired, 1);

    /* ターン2: CD 3→2 → スキップ */
    fired = evt_tick(2, NULL);
    check_eq("turn=2: cooldown (no fire)", fired, 0);

    /* ターン3: CD 2→1 → スキップ */
    fired = evt_tick(3, NULL);
    check_eq("turn=3: cooldown (no fire)", fired, 0);

    /* ターン4: CD 1→0 → 発火 (CD解消した最初のターン) */
    fired = evt_tick(4, NULL);
    check_eq("turn=4: CD expired, fired", fired, 1);

    evt_shutdown();
}

/* ====================================================================== */
/*  テスト10: DBロードテスト                                                */
/* ====================================================================== */

static void test_db_load(void)
{
    int rc;
    int total_fired = 0;
    int i;

    header("Test 10: DB Load");

    rc = evt_init("/db/events_test.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] evt_init returned %d "
            "(is events_test.db deployed?)\n", rc);
        return;
    }
    check("evt_init(/db/events_test.db)", rc == 0);

    /* 定義数を確認 */
    api->kprintf(ATTR_WHITE, "  def_count = %d\n", (int)g_evt_def_count);
    check("def_count > 0", (int)g_evt_def_count > 0);

    /* 定義内容表示 */
    for (i = 0; i < (int)g_evt_def_count && i < 5; i++) {
        api->kprintf(ATTR_WHITE,
            "  def[%d]: id=%d type=%d w=%d period=%d cd=%d dur=%d grp=%d\n",
            i, (int)g_evt_defs[i].id, (int)g_evt_defs[i].type,
            (int)g_evt_defs[i].weight, (int)g_evt_defs[i].period,
            (int)g_evt_defs[i].cooldown, (int)g_evt_defs[i].duration,
            (int)g_evt_defs[i].group);
    }

    /* PERIODICイベント (id=1, period=4) のテスト */
    rng_seed(99);
    for (i = 1; i <= 20; i++) {
        int f = evt_tick((u16)i, NULL);
        total_fired += f;
    }
    api->kprintf(ATTR_WHITE, "  total fired in 20 turns: %d\n", total_fired);
    check("DB events fired > 0", total_fired > 0);

    evt_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "evt_test: libos32event test suite\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* 乱数初期化 */
    rng_seed(0xDEADBEEF);

    /* AI ライブラリ初期化 (ai_weighted_pick 用) */
    ai_init(NULL);

    /* Phase 1 テスト */
    test_init();
    test_periodic();
    test_random();
    test_condition();
    test_active();
    test_exclusion();
    test_trigger();
    test_chain();
    test_cooldown();
    test_db_load();

    /* AI ライブラリ終了 */
    ai_shutdown();

    /* サマリ */
    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n",
                 g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }
    return 0;
}
