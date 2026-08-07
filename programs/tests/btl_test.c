/* ======================================================================== */
/*  BTL_TEST.C — libos32battle テストプログラム                              */
/*                                                                          */
/*  KernelAPI kprintf でバトルエンジンの全APIをテストする。                  */
/* ======================================================================== */

#include "os32api.h"
#include "libos32battle.h"
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

static void check_range(const char *label, int val, int lo, int hi)
{
    g_total++;
    if (val >= lo && val <= hi) {
        g_passed++;
        api->kprintf(ATTR_GREEN, "  [OK] %s = %d [%d..%d]\n",
                     label, val, lo, hi);
    } else {
        api->kprintf(ATTR_RED, "  [NG] %s = %d, expect [%d..%d]\n",
                     label, val, lo, hi);
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

    rc = btl_init(NULL);
    check_eq("btl_init(NULL)", rc, 0);
    btl_shutdown();
}

/* ====================================================================== */
/*  テスト2: ダメージ計算                                                   */
/* ====================================================================== */

static void test_damage_calc(void)
{
    int dmg, i;
    int min_dmg, max_dmg;

    header("Test 2: Damage Calc");
    btl_init(NULL);
    rng_seed(42);

    /* atk=20, def=10 → base = 20+10-10 = 20, +rng(-3,3) → [17..23] */
    min_dmg = 9999;
    max_dmg = -9999;
    for (i = 0; i < 100; i++) {
        dmg = btl_calc_damage(20, 10);
        if (dmg < min_dmg) min_dmg = dmg;
        if (dmg > max_dmg) max_dmg = dmg;
    }
    check_range("phys dmg min", min_dmg, 17, 20);
    check_range("phys dmg max", max_dmg, 20, 23);

    /* atk < def → ダメージ0 */
    dmg = btl_calc_damage(5, 30);
    check_eq("atk<def -> 0", dmg, 0);

    /* 魔法ダメージ: mag=15, def=10 → 15*2-10=20, +rng → [17..23] */
    min_dmg = 9999;
    max_dmg = -9999;
    for (i = 0; i < 100; i++) {
        dmg = btl_calc_magic_damage(15, 10);
        if (dmg < min_dmg) min_dmg = dmg;
        if (dmg > max_dmg) max_dmg = dmg;
    }
    check_range("magic dmg min", min_dmg, 17, 20);
    check_range("magic dmg max", max_dmg, 20, 23);

    /* 防御ボーナス: def=100 → 125 */
    check_eq("guard def 100->125", btl_effective_def_guard(100), 125);
    check_eq("guard def 80->100", btl_effective_def_guard(80), 100);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト3: 回避・逃走確率                                                 */
/* ====================================================================== */

static void test_dodge_flee(void)
{
    int rate;
    header("Test 3: Dodge & Flee");

    btl_init(NULL);

    /* 回避率: def_spd > atk_spd */
    rate = btl_calc_dodge_rate(10, 20);
    check_eq("dodge (10,20) = 20%", rate, 20);

    rate = btl_calc_dodge_rate(10, 10);
    check_eq("dodge same spd = 0%", rate, 0);

    rate = btl_calc_dodge_rate(10, 50);
    check_eq("dodge cap 50%", rate, 50);

    /* 逃走確率 */
    rate = btl_calc_flee_rate(10, 10);
    check_eq("flee same spd = 20%", rate, 20);

    rate = btl_calc_flee_rate(20, 10);
    check_eq("flee 2x spd = 80%", rate, 80);

    rate = btl_calc_flee_rate(5, 10);
    check_eq("flee slow = 20%", rate, 20);

    rate = btl_calc_flee_rate(0, 10);
    check_eq("flee runner=0 = 20%", rate, 20);

    rate = btl_calc_flee_rate(10, 0);
    check_eq("flee chaser=0 = 80%", rate, 80);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト4: ポリシー差し替え                                               */
/* ====================================================================== */

static int custom_damage(int atk, int def)
{
    /* カスタム計算: atk*2 - def */
    int d = atk * 2 - def;
    return (d < 0) ? 0 : d;
}

static void test_policy(void)
{
    int dmg;
    header("Test 4: Policy Override");

    btl_init(NULL);

    btl_set_damage_policy(custom_damage);
    dmg = btl_calc_damage(10, 5);
    check_eq("custom: 10*2-5=15", dmg, 15);

    btl_set_damage_policy(NULL);
    btl_shutdown();
}

/* ====================================================================== */
/*  テスト5: 先攻判定                                                       */
/* ====================================================================== */

static void test_first_strike(void)
{
    int res;
    header("Test 5: First Strike");

    rng_seed(123);

    res = btl_first_strike(20, 10);
    check_eq("p1 faster", res, 1);

    res = btl_first_strike(5, 15);
    check_eq("p2 faster", res, 2);

    /* 同値: ランダム → 100回で両方出ることを確認 */
    {
        int p1_cnt = 0, p2_cnt = 0, i;
        for (i = 0; i < 100; i++) {
            res = btl_first_strike(10, 10);
            if (res == 1) p1_cnt++;
            else p2_cnt++;
        }
        check("tie: both chosen", p1_cnt > 10 && p2_cnt > 10);
    }
}

/* ====================================================================== */
/*  テスト6: 状態異常                                                       */
/* ====================================================================== */

static void test_status(void)
{
    BtlUnit unit;
    int prevented;

    header("Test 6: Status Effects");
    btl_init(NULL);

    memset(&unit, 0, sizeof(unit));
    unit.hp = 100;
    unit.max_hp = 100;

    /* 毒を付与 */
    btl_apply_status(&unit, BTL_STATUS_POISON);
    check("has poison", btl_has_status(&unit, BTL_STATUS_POISON));
    check("no para", !btl_has_status(&unit, BTL_STATUS_PARA));

    /* tick: 毒ダメージ (フォールバック: HP-1) */
    prevented = btl_status_tick(&unit);
    check_eq("tick: not prevented", prevented, 0);
    check_eq("tick: hp=99", (int)unit.hp, 99);

    /* 麻痺を追加 */
    btl_apply_status(&unit, BTL_STATUS_PARA);
    prevented = btl_status_tick(&unit);
    check_eq("para: prevented", prevented, 1);
    check_eq("para+poison: hp=98", (int)unit.hp, 98);

    /* 解除 */
    btl_clear_status(&unit, BTL_STATUS_PARA);
    check("para cleared", !btl_has_status(&unit, BTL_STATUS_PARA));
    check("poison still", btl_has_status(&unit, BTL_STATUS_POISON));

    btl_clear_status(&unit, BTL_STATUS_POISON);
    prevented = btl_status_tick(&unit);
    check_eq("all clear: not prevented", prevented, 0);
    check_eq("all clear: hp unchanged", (int)unit.hp, 98);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト7: 修飾子 (バフ/デバフ)                                           */
/* ====================================================================== */

static void test_modifiers(void)
{
    BtlUnit unit;
    BtlModifier mods[BTL_MOD_MAX];
    BtlModifier m;
    i16 eff;
    int rc;

    header("Test 7: Modifiers");
    btl_init(NULL);

    memset(&unit, 0, sizeof(unit));
    unit.atk = 50;
    unit.def = 30;
    unit.spd = 20;
    unit.mag = 15;
    memset(mods, 0, sizeof(mods));

    /* 基礎値確認 */
    eff = btl_effective_stat(&unit, mods, BTL_STAT_ATK);
    check_eq("base atk=50", (int)eff, 50);

    /* ATK +10 バフ追加 (3ターン) */
    memset(&m, 0, sizeof(m));
    m.stat = BTL_STAT_ATK;
    m.turns = 3;
    m.add_value = 10;
    m.mul_pct = 100;

    rc = btl_add_modifier(&unit, mods, &m);
    check_eq("add mod", rc, 0);
    check_eq("mod_count=1", (int)unit.modifier_count, 1);

    eff = btl_effective_stat(&unit, mods, BTL_STAT_ATK);
    check_eq("atk+10=60", (int)eff, 60);

    /* DEF x150% バフ追加 */
    memset(&m, 0, sizeof(m));
    m.stat = BTL_STAT_DEF;
    m.turns = 2;
    m.add_value = 0;
    m.mul_pct = 150;

    rc = btl_add_modifier(&unit, mods, &m);
    check_eq("add def mod", rc, 0);

    eff = btl_effective_stat(&unit, mods, BTL_STAT_DEF);
    check_eq("def*1.5=45", (int)eff, 45);

    /* ターン経過 */
    btl_tick_modifiers(&unit, mods);
    check_eq("tick: atk turns=2", (int)mods[0].turns, 2);

    btl_tick_modifiers(&unit, mods);
    /* DEF修飾子 (turns=2→1→0=期限切れ) */
    check_eq("tick2: mod_count", (int)unit.modifier_count, 1);

    eff = btl_effective_stat(&unit, mods, BTL_STAT_DEF);
    check_eq("def back to 30", (int)eff, 30);

    /* クリア */
    btl_clear_modifiers(&unit, mods);
    check_eq("clear: mod_count=0", (int)unit.modifier_count, 0);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト8: コマンドマトリクス (手動設定)                                   */
/* ====================================================================== */

static void test_command_matrix(void)
{
    u8 res;
    header("Test 8: Command Matrix (manual)");

    btl_init(NULL);

    /* デフォルト (全て0=NORMAL) */
    res = btl_resolve_commands(0, 0);
    check_eq("default: NORMAL", (int)res, BTL_RES_NORMAL);

    /* 範囲外 */
    res = btl_resolve_commands(BTL_CMD_MAX, 0);
    check_eq("out-of-range: NORMAL", (int)res, BTL_RES_NORMAL);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト9: btl_resolve_turn (統合テスト)                                   */
/* ====================================================================== */

static void test_resolve_turn(void)
{
    BtlUnit atk_u, def_u;
    BtlResult res;

    header("Test 9: Resolve Turn");
    btl_init(NULL);
    rng_seed(9999);

    memset(&atk_u, 0, sizeof(atk_u));
    atk_u.hp = 100; atk_u.max_hp = 100;
    atk_u.atk = 30; atk_u.def = 15;
    atk_u.spd = 20; atk_u.mag = 10;

    memset(&def_u, 0, sizeof(def_u));
    def_u.hp = 80; def_u.max_hp = 80;
    def_u.atk = 25; def_u.def = 20;
    def_u.spd = 15; def_u.mag = 8;

    /* 通常攻撃 (マトリクス未設定=NORMAL) */
    res = btl_resolve_turn(&atk_u, 0, &def_u, 0);
    check_eq("turn: result_type", (int)res.result_type, BTL_RES_NORMAL);
    api->kprintf(ATTR_WHITE, "    damage=%d, dodge=%d\n",
                 (int)res.damage, (int)res.is_dodge);

    /* 属性なし → 等倍 */
    check_eq("elem_mul neutral", (int)res.element_mul, BTL_ELEM_NEUTRAL);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト10: DB付きテスト                                                  */
/* ====================================================================== */

static void test_db_load(void)
{
    int rc;
    u8 res;
    BtlUnit atk_u, def_u;
    BtlResult bres;
    i16 mul;

    header("Test 10: DB Load");

    rc = btl_init("/host/assets/battle.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] btl_init returned %d "
            "(is battle.db deployed?)\n", rc);
        return;
    }
    check("btl_init(battle.db)", rc == 0);

    /* コマンドマトリクス確認 */
    res = btl_resolve_commands(0, 0);
    check_eq("atk vs none = NORMAL", (int)res, BTL_RES_NORMAL);

    res = btl_resolve_commands(0, 1);
    check_eq("atk vs guard = GUARD", (int)res, BTL_RES_GUARD);

    res = btl_resolve_commands(0, 3);
    check_eq("atk vs reflect = REFLECT", (int)res, BTL_RES_REFLECT);

    res = btl_resolve_commands(1, 3);
    check_eq("strong vs reflect = COUNTER", (int)res, BTL_RES_COUNTER);

    res = btl_resolve_commands(3, 0);
    check_eq("charge vs none = MISS", (int)res, BTL_RES_MISS);

    res = btl_resolve_commands(4, 0);
    check_eq("flee vs none = YIELD", (int)res, BTL_RES_YIELD);

    /* 属性相性テスト */
    mul = btl_element_multiplier(1, 2);   /* 火→氷 */
    check_eq("fire->ice = 512", (int)mul, 512);

    mul = btl_element_multiplier(1, 1);   /* 火→火 */
    check_eq("fire->fire = 128", (int)mul, 128);

    mul = btl_element_multiplier(0, 2);   /* 無→氷 */
    check_eq("none->ice = 256", (int)mul, 256);

    /* 属性付きターン解決 */
    rng_seed(7777);
    memset(&atk_u, 0, sizeof(atk_u));
    atk_u.hp = 100; atk_u.max_hp = 100;
    atk_u.atk = 30; atk_u.def = 15;
    atk_u.spd = 20; atk_u.elements = 1;  /* 火属性 */

    memset(&def_u, 0, sizeof(def_u));
    def_u.hp = 80; def_u.max_hp = 80;
    def_u.atk = 25; def_u.def = 20;
    def_u.spd = 15; def_u.elements = 2;  /* 氷属性 */

    bres = btl_resolve_turn(&atk_u, 0, &def_u, 0);
    api->kprintf(ATTR_WHITE, "    fire->ice: dmg=%d, elem_mul=%d\n",
                 (int)bres.damage, (int)bres.element_mul);
    check_eq("fire->ice mul", (int)bres.element_mul, 512);
    /* 2倍ダメージなので通常より大きいはず */
    if (!bres.is_dodge) {
        check("fire->ice dmg > 0", (int)bres.damage > 0);
    }

    /* 状態異常tick (DB定義: 毒=毎ターン2ダメージ) */
    {
        BtlUnit poisoned;
        int prev;
        memset(&poisoned, 0, sizeof(poisoned));
        poisoned.hp = 50; poisoned.max_hp = 50;
        btl_apply_status(&poisoned, BTL_STATUS_POISON);
        prev = (int)poisoned.hp;
        btl_status_tick(&poisoned);
        check_eq("DB poison tick: hp-2", (int)poisoned.hp, prev - 2);
    }

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト11: コールバック                                                  */
/* ====================================================================== */

static int g_cb_called;
static void test_callback_fn(const BtlUnit *a, const BtlUnit *d,
                              const BtlResult *r)
{
    (void)a; (void)d; (void)r;
    g_cb_called++;
}

static void test_callback(void)
{
    BtlUnit atk_u, def_u;

    header("Test 11: Result Callback");
    btl_init(NULL);
    rng_seed(555);

    g_cb_called = 0;
    btl_set_result_callback(test_callback_fn);

    memset(&atk_u, 0, sizeof(atk_u));
    atk_u.atk = 20; atk_u.spd = 10;
    memset(&def_u, 0, sizeof(def_u));
    def_u.def = 10; def_u.spd = 5;

    btl_resolve_turn(&atk_u, 0, &def_u, 0);
    check_eq("callback called", g_cb_called, 1);

    btl_set_result_callback(NULL);
    btl_shutdown();
}

/* ====================================================================== */
/*  テスト12: パーティバトル (btl_resolve_multi) [Phase 2]                  */
/* ====================================================================== */

static void test_resolve_multi(void)
{
    BtlUnit atk_u;
    BtlUnit defs[3];
    u8 def_cmds[3];
    BtlResult results[3];
    int rc;
    int i;

    header("Test 12: Resolve Multi (Party)");
    btl_init(NULL);
    rng_seed(12345);

    memset(&atk_u, 0, sizeof(atk_u));
    atk_u.hp = 100; atk_u.max_hp = 100;
    atk_u.atk = 30; atk_u.def = 15;
    atk_u.spd = 25;

    for (i = 0; i < 3; i++) {
        memset(&defs[i], 0, sizeof(BtlUnit));
        defs[i].hp = 50; defs[i].max_hp = 50;
        defs[i].atk = 10; defs[i].def = 10 + i * 5;
        defs[i].spd = 10;
        def_cmds[i] = 0;  /* 全員無防備 */
    }

    rc = btl_resolve_multi(&atk_u, 0, defs, def_cmds, results, 3);
    check_eq("multi: count=3", rc, 3);

    for (i = 0; i < 3; i++) {
        check_eq("multi: result_type", (int)results[i].result_type,
                 BTL_RES_NORMAL);
        api->kprintf(ATTR_WHITE, "    def[%d]: dmg=%d, dodge=%d\n",
                     i, (int)results[i].damage, (int)results[i].is_dodge);
    }

    /* DEFが高い敵ほどダメージが低い (回避なしの場合) */
    if (!results[0].is_dodge && !results[2].is_dodge) {
        check("higher def = less dmg",
              (int)results[0].damage >= (int)results[2].damage);
    }

    /* NULL引数テスト */
    rc = btl_resolve_multi(NULL, 0, defs, def_cmds, results, 3);
    check_eq("multi: NULL atk -> 0", rc, 0);

    rc = btl_resolve_multi(&atk_u, 0, defs, def_cmds, results, 0);
    check_eq("multi: count=0 -> 0", rc, 0);

    btl_shutdown();
}

/* ====================================================================== */
/*  テスト13: 変身システム [Phase 2]                                        */
/* ====================================================================== */

static void test_transform(void)
{
    BtlUnit unit;
    BtlTransformState state;
    BtlTransformDef def;
    int rc;
    int i;

    header("Test 13: Transform");
    btl_init(NULL);
    rng_seed(54321);

    /* ユニット初期化 */
    memset(&unit, 0, sizeof(unit));
    unit.hp = 80; unit.max_hp = 100;
    unit.atk = 50; unit.def = 30;
    unit.spd = 20; unit.mag = 15;

    memset(&state, 0, sizeof(state));

    /* 変身定義: ステ2倍, HP3倍, 5ターン, 解除確率0%, 武器ATK+10, 盾DEF+5, 鎧DEF+5 */
    memset(&def, 0, sizeof(def));
    def.stat_mul_pct = 200;
    def.hp_mul_pct   = 300;
    def.max_turns    = 5;
    def.release_rate = 0;
    def.weapon_atk   = 10;
    def.shield_def   = 5;
    def.armor_def    = 5;

    /* 変身開始 */
    rc = btl_transform(&unit, &state, &def);
    check_eq("transform ok", rc, 0);
    check_eq("state active", (int)state.active, 1);

    /* ステ確認: atk = 50*2 + 10 = 110 */
    check_eq("trans atk=110", (int)unit.atk, 110);
    /* def = 30*2 + 5 + 5 = 70 */
    check_eq("trans def=70", (int)unit.def, 70);
    /* spd = 20*2 = 40 */
    check_eq("trans spd=40", (int)unit.spd, 40);
    /* mag = 15*2 = 30 */
    check_eq("trans mag=30", (int)unit.mag, 30);
    /* max_hp = 100*3 = 300, hp = 80*300/100 = 240 */
    check_eq("trans max_hp=300", (int)unit.max_hp, 300);
    check_eq("trans hp=240", (int)unit.hp, 240);

    /* 保存値確認 */
    check_eq("orig atk=50", (int)state.orig_atk, 50);
    check_eq("orig max_hp=100", (int)state.orig_max_hp, 100);
    check_eq("turns_left=5", (int)state.turns_left, 5);

    /* 二重変身防止 */
    rc = btl_transform(&unit, &state, &def);
    check_eq("double transform = -1", rc, -1);

    /* ターン経過 */
    for (i = 0; i < 4; i++) {
        rc = btl_transform_tick(&unit, &state);
        check_eq("tick: continue", rc, 0);
    }
    check_eq("after 4 ticks: turns_left=1", (int)state.turns_left, 1);

    /* 5ターン目 → 自動解除 */
    rc = btl_transform_tick(&unit, &state);
    check_eq("tick5: released", rc, 1);
    check_eq("released: active=0", (int)state.active, 0);

    /* ステータス復元確認 */
    check_eq("restored atk=50", (int)unit.atk, 50);
    check_eq("restored def=30", (int)unit.def, 30);
    check_eq("restored spd=20", (int)unit.spd, 20);
    check_eq("restored mag=15", (int)unit.mag, 15);
    check_eq("restored max_hp=100", (int)unit.max_hp, 100);
    /* HP: 変身時に 80 → 240 (比率80%を維持して拡大) したので、
     * 解除時も比率を維持して 240 * 100 / 300 = 80 に戻る。
     * 単純クランプ(=100)にすると変身→解除するだけで全快してしまう。 */
    check_eq("hp restored=80", (int)unit.hp, 80);

    /* 強制解除テスト */
    memset(&state, 0, sizeof(state));
    unit.hp = 100;
    def.max_turns = 0;   /* 無制限 */
    def.release_rate = 0;
    rc = btl_transform(&unit, &state, &def);
    check_eq("unlimited transform", rc, 0);

    /* 無制限のためtickでは解除されない */
    rc = btl_transform_tick(&unit, &state);
    check_eq("unlimited: continue", rc, 0);

    /* 強制解除 */
    btl_transform_release(&unit, &state);
    check_eq("force release: active=0", (int)state.active, 0);
    check_eq("force: atk=50", (int)unit.atk, 50);

    /* 確率解除テスト */
    memset(&state, 0, sizeof(state));
    unit.hp = 100; unit.max_hp = 100;
    unit.atk = 50; unit.def = 30; unit.spd = 20; unit.mag = 15;
    def.max_turns = 0;    /* ターン解除なし */
    def.release_rate = 50; /* 50%確率解除 */
    rc = btl_transform(&unit, &state, &def);
    check_eq("prob transform", rc, 0);

    /* 100回tickして少なくとも1回は解除されるはず */
    {
        int released = 0;
        for (i = 0; i < 100; i++) {
            if (!state.active) break;
            rc = btl_transform_tick(&unit, &state);
            if (rc == 1) released = 1;
        }
        check("prob: eventually released", released || !state.active);
    }

    btl_shutdown();
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "btl_test: libos32battle test suite (Phase 1+2)\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    rng_seed(0xBA77E01);

    /* Phase 1 テスト */
    test_init_no_db();
    test_damage_calc();
    test_dodge_flee();
    test_policy();
    test_first_strike();
    test_status();
    test_modifiers();
    test_command_matrix();
    test_resolve_turn();
    test_db_load();
    test_callback();

    /* Phase 2 テスト */
    test_resolve_multi();
    test_transform();

    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n",
                 g_passed, g_total);
    if (g_passed == g_total) {
        api->kprintf(ATTR_GREEN, "All tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "%d test(s) failed.\n", g_total - g_passed);
    }
    return 0;
}

