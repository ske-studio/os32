/* ======================================================================== */
/*  ECON_INTERNAL.H — libos32econ 内部共有ヘッダ                             */
/*                                                                          */
/*  ライブラリ内部の複数ソースファイル間で共有するグローバル変数・関数の       */
/*  extern 宣言を集約する。外部からはインクルードしない。                     */
/* ======================================================================== */

#ifndef ECON_INTERNAL_H
#define ECON_INTERNAL_H

#include "libos32econ.h"
#include "libos32db.h"

/* ====================================================================== */
/*  外部参照: KernelAPI                                                    */
/* ====================================================================== */

extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  外部参照: memset (libcから提供)                                        */
/* ====================================================================== */

extern void *memset(void *, int, unsigned int);

/* ====================================================================== */
/*  内部グローバル変数 (econ_core.c で定義)                                */
/* ====================================================================== */

/* データキャッシュ */
extern EconItem       g_items[ECON_MAX_ITEMS];
extern int            g_item_count;
extern EconMarket     g_markets[ECON_MAX_MARKETS];
extern int            g_market_count;
extern EconStock      g_stocks[ECON_MAX_STOCKS];
extern int            g_stock_count;
extern EconRoute      g_routes[ECON_MAX_ROUTES];
extern int            g_route_count;
extern EconCurrency   g_currencies[ECON_MAX_CURRENCIES];
extern int            g_currency_count;
extern EconDiplomacy  g_diplomacy[ECON_MAX_DIPLOMACY];
extern int            g_diplomacy_count;
extern EconMerchant   g_merchants[ECON_MAX_MERCHANTS];
extern int            g_merchant_count;
extern EconRecipe     g_recipes[ECON_MAX_RECIPES];
extern int            g_recipe_count;

/* LUTカーブ (動的生成) */
extern u8 g_curve_lut[ECON_MAX_CURVES][ECON_CURVE_SIZE];

/* 減衰LUT */
extern u8 g_decay_lut[128];

/* 距離減衰LUT */
extern u8 g_distance_lut[32];

/* 取引履歴リングバッファ */
extern EconTradeEntry g_trade_log[ECON_MAX_TRADE_LOG];
extern int            g_trade_log_head;
extern int            g_trade_log_count;

/* DB接続ハンドル */
extern db_handle_t    g_econ_db;

/* ターンカウンタ */
extern u32            g_turn_count;
extern u8             g_current_season;

/* ポリシー関数ポインタ */
extern econ_price_policy_fn   g_price_policy;
extern econ_restock_policy_fn g_restock_policy;
extern econ_trade_policy_fn   g_trade_policy;
extern econ_turn_policy_fn    g_turn_policy;

/* コールバック */
extern econ_trade_callback    g_trade_cb;
extern econ_price_callback    g_price_cb;

/* ====================================================================== */
/*  内部ヘルパー関数                                                       */
/* ====================================================================== */

/* market_id から g_markets インデックスを取得 (-1=見つからない) */
int econ__find_market(u16 market_id);

/* item_id から g_items インデックスを取得 (-1=見つからない) */
int econ__find_item(u16 item_id);

/* market_id + item_id から g_stocks インデックスを取得 (-1=見つからない) */
int econ__find_stock(u16 market_id, u16 item_id);

/* from_id + to_id から g_routes インデックスを取得 (-1=見つからない) */
int econ__find_route(u8 from_id, u8 to_id);

/* currency_id から g_currencies インデックスを取得 (-1=見つからない) */
int econ__find_currency(u8 currency_id);

/* nation_a + nation_b の外交関係を取得 (-1=見つからない) */
int econ__find_diplomacy(u8 nation_a, u8 nation_b);

/* merchant_id から g_merchants インデックスを取得 (-1=見つからない) */
int econ__find_merchant(u16 merchant_id);

/* recipe_id から g_recipes インデックスを取得 (-1=見つからない) */
int econ__find_recipe(u16 recipe_id);

/* デフォルト価格計算 (ポリシー未設定時に使用) */
u16 econ__default_price(const EconItem *item, i16 stock_pct,
                         u16 season_day, i8 diplomacy);

/* 取引履歴に記録 */
void econ__log_trade(u16 market_id, u16 item_id,
                     u16 qty, u16 unit_price, u8 buyer, u8 seller);

/* 需給カーブ LUT から倍率を参照 (0-255 → 50-200% のような倍率) */
u16 econ__curve_lookup(u8 curve_type, u8 index);

#endif /* ECON_INTERNAL_H */
