/* ======================================================================== */
/*  RPG_TEST.C — libos32rpg テストプログラム                                */
/* ======================================================================== */

#include "os32api.h"
#include "libos32rpg.h"
#include "libos32db.h"
#include "libos32math.h"
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
/*  テスト1: DBロードと初期値                                               */
/* ====================================================================== */
static void test_init_and_db(void)
{
    int rc;
    RpgActor actor;

    header("Test 1: Init and Class Load");

    rc = rpg_init("/db/rpg.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW, "  [SKIP] rpg_init failed (rpg.db deployed?)\n");
        return;
    }
    check("rpg_init success", rc == 0);

    /* スサノオ (class_id = 0) の初期化 */
    rpg_actor_init(&actor, 0);
    check_eq("susanoo lvl=1", actor.level, 1);
    check_eq("susanoo class=0", actor.class_id, 0);
    check_eq("susanoo hp=50", actor.hp, 50);
    check_eq("susanoo max_hp=50", actor.max_hp, 50);
    check_eq("susanoo atk=12", actor.atk, 12); /* 10 + 氏神ボーナス2 */
    check_eq("susanoo def=10", actor.def, 10);
    check_eq("susanoo spd=10", actor.spd, 10);
    check_eq("susanoo mag=10", actor.mag, 10);
    check_eq("susanoo status=0", actor.status, 0);
    check_eq("susanoo dead_turns=0", actor.dead_turns, 0);
    check_eq("susanoo pending_points=0", actor.pending_points, 0);
}

/* ====================================================================== */
/*  テスト2: 経験値とレベルアップ成長                                      */
/* ====================================================================== */
static void test_levelup(void)
{
    RpgActor actor;
    RpgLevelResult res;
    int lv_up;
    u32 exp_needed;

    header("Test 2: Exp and Level Up");

    rpg_actor_init(&actor, 0); /* スサノオ: ATK+2 成長 */

    /* Lv2 に必要な必要累計経験値 = 2*2*10 = 40 */
    exp_needed = rpg_exp_for_level(2);
    check_eq("exp_for_level(2) = 40", (int)exp_needed, 40);
    check_eq("exp_to_next = 40", (int)rpg_exp_to_next(&actor), 40);

    /* 経験値 30 追加 -> レベルアップしない */
    lv_up = rpg_add_exp(&actor, 30, &res);
    check_eq("add 30exp: no levelup", lv_up, 0);
    check_eq("actor level remains 1", actor.level, 1);
    check_eq("actor exp = 30", (int)actor.exp, 30);
    check_eq("exp_to_next = 10", (int)rpg_exp_to_next(&actor), 10);

    /* 経験値 15 追加 -> レベル2へ (累計45) */
    lv_up = rpg_add_exp(&actor, 15, &res);
    check_eq("add 15exp: levelup", lv_up, 1);
    check_eq("actor level is now 2", actor.level, 2);
    check_eq("levels gained = 1", res.levels_gained, 1);
    check_eq("free points = 4", res.free_points, 4);
    check_eq("actor pending_points = 4", actor.pending_points, 4);

    /* パラメータの成長チェック (スサノオ成長: ATK+2) */
    check_eq("susanoo LevelUp atk=14", actor.atk, 14);
    check_eq("susanoo LevelUp def=10", actor.def, 10);

    /* さらに 1000 exp 追加 -> レベルアップ */
    rpg_add_exp(&actor, 1000, &res);
    check("multiple levelup", actor.level > 2);
    api->kprintf(ATTR_WHITE, "    multiple levelup: lvl=%d, pending=%d, max_hp=%d\n",
                 actor.level, actor.pending_points, actor.max_hp);
}

/* ====================================================================== */
/*  テスト3: ステータス手動配分                                             */
/* ====================================================================== */
static void test_stat_alloc(void)
{
    RpgActor actor;
    int rc;

    header("Test 3: Stat Allocation");

    rpg_actor_init(&actor, 0);
    actor.pending_points = 2;

    /* ATK に配分 */
    rc = rpg_alloc_point(&actor, BTL_STAT_ATK);
    check_eq("alloc ATK success", rc, 0);
    check_eq("atk = 13", actor.atk, 13);
    check_eq("pending_points = 1", actor.pending_points, 1);

    /* DEF に配分 */
    rc = rpg_alloc_point(&actor, BTL_STAT_DEF);
    check_eq("alloc DEF success", rc, 0);
    check_eq("def = 11", actor.def, 11);
    check_eq("pending_points = 0", actor.pending_points, 0);

    /* ポイントなし状態で配分試行 -> 失敗 */
    rc = rpg_alloc_point(&actor, BTL_STAT_ATK);
    check_eq("alloc no points -> failure", rc, -1);
}

/* ====================================================================== */
/*  テスト4: フィールド状態異常 Tick                                        */
/* ====================================================================== */
static void test_status_tick(void)
{
    RpgActor actor;
    RpgTickLog log;
    int blocked;

    header("Test 4: Field Status Tick");

    rpg_actor_init(&actor, 0);
    actor.hp = 10;
    actor.level = 5;

    /* 毒 (STATUS_POISON = 1) を付与 */
    rpg_status_apply(&actor, 1);
    check("poison applied", rpg_has_status(&actor, 1));

    /* 毒Tick実行 -> 比例ダメージ (Lv5 なので 5ダメージ) */
    blocked = rpg_status_tick(&actor, &log);
    check_eq("poison tick not blocked", blocked, 0);
    check_eq("poison damage = 5", log.tick_damage, 5);
    check_eq("actor hp = 5", actor.hp, 5);

    /* 再度Tick実行 -> ダメージは 5 だが、毒は非致死なので HP1 で耐える */
    blocked = rpg_status_tick(&actor, &log);
    check_eq("poison damage (lethal cap) = 5", log.tick_damage, 5);
    check_eq("actor hp stays at 1", actor.hp, 1);
    check("still poisoned", rpg_has_status(&actor, 1));

    /* 毒解除 */
    rpg_status_clear(&actor, 1);
    check("poison cleared", !rpg_has_status(&actor, 1));

    /* 麻痺 (STATUS_PARALYZE = 2) を付与 */
    rpg_status_apply(&actor, 2);
    
    /* 自然回復が 50% 確率で起きるため、シード固定なしで複数回呼び出して確認するか
       あるいは回復ログをチェックする */
    rng_seed(12345); /* シード設定 */
    blocked = rpg_status_tick(&actor, &log);
    api->kprintf(ATTR_WHITE, "    paralyze tick 1: blocked=%d, cleared=%d, hp=%d\n",
                 blocked, log.cleared, actor.hp);
}

/* ====================================================================== */
/*  テスト5: 死亡とリボーン                                                 */
/* ====================================================================== */
static void test_death_and_reborn(void)
{
    RpgActor actor;
    int rc;

    header("Test 5: Death and Reborn Table");

    rpg_actor_init(&actor, 0);
    actor.hp = 50;

    /* 死亡状態にする */
    rpg_set_dead(&actor, 0);
    check("actor is dead", rpg_is_dead(&actor));
    check_eq("actor hp = 0", actor.hp, 0);
    check_eq("dead_turns = 1", actor.dead_turns, 1);

    /* 4位（最下位、rank_bucket=4）の復活待機時間: min=2, max=4 */
    /* 死亡1ターン目 (dead_turns=1) -> min=2 未満なので復活しない */
    rc = rpg_reborn_check(&actor, 4, 4);
    check_eq("reborn_check rank4 turn1 = 0", rc, 0);
    check_eq("dead_turns incremented = 2", actor.dead_turns, 2);

    /* 死亡2ターン目 (dead_turns=2) -> min=2 に到達。
       min=2, max=4 なので確率判定。シード固定で制御可能。 */
    rng_seed(999); /* 乱数調整 */
    rc = rpg_reborn_check(&actor, 4, 4);
    api->kprintf(ATTR_WHITE, "    reborn_check rank4 turn2: rc=%d, dead_turns=%d\n",
                 rc, actor.dead_turns);

    /* 最大ターン max=4 なので、dead_turns=4 以上になれば確定復活 */
    actor.dead_turns = 4;
    rc = rpg_reborn_check(&actor, 4, 4);
    check_eq("reborn_check rank4 turn4 (max) = 1 (reborned)", rc, 1);
    check_eq("actor hp recovered = 50", actor.hp, 50);
    check("actor is alive now", !rpg_is_dead(&actor));
}

/* ====================================================================== */
/*  テスト6: 順位計算とブリッジ                                             */
/* ====================================================================== */
static void test_rank_and_bridge(void)
{
    u32 scores[4] = { 1000, 5000, 2000, 2000 };
    RpgActor actor;
    BtlUnit unit;

    header("Test 6: Rank and Battle Unit Bridge");

    /* 順位計算 (同点タイブレークを含む) */
    check_eq("score[0]=1000 -> rank 4", rpg_rank(scores, 4, 0), 4);
    check_eq("score[1]=5000 -> rank 1", rpg_rank(scores, 4, 1), 1);
    /* index 2 と 3 は同点 2000。若い方の index 2 が 2位、index 3 が 3位 */
    check_eq("score[2]=2000 -> rank 2", rpg_rank(scores, 4, 2), 2);
    check_eq("score[3]=2000 -> rank 3", rpg_rank(scores, 4, 3), 3);

    /* BtlUnit への変換 */
    rpg_actor_init(&actor, 0);
    actor.hp = 35;
    actor.atk = 15;
    actor.status = 1;

    rpg_to_btl_unit(&actor, &unit);
    check_eq("btl unit hp = 35", unit.hp, 35);
    check_eq("btl unit max_hp = 50", unit.max_hp, 50);
    check_eq("btl unit atk = 15", unit.atk, 15);
    check_eq("btl unit status = 1", (int)unit.status, 1);

    /* 戦闘後のステータス書き戻し */
    unit.hp = 0; /* 戦闘でHPが0になった */
    unit.status = 0; /* 状態異常解除 */

    rpg_from_btl_unit(&actor, &unit);
    check_eq("actor hp now = 0", actor.hp, 0);
    check_eq("actor status = 0", (int)actor.status, 0);
    check("actor is dead", rpg_is_dead(&actor));
}

/* ====================================================================== */
/*  メイン                                                                 */
/* ====================================================================== */
int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    g_total = 0;
    g_passed = 0;

    api->kprintf(ATTR_CYAN, "rpg_test: libos32rpg test suite\n");

    test_init_and_db();
    test_levelup();
    test_stat_alloc();
    test_status_tick();
    test_death_and_reborn();
    test_rank_and_bridge();

    rpg_shutdown();

    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n", g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All rpg_test tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }

    return 0;
}
