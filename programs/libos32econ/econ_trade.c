/* ======================================================================== */
/*  ECON_TRADE.C — 取引ロジック                                              */
/*                                                                          */
/*  プレイヤーの売買、NPC商人との取引、値引き交渉を担当。                     */
/*  取引が行われると在庫・需要・price_modが動的に変動する。                   */
/* ======================================================================== */

#include "econ_internal.h"

/* ====================================================================== */
/*  内部: 需給による price_mod 更新                                        */
/* ====================================================================== */

/*
 * 購入時: 在庫減 → price_mod 上昇 (品薄 = 値上がり)
 * 販売時: 在庫増 → price_mod 下降 (過剰 = 値下がり)
 *
 * 変動幅は弾力性 (elasticity) で制御。
 * elasticity=50 (デフォルト) → 在庫50%で倍率±25%程度
 */
static void update_price_mod(int si, i16 stock_delta)
{
    i32 ratio;
    i32 old_mod;
    i32 new_mod;
    int ii;

    if (g_stocks[si].max_stock == 0) return;

    /* 在庫充足率 (0-100%) */
    ratio = (i32)g_stocks[si].stock * 100 / (i32)g_stocks[si].max_stock;
    if (ratio < 0) ratio = 0;
    if (ratio > 200) ratio = 200;

    /* 弾力性に基づく倍率計算 */
    /* 在庫50% = 100%, 在庫0% = 100+elasticity%, 在庫100% = 100-elasticity/2% */
    ii = econ__find_item(g_stocks[si].item_id);
    if (ii < 0) return;

    old_mod = (i32)g_stocks[si].price_mod;

    /* 倍率の直接計算: 在庫が少ないほど高く、多いほど安く */
    /* 基準: ratio=50 → mod=100, ratio=0 → mod=100+ela, ratio=100 → mod=100-ela/2 */
    new_mod = 100 + ((i32)g_items[ii].elasticity * (50 - ratio)) / 50;

    /* 急激な変動を防ぐ: 1ターンあたり最大±10% */
    if (new_mod > old_mod + 10) new_mod = old_mod + 10;
    if (new_mod < old_mod - 10) new_mod = old_mod - 10;

    /* 範囲制限: 50% ～ 300% */
    if (new_mod < 50) new_mod = 50;
    if (new_mod > 300) new_mod = 300;

    /* コールバック通知 */
    if (g_price_cb && new_mod != old_mod) {
        g_price_cb(g_stocks[si].market_id, g_stocks[si].item_id,
                   (i16)old_mod, (i16)new_mod);
    }

    g_stocks[si].price_mod = (u16)new_mod;
}

/* ====================================================================== */
/*  公開API: 購入                                                          */
/* ====================================================================== */

u16 econ_buy(u16 market_id, u16 item_id, u16 qty, u32 *wallet)
{
    int si;
    u16 unit_price;
    u32 total_cost;
    u16 actual_qty;

    if (!wallet) return 0;

    si = econ__find_stock(market_id, item_id);
    if (si < 0) return 0;

    /* 在庫チェック */
    if (g_stocks[si].stock <= 0) return 0;

    /* 購入可能数の決定 */
    actual_qty = qty;
    if ((i16)actual_qty > g_stocks[si].stock) {
        actual_qty = (u16)g_stocks[si].stock;
    }

    /* ポリシーによる判定 */
    if (g_trade_policy) {
        actual_qty = g_trade_policy(market_id, item_id, actual_qty, *wallet);
        if (actual_qty == 0) return 0;
    }

    /* 価格計算 */
    unit_price = econ_get_price(market_id, item_id);
    total_cost = (u32)unit_price * actual_qty;

    /* 所持金チェック */
    if (total_cost > *wallet) {
        /* 買える分だけ買う */
        actual_qty = (u16)(*wallet / unit_price);
        if (actual_qty == 0) return 0;
        total_cost = (u32)unit_price * actual_qty;
    }

    /* 取引実行 */
    *wallet -= total_cost;
    g_stocks[si].stock -= (i16)actual_qty;

    /* 需要増加 */
    if (g_stocks[si].demand < 65535 - actual_qty) {
        g_stocks[si].demand += actual_qty;
    }

    /* 価格変動 */
    update_price_mod(si, -(i16)actual_qty);

    /* 取引ログ */
    econ__log_trade(market_id, item_id, actual_qty, unit_price, 0xFF, 0);

    /* コールバック */
    if (g_trade_cb) {
        g_trade_cb(market_id, item_id, actual_qty, unit_price, 1);
    }

    return actual_qty;
}

/* ====================================================================== */
/*  公開API: 販売                                                          */
/* ====================================================================== */

u16 econ_sell(u16 market_id, u16 item_id, u16 qty, u32 *wallet)
{
    int si;
    u16 unit_price;
    u32 total_revenue;
    u16 actual_qty;

    if (!wallet) return 0;

    si = econ__find_stock(market_id, item_id);
    if (si < 0) return 0;

    /* 在庫上限チェック */
    actual_qty = qty;
    if ((i32)g_stocks[si].stock + actual_qty > (i32)g_stocks[si].max_stock) {
        actual_qty = (u16)(g_stocks[si].max_stock - g_stocks[si].stock);
    }
    if (actual_qty == 0) return 0;

    /* 売却価格は購入価格の 80% (スプレッド) */
    unit_price = econ_get_price(market_id, item_id);
    unit_price = (u16)((u32)unit_price * 80 / 100);
    if (unit_price < 1) unit_price = 1;

    total_revenue = (u32)unit_price * actual_qty;

    /* 取引実行 */
    *wallet += total_revenue;
    g_stocks[si].stock += (i16)actual_qty;

    /* 需要減少 */
    if (g_stocks[si].demand > actual_qty) {
        g_stocks[si].demand -= actual_qty;
    } else {
        g_stocks[si].demand = 0;
    }

    /* 価格変動 */
    update_price_mod(si, (i16)actual_qty);

    /* 取引ログ */
    econ__log_trade(market_id, item_id, actual_qty, unit_price, 0, 0xFF);

    /* コールバック */
    if (g_trade_cb) {
        g_trade_cb(market_id, item_id, actual_qty, unit_price, 0);
    }

    return actual_qty;
}

/* ====================================================================== */
/*  公開API: NPC商人との取引                                               */
/* ====================================================================== */

u16 econ_buy_from(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet)
{
    int mi;
    u16 unit_price;
    u32 total_cost;
    u16 actual_qty;
    int si;

    if (!wallet) return 0;

    mi = econ__find_merchant(merchant_id);
    if (mi < 0) return 0;

    /* 商人の市場からの価格に sell_margin を適用 */
    unit_price = econ_get_price(g_merchants[mi].market_id, item_id);
    unit_price = (u16)((u32)unit_price * g_merchants[mi].sell_margin / 100);
    if (unit_price < 1) unit_price = 1;

    /* 在庫確認 */
    si = econ__find_stock(g_merchants[mi].market_id, item_id);
    if (si < 0) return 0;

    actual_qty = qty;
    if ((i16)actual_qty > g_stocks[si].stock) {
        actual_qty = (u16)g_stocks[si].stock;
    }
    if (actual_qty == 0) return 0;

    total_cost = (u32)unit_price * actual_qty;
    if (total_cost > *wallet) {
        actual_qty = (u16)(*wallet / unit_price);
        if (actual_qty == 0) return 0;
        total_cost = (u32)unit_price * actual_qty;
    }

    *wallet -= total_cost;
    g_stocks[si].stock -= (i16)actual_qty;
    update_price_mod(si, -(i16)actual_qty);
    econ__log_trade(g_merchants[mi].market_id, item_id,
                    actual_qty, unit_price, 0xFF, (u8)merchant_id);

    if (g_trade_cb) {
        g_trade_cb(g_merchants[mi].market_id, item_id,
                   actual_qty, unit_price, 1);
    }

    return actual_qty;
}

u16 econ_sell_to(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet)
{
    int mi;
    u16 unit_price;
    u32 total_revenue;
    u16 actual_qty;
    int si;

    if (!wallet) return 0;

    mi = econ__find_merchant(merchant_id);
    if (mi < 0) return 0;

    /* 商人の買取価格 = 市場価格 × buy_margin% */
    unit_price = econ_get_price(g_merchants[mi].market_id, item_id);
    unit_price = (u16)((u32)unit_price * g_merchants[mi].buy_margin / 100);
    if (unit_price < 1) unit_price = 1;

    si = econ__find_stock(g_merchants[mi].market_id, item_id);
    if (si < 0) return 0;

    actual_qty = qty;
    if ((i32)g_stocks[si].stock + actual_qty > (i32)g_stocks[si].max_stock) {
        actual_qty = (u16)(g_stocks[si].max_stock - g_stocks[si].stock);
    }
    if (actual_qty == 0) return 0;

    total_revenue = (u32)unit_price * actual_qty;
    *wallet += total_revenue;
    g_stocks[si].stock += (i16)actual_qty;
    update_price_mod(si, (i16)actual_qty);
    econ__log_trade(g_merchants[mi].market_id, item_id,
                    actual_qty, unit_price, (u8)merchant_id, 0xFF);

    if (g_trade_cb) {
        g_trade_cb(g_merchants[mi].market_id, item_id,
                   actual_qty, unit_price, 0);
    }

    return actual_qty;
}

/* ====================================================================== */
/*  公開API: 値引き交渉                                                    */
/* ====================================================================== */

int econ_haggle(u16 merchant_id, u16 item_id, u8 *discount_pct)
{
    int mi;
    int base_chance;
    u32 roll;
    (void)item_id;

    if (!discount_pct) return -1;

    mi = econ__find_merchant(merchant_id);
    if (mi < 0) return -1;

    /* 成功確率: 商人のmood × 0.5 + 25 (%) */
    base_chance = (int)g_merchants[mi].mood / 2 + 25;

    /* 簡易乱数 (ターンカウンタベース — libos32math rng未使用版) */
    roll = (g_turn_count * 2654435761u) >> 24;  /* 0-255 */

    if ((int)(roll * 100 / 255) < base_chance) {
        /* 成功: 5-15% の割引 */
        *discount_pct = (u8)(5 + (roll % 11));

        /* 成功すると mood 低下 (次回は難しくなる) */
        if (g_merchants[mi].mood > 10) {
            g_merchants[mi].mood -= 10;
        }
        return 0;
    }

    /* 失敗: mood さらに低下 */
    *discount_pct = 0;
    if (g_merchants[mi].mood > 5) {
        g_merchants[mi].mood -= 5;
    }
    return -1;
}
