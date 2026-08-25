/* ======================================================================== */
/*  LIBOS32ECON.H — OS32 経済エンジンライブラリ 公開ヘッダ                   */
/*                                                                          */
/*  ターン制のデータ駆動型経済シミュレーションエンジン。                       */
/*  市場・通貨・需給・外交をSQLiteルール辞書とC89整数演算で管理し、           */
/*  ゲーム内経済の「創発的挙動」を実現する。                                  */
/*                                                                          */
/*  依存: libos32math (isin/fix16_t/lerp_int), libos32db (KAPI経由SQLite)   */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32ECON_H
#define LIBOS32ECON_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 季節定数                                                            */
/* ====================================================================== */

#define ECON_SEASON_SPRING  0
#define ECON_SEASON_SUMMER  1
#define ECON_SEASON_AUTUMN  2
#define ECON_SEASON_WINTER  3

/* ====================================================================== */
/*  2. 商品カテゴリ                                                        */
/* ====================================================================== */

#define ITEM_CAT_FOOD       0
#define ITEM_CAT_MATERIAL   1
#define ITEM_CAT_WEAPON     2
#define ITEM_CAT_ARMOR      3
#define ITEM_CAT_POTION     4
#define ITEM_CAT_LUXURY     5
#define ITEM_CAT_TOOL       6
#define ITEM_CAT_MISC       7

/* ====================================================================== */
/*  3. 配列上限                                                            */
/* ====================================================================== */

#define ECON_MAX_ITEMS       64
#define ECON_MAX_MARKETS      8
#define ECON_MAX_STOCKS      64
#define ECON_MAX_ROUTES      32
#define ECON_MAX_CURRENCIES   4
#define ECON_MAX_DIPLOMACY   16
#define ECON_MAX_CURVES       4
#define ECON_CURVE_SIZE      64
#define ECON_MAX_TRADE_LOG   32
#define ECON_MAX_MERCHANTS   16
#define ECON_MAX_RECIPES     32
#define ECON_MAX_RECIPE_MAT   8

/* 同時管理不動産上限。対戦スゴロクRPG の実マップが 59 村を持つため 64 とする
   (32 のままだと load_estates() が黙って 32 件で打ち切る) */
#define ECON_ESTATE_MAX      64
#define ECON_ESTATE_LV_MAX    8   /* 最大レベル */
#define ECON_ESTATE_TYPE_MAX  8   /* 不動産種別上限 */
#define ECON_OWNER_NONE    0xFF
#define ECON_SHARE_FULL     100   /* 単独統治時の収入分配率 */

/* リフレーション間隔 (ターン数) */
#define ECON_REFL_INTERVAL  360

/* ====================================================================== */
/*  4. データ構造体                                                        */
/* ====================================================================== */

/* 商品 (EconItem) — RAMキャッシュ */
typedef struct {
    u16  id;
    u16  base_price;       /* 基準価格 */
    u8   category;         /* ITEM_CAT_* */
    u8   rarity;
    u8   weight;           /* 重量 (輸送コスト係数) */
    u8   curve_type;       /* カーブ種別 (LUTインデックス) */
    u8   season_amp;       /* 季節振幅 (0=影響なし) */
    u8   diplo_weight;     /* 外交影響度 */
    u8   elasticity;       /* 弾力性 (線形カーブ時) */
    u8   flags;
} EconItem;                /* 12B × 64 = 768B */

/* 市場 (EconMarket) — RAMキャッシュ */
typedef struct {
    u16  id;
    u8   nation_id;        /* 所属国家 → 使用通貨決定 */
    u8   tax_rate;         /* 税率 (%) */
    u16  wealth;           /* 経済力 */
    u16  pop;              /* 人口 */
    u8   active;           /* 0=非アクティブ */
    u8   _pad;
} EconMarket;              /* 10B × 8 = 80B */

/* 市場在庫 (EconStock) */
typedef struct {
    u16  item_id;
    i16  stock;            /* 現在在庫 */
    u16  max_stock;
    u16  demand;           /* 需要 */
    u16  price_mod;        /* 現在価格倍率 (%) */
    u8   restock_rate;
    u8   market_id;
} EconStock;               /* 12B × 64 = 768B */

/* 交易ルート (EconRoute) — 距離モデル */
typedef struct {
    u8   from_id;
    u8   to_id;
    u8   distance;         /* 輸送コスト係数 */
    u8   risk;             /* 盗賊リスク (%) */
    u8   tariff;           /* 関税率 (%) */
    u8   flags;            /* bit0=封鎖中 */
} EconRoute;               /* 6B × 32 = 192B */

/* 通貨 (EconCurrency) */
typedef struct {
    u8   id;
    u8   nation_id;
    u16  supply;           /* 流通量 (上限65535) */
    u16  base_value;       /* 基準価値 (100=標準) */
    u16  _pad;
} EconCurrency;            /* 8B × 4 = 32B */

/* 外交関係 (EconDiplomacy) */
typedef struct {
    u8   nation_a;
    u8   nation_b;
    i8   relation;         /* -100(敵対) ～ +100(同盟) */
    u8   flags;            /* bit0=通商条約, bit1=禁輸 */
} EconDiplomacy;           /* 4B × 16 = 64B */

/* 取引履歴 (リングバッファ) */
typedef struct {
    u32  tick;
    u16  market_id;
    u16  item_id;
    u16  qty;
    u16  unit_price;
    u8   buyer;
    u8   seller;
} EconTradeEntry;          /* 14B × 32 = 448B */

/* NPC商人 */
typedef struct {
    u16  id;
    u16  market_id;
    u8   buy_margin;       /* 買取マージン (%) */
    u8   sell_margin;      /* 販売マージン (%) */
    u8   specialty;        /* 専門カテゴリ */
    u8   mood;             /* 気分 (0-100) */
} EconMerchant;            /* 8B */

/* クラフトレシピ */
typedef struct {
    u16  id;
    u16  result_id;        /* 生成物の item_id */
    u8   result_qty;
    u8   craft_time;       /* ターン数 */
    u8   skill_req;        /* 必要スキルレベル */
    u8   mat_count;        /* 素材数 */
    u16  mat_ids[ECON_MAX_RECIPE_MAT];   /* 素材 item_id */
    u8   mat_qtys[ECON_MAX_RECIPE_MAT];  /* 素材必要数 */
    u8   _pad[ECON_MAX_RECIPE_MAT];      /* アライメント用 */
} EconRecipe;

/* 不動産種別 */
#define ESTATE_TYPE_VILLAGE  0   /* 村 */
#define ESTATE_TYPE_PORT     1   /* 港 */
#define ESTATE_TYPE_FORT     2   /* 砦 */
#define ESTATE_TYPE_MINE     3   /* 鉱山 */

/* 不動産フラグ */
#define ESTATE_FLAG_BLOCKED  0x01  /* 封鎖 */
#define ESTATE_FLAG_LOOTED   0x02  /* 略奪中 */

/* 不動産 (EconEstate) — RAMキャッシュ */
typedef struct {
    u16  id;
    u8   owner;           /* オーナーID (0xFF=無主) */
    u8   level;           /* 1〜ECON_ESTATE_LV_MAX */
    u8   type;            /* ESTATE_TYPE_* */
    u8   flags;           /* 封鎖/略奪中等 */
    u8   co_owner;        /* 共同統治オーナーID (0xFF=なし) */
    u8   share_pct;       /* 主オーナー収入分配率% (100=単独) */
    u32  base_value;      /* 基本価値 */
    u32  value;           /* 現在価値 (base_value * レベル倍率) */
    u32  tax;             /* 蓄積上納金 */
    u8   monster_id;      /* 支配モンスターID (0=なし) */
    u8   stage;           /* 所属ステージ */
    u16  _pad2;
} EconEstate;              /* 24B * 64 = 1536B */

/* レベルテーブル (RAMキャッシュ) */
typedef struct {
    u8   level;
    u8   income_mul;      /* 収入倍率% (100=等倍) */
    u8   value_mul;       /* 価値倍率% */
    u8   _pad;
    u32  invest_cost_div; /* 投資費用の除数 */
} EconEstateLevelDef;      /* 8B * 8 = 64B */

/* 不動産種別定義 (RAMキャッシュ) */
/* 種別ごとの特殊収入ボーナスを定義する */
#define ESTATE_BONUS_NONE    0  /* 固定ボーナスのみ */
#define ESTATE_BONUS_TRADE   1  /* 交易ルート数連動 (港) */
#define ESTATE_BONUS_MINERAL 2  /* 鉱物ボーナス (鉱山) */
typedef struct {
    u8   type;            /* ESTATE_TYPE_* */
    u8   bonus_pct;       /* 基本ボーナス% (0=ボーナスなし) */
    u8   bonus_mode;      /* ESTATE_BONUS_* */
    u8   bonus_per_route; /* TRADEモード: ルート1本当たりの追加% */
} EconEstateTypeDef;       /* 4B * 8 = 32B */

/* ====================================================================== */
/*  5. ポリシー関数型 (差し替え可能な計算ロジック)                          */
/* ====================================================================== */

/* 価格計算ポリシー */
typedef u16 (*econ_price_policy_fn)(
    const EconItem *item, i16 stock_pct,
    u16 season_day, i8 diplomacy);

/* 在庫回復ポリシー */
typedef i16 (*econ_restock_policy_fn)(
    const EconStock *stock, u16 elapsed_turns);

/* 取引判定ポリシー */
typedef u16 (*econ_trade_policy_fn)(
    u16 market_id, u16 item_id, u16 qty, u32 wallet);

/* ターン更新ポリシー */
typedef void (*econ_turn_policy_fn)(
    u16 market_id, u8 season);

/* ====================================================================== */
/*  6. コールバック型                                                      */
/* ====================================================================== */

typedef void (*econ_trade_callback)(u16 market_id, u16 item_id,
                                     u16 qty, u16 price, u8 is_buy);
typedef void (*econ_price_callback)(u16 market_id, u16 item_id,
                                     i16 old_mod, i16 new_mod);

/* ====================================================================== */
/*  7. API — システム管理                                                  */
/* ====================================================================== */

int  econ_init(const char *db_path);
void econ_shutdown(void);
void econ_reset(void);

/* ====================================================================== */
/*  8. API — 市場操作                                                      */
/* ====================================================================== */

int  econ_activate_market(u16 market_id);
u16  econ_get_price(u16 market_id, u16 item_id);
i16  econ_get_stock(u16 market_id, u16 item_id);
u8   econ_get_demand(u16 market_id, u16 item_id);
u16  econ_transport_cost(u16 from_market, u16 to_market, u16 item_id);
i16  econ_trade_profit(u16 from_market, u16 to_market, u16 item_id);

/* ====================================================================== */
/*  9. API — 取引                                                          */
/* ====================================================================== */

u16  econ_buy(u16 market_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_sell(u16 market_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_buy_from(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_sell_to(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet);
int  econ_haggle(u16 merchant_id, u16 item_id, u8 *discount_pct);

/* ====================================================================== */
/*  10. API — シミュレーション                                             */
/* ====================================================================== */

void econ_turn_advance(u16 season_day);

/* ====================================================================== */
/*  11. API — 通貨・為替                                                   */
/* ====================================================================== */

u16  econ_exchange_rate(u8 currency_a, u8 currency_b);
u32  econ_convert(u32 amount, u8 from_currency, u8 to_currency);
void econ_mint(u8 nation_id, u16 amount);
void econ_set_tax(u16 market_id, u8 rate);

/* ====================================================================== */
/*  12. API — 外交                                                         */
/* ====================================================================== */

u16  econ_diplomacy_modifier(u8 player_nation, u16 market_id);
void econ_change_relation(u8 nation_a, u8 nation_b, i8 delta);

/* ====================================================================== */
/*  13. API — クエリ・分析                                                 */
/* ====================================================================== */

u16  econ_cheapest_market(u16 item_id);
u16  econ_priciest_market(u16 item_id);
i16  econ_price_trend(u16 market_id, u16 item_id);
i16  econ_price_diff(u16 market_a, u16 market_b, u16 item_id);

/* ====================================================================== */
/*  14. API — クラフト                                                     */
/* ====================================================================== */

int  econ_can_craft(u16 recipe_id, const void *inventory);
int  econ_craft(u16 recipe_id, void *inventory);
int  econ_get_recipes(u16 category, u16 *out_ids, int max_count);

/* ====================================================================== */
/*  15. API — ポリシー設定                                                 */
/* ====================================================================== */

void econ_set_price_policy(econ_price_policy_fn fn);
void econ_set_restock_policy(econ_restock_policy_fn fn);
void econ_set_trade_policy(econ_trade_policy_fn fn);
void econ_set_turn_policy(econ_turn_policy_fn fn);

/* ====================================================================== */
/*  16. API — コールバック                                                 */
/* ====================================================================== */

void econ_set_trade_callback(econ_trade_callback cb);
void econ_set_price_callback(econ_price_callback cb);

/* ====================================================================== */
/*  17. API — Chem連携                                                     */
/* ====================================================================== */

void econ_on_chem_event(u16 market_id, u32 chem_elem,
                         u8 chem_action, i16 impact);

/* ====================================================================== */
/*  18. API — デバッグ                                                     */
/* ====================================================================== */

int  econ_item_count(void);
int  econ_market_count(void);
int  econ_active_market_count(void);
void econ_debug_dump(u16 market_id);

/* ====================================================================== */
/*  19. API — 不動産 (estate)                                              */
/* ====================================================================== */

/* 初期化 (econ_init 内部から自動呼出し) */
int  econ_estate_init(void);

/* 統治操作 */
int  econ_estate_claim(u16 id, u8 owner);
void econ_estate_release(u16 id);
void econ_estate_set_monster(u16 id, u8 monster_id);
void econ_estate_clear_monster(u16 id);

/* 投資 */
int  econ_estate_invest(u16 id, u32 *wallet);
void econ_estate_damage(u16 id);
u32  econ_estate_invest_cost(u16 id);

/* 収入管理 */
void econ_estate_accumulate(void);
u32  econ_estate_collect(u8 owner);
u32  econ_estate_collect_one(u16 id);
u32  econ_estate_income(u16 id);

/* 資産計算 */
u32  econ_estate_total_value(u8 owner);
int  econ_estate_count(u8 owner);
int  econ_estate_list(u8 owner, u16 *out, int max);

/* クエリ */
const EconEstate *econ_estate_get(u16 id);
/* 読み込み順 (id昇順) の index でアクセスする。
   id が連番である保証はないので、全件走査にはこちらを使う。
   範囲外なら NULL */
const EconEstate *econ_estate_at(int index);
int  econ_estate_total(void);
u8   econ_estate_level(u16 id);
u32  econ_estate_value(u16 id);
void econ_estate_set_flag(u16 id, u8 flag);
void econ_estate_clear_flag(u16 id, u8 flag);
int  econ_estate_has_flag(u16 id, u8 flag);

/* 種別ボーナス */
u32  econ_estate_type_bonus(u16 id);
const EconEstateTypeDef *econ_estate_type_def(u8 type);

/* 共同統治 */
int  econ_estate_set_co_owner(u16 id, u8 co_owner, u8 share_pct);
void econ_estate_clear_co_owner(u16 id);
u32  econ_estate_collect_co(u8 co_owner);

#endif /* LIBOS32ECON_H */
