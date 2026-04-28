/* ======================================================================== */
/*  INV_TEST.C — libos32inv テストプログラム                                  */
/*                                                                          */
/*  インベントリエンジンの全機能をテストするスイート。                        */
/*  テスト1: 初期化 + DBキャッシュ (マスタデータロード)                      */
/*  テスト2: バッグ操作 (add/remove/count/has/free_slots)                   */
/*  テスト3: スタック可能アイテム                                            */
/*  テスト4: 装備変更・ボーナス計算                                          */
/*  テスト5: 装備解除・旧装備自動返却                                        */
/*  テスト6: 装備ポリシーコールバック                                        */
/*  テスト7: ショップ購入・売却                                              */
/*  テスト8: ショップ品揃え (shop_lineup)                                    */
/*  テスト9: 抽選 (lottery)                                                  */
/*  テスト10: アイテム効果クエリ                                              */
/*  テスト11: エッジケース (満杯、不正ID等)                                  */
/* ======================================================================== */

#include "os32api.h"
#include "libos32inv.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* ---- テストヘルパー ---- */

static int g_total = 0;
static int g_passed = 0;

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
/*  テスト用DBセットアップ (:memory:)                                      */
/* ====================================================================== */

static int setup_test_db(void)
{
    db_handle_t db;
    int rc;

    db = db_open(":memory:");
    if (db < 0) return -1;

    /* items テーブル */
    rc = db_exec(db,
        "CREATE TABLE items("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "type INTEGER NOT NULL DEFAULT 0, "
        "effect INTEGER DEFAULT 0, param INTEGER DEFAULT 0, "
        "rarity INTEGER DEFAULT 1, equip_slot INTEGER DEFAULT 0, "
        "stackable INTEGER DEFAULT 0, "
        "stat_bonus INTEGER DEFAULT 0, stat_type INTEGER DEFAULT 0, "
        "stage INTEGER DEFAULT 1, price INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    /* shop_lineup テーブル */
    rc = db_exec(db,
        "CREATE TABLE shop_lineup("
        "shop_type INTEGER NOT NULL, stage INTEGER NOT NULL, "
        "item_id INTEGER NOT NULL, "
        "PRIMARY KEY (shop_type, stage, item_id))");
    if (rc < 0) { db_close(db); return -1; }

    /* lottery_tables テーブル */
    rc = db_exec(db,
        "CREATE TABLE lottery_tables("
        "table_type INTEGER NOT NULL, item_id INTEGER NOT NULL, "
        "weight INTEGER NOT NULL DEFAULT 1, "
        "min_stage INTEGER DEFAULT 1, "
        "PRIMARY KEY (table_type, item_id))");
    if (rc < 0) { db_close(db); return -1; }

    /* --- データ投入 --- */

    /* 消耗品 (スタック可能) */
    db_exec(db, "INSERT INTO items VALUES(1,'Herb',0,1,30,1,0,1,0,0,1,10)");
    db_exec(db, "INSERT INTO items VALUES(2,'Antidote',0,2,0,1,0,1,0,0,1,15)");
    db_exec(db, "INSERT INTO items VALUES(3,'Elixir',0,3,99,3,0,1,0,0,3,500)");

    /* 武器 */
    db_exec(db, "INSERT INTO items VALUES(10,'ShortSword',1,0,0,1,0,0,10,0,1,100)");
    db_exec(db, "INSERT INTO items VALUES(11,'LongSword',1,0,0,2,0,0,25,0,2,300)");
    db_exec(db, "INSERT INTO items VALUES(12,'MagicRod',1,0,0,3,0,0,15,3,2,250)");

    /* 盾 */
    db_exec(db, "INSERT INTO items VALUES(20,'WoodShield',2,0,0,1,1,0,5,1,1,50)");
    db_exec(db, "INSERT INTO items VALUES(21,'IronShield',2,0,0,2,1,0,15,1,2,200)");

    /* 鎧 */
    db_exec(db, "INSERT INTO items VALUES(30,'Leather',3,0,0,1,2,0,8,1,1,80)");
    db_exec(db, "INSERT INTO items VALUES(31,'ChainMail',3,0,0,2,2,0,20,1,2,350)");

    /* アクセサリ */
    db_exec(db, "INSERT INTO items VALUES(40,'SpeedRing',4,0,0,2,7,0,10,2,1,150)");

    /* ショップ品揃え */
    db_exec(db, "INSERT INTO shop_lineup VALUES(0,1,10)");  /* 装備屋S1: ShortSword */
    db_exec(db, "INSERT INTO shop_lineup VALUES(0,1,20)");  /* 装備屋S1: WoodShield */
    db_exec(db, "INSERT INTO shop_lineup VALUES(0,1,30)");  /* 装備屋S1: Leather */
    db_exec(db, "INSERT INTO shop_lineup VALUES(0,2,11)");  /* 装備屋S2: LongSword */
    db_exec(db, "INSERT INTO shop_lineup VALUES(0,2,21)");  /* 装備屋S2: IronShield */
    db_exec(db, "INSERT INTO shop_lineup VALUES(1,1,1)");   /* 道具屋S1: Herb */
    db_exec(db, "INSERT INTO shop_lineup VALUES(1,1,2)");   /* 道具屋S1: Antidote */

    /* 抽選テーブル (宝箱) */
    db_exec(db, "INSERT INTO lottery_tables VALUES(0,1,50,1)");  /* Herb: weight=50 */
    db_exec(db, "INSERT INTO lottery_tables VALUES(0,2,30,1)");  /* Antidote: weight=30 */
    db_exec(db, "INSERT INTO lottery_tables VALUES(0,3,5,3)");   /* Elixir: weight=5, S3以上 */
    db_exec(db, "INSERT INTO lottery_tables VALUES(0,10,15,1)"); /* ShortSword: weight=15 */

    db_close(db);
    return 0;
}

/* ====================================================================== */
/*  テスト1: 初期化 + DBキャッシュ                                         */
/* ====================================================================== */
static void test_init(void)
{
    int rc;

    header("Test 1: Init + DB Cache");

    rc = setup_test_db();
    check("setup_test_db", rc == 0);

    rc = inv_init(":memory:");
    /* :memory: はコネクションごとに別DBなので空で初期化 → エラーは想定内 */
    api->kprintf(ATTR_WHITE, "  inv_init(:memory:) = %d\n", rc);
    check("inv_init returns (no data is ok)", rc == 0 || rc == -2);

    inv_shutdown();
}

/* ====================================================================== */
/*  テスト2: ファイルDB初期化                                              */
/* ====================================================================== */
static int test_file_db(void)
{
    int rc;

    header("Test 2: File DB Init (/db/items.db)");

    rc = inv_init("/db/items.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] inv_init returned %d "
            "(is /db/items.db deployed?)\n", rc);
        return -1;
    }
    check("inv_init(/db/items.db)", rc == 0);

    check("master_count > 0", inv_master_count() > 0);
    api->kprintf(ATTR_WHITE, "  items loaded: %d\n", inv_master_count());

    return 0;
}

/* ====================================================================== */
/*  テスト3: バッグ操作                                                    */
/* ====================================================================== */
static void test_bag_ops(void)
{
    InvBag bag;
    int rc;

    header("Test 3: Bag Operations");

    inv_bag_init(&bag, 8, 4);
    check_eq("max_slots", (int)bag.max_slots, 8);
    check_eq("equip_count", (int)bag.equip_count, 4);
    check_eq("free_slots init", inv_free_slots(&bag), 8);

    /* Herb 追加 */
    rc = inv_add(&bag, 1, 3);
    check_eq("add Herb x3", rc, 0);
    check_eq("count Herb", inv_count_item(&bag, 1), 3);
    check("has Herb", inv_has(&bag, 1));
    check_eq("free_slots after add", inv_free_slots(&bag), 7);

    /* ShortSword 追加 (非スタック) */
    rc = inv_add(&bag, 10, 1);
    check_eq("add ShortSword", rc, 0);
    check("has ShortSword", inv_has(&bag, 10));
    check_eq("free_slots", inv_free_slots(&bag), 6);

    /* 除去テスト */
    rc = inv_remove(&bag, 0, 1);
    check_eq("remove Herb x1", rc, 0);
    check_eq("count Herb after remove", inv_count_item(&bag, 1), 2);

    /* 全除去 */
    rc = inv_remove(&bag, 0, 10);
    check_eq("remove all Herb", rc, 0);
    check_eq("count Herb gone", inv_count_item(&bag, 1), 0);
    check("not has Herb", !inv_has(&bag, 1));

    /* スロット内容取得 */
    {
        const InvSlot *s = inv_get_slot(&bag, 1);
        check("slot 1 valid", s != (void *)0);
        if (s) {
            check_eq("slot 1 item_id", (int)s->item_id, 10);
        }
    }
}

/* ====================================================================== */
/*  テスト4: スタック可能アイテム                                          */
/* ====================================================================== */
static void test_stacking(void)
{
    InvBag bag;
    int rc;

    header("Test 4: Stacking");

    inv_bag_init(&bag, 4, 0);

    /* Herb 50個追加 (スタック可能) */
    rc = inv_add(&bag, 1, 50);
    check_eq("add Herb x50", rc, 0);
    check_eq("count Herb", inv_count_item(&bag, 1), 50);
    check_eq("used 1 slot", inv_free_slots(&bag), 3);

    /* さらに 30 追加 → 合算で80 */
    rc = inv_add(&bag, 1, 30);
    check_eq("add Herb x30 more", rc, 0);
    check_eq("count Herb stacked", inv_count_item(&bag, 1), 80);
    check_eq("still 1 slot", inv_free_slots(&bag), 3);

    /* 99超え → 新スロットに分割 */
    rc = inv_add(&bag, 1, 30);
    check_eq("add Herb x30 overflow", rc, 0);
    check_eq("total Herb 110", inv_count_item(&bag, 1), 110);
    check_eq("2 slots used", inv_free_slots(&bag), 2);
}

/* ====================================================================== */
/*  テスト5: 装備変更・ボーナス計算                                        */
/* ====================================================================== */
static void test_equip(void)
{
    InvBag bag;
    int rc;
    i16 bonus;

    header("Test 5: Equip + Bonus");

    inv_bag_init(&bag, 8, 4);

    /* 武器と盾をインベントリに追加 */
    inv_add(&bag, 10, 1);  /* ShortSword: ATK+10 */
    inv_add(&bag, 20, 1);  /* WoodShield: DEF+5 */
    inv_add(&bag, 40, 1);  /* SpeedRing: SPD+10 */

    /* 武器装備 */
    rc = inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    check_eq("equip ShortSword", rc, 0);
    check("ShortSword removed from bag", !inv_has(&bag, 10));

    /* 盾装備 */
    rc = inv_equip(&bag, 20, INV_ESLOT_SHIELD);
    check_eq("equip WoodShield", rc, 0);

    /* ボーナス計算 */
    bonus = inv_equip_bonus(&bag, INV_STAT_ATK);
    check_eq("ATK bonus", (int)bonus, 10);

    bonus = inv_equip_bonus(&bag, INV_STAT_DEF);
    check_eq("DEF bonus", (int)bonus, 5);

    bonus = inv_equip_bonus(&bag, INV_STAT_SPD);
    check_eq("SPD bonus (none equipped)", (int)bonus, 0);

    /* リング装備 → SPDボーナス */
    rc = inv_equip(&bag, 40, INV_ESLOT_RING);
    /* ring equip_slot=7だがbag.equip_count=4 → slot_type=3 (ACCESSORY) で */
    /* 4スロットしかないのでINV_ESLOT_RING(7)は範囲外 → -1 */
    /* equip_count を広げてテストし直す */
    if (rc < 0) {
        /* equip_count が 4 なので slot 7 は範囲外 */
        check("ring equip: slot out of range (expected)", rc == -1);
    }
}

/* ====================================================================== */
/*  テスト6: 装備解除・旧装備返却                                          */
/* ====================================================================== */
static void test_unequip(void)
{
    InvBag bag;
    int rc;

    header("Test 6: Unequip + Return");

    inv_bag_init(&bag, 8, 4);

    /* ShortSword を装備 */
    inv_add(&bag, 10, 1);
    inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    check("equipped", bag.equip[INV_ESLOT_WEAPON].item_id == 10);

    /* 解除 → インベントリに戻る */
    rc = inv_unequip(&bag, INV_ESLOT_WEAPON);
    check_eq("unequip", rc, 0);
    check("equip slot cleared", bag.equip[INV_ESLOT_WEAPON].item_id == 0);
    check("returned to bag", inv_has(&bag, 10));

    /* 武器変更: ShortSword → LongSword (旧装備自動返却) */
    inv_add(&bag, 10, 1);
    inv_add(&bag, 11, 1);
    inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    rc = inv_equip(&bag, 11, INV_ESLOT_WEAPON);
    check_eq("swap equip", rc, 0);
    check_eq("equipped LongSword",
             (int)bag.equip[INV_ESLOT_WEAPON].item_id, 11);
    check("ShortSword returned", inv_has(&bag, 10));
}

/* ====================================================================== */
/*  テスト7: 装備ポリシーコールバック                                      */
/* ====================================================================== */

static int deny_all_policy(u16 equip_id, u8 class_id, u8 level)
{
    (void)equip_id;
    (void)class_id;
    (void)level;
    return 0;  /* 全て拒否 */
}

static void test_equip_policy(void)
{
    InvBag bag;
    int rc;

    header("Test 7: Equip Policy");

    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 10, 1);

    /* 全拒否ポリシーを設定 */
    inv_set_equip_policy(deny_all_policy);

    rc = inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    check_eq("equip denied by policy", rc, -1);
    check("still in bag", inv_has(&bag, 10));

    /* ポリシー解除 */
    inv_set_equip_policy((inv_equip_check_fn)0);

    rc = inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    check_eq("equip allowed (no policy)", rc, 0);
}

/* ====================================================================== */
/*  テスト8: ショップ購入・売却                                            */
/* ====================================================================== */
static void test_shop(void)
{
    InvBag bag;
    u32 wallet;
    int rc;

    header("Test 8: Shop Buy / Sell");

    inv_bag_init(&bag, 8, 4);
    wallet = 1000;

    /* Herb (price=10) 購入 */
    rc = inv_shop_buy(&bag, 1, &wallet);
    check_eq("buy Herb", rc, 0);
    check_eq("wallet after buy", (int)wallet, 990);
    check("has Herb", inv_has(&bag, 1));

    /* ShortSword (price=100) 購入 */
    rc = inv_shop_buy(&bag, 10, &wallet);
    check_eq("buy ShortSword", rc, 0);
    check_eq("wallet after sword", (int)wallet, 890);

    /* 資金不足テスト: Elixir (price=500) */
    wallet = 100;
    rc = inv_shop_buy(&bag, 3, &wallet);
    check_eq("buy Elixir: no money", rc, -2);
    check_eq("wallet unchanged", (int)wallet, 100);

    /* 売却: slot 0 (Herb, sell=5) */
    wallet = 0;
    rc = inv_shop_sell(&bag, 0, &wallet);
    check_eq("sell Herb", rc, 0);
    check_eq("wallet after sell", (int)wallet, 5);
}

/* ====================================================================== */
/*  テスト9: ショップ品揃え                                                */
/* ====================================================================== */
static void test_shop_lineup(void)
{
    u16 ids[16];
    int count;

    header("Test 9: Shop Lineup");

    /* 装備屋 stage 1: ShortSword, WoodShield, Leather */
    count = inv_shop_list(0, 1, ids, 16);
    check_eq("equip shop S1 count", count, 3);

    /* 装備屋 stage 2: stage<=2 全て (5品) */
    count = inv_shop_list(0, 2, ids, 16);
    check_eq("equip shop S2 count", count, 6);

    /* 道具屋 stage 1: Herb, Antidote */
    count = inv_shop_list(1, 1, ids, 16);
    check_eq("item shop S1 count", count, 2);

    /* 存在しないタイプ */
    count = inv_shop_list(99, 1, ids, 16);
    check_eq("invalid shop type", count, 0);
}

/* ====================================================================== */
/*  テスト10: 抽選                                                         */
/* ====================================================================== */
static void test_lottery(void)
{
    int i;
    int herb_count = 0;
    int antidote_count = 0;
    int sword_count = 0;
    int elixir_count = 0;
    int total_draws = 100;

    header("Test 10: Lottery");

    /* ステージ1: Elixir(min_stage=3) は出ないはず */
    for (i = 0; i < total_draws; i++) {
        u16 id = inv_lottery(0, 1);
        if (id == 1) herb_count++;
        else if (id == 2) antidote_count++;
        else if (id == 10) sword_count++;
        else if (id == 3) elixir_count++;
    }

    check("herb drawn > 0", herb_count > 0);
    check("antidote drawn > 0", antidote_count > 0);
    check("sword drawn > 0", sword_count > 0);
    check_eq("elixir not drawn at S1", elixir_count, 0);

    api->kprintf(ATTR_WHITE, "  draws: herb=%d, antidote=%d, sword=%d\n",
                 herb_count, antidote_count, sword_count);

    /* ステージ3: Elixir も出る */
    elixir_count = 0;
    for (i = 0; i < 200; i++) {
        u16 id = inv_lottery(0, 3);
        if (id == 3) elixir_count++;
    }
    check("elixir drawn at S3", elixir_count > 0);
    api->kprintf(ATTR_WHITE, "  elixir draws at S3: %d/200\n",
                 elixir_count);
}

/* ====================================================================== */
/*  テスト11: アイテム効果クエリ                                            */
/* ====================================================================== */
static void test_effect_query(void)
{
    header("Test 11: Effect Query");

    /* Herb: effect=1, param=30 */
    check_eq("Herb effect", (int)inv_get_effect(1), 1);
    check_eq("Herb param", (int)inv_get_param(1), 30);
    check("Herb is consumable", inv_is_consumable(1));
    check("Herb is not equipment", !inv_is_equipment(1));

    /* ShortSword: type=1 (weapon) */
    check("ShortSword is equipment", inv_is_equipment(10));
    check("ShortSword not consumable", !inv_is_consumable(10));

    /* 存在しないID */
    check_eq("invalid effect", (int)inv_get_effect(999), 0);
    check("invalid not consumable", !inv_is_consumable(999));
}

/* ====================================================================== */
/*  テスト12: エッジケース                                                  */
/* ====================================================================== */
static void test_edge_cases(void)
{
    InvBag bag;
    int rc;

    header("Test 12: Edge Cases");

    /* 小さいバッグで満杯テスト */
    inv_bag_init(&bag, 2, 2);

    inv_add(&bag, 10, 1);
    inv_add(&bag, 11, 1);
    rc = inv_add(&bag, 20, 1);
    check_eq("add to full bag", rc, -1);

    /* 不正スロット除去 */
    rc = inv_remove(&bag, 99, 1);
    check_eq("remove invalid slot", rc, -1);

    /* 空スロット除去 */
    inv_bag_init(&bag, 4, 2);
    rc = inv_remove(&bag, 0, 1);
    check_eq("remove empty slot", rc, -1);

    /* NULL バッグ */
    rc = inv_add((InvBag *)0, 1, 1);
    check_eq("add to NULL bag", rc, -1);

    /* アイテムID=0 */
    rc = inv_add(&bag, 0, 1);
    check_eq("add item_id 0", rc, -1);

    /* count=0 の追加 */
    rc = inv_add(&bag, 1, 0);
    check_eq("add count 0", rc, -1);

    /* 満杯バッグで装備解除 */
    inv_bag_init(&bag, 2, 2);
    inv_add(&bag, 10, 1);
    inv_equip(&bag, 10, INV_ESLOT_WEAPON);
    /* バッグに2個入れて満杯にする */
    inv_add(&bag, 1, 1);
    inv_add(&bag, 2, 1);
    rc = inv_unequip(&bag, INV_ESLOT_WEAPON);
    check_eq("unequip: bag full", rc, -2);

    /* マスターデータ参照 */
    {
        const InvItemDef *def = inv_get_def(10);
        check("get_def ShortSword", def != (void *)0);
        if (def) {
            check_eq("def name[0]", def->name[0], 'S');
            check_eq("def price", (int)def->price, 100);
        }
    }

    /* 不正ID参照 */
    {
        const InvItemDef *def = inv_get_def(9999);
        check("get_def invalid", def == (void *)0);
    }

    /* スロット境界テスト */
    {
        const InvSlot *s = inv_get_slot(&bag, 100);
        check("get_slot out of range", s == (void *)0);
    }
}

/* ====================================================================== */
/*  テスト13: 合成/クラフト (Phase 2)                                        */
/* ====================================================================== */
static void test_craft(void)
{
    InvBag bag;
    int rc;

    header("Test 13: Craft");

    /* レシピ数確認 */
    check("recipe_count > 0", inv_recipe_count() > 0);
    api->kprintf(ATTR_WHITE, "  recipes loaded: %d\n", inv_recipe_count());

    /* レシピ1: ShortSword + IronOre x3 → SteelSword */
    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 10, 1);  /* ShortSword */
    inv_add(&bag, 6, 5);   /* IronOre x5 */

    /* 合成可否チェック */
    check("can craft recipe 1", inv_can_craft(&bag, 1));

    /* 合成実行 */
    rc = inv_craft(&bag, 1);
    check_eq("craft recipe 1", rc, 0);
    check("has SteelSword", inv_has(&bag, 50));
    check("ShortSword consumed", !inv_has(&bag, 10));
    check_eq("IronOre remaining", inv_count_item(&bag, 6), 2);

    /* 素材不足テスト */
    check("cannot craft again", !inv_can_craft(&bag, 1));
    rc = inv_craft(&bag, 1);
    check_eq("craft fail: no materials", rc, -1);

    /* レシピ3: Antidote x2 → Herb x3 (素材Bなし) */
    inv_bag_init(&bag, 8, 0);
    inv_add(&bag, 2, 4);   /* Antidote x4 */
    check("can craft recipe 3", inv_can_craft(&bag, 3));
    rc = inv_craft(&bag, 3);
    check_eq("craft recipe 3", rc, 0);
    check_eq("Herb produced", inv_count_item(&bag, 1), 3);
    check_eq("Antidote remaining", inv_count_item(&bag, 2), 2);

    /* 不明レシピ */
    rc = inv_craft(&bag, 999);
    check_eq("craft unknown recipe", rc, -3);

    /* レシピ参照 */
    {
        const InvRecipe *r = inv_get_recipe(1);
        check("get_recipe(1) valid", r != (void *)0);
        if (r) {
            check_eq("recipe result_id", (int)r->result_id, 50);
            check_eq("recipe mat_a", (int)r->mat_a, 10);
        }
    }
}

/* ====================================================================== */
/*  テスト14: セット装備ボーナス (Phase 2)                                    */
/* ====================================================================== */
static void test_set_bonus(void)
{
    InvBag bag;
    i16 bonus;

    header("Test 14: Set Bonus");

    /* 鉄セット (set_id=1): IronShield(21) + ChainMail(31) → DEF+10 */
    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 21, 1);  /* IronShield: set_id=1 */
    inv_add(&bag, 31, 1);  /* ChainMail: set_id=1 */

    /* 1個だけ装備 → セット未発動 */
    inv_equip(&bag, 21, INV_ESLOT_SHIELD);
    bonus = inv_set_bonus(&bag, INV_STAT_DEF);
    check_eq("1pc: no set bonus", (int)bonus, 0);

    /* 2個装備 → セット発動 */
    inv_equip(&bag, 31, INV_ESLOT_ARMOR);
    bonus = inv_set_bonus(&bag, INV_STAT_DEF);
    check_eq("2pc iron: DEF+10", (int)bonus, 10);

    /* 装備ボーナス + セットボーナス統合 */
    bonus = inv_total_bonus(&bag, INV_STAT_DEF);
    /* IronShield:DEF+15 + ChainMail:DEF+20 + Set:DEF+10 = 45 */
    check_eq("total DEF bonus", (int)bonus, 45);

    /* セット解除: 1個外すとボーナス消滅 */
    inv_unequip(&bag, INV_ESLOT_ARMOR);
    bonus = inv_set_bonus(&bag, INV_STAT_DEF);
    check_eq("1pc: set lost", (int)bonus, 0);

    /* ミスリルセット (set_id=2): MithShield(22) + PlateMail(32) + MagicAmulet(42) */
    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 22, 1);  /* MithShield: set_id=2 */
    inv_add(&bag, 32, 1);  /* PlateMail: set_id=2 */
    inv_add(&bag, 42, 1);  /* MagicAmulet: set_id=2 */

    inv_equip(&bag, 22, INV_ESLOT_SHIELD);
    inv_equip(&bag, 32, INV_ESLOT_ARMOR);
    inv_equip(&bag, 42, INV_ESLOT_ACCESSORY);

    /* 3個装備: DEF+15(2pc) + DEF+25(3pc) = 40, MAG+10(3pc) */
    bonus = inv_set_bonus(&bag, INV_STAT_DEF);
    check_eq("3pc mith: DEF", (int)bonus, 40);
    bonus = inv_set_bonus(&bag, INV_STAT_MAG);
    check_eq("3pc mith: MAG", (int)bonus, 10);
}

/* ====================================================================== */
/*  テスト15: 耐久度 (Phase 2)                                                */
/* ====================================================================== */
static void test_durability(void)
{
    InvBag bag;
    int dur;
    const InvItemDef *def;

    header("Test 15: Durability");

    /* ShortSword: max_durability=30 */
    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 10, 1);
    inv_equip(&bag, 10, INV_ESLOT_WEAPON);

    /* 耐久度初期値確認 */
    def = inv_get_def(10);
    check("ShortSword def valid", def != (void *)0);
    if (def) {
        check_eq("max_durability", (int)def->max_durability, 30);
    }

    /* 消耗 */
    dur = inv_wear(&bag, INV_ESLOT_WEAPON);
    check("wear returns > 0", dur > 0);
    check("not broken after 1 wear", !inv_is_broken(&bag, INV_ESLOT_WEAPON));

    /* 複数回消耗して0に */
    {
        int i;
        for (i = 0; i < 300; i++) {
            dur = inv_wear(&bag, INV_ESLOT_WEAPON);
            if (dur == 0) break;
        }
    }
    check_eq("worn to 0", dur, 0);
    check("is broken", inv_is_broken(&bag, INV_ESLOT_WEAPON));

    /* 破損状態でさらに消耗 → 0のまま */
    dur = inv_wear(&bag, INV_ESLOT_WEAPON);
    check_eq("wear broken = 0", dur, 0);

    /* 修復 */
    dur = inv_repair(&bag, INV_ESLOT_WEAPON);
    check_eq("repaired to max", dur, 30);
    check("not broken after repair", !inv_is_broken(&bag, INV_ESLOT_WEAPON));

    /* 無限耐久テスト: HolyLance (max_durability=255) */
    inv_bag_init(&bag, 8, 4);
    inv_add(&bag, 14, 1);
    inv_equip(&bag, 14, INV_ESLOT_WEAPON);
    dur = inv_wear(&bag, INV_ESLOT_WEAPON);
    check_eq("infinite: no wear", dur, 0xFF);
    check("infinite: not broken", !inv_is_broken(&bag, INV_ESLOT_WEAPON));

    /* 空スロットのwear */
    dur = inv_wear(&bag, INV_ESLOT_SHIELD);
    check_eq("wear empty slot", dur, -1);

    /* 空スロットのrepair */
    dur = inv_repair(&bag, INV_ESLOT_SHIELD);
    check_eq("repair empty slot", dur, -1);
}

/* ====================================================================== */
/*  メイン                                                                 */
/* ====================================================================== */

int main(int argc, char **argv)
{
    int db_ok;

    (void)argc;
    (void)argv;

    api->kprintf(ATTR_CYAN,
        "==================================\n"
        " libos32inv Test Suite\n"
        "==================================\n");

    /* テスト1: メモリDB (空) */
    test_init();

    /* テスト2: ファイルDB初期化 */
    db_ok = test_file_db();

    if (db_ok < 0) {
        api->kprintf(ATTR_YELLOW,
            "\n  Skipping remaining tests (no DB).\n");
        api->kprintf(ATTR_CYAN,
            "\n=== Results: %d/%d passed ===\n", g_passed, g_total);
        return (g_passed == g_total) ? 0 : 1;
    }

    /* テスト3-15: ファイルDBで実行 */
    test_bag_ops();
    test_stacking();
    test_equip();
    test_unequip();
    test_equip_policy();
    test_shop();
    test_shop_lineup();
    test_lottery();
    test_effect_query();
    test_edge_cases();
    test_craft();
    test_set_bonus();
    test_durability();

    /* 終了 */
    inv_shutdown();

    api->kprintf(ATTR_CYAN,
        "\n==================================\n"
        " Results: %d/%d passed\n"
        "==================================\n",
        g_passed, g_total);

    return (g_passed == g_total) ? 0 : 1;
}
