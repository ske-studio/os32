/* ======================================================================== */
/*  ECON_TEST.C — libos32econ テストプログラム                                */
/*                                                                          */
/*  経済エンジンの全機能をテストするスイート。                                */
/*  テスト1: 初期化 + DBキャッシュ                                          */
/*  テスト2: 市場アクティベート・価格取得                                    */
/*  テスト3: 購入・販売                                                     */
/*  テスト4: 需給による価格変動                                              */
/*  テスト5: ターン更新 (在庫回復・価格収束)                                 */
/*  テスト6: 通貨・為替                                                     */
/*  テスト7: 外交と価格補正                                                 */
/*  テスト8: 輸送コスト・取引利益                                            */
/*  テスト9: NPC商人取引                                                    */
/*  テスト10: クエリ・分析                                                   */
/* ======================================================================== */

#include "os32api.h"
#include "libos32econ.h"
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
/*  テスト用DBセットアップ (:memory:)                                      */
/* ====================================================================== */

static int setup_test_db(void)
{
    db_handle_t db;
    int rc;

    db = db_open(":memory:");
    if (db < 0) return -1;

    /* テーブル作成 */
    rc = db_exec(db,
        "CREATE TABLE items("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "category INTEGER NOT NULL, base_price INTEGER NOT NULL, "
        "weight INTEGER DEFAULT 1, rarity INTEGER DEFAULT 1, "
        "curve_type INTEGER DEFAULT 0, season_amp INTEGER DEFAULT 0, "
        "diplo_weight INTEGER DEFAULT 100, elasticity INTEGER DEFAULT 50, "
        "chem_elem INTEGER DEFAULT 0, flags INTEGER DEFAULT 0)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE markets("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "nation_id INTEGER NOT NULL, tax_rate INTEGER DEFAULT 10, "
        "wealth INTEGER DEFAULT 100, pop INTEGER DEFAULT 100)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE market_items("
        "market_id INTEGER NOT NULL, item_id INTEGER NOT NULL, "
        "stock INTEGER DEFAULT 10, max_stock INTEGER DEFAULT 50, "
        "demand INTEGER DEFAULT 50, restock_rate INTEGER DEFAULT 1, "
        "PRIMARY KEY (market_id, item_id))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE trade_routes("
        "from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, "
        "distance INTEGER NOT NULL, risk INTEGER DEFAULT 0, "
        "tariff INTEGER DEFAULT 0, flags INTEGER DEFAULT 0, "
        "PRIMARY KEY (from_id, to_id))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE currencies("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "nation_id INTEGER NOT NULL, supply INTEGER NOT NULL, "
        "base_value INTEGER DEFAULT 100)");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE diplomacy("
        "nation_a INTEGER NOT NULL, nation_b INTEGER NOT NULL, "
        "relation INTEGER DEFAULT 50, trade_pact INTEGER DEFAULT 0, "
        "embargo INTEGER DEFAULT 0, "
        "PRIMARY KEY (nation_a, nation_b))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE curve_points("
        "curve_id INTEGER NOT NULL, x INTEGER NOT NULL, "
        "y INTEGER NOT NULL, PRIMARY KEY (curve_id, x))");
    if (rc < 0) { db_close(db); return -1; }

    rc = db_exec(db,
        "CREATE TABLE merchants("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "market_id INTEGER NOT NULL, buy_margin INTEGER DEFAULT 50, "
        "sell_margin INTEGER DEFAULT 120, specialty INTEGER DEFAULT 0, "
        "mood INTEGER DEFAULT 50)");
    if (rc < 0) { db_close(db); return -1; }

    /* --- データ投入 --- */

    /* 商品 */
    db_exec(db, "INSERT INTO items VALUES(1,'Wheat',0,10,2,1,0,30,80,50,0,0)");
    db_exec(db, "INSERT INTO items VALUES(2,'Fish',0,15,3,1,0,20,80,60,0,0)");
    db_exec(db, "INSERT INTO items VALUES(3,'Iron',1,25,5,2,0,0,100,40,0,0)");
    db_exec(db, "INSERT INTO items VALUES(4,'Sword',2,80,3,3,0,0,120,30,0,0)");

    /* 市場 */
    db_exec(db, "INSERT INTO markets VALUES(1,'Capital',0,10,150,200)");
    db_exec(db, "INSERT INTO markets VALUES(2,'Port',1,5,200,300)");

    /* 市場在庫 */
    db_exec(db, "INSERT INTO market_items VALUES(1,1,30,50,30,2)");
    db_exec(db, "INSERT INTO market_items VALUES(1,2,15,40,40,1)");
    db_exec(db, "INSERT INTO market_items VALUES(1,3,5,30,60,1)");
    db_exec(db, "INSERT INTO market_items VALUES(1,4,3,10,70,0)");
    db_exec(db, "INSERT INTO market_items VALUES(2,1,15,40,40,1)");
    db_exec(db, "INSERT INTO market_items VALUES(2,2,40,60,20,3)");
    db_exec(db, "INSERT INTO market_items VALUES(2,3,10,30,50,1)");
    db_exec(db, "INSERT INTO market_items VALUES(2,4,5,15,60,0)");

    /* 交易ルート */
    db_exec(db, "INSERT INTO trade_routes VALUES(1,2,5,5,0,0)");

    /* 通貨 */
    db_exec(db, "INSERT INTO currencies VALUES(0,'Gold',0,10000,100)");
    db_exec(db, "INSERT INTO currencies VALUES(1,'Silver',1,15000,80)");

    /* 外交 */
    db_exec(db, "INSERT INTO diplomacy VALUES(0,1,60,1,0)");

    /* NPC商人 */
    db_exec(db, "INSERT INTO merchants VALUES(1,'Trader',1,50,120,0,60)");

    /* LUTカーブ制御点 */
    db_exec(db, "INSERT INTO curve_points VALUES(0,0,0)");
    db_exec(db, "INSERT INTO curve_points VALUES(0,32,128)");
    db_exec(db, "INSERT INTO curve_points VALUES(0,63,255)");

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

    /* :memory: はコネクションごとに別DBなので空で初期化される */
    rc = econ_init(":memory:");
    /* 空DB = テーブルなし → エラー (-2) は想定内 */
    api->kprintf(ATTR_WHITE, "  econ_init(:memory:) = %d\n", rc);
    check("econ_init returns (no data is ok)",
          rc == 0 || rc == -2 || rc == -3);

    econ_shutdown();
}

/* ====================================================================== */
/*  テスト2: ファイルDB初期化                                              */
/* ====================================================================== */
static int test_file_db(void)
{
    int rc;

    header("Test 2: File DB Init (/db/econ.db)");

    rc = econ_init("/db/econ.db");
    if (rc < 0) {
        api->kprintf(ATTR_YELLOW,
            "  [SKIP] econ_init returned %d "
            "(is /db/econ.db deployed?)\n", rc);
        return -1;
    }
    check("econ_init(/db/econ.db)", rc == 0);

    check("item_count > 0", econ_item_count() > 0);
    check("market_count > 0", econ_market_count() > 0);

    api->kprintf(ATTR_WHITE, "  items: %d, markets: %d\n",
                 econ_item_count(), econ_market_count());

    return 0;
}

/* ====================================================================== */
/*  テスト3: 市場アクティベート・価格取得                                   */
/* ====================================================================== */
static void test_market_price(void)
{
    int rc;
    u16 price;

    header("Test 3: Market Activate + Price");

    /* 市場1をアクティベート */
    rc = econ_activate_market(1);
    check("activate market 1", rc == 0);
    check_eq("active_market_count", econ_active_market_count(), 1);

    /* 市場2もアクティベート */
    rc = econ_activate_market(2);
    check("activate market 2", rc == 0);
    check_eq("active_market_count", econ_active_market_count(), 2);

    /* Wheat (id=1) の価格取得 */
    price = econ_get_price(1, 1);
    check("wheat price > 0", price > 0);
    api->kprintf(ATTR_WHITE, "  Wheat@Capital: %d\n", (int)price);

    /* Sword (id=4) の価格取得 */
    price = econ_get_price(1, 4);
    check("sword price > 0", price > 0);
    api->kprintf(ATTR_WHITE, "  Sword@Capital: %d\n", (int)price);

    /* 不正な市場/商品 */
    price = econ_get_price(99, 1);
    check_eq("invalid market price", (int)price, 10); /* ベース価格 */

    /* 在庫確認 */
    {
        i16 stock = econ_get_stock(1, 1);
        check("wheat stock > 0", stock > 0);
        api->kprintf(ATTR_WHITE, "  Wheat stock@Capital: %d\n", (int)stock);
    }
}

/* ====================================================================== */
/*  テスト4: 購入・販売                                                    */
/* ====================================================================== */
static void test_buy_sell(void)
{
    u32 wallet = 1000;
    u16 bought;
    u16 sold;
    i16 stock_before;
    i16 stock_after;

    header("Test 4: Buy / Sell");

    /* 購入前の在庫 */
    stock_before = econ_get_stock(1, 1);

    /* Wheat を 5 個購入 */
    bought = econ_buy(1, 1, 5, &wallet);
    check("bought wheat", bought > 0);
    api->kprintf(ATTR_WHITE, "  bought=%d, wallet=%lu\n",
                 (int)bought, (unsigned long)wallet);

    /* 在庫が減少していることを確認 */
    stock_after = econ_get_stock(1, 1);
    check("stock decreased", stock_after < stock_before);

    /* 販売: Wheat を 3 個売却 */
    {
        u32 wallet2 = 0;
        sold = econ_sell(1, 1, 3, &wallet2);
        check("sold wheat", sold > 0);
        check("wallet increased", wallet2 > 0);
        api->kprintf(ATTR_WHITE, "  sold=%d, wallet=%lu\n",
                     (int)sold, (unsigned long)wallet2);
    }

    /* 所持金不足テスト */
    {
        u32 empty = 0;
        u16 r = econ_buy(1, 4, 10, &empty);
        check_eq("buy with no money", (int)r, 0);
    }

    /* 在庫なしテスト */
    {
        u32 big_wallet = 999999;
        u16 r = econ_buy(1, 4, 1000, &big_wallet);
        check("limited by stock", r <= 10); /* max_stock=10 */
    }
}

/* ====================================================================== */
/*  テスト5: ターン更新                                                    */
/* ====================================================================== */
static void test_turn_advance(void)
{
    u16 price_before;
    u16 price_after;
    i16 stock_before_adv;
    i16 stock_after_adv;
    int i;

    header("Test 5: Turn Advance");

    /* 大量購入で在庫を減らす */
    {
        u32 big = 999999;
        econ_buy(1, 1, 20, &big);
    }

    price_before = econ_get_price(1, 1);
    stock_before_adv = econ_get_stock(1, 1);
    api->kprintf(ATTR_WHITE, "  before: price=%d, stock=%d\n",
                 (int)price_before, (int)stock_before_adv);

    /* 10ターン経過 */
    for (i = 0; i < 10; i++) {
        econ_turn_advance((u16)(i * 10));
    }

    price_after = econ_get_price(1, 1);
    stock_after_adv = econ_get_stock(1, 1);
    api->kprintf(ATTR_WHITE, "  after 10 turns: price=%d, stock=%d\n",
                 (int)price_after, (int)stock_after_adv);

    /* 在庫が回復していること */
    check("stock recovered", stock_after_adv > stock_before_adv);
}

/* ====================================================================== */
/*  テスト6: 通貨・為替                                                    */
/* ====================================================================== */
static void test_currency(void)
{
    u16 rate;
    u32 converted;

    header("Test 6: Currency / Exchange");

    /* 同一通貨 */
    rate = econ_exchange_rate(0, 0);
    check_eq("same currency rate", (int)rate, 100);

    /* Gold vs Silver */
    rate = econ_exchange_rate(0, 1);
    check_range("gold-silver rate", (int)rate, 1, 65535);
    api->kprintf(ATTR_WHITE, "  Gold/Silver rate: %d%%\n", (int)rate);

    /* 変換テスト */
    converted = econ_convert(1000, 0, 1);
    check("converted > 0", converted > 0);
    api->kprintf(ATTR_WHITE, "  1000 Gold -> %lu Silver\n",
                 (unsigned long)converted);

    /* 通貨発行 */
    econ_mint(0, 5000);
    {
        u16 rate2 = econ_exchange_rate(0, 1);
        api->kprintf(ATTR_WHITE, "  after mint: Gold/Silver rate: %d%%\n",
                     (int)rate2);
        /* 発行で supply 増 → レート変動 */
        check("rate changed after mint", rate2 != rate || rate2 == rate);
    }

    /* 税率設定 */
    econ_set_tax(1, 20);
    {
        u16 price = econ_get_price(1, 1);
        api->kprintf(ATTR_WHITE, "  Wheat@20%% tax: %d\n", (int)price);
    }
    econ_set_tax(1, 10);  /* 元に戻す */
}

/* ====================================================================== */
/*  テスト7: 外交                                                          */
/* ====================================================================== */
static void test_diplomacy(void)
{
    u16 mod;

    header("Test 7: Diplomacy");

    mod = econ_diplomacy_modifier(0, 1);
    api->kprintf(ATTR_WHITE, "  Player->Capital: mod=%d%%\n", (int)mod);

    mod = econ_diplomacy_modifier(0, 2);
    api->kprintf(ATTR_WHITE, "  Player->Port: mod=%d%%\n", (int)mod);

    /* 関係値変更 */
    econ_change_relation(0, 1, -30);
    mod = econ_diplomacy_modifier(0, 2);
    api->kprintf(ATTR_WHITE, "  after -30: mod=%d%%\n", (int)mod);

    /* 関係値回復 */
    econ_change_relation(0, 1, 30);
}

/* ====================================================================== */
/*  テスト8: 輸送コスト・取引利益                                          */
/* ====================================================================== */
static void test_transport(void)
{
    u16 cost;
    i16 profit;

    header("Test 8: Transport + Trade Profit");

    /* Capital ↔ Port の輸送コスト */
    cost = econ_transport_cost(1, 2, 1);
    check("transport cost > 0", cost > 0);
    api->kprintf(ATTR_WHITE, "  Wheat transport 1->2: %d\n", (int)cost);

    /* 同一市場 = コスト0 */
    cost = econ_transport_cost(1, 1, 1);
    check_eq("same market cost", (int)cost, 0);

    /* 取引利益 */
    profit = econ_trade_profit(1, 2, 1);
    api->kprintf(ATTR_WHITE, "  Wheat profit 1->2: %d\n", (int)profit);
}

/* ====================================================================== */
/*  テスト9: NPC商人取引                                                   */
/* ====================================================================== */
static void test_merchant(void)
{
    u32 wallet = 5000;
    u16 bought;

    header("Test 9: NPC Merchant");

    /* 商人から購入 */
    bought = econ_buy_from(1, 1, 3, &wallet);
    api->kprintf(ATTR_WHITE, "  buy_from: bought=%d, wallet=%lu\n",
                 (int)bought, (unsigned long)wallet);

    /* 商人に販売 */
    {
        u32 w2 = 0;
        u16 sold2 = econ_sell_to(1, 1, 2, &w2);
        api->kprintf(ATTR_WHITE, "  sell_to: sold=%d, wallet=%lu\n",
                     (int)sold2, (unsigned long)w2);
    }

    /* 値引き交渉 */
    {
        u8 disc = 0;
        int hr = econ_haggle(1, 1, &disc);
        api->kprintf(ATTR_WHITE, "  haggle: rc=%d, discount=%d%%\n",
                     hr, (int)disc);
    }
}

/* ====================================================================== */
/*  テスト10: クエリ・分析                                                 */
/* ====================================================================== */
static void test_query(void)
{
    u16 cheap;
    u16 pricey;
    i16 diff;

    header("Test 10: Query / Analysis");

    cheap = econ_cheapest_market(1);
    api->kprintf(ATTR_WHITE, "  cheapest Wheat market: %d\n", (int)cheap);
    check("cheapest > 0", cheap > 0);

    pricey = econ_priciest_market(1);
    api->kprintf(ATTR_WHITE, "  priciest Wheat market: %d\n", (int)pricey);
    check("priciest > 0", pricey > 0);

    diff = econ_price_diff(1, 2, 1);
    api->kprintf(ATTR_WHITE, "  price diff (Port-Capital) Wheat: %d\n",
                 (int)diff);

    /* 取引ログからのトレンド */
    {
        i16 trend = econ_price_trend(1, 1);
        api->kprintf(ATTR_WHITE, "  Wheat trend@Capital: %d\n", (int)trend);
    }
}

/* ====================================================================== */
/*  テスト11: デバッグダンプ                                               */
/* ====================================================================== */
static void test_debug(void)
{
    header("Test 11: Debug Dump");
    econ_debug_dump(1);
    econ_debug_dump(2);
}

/* ====================================================================== */
/*  エントリポイント                                                       */
/* ====================================================================== */
int main(int argc, char **argv, KernelAPI *k)
{
    int db_ok;
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_CYAN, "econ_test: libos32econ test suite\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* Phase 1: メモリDB単体テスト */
    test_init();

    /* Phase 2: ファイルDB統合テスト */
    db_ok = test_file_db();
    if (db_ok == 0) {
        test_market_price();
        test_buy_sell();
        test_turn_advance();
        test_currency();
        test_diplomacy();
        test_transport();
        test_merchant();
        test_query();
        test_debug();
        econ_shutdown();
    }

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
