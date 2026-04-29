/* ======================================================================== */
/*  ECON_CORE.C — 経済エンジン コア実装                                      */
/*                                                                          */
/*  初期化・終了・DBからのマスタデータキャッシュ・LUT生成を担当。              */
/*  DB操作は起動/終了時のみ行い、ランタイムではRAMキャッシュのみを参照する。  */
/* ======================================================================== */

#include "econ_internal.h"
#include "libos32math.h"

/* ====================================================================== */
/*  グローバル変数定義 (econ_internal.h で extern 宣言)                     */
/* ====================================================================== */

EconItem       g_items[ECON_MAX_ITEMS];
int            g_item_count;
EconMarket     g_markets[ECON_MAX_MARKETS];
int            g_market_count;
EconStock      g_stocks[ECON_MAX_STOCKS];
int            g_stock_count;
EconRoute      g_routes[ECON_MAX_ROUTES];
int            g_route_count;
EconCurrency   g_currencies[ECON_MAX_CURRENCIES];
int            g_currency_count;
EconDiplomacy  g_diplomacy[ECON_MAX_DIPLOMACY];
int            g_diplomacy_count;
EconMerchant   g_merchants[ECON_MAX_MERCHANTS];
int            g_merchant_count;
EconRecipe     g_recipes[ECON_MAX_RECIPES];
int            g_recipe_count;

u8 g_curve_lut[ECON_MAX_CURVES][ECON_CURVE_SIZE];
u8 g_decay_lut[128];
u8 g_distance_lut[32];

EconTradeEntry g_trade_log[ECON_MAX_TRADE_LOG];
int            g_trade_log_head;
int            g_trade_log_count;

db_handle_t    g_econ_db = -1;

u32            g_turn_count;
u8             g_current_season;

econ_price_policy_fn   g_price_policy;
econ_restock_policy_fn g_restock_policy;
econ_trade_policy_fn   g_trade_policy;
econ_turn_policy_fn    g_turn_policy;

econ_trade_callback    g_trade_cb;
econ_price_callback    g_price_cb;

/* ====================================================================== */
/*  内部ヘルパー: 検索関数                                                 */
/* ====================================================================== */

int econ__find_market(u16 market_id)
{
    return DB_FIND_BY_FIELD(g_markets, g_market_count, id, market_id);
}

int econ__find_item(u16 item_id)
{
    return DB_FIND_BY_FIELD(g_items, g_item_count, id, item_id);
}

int econ__find_stock(u16 market_id, u16 item_id)
{
    int i;
    for (i = 0; i < g_stock_count; i++) {
        if (g_stocks[i].market_id == (u8)market_id &&
            g_stocks[i].item_id == item_id)
            return i;
    }
    return -1;
}

int econ__find_route(u8 from_id, u8 to_id)
{
    int i;
    for (i = 0; i < g_route_count; i++) {
        if (g_routes[i].from_id == from_id &&
            g_routes[i].to_id == to_id)
            return i;
    }
    /* 逆方向も探す */
    for (i = 0; i < g_route_count; i++) {
        if (g_routes[i].from_id == to_id &&
            g_routes[i].to_id == from_id)
            return i;
    }
    return -1;
}

int econ__find_currency(u8 currency_id)
{
    int i;
    for (i = 0; i < g_currency_count; i++) {
        if (g_currencies[i].id == currency_id) return i;
    }
    return -1;
}

int econ__find_diplomacy(u8 nation_a, u8 nation_b)
{
    int i;
    for (i = 0; i < g_diplomacy_count; i++) {
        if ((g_diplomacy[i].nation_a == nation_a &&
             g_diplomacy[i].nation_b == nation_b) ||
            (g_diplomacy[i].nation_a == nation_b &&
             g_diplomacy[i].nation_b == nation_a))
            return i;
    }
    return -1;
}

int econ__find_merchant(u16 merchant_id)
{
    return DB_FIND_BY_FIELD(g_merchants, g_merchant_count, id, merchant_id);
}

int econ__find_recipe(u16 recipe_id)
{
    return DB_FIND_BY_FIELD(g_recipes, g_recipe_count, id, recipe_id);
}

/* ====================================================================== */
/*  内部ヘルパー: 取引ログ                                                 */
/* ====================================================================== */

void econ__log_trade(u16 market_id, u16 item_id,
                     u16 qty, u16 unit_price, u8 buyer, u8 seller)
{
    EconTradeEntry *e;
    e = &g_trade_log[g_trade_log_head];
    e->tick = g_turn_count;
    e->market_id = market_id;
    e->item_id = item_id;
    e->qty = qty;
    e->unit_price = unit_price;
    e->buyer = buyer;
    e->seller = seller;

    g_trade_log_head = (g_trade_log_head + 1) % ECON_MAX_TRADE_LOG;
    if (g_trade_log_count < ECON_MAX_TRADE_LOG) {
        g_trade_log_count++;
    }
}

/* ====================================================================== */
/*  内部ヘルパー: LUTカーブ参照                                            */
/* ====================================================================== */

u16 econ__curve_lookup(u8 curve_type, u8 index)
{
    u8 raw;
    if (curve_type >= ECON_MAX_CURVES) return 100;
    if (index >= ECON_CURVE_SIZE) index = ECON_CURVE_SIZE - 1;
    raw = g_curve_lut[curve_type][index];
    /* raw は 0-255 → 50-200% にマッピング */
    /* 128 = 100% (ベースライン) */
    return (u16)(50 + ((u16)raw * 150) / 255);
}

/* ====================================================================== */
/*  DB読み込み: 商品マスタ                                                 */
/* ====================================================================== */

static int load_items(void)
{
    return DB_LOAD_TABLE(g_econ_db,
        "SELECT id, base_price, category, rarity, weight, "
        "curve_type, season_amp, diplo_weight, elasticity, flags "
        "FROM items ORDER BY id",
        g_items, ECON_MAX_ITEMS, g_item_count,
        {
            row->id          = (u16)db_column_int(0);
            row->base_price  = (u16)db_column_int(1);
            row->category    = (u8)db_column_int(2);
            row->rarity      = (u8)db_column_int(3);
            row->weight      = (u8)db_column_int(4);
            row->curve_type  = (u8)db_column_int(5);
            row->season_amp  = (u8)db_column_int(6);
            row->diplo_weight = (u8)db_column_int(7);
            row->elasticity  = (u8)db_column_int(8);
            row->flags       = (u8)db_column_int(9);
        });
}

/* ====================================================================== */
/*  DB読み込み: 市場                                                       */
/* ====================================================================== */

static int load_markets(void)
{
    return DB_LOAD_TABLE(g_econ_db,
        "SELECT id, nation_id, tax_rate, wealth, pop "
        "FROM markets ORDER BY id",
        g_markets, ECON_MAX_MARKETS, g_market_count,
        {
            row->id        = (u16)db_column_int(0);
            row->nation_id = (u8)db_column_int(1);
            row->tax_rate  = (u8)db_column_int(2);
            row->wealth    = (u16)db_column_int(3);
            row->pop       = (u16)db_column_int(4);
            row->active    = 0;
            row->_pad      = 0;
        });
}

/* ====================================================================== */
/*  DB読み込み: 市場在庫                                                   */
/* ====================================================================== */

static int load_stocks(void)
{
    return DB_LOAD_TABLE(g_econ_db,
        "SELECT market_id, item_id, stock, max_stock, "
        "demand, restock_rate "
        "FROM market_items ORDER BY market_id, item_id",
        g_stocks, ECON_MAX_STOCKS, g_stock_count,
        {
            row->market_id   = (u8)db_column_int(0);
            row->item_id     = (u16)db_column_int(1);
            row->stock       = (i16)db_column_int(2);
            row->max_stock   = (u16)db_column_int(3);
            row->demand      = (u16)db_column_int(4);
            row->restock_rate = (u8)db_column_int(5);
            row->price_mod   = 100;
        });
}

/* ====================================================================== */
/*  DB読み込み: 交易ルート                                                 */
/* ====================================================================== */

static int load_routes(void)
{
    return DB_LOAD_TABLE_OPT(g_econ_db,
        "SELECT from_id, to_id, distance, risk, tariff, flags "
        "FROM trade_routes",
        g_routes, ECON_MAX_ROUTES, g_route_count,
        {
            row->from_id  = (u8)db_column_int(0);
            row->to_id    = (u8)db_column_int(1);
            row->distance = (u8)db_column_int(2);
            row->risk     = (u8)db_column_int(3);
            row->tariff   = (u8)db_column_int(4);
            row->flags    = (u8)db_column_int(5);
        });
}

/* ====================================================================== */
/*  DB読み込み: 通貨                                                       */
/* ====================================================================== */

static int load_currencies(void)
{
    return DB_LOAD_TABLE_OPT(g_econ_db,
        "SELECT id, nation_id, supply, base_value "
        "FROM currencies ORDER BY id",
        g_currencies, ECON_MAX_CURRENCIES, g_currency_count,
        {
            row->id         = (u8)db_column_int(0);
            row->nation_id  = (u8)db_column_int(1);
            row->supply     = (u16)db_column_int(2);
            row->base_value = (u16)db_column_int(3);
            row->_pad       = 0;
        });
}

/* ====================================================================== */
/*  DB読み込み: 外交関係                                                   */
/* ====================================================================== */

static int load_diplomacy(void)
{
    int rc;
    int count = 0;

    rc = db_query(g_econ_db,
        "SELECT nation_a, nation_b, relation, "
        "trade_pact, embargo "
        "FROM diplomacy");
    if (rc < 0) return -1;

    while (rc == DB_STATUS_ROW && count < ECON_MAX_DIPLOMACY) {
        u8 flags = 0;
        g_diplomacy[count].nation_a = (u8)db_column_int(0);
        g_diplomacy[count].nation_b = (u8)db_column_int(1);
        g_diplomacy[count].relation = (i8)db_column_int(2);
        if (db_column_int(3)) flags |= 0x01;  /* 通商条約 */
        if (db_column_int(4)) flags |= 0x02;  /* 禁輸 */
        g_diplomacy[count].flags = flags;
        count++;
        rc = db_step(g_econ_db);
    }

    g_diplomacy_count = count;
    return count;
}

/* ====================================================================== */
/*  DB読み込み: NPC商人                                                    */
/* ====================================================================== */

static int load_merchants(void)
{
    return DB_LOAD_TABLE_OPT(g_econ_db,
        "SELECT id, market_id, buy_margin, sell_margin, "
        "specialty, mood "
        "FROM merchants ORDER BY id",
        g_merchants, ECON_MAX_MERCHANTS, g_merchant_count,
        {
            row->id          = (u16)db_column_int(0);
            row->market_id   = (u16)db_column_int(1);
            row->buy_margin  = (u8)db_column_int(2);
            row->sell_margin = (u8)db_column_int(3);
            row->specialty   = (u8)db_column_int(4);
            row->mood        = (u8)db_column_int(5);
        });
}

/* ====================================================================== */
/*  LUTカーブ生成: SQL制御点からlerp_int()で補間                           */
/* ====================================================================== */

static void generate_curve_lut(void)
{
    int curve_id;
    int rc;

    memset(g_curve_lut, 128, sizeof(g_curve_lut));

    for (curve_id = 0; curve_id < ECON_MAX_CURVES; curve_id++) {
        /* 制御点を読み込み */
        int pts_x[16];
        int pts_y[16];
        int pt_count = 0;
        char sql[128];
        char *p;
        int cid;
        int i;

        /* SQLクエリ生成: "SELECT x, y FROM curve_points WHERE curve_id=N ORDER BY x" */
        p = sql;
        {
            const char *prefix = "SELECT x, y FROM curve_points WHERE curve_id=";
            while (*prefix) { *p++ = *prefix++; }
        }
        /* curve_id を文字列に変換 */
        cid = curve_id;
        {
            char digits[4];
            int di = 0;
            if (cid == 0) {
                digits[di++] = '0';
            } else {
                while (cid > 0) {
                    digits[di++] = '0' + (char)(cid % 10);
                    cid /= 10;
                }
            }
            while (di > 0) { *p++ = digits[--di]; }
        }
        {
            const char *suffix = " ORDER BY x";
            while (*suffix) { *p++ = *suffix++; }
        }
        *p = '\0';

        rc = db_query(g_econ_db, sql);
        if (rc < 0) continue;

        while (rc == DB_STATUS_ROW && pt_count < 16) {
            pts_x[pt_count] = db_column_int(0);
            pts_y[pt_count] = db_column_int(1);
            pt_count++;
            rc = db_step(g_econ_db);
        }

        if (pt_count < 2) continue;

        /* lerp_int で 64 点テーブルを補間生成 */
        for (i = 0; i < ECON_CURVE_SIZE; i++) {
            int x_pos;
            int seg;
            int val;

            /* i を制御点のX範囲にマッピング */
            x_pos = lerp_int(pts_x[0], pts_x[pt_count - 1],
                             i, ECON_CURVE_SIZE - 1);

            /* 対応するセグメントを探す */
            for (seg = 0; seg < pt_count - 1; seg++) {
                if (x_pos <= pts_x[seg + 1]) break;
            }
            if (seg >= pt_count - 1) seg = pt_count - 2;

            /* セグメント内で補間 */
            if (pts_x[seg + 1] == pts_x[seg]) {
                val = pts_y[seg];
            } else {
                val = lerp_int(pts_y[seg], pts_y[seg + 1],
                               x_pos - pts_x[seg],
                               pts_x[seg + 1] - pts_x[seg]);
            }

            /* 0-255 にクランプ */
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            g_curve_lut[curve_id][i] = (u8)val;
        }
    }
}

/* ====================================================================== */
/*  減衰LUT・距離LUT生成                                                   */
/* ====================================================================== */

static void generate_decay_lut(void)
{
    int i;
    /* 対数的減衰: 128段階で 255→0 に減衰 */
    for (i = 0; i < 128; i++) {
        /* 簡易対数減衰: y = 255 * (1 - i/128)^2 */
        int r = 128 - i;
        g_decay_lut[i] = (u8)((r * r * 255) / (128 * 128));
    }
}

static void generate_distance_lut(void)
{
    int i;
    /* 距離に対する輸送コスト倍率 (対数的) */
    /* distance 0=100%, 1=110%, ... 31=300% くらい */
    for (i = 0; i < 32; i++) {
        /* 線形近似: 100 + i * 6.5 ≈ 100 + (i * 13) / 2 */
        int val = 100 + (i * 13) / 2;
        if (val > 255) val = 255;
        g_distance_lut[i] = (u8)val;
    }
}

/* ====================================================================== */
/*  公開API: システム管理                                                   */
/* ====================================================================== */

int econ_init(const char *db_path)
{
    int rc;

    /* 既に初期化済みの場合はシャットダウンしてから再初期化 */
    if (g_econ_db >= 0) {
        econ_shutdown();
    }

    /* 内部状態クリア */
    memset(g_items, 0, sizeof(g_items));
    memset(g_markets, 0, sizeof(g_markets));
    memset(g_stocks, 0, sizeof(g_stocks));
    memset(g_routes, 0, sizeof(g_routes));
    memset(g_currencies, 0, sizeof(g_currencies));
    memset(g_diplomacy, 0, sizeof(g_diplomacy));
    memset(g_merchants, 0, sizeof(g_merchants));
    memset(g_recipes, 0, sizeof(g_recipes));
    memset(g_trade_log, 0, sizeof(g_trade_log));
    g_item_count = 0;
    g_market_count = 0;
    g_stock_count = 0;
    g_route_count = 0;
    g_currency_count = 0;
    g_diplomacy_count = 0;
    g_merchant_count = 0;
    g_recipe_count = 0;
    g_trade_log_head = 0;
    g_trade_log_count = 0;
    g_turn_count = 0;
    g_current_season = ECON_SEASON_SPRING;
    g_price_policy = (econ_price_policy_fn)0;
    g_restock_policy = (econ_restock_policy_fn)0;
    g_trade_policy = (econ_trade_policy_fn)0;
    g_turn_policy = (econ_turn_policy_fn)0;
    g_trade_cb = (econ_trade_callback)0;
    g_price_cb = (econ_price_callback)0;

    /* DB接続 */
    g_econ_db = db_open(db_path);
    if (g_econ_db < 0) return -1;

    /* マスタデータ読み込み */
    rc = load_items();
    if (rc < 0) { db_close(g_econ_db); g_econ_db = -1; return -2; }

    rc = load_markets();
    if (rc < 0) { db_close(g_econ_db); g_econ_db = -1; return -3; }

    rc = load_stocks();
    if (rc < 0) { db_close(g_econ_db); g_econ_db = -1; return -4; }

    /* 以下はオプショナル (テーブルがなくてもエラーにしない) */
    load_routes();
    load_currencies();
    load_diplomacy();
    load_merchants();

    /* LUT生成 */
    generate_curve_lut();
    generate_decay_lut();
    generate_distance_lut();

    /* 不動産サブシステム初期化 (オプショナル) */
    econ_estate_init();

    return 0;
}

void econ_shutdown(void)
{
    if (g_econ_db >= 0) {
        db_close(g_econ_db);
        g_econ_db = -1;
    }
    g_item_count = 0;
    g_market_count = 0;
    g_stock_count = 0;
    g_route_count = 0;
    g_currency_count = 0;
    g_diplomacy_count = 0;
    g_merchant_count = 0;
    g_recipe_count = 0;
    g_estate_count = 0;
    g_estate_level_count = 0;
    g_estate_type_count = 0;
    g_price_policy = (econ_price_policy_fn)0;
    g_restock_policy = (econ_restock_policy_fn)0;
    g_trade_policy = (econ_trade_policy_fn)0;
    g_turn_policy = (econ_turn_policy_fn)0;
    g_trade_cb = (econ_trade_callback)0;
    g_price_cb = (econ_price_callback)0;
}

void econ_reset(void)
{
    /* 在庫を初期値に戻す (マスタデータは保持) */
    int i;
    for (i = 0; i < g_stock_count; i++) {
        g_stocks[i].price_mod = 100;
    }
    memset(g_trade_log, 0, sizeof(g_trade_log));
    g_trade_log_head = 0;
    g_trade_log_count = 0;
    g_turn_count = 0;
    g_current_season = ECON_SEASON_SPRING;

    /* DBから在庫を再読み込み */
    if (g_econ_db >= 0) {
        g_stock_count = 0;
        load_stocks();
    }

    /* 不動産の上納金をリセット */
    {
        int i;
        for (i = 0; i < g_estate_count; i++) {
            g_estates[i].tax = 0;
        }
    }
}

/* ====================================================================== */
/*  公開API: ポリシー設定                                                   */
/* ====================================================================== */

void econ_set_price_policy(econ_price_policy_fn fn)
{
    g_price_policy = fn;
}

void econ_set_restock_policy(econ_restock_policy_fn fn)
{
    g_restock_policy = fn;
}

void econ_set_trade_policy(econ_trade_policy_fn fn)
{
    g_trade_policy = fn;
}

void econ_set_turn_policy(econ_turn_policy_fn fn)
{
    g_turn_policy = fn;
}

/* ====================================================================== */
/*  公開API: コールバック設定                                               */
/* ====================================================================== */

void econ_set_trade_callback(econ_trade_callback cb)
{
    g_trade_cb = cb;
}

void econ_set_price_callback(econ_price_callback cb)
{
    g_price_cb = cb;
}

/* ====================================================================== */
/*  公開API: デバッグ                                                      */
/* ====================================================================== */

int econ_item_count(void)
{
    return g_item_count;
}

int econ_market_count(void)
{
    return g_market_count;
}

int econ_active_market_count(void)
{
    int i;
    int count = 0;
    for (i = 0; i < g_market_count; i++) {
        if (g_markets[i].active) count++;
    }
    return count;
}

void econ_debug_dump(u16 market_id)
{
    int mi;
    int i;

    mi = econ__find_market(market_id);
    if (mi < 0) {
        api->kprintf(ATTR_RED, "econ: market %d not found\n",
                     (int)market_id);
        return;
    }

    api->kprintf(ATTR_CYAN, "=== Market %d (nation=%d, tax=%d%%) ===\n",
                 (int)market_id, g_markets[mi].nation_id,
                 g_markets[mi].tax_rate);
    api->kprintf(ATTR_WHITE, "  wealth=%d, pop=%d, active=%d\n",
                 g_markets[mi].wealth, g_markets[mi].pop,
                 g_markets[mi].active);

    for (i = 0; i < g_stock_count; i++) {
        if (g_stocks[i].market_id == (u8)market_id) {
            int ii = econ__find_item(g_stocks[i].item_id);
            u16 price = econ_get_price(market_id, g_stocks[i].item_id);
            api->kprintf(ATTR_WHITE,
                "  item=%d stock=%d/%d demand=%d price=%d (mod=%d%%)\n",
                g_stocks[i].item_id, g_stocks[i].stock,
                g_stocks[i].max_stock, g_stocks[i].demand,
                price, g_stocks[i].price_mod);
            (void)ii;
        }
    }
}
