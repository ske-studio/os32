/* ======================================================================== */
/*  ECON_SIM.C — シミュレーション (ターン更新)                                */
/*                                                                          */
/*  econ_turn_advance: ターン経過時の全市場更新                               */
/*    1. 在庫回復 (restock)                                                  */
/*    2. 需給による価格変動 (price_mod 収束)                                  */
/*    3. リフレーション (N ターンごと)                                        */
/*    4. NPC自律取引 (Phase 6 拡張用スタブ)                                  */
/* ======================================================================== */

#include "econ_internal.h"
#include "libos32math.h"

/* ====================================================================== */
/*  内部: 在庫回復処理                                                     */
/* ====================================================================== */

static void restock_all(void)
{
    int i;
    for (i = 0; i < g_stock_count; i++) {
        i16 delta;

        /* カスタムポリシーがあればそちらを使用 */
        if (g_restock_policy) {
            delta = g_restock_policy(&g_stocks[i], 1);
        } else {
            /* デフォルト: restock_rate 分だけ回復 */
            delta = (i16)g_stocks[i].restock_rate;
        }

        g_stocks[i].stock += delta;

        /* 上限クランプ */
        if (g_stocks[i].stock > (i16)g_stocks[i].max_stock) {
            g_stocks[i].stock = (i16)g_stocks[i].max_stock;
        }
        /* 下限クランプ (0未満にはならない) */
        if (g_stocks[i].stock < 0) {
            g_stocks[i].stock = 0;
        }
    }
}

/* ====================================================================== */
/*  内部: 需給均衡への価格収束                                             */
/* ====================================================================== */

static void converge_prices(void)
{
    int i;
    for (i = 0; i < g_stock_count; i++) {
        i32 ratio;
        i32 target_mod;
        i32 cur_mod;
        int ii;

        if (g_stocks[i].max_stock == 0) continue;

        /* 在庫充足率 */
        ratio = (i32)g_stocks[i].stock * 100 / (i32)g_stocks[i].max_stock;

        /* 弾力性から目標倍率を算出 */
        ii = econ__find_item(g_stocks[i].item_id);
        if (ii < 0) continue;

        target_mod = 100 + ((i32)g_items[ii].elasticity * (50 - ratio)) / 50;
        if (target_mod < 50) target_mod = 50;
        if (target_mod > 300) target_mod = 300;

        /* 現在の price_mod を目標に向けて 1 ステップ収束 */
        cur_mod = (i32)g_stocks[i].price_mod;
        if (cur_mod < target_mod) {
            cur_mod += 1;
        } else if (cur_mod > target_mod) {
            cur_mod -= 1;
        }

        g_stocks[i].price_mod = (u16)cur_mod;
    }
}

/* ====================================================================== */
/*  内部: 需要の自然減衰                                                   */
/* ====================================================================== */

static void decay_demand(void)
{
    int i;
    for (i = 0; i < g_stock_count; i++) {
        /* 毎ターン 1 ずつ自然減衰 */
        if (g_stocks[i].demand > 0) {
            g_stocks[i].demand--;
        }
    }
}

/* ====================================================================== */
/*  内部: リフレーション (通貨正規化)                                      */
/* ====================================================================== */

static void reflation(void)
{
    int i;
    u16 max_supply = 0;
    u32 scale;

    /* 最大流通量を見つける */
    for (i = 0; i < g_currency_count; i++) {
        if (g_currencies[i].supply > max_supply) {
            max_supply = g_currencies[i].supply;
        }
    }

    /* オーバーフロー圏内 (32768以上) でなければスキップ */
    if (max_supply < 32768) return;

    /* 全通貨を比率保存で正規化 (最大値を 16384 に) */
    scale = ((u32)16384 * 256) / max_supply;  /* 8.8 固定小数 */

    for (i = 0; i < g_currency_count; i++) {
        g_currencies[i].supply =
            (u16)((u32)g_currencies[i].supply * scale / 256);
        if (g_currencies[i].supply < 1) g_currencies[i].supply = 1;
    }
}

/* ====================================================================== */
/*  公開API: ターン更新                                                    */
/* ====================================================================== */

void econ_turn_advance(u16 season_day)
{
    int i;

    g_turn_count++;
    g_current_season = (u8)(season_day / 90);  /* 0-89=春, 90-179=夏, ... */
    if (g_current_season > 3) g_current_season = 3;

    /* 1. 在庫回復 */
    restock_all();

    /* 2. 需給均衡への価格収束 */
    converge_prices();

    /* 3. 需要の自然減衰 */
    decay_demand();

    /* 4. カスタムターンポリシー */
    if (g_turn_policy) {
        for (i = 0; i < g_market_count; i++) {
            if (g_markets[i].active) {
                g_turn_policy(g_markets[i].id, g_current_season);
            }
        }
    }

    /* 5. リフレーション (N ターンごと) */
    if (g_turn_count % ECON_REFL_INTERVAL == 0 && g_currency_count > 0) {
        reflation();
    }
}

/* ====================================================================== */
/*  公開API: Chem連携イベント                                              */
/* ====================================================================== */

void econ_on_chem_event(u16 market_id, u32 chem_elem,
                         u8 chem_action, i16 impact)
{
    int i;
    (void)chem_elem;
    (void)chem_action;

    /*
     * 化学イベント (火事、洪水等) による市場在庫への影響。
     * impact > 0: 在庫増加 (例: 豊作)
     * impact < 0: 在庫減少 (例: 火災による損失)
     */
    for (i = 0; i < g_stock_count; i++) {
        if (g_stocks[i].market_id == (u8)market_id) {
            g_stocks[i].stock += impact;
            if (g_stocks[i].stock < 0) g_stocks[i].stock = 0;
            if (g_stocks[i].stock > (i16)g_stocks[i].max_stock) {
                g_stocks[i].stock = (i16)g_stocks[i].max_stock;
            }
        }
    }
}
