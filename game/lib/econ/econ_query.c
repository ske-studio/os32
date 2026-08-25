/* ======================================================================== */
/*  ECON_QUERY.C — クエリ・分析関数                                          */
/*                                                                          */
/*  最安/最高市場の検索、価格トレンド分析、市場間価格差の計算を担当。          */
/* ======================================================================== */

#include "econ_internal.h"

/* ====================================================================== */
/*  公開API: 最安市場の検索                                                */
/* ====================================================================== */

u16 econ_cheapest_market(u16 item_id)
{
    int i;
    u16 best_price = 65535;
    u16 best_market = 0;

    for (i = 0; i < g_market_count; i++) {
        u16 price;
        if (!g_markets[i].active) continue;

        price = econ_get_price(g_markets[i].id, item_id);
        if (price > 0 && price < best_price) {
            best_price = price;
            best_market = g_markets[i].id;
        }
    }

    return best_market;
}

/* ====================================================================== */
/*  公開API: 最高市場の検索                                                */
/* ====================================================================== */

u16 econ_priciest_market(u16 item_id)
{
    int i;
    u16 best_price = 0;
    u16 best_market = 0;

    for (i = 0; i < g_market_count; i++) {
        u16 price;
        if (!g_markets[i].active) continue;

        price = econ_get_price(g_markets[i].id, item_id);
        if (price > best_price) {
            best_price = price;
            best_market = g_markets[i].id;
        }
    }

    return best_market;
}

/* ====================================================================== */
/*  公開API: 価格トレンド                                                  */
/* ====================================================================== */

/*
 * 取引ログからアイテムの価格変動トレンドを計算。
 * 戻り値: 正=値上がり傾向, 負=値下がり傾向, 0=横ばい
 */
i16 econ_price_trend(u16 market_id, u16 item_id)
{
    int i;
    int idx;
    int count = 0;
    i32 first_price = 0;
    i32 last_price = 0;
    u32 first_tick = 0xFFFFFFFF;
    u32 last_tick = 0;

    /* リングバッファを逆順に走査して最新/最古の価格を取得 */
    for (i = 0; i < g_trade_log_count; i++) {
        idx = (g_trade_log_head - 1 - i + ECON_MAX_TRADE_LOG)
              % ECON_MAX_TRADE_LOG;

        if (g_trade_log[idx].market_id == market_id &&
            g_trade_log[idx].item_id == item_id) {

            if (g_trade_log[idx].tick < first_tick) {
                first_tick = g_trade_log[idx].tick;
                first_price = (i32)g_trade_log[idx].unit_price;
            }
            if (g_trade_log[idx].tick > last_tick) {
                last_tick = g_trade_log[idx].tick;
                last_price = (i32)g_trade_log[idx].unit_price;
            }
            count++;
        }
    }

    if (count < 2) return 0;

    /* トレンド = 最新価格 - 最古価格 */
    {
        i32 diff = last_price - first_price;
        if (diff > 32767) diff = 32767;
        if (diff < -32768) diff = -32768;
        return (i16)diff;
    }
}

/* ====================================================================== */
/*  公開API: 市場間価格差                                                  */
/* ====================================================================== */

i16 econ_price_diff(u16 market_a, u16 market_b, u16 item_id)
{
    i32 diff;
    u16 pa = econ_get_price(market_a, item_id);
    u16 pb = econ_get_price(market_b, item_id);

    diff = (i32)pb - (i32)pa;
    if (diff > 32767) diff = 32767;
    if (diff < -32768) diff = -32768;

    return (i16)diff;
}
