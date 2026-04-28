/* ======================================================================== */
/*  ECON_MARKET.C — 市場操作・価格計算                                       */
/*                                                                          */
/*  市場のアクティベート、価格計算（需給カーブ + 季節 + 外交 + 税）、          */
/*  輸送コスト、取引利益の見積もりを担当。                                    */
/* ======================================================================== */

#include "econ_internal.h"
#include "libos32math.h"

/* ====================================================================== */
/*  デフォルト価格計算                                                     */
/* ====================================================================== */

/*
 * 最終価格 = base_price
 *          × (price_mod / 100)        需給カーブ
 *          × (diplomacy_mod / 100)    外交補正
 *          + season_delta             季節 (isin)
 *          + tax                      税
 */
u16 econ__default_price(const EconItem *item, i16 stock_pct,
                         u16 season_day, i8 diplomacy)
{
    i32 price;
    i32 season_delta;
    i32 diplo_mod;

    /* ベース価格 × 需給倍率 */
    price = (i32)item->base_price * (i32)stock_pct / 100;

    /* 季節補正: isin() を使って正弦波変動 */
    if (item->season_amp > 0) {
        /* season_day: 0-359 (1年=360日想定) */
        /* isin のインデックス: 0-511 → season_day を 512 分割に変換 */
        int sin_idx = (int)season_day * 512 / 360;
        i32 sin_val = isin(sin_idx);  /* -32767 ~ +32767 */
        /* 振幅スケーリング: season_amp (0-255) */
        season_delta = (sin_val * (i32)item->season_amp) / 32767;
        price += season_delta;
    }

    /* 外交補正: relation (-100~+100) → 倍率 80%~120% */
    diplo_mod = 100 + ((i32)diplomacy * (i32)item->diplo_weight / 500);
    price = price * diplo_mod / 100;

    /* 下限保護 */
    if (price < 1) price = 1;
    if (price > 65535) price = 65535;

    return (u16)price;
}

/* ====================================================================== */
/*  公開API: 市場操作                                                      */
/* ====================================================================== */

int econ_activate_market(u16 market_id)
{
    int mi = econ__find_market(market_id);
    if (mi < 0) return -1;
    g_markets[mi].active = 1;
    return 0;
}

u16 econ_get_price(u16 market_id, u16 item_id)
{
    int ii;
    int si;
    i16 stock_pct;
    i8 diplo;
    u16 price;
    u16 tax;
    int mi;

    ii = econ__find_item(item_id);
    if (ii < 0) return 0;

    si = econ__find_stock(market_id, item_id);
    if (si < 0) {
        /* 在庫エントリがない場合はベース価格を返す */
        return g_items[ii].base_price;
    }

    /* 需給倍率 */
    stock_pct = (i16)g_stocks[si].price_mod;

    /* 外交補正値の取得 */
    mi = econ__find_market(market_id);
    diplo = 0;
    if (mi >= 0) {
        /* TODO: プレイヤー国家IDを動的に取得する仕組みが必要 */
        /* 暫定的に nation_id=0 をプレイヤー国家と仮定 */
        int di = econ__find_diplomacy(0, g_markets[mi].nation_id);
        if (di >= 0) {
            diplo = g_diplomacy[di].relation;
        }
    }

    /* 価格計算 (ポリシー関数があればそちらを使用) */
    if (g_price_policy) {
        price = g_price_policy(&g_items[ii], stock_pct,
                               (u16)(g_turn_count % 360), diplo);
    } else {
        price = econ__default_price(&g_items[ii], stock_pct,
                                    (u16)(g_turn_count % 360), diplo);
    }

    /* 税金の加算 */
    if (mi >= 0 && g_markets[mi].tax_rate > 0) {
        tax = (u16)((u32)price * g_markets[mi].tax_rate / 100);
        price += tax;
        if (price > 65535) price = 65535;
    }

    return price;
}

i16 econ_get_stock(u16 market_id, u16 item_id)
{
    int si = econ__find_stock(market_id, item_id);
    if (si < 0) return 0;
    return g_stocks[si].stock;
}

u8 econ_get_demand(u16 market_id, u16 item_id)
{
    int si = econ__find_stock(market_id, item_id);
    if (si < 0) return 0;
    return (u8)(g_stocks[si].demand > 255 ? 255 : g_stocks[si].demand);
}

u16 econ_transport_cost(u16 from_market, u16 to_market, u16 item_id)
{
    int mi_from;
    int mi_to;
    int ri;
    int ii;
    u16 base_cost;
    u16 dist_mod;

    if (from_market == to_market) return 0;

    mi_from = econ__find_market(from_market);
    mi_to = econ__find_market(to_market);
    if (mi_from < 0 || mi_to < 0) return 0;

    /* 交易ルートの検索 */
    ri = econ__find_route((u8)g_markets[mi_from].id,
                          (u8)g_markets[mi_to].id);
    if (ri < 0) return 65535; /* ルートなし = 輸送不可 */

    /* 封鎖チェック */
    if (g_routes[ri].flags & 0x01) return 65535;

    /* 商品の重量 */
    ii = econ__find_item(item_id);
    if (ii < 0) return 0;

    /* 基本輸送コスト = 距離 × 重量 */
    base_cost = (u16)g_routes[ri].distance * g_items[ii].weight;

    /* 距離LUTによる倍率 */
    dist_mod = g_distance_lut[g_routes[ri].distance < 32 ?
                              g_routes[ri].distance : 31];
    base_cost = (u16)((u32)base_cost * dist_mod / 100);

    /* 関税の加算 */
    if (g_routes[ri].tariff > 0) {
        u16 tariff = (u16)((u32)base_cost * g_routes[ri].tariff / 100);
        base_cost += tariff;
    }

    return base_cost;
}

i16 econ_trade_profit(u16 from_market, u16 to_market, u16 item_id)
{
    u16 buy_price;
    u16 sell_price;
    u16 transport;
    i32 profit;

    buy_price = econ_get_price(from_market, item_id);
    sell_price = econ_get_price(to_market, item_id);
    transport = econ_transport_cost(from_market, to_market, item_id);

    if (transport >= 65535) return -32768; /* ルートなし */

    profit = (i32)sell_price - (i32)buy_price - (i32)transport;
    if (profit > 32767) profit = 32767;
    if (profit < -32768) profit = -32768;

    return (i16)profit;
}
