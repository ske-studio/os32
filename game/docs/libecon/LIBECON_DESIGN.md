# libos32econ — 経済エンジンライブラリ設計書

## 1. 概要

ターン制の**データ駆動型経済シミュレーションエンジン**。
市場・通貨・需給・外交をSQLiteルール辞書とC89整数演算で管理し、
ゲーム内経済の「創発的挙動」を実現する。

### 設計思想

- **ルール定義はSQL**: 係数・カーブ制御点・市場設定をSQLで定義
- **ランタイムはC + LUT**: ターン更新はSQL不使用、整数演算 + LUT参照
- **起動時キャッシュ**: マスタデータとLUTカーブを起動時にRAMへロード
- **ポリシー注入**: 価格計算等の核心ロジックを関数ポインタで差し替え可能
- **ターン制**: 毎フレームではなくゲーム内時間単位で更新 (CPU負荷軽減)

### 依存関係

```
libos32math  (isin/fix16_t/lerp_int)
     ↑
libos32econ  (math + db)
     ↑
libos32db    (KAPI経由SQLite)
```

`libos32econ` は **描画 (gfx) には依存しない**。
経済シミュレーションとデータ管理のみを担当し、UIはゲーム側が担当する。

### libos32chemとの対比

| 側面 | libos32chem | libos32econ |
|------|-------------|-------------|
| 管理対象 | 物質の属性・温度・状態 | 資源・通貨・価格・需給 |
| ルール定義 | 反応ルール (FIRE+WOOD→IGNITE) | 係数・カーブ・市場設定 |
| ホットパス | 毎フレーム温度更新 | ターンごと価格・在庫更新 |
| 創発性 | 火が木に燃え移り水で消える | インフレ、物資不足、交易利益 |
| 差し替え | callback (通知のみ) | **ポリシー関数 (計算ロジック自体)** |

### ECS層との関係

libos32chemと**同じ層**。アダプタ方式で疎結合:

```
ECS (COMP_ECON)  ←→  libos32econ  ←→  libos32db
     ↕ アダプタ
ECS (COMP_CHEM)  ←→  libos32chem  ←→  libos32db
```

---

## 2. ターン制設計

econは**ゲーム内時間単位 (ターン)** で更新する。
1ターン = 1ゲーム日 を想定 (ゲーム側がタイミングを制御):

```c
#define ECON_SEASON_SPRING  0
#define ECON_SEASON_SUMMER  1
#define ECON_SEASON_AUTUMN  2
#define ECON_SEASON_WINTER  3

/* ゲームループ内での使い方 */
/*   chem_update();                  毎フレーム       */
/*   if (day_elapsed)                                 */
/*       econ_turn_advance(season);  ターン制 (1日1回) */
```

---

## 3. コアデータ構造

### 3.1 商品 (EconItem) — RAMキャッシュ

```c
#define ECON_MAX_ITEMS    64

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
```

### 3.2 市場 (EconMarket) — RAMキャッシュ

```c
#define ECON_MAX_MARKETS   8

typedef struct {
    u16  id;
    u8   nation_id;        /* 所属国家 → 使用通貨決定 */
    u8   tax_rate;         /* 税率 (%) */
    u16  wealth;           /* 経済力 */
    u16  pop;              /* 人口 */
    u8   active;           /* 0=非アクティブ */
    u8   _pad;
} EconMarket;              /* 10B × 8 = 80B */
```

### 3.3 市場在庫 (EconStock)

```c
#define ECON_MAX_STOCKS   64

typedef struct {
    u16  item_id;
    i16  stock;            /* 現在在庫 */
    u16  max_stock;
    u16  demand;           /* 需要 */
    u16  price_mod;        /* 現在価格倍率 (%) */
    u8   restock_rate;
    u8   market_id;
} EconStock;               /* 12B × 64 = 768B */
```

### 3.4 交易ルート (EconRoute) — 距離モデル

```c
#define ECON_MAX_ROUTES   32

typedef struct {
    u8   from_id;
    u8   to_id;
    u8   distance;         /* 輸送コスト係数 */
    u8   risk;             /* 盗賊リスク (%) */
    u8   tariff;           /* 関税率 (%) */
    u8   flags;            /* bit0=封鎖中 */
} EconRoute;               /* 6B × 32 = 192B */
```

### 3.5 通貨 (EconCurrency)

```c
#define ECON_MAX_CURRENCIES 4

typedef struct {
    u8   id;
    u8   nation_id;
    u16  supply;           /* 流通量 (上限65535) */
    u16  base_value;       /* 基準価値 (100=標準) */
    u16  _pad;
} EconCurrency;            /* 8B × 4 = 32B */
```

### 3.6 外交関係 (EconDiplomacy)

```c
#define ECON_MAX_DIPLOMACY 16

typedef struct {
    u8   nation_a;
    u8   nation_b;
    i8   relation;         /* -100(敵対) ～ +100(同盟) */
    u8   flags;            /* bit0=通商条約, bit1=禁輸 */
} EconDiplomacy;           /* 4B × 16 = 64B */
```

### 3.7 LUTカーブ (動的生成)

```c
#define ECON_MAX_CURVES    4
#define ECON_CURVE_SIZE   64

/* init時にSQLの制御点からlerp_int()で補間生成 */
static u8 curve_lut[ECON_MAX_CURVES][ECON_CURVE_SIZE];  /* 256B */
```

### 3.8 取引履歴 (リングバッファ)

```c
#define ECON_MAX_TRADE_LOG 32

typedef struct {
    u32  tick;
    u16  market_id;
    u16  item_id;
    u16  qty;
    u16  unit_price;
    u8   buyer;
    u8   seller;
} EconTradeEntry;          /* 14B × 32 = 448B */
```

---

## 4. SQLテーブル設計

ファイル: `/db/econ.db`

```sql
/* 商品マスタ */
CREATE TABLE items (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    category    INTEGER NOT NULL,
    base_price  INTEGER NOT NULL,
    weight      INTEGER DEFAULT 1,
    rarity      INTEGER DEFAULT 1,
    curve_type  INTEGER DEFAULT 0,
    season_amp  INTEGER DEFAULT 0,
    diplo_weight INTEGER DEFAULT 100,
    elasticity  INTEGER DEFAULT 50,
    chem_elem   INTEGER DEFAULT 0,
    flags       INTEGER DEFAULT 0
);

/* 市場 */
CREATE TABLE markets (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    nation_id   INTEGER NOT NULL,
    tax_rate    INTEGER DEFAULT 10,
    wealth      INTEGER DEFAULT 100,
    pop         INTEGER DEFAULT 100
);

/* 市場在庫 */
CREATE TABLE market_items (
    market_id   INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    stock       INTEGER DEFAULT 10,
    max_stock   INTEGER DEFAULT 50,
    demand      INTEGER DEFAULT 50,
    restock_rate INTEGER DEFAULT 1,
    PRIMARY KEY (market_id, item_id)
);

/* 交易ルート */
CREATE TABLE trade_routes (
    from_id   INTEGER NOT NULL,
    to_id     INTEGER NOT NULL,
    distance  INTEGER NOT NULL,
    risk      INTEGER DEFAULT 0,
    tariff    INTEGER DEFAULT 0,
    flags     INTEGER DEFAULT 0,
    PRIMARY KEY (from_id, to_id)
);

/* 通貨 */
CREATE TABLE currencies (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    nation_id   INTEGER NOT NULL,
    supply      INTEGER NOT NULL,
    base_value  INTEGER DEFAULT 100
);

/* 国家 */
CREATE TABLE nations (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    currency_id INTEGER NOT NULL
);

/* 外交関係 */
CREATE TABLE diplomacy (
    nation_a    INTEGER NOT NULL,
    nation_b    INTEGER NOT NULL,
    relation    INTEGER DEFAULT 50,
    trade_pact  INTEGER DEFAULT 0,
    embargo     INTEGER DEFAULT 0,
    PRIMARY KEY (nation_a, nation_b)
);

/* LUTカーブ制御点 */
CREATE TABLE curve_points (
    curve_id    INTEGER NOT NULL,
    x           INTEGER NOT NULL,
    y           INTEGER NOT NULL,
    PRIMARY KEY (curve_id, x)
);

/* 季節修正 (オプション、isin()代替) */
CREATE TABLE season_modifiers (
    item_id     INTEGER NOT NULL,
    season      INTEGER NOT NULL,
    supply_mod  INTEGER DEFAULT 100,
    demand_mod  INTEGER DEFAULT 100,
    PRIMARY KEY (item_id, season)
);

/* NPC商人 */
CREATE TABLE merchants (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    market_id   INTEGER NOT NULL,
    buy_margin  INTEGER DEFAULT 50,
    sell_margin INTEGER DEFAULT 120,
    specialty   INTEGER DEFAULT 0,
    mood        INTEGER DEFAULT 50
);

/* クラフトレシピ */
CREATE TABLE recipes (
    id          INTEGER PRIMARY KEY,
    result_id   INTEGER NOT NULL,
    result_qty  INTEGER DEFAULT 1,
    craft_time  INTEGER DEFAULT 1,
    skill_req   INTEGER DEFAULT 0
);

CREATE TABLE recipe_materials (
    recipe_id   INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    quantity    INTEGER NOT NULL,
    PRIMARY KEY (recipe_id, item_id)
);
```

---

## 5. ポリシー注入設計

計算ロジックを関数ポインタで差し替え可能にし、共通ライブラリとしての汎用性を確保する。

### 5.1 ポリシー関数型

```c
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
```

### 5.2 設計原則

- **NULL = デフォルト実装** を使用 (ライブラリ組込みの需給カーブ型モデル)
- ユーザーは必要なポリシーのみ差し替え、残りはデフォルト
- コスト: 間接CALL +3cyc/回 → 全体で +0.009ms (無視可能)

### 5.3 デフォルトで変えられる範囲

| 変更内容 | 方法 | C変更 |
|----------|------|-------|
| 弾力性・振幅等の係数調整 | SQL更新 | 不要 |
| カーブ形状の変更 | curve_points更新 | 不要 |
| 価格計算式の差し替え | econ_set_price_policy() | ユーザーC |
| 全く新しい変数の追加 | ポリシー関数内で自由 | ユーザーC |

---

## 6. API設計

### 6.1 システム管理

```c
int  econ_init(const char *db_path);
void econ_shutdown(void);
void econ_reset(void);
```

### 6.2 市場操作

```c
int  econ_activate_market(u16 market_id);
u16  econ_get_price(u16 market_id, u16 item_id);
i16  econ_get_stock(u16 market_id, u16 item_id);
u8   econ_get_demand(u16 market_id, u16 item_id);
u16  econ_transport_cost(u16 from_market, u16 to_market, u16 item_id);
i16  econ_trade_profit(u16 from_market, u16 to_market, u16 item_id);
```

### 6.3 取引

```c
u16  econ_buy(u16 market_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_sell(u16 market_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_buy_from(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet);
u16  econ_sell_to(u16 merchant_id, u16 item_id, u16 qty, u32 *wallet);
int  econ_haggle(u16 merchant_id, u16 item_id, u8 *discount_pct);
```

### 6.4 シミュレーション

```c
void econ_turn_advance(u16 season_day);
void econ_daily_update(void);   /* econ_turn_advance内部から呼ばれる */
```

### 6.5 通貨・為替

```c
u16  econ_exchange_rate(u8 currency_a, u8 currency_b);
u32  econ_convert(u32 amount, u8 from_currency, u8 to_currency);
void econ_mint(u8 nation_id, u16 amount);
void econ_set_tax(u16 market_id, u8 rate);
```

### 6.6 外交

```c
u16  econ_diplomacy_modifier(u8 player_nation, u16 market_id);
void econ_change_relation(u8 nation_a, u8 nation_b, i8 delta);
```

### 6.7 クエリ・分析

```c
u16  econ_cheapest_market(u16 item_id);
u16  econ_priciest_market(u16 item_id);
i16  econ_price_trend(u16 market_id, u16 item_id);
i16  econ_price_diff(u16 market_a, u16 market_b, u16 item_id);
```

### 6.8 クラフト

```c
int  econ_can_craft(u16 recipe_id, const void *inventory);
int  econ_craft(u16 recipe_id, void *inventory);
int  econ_get_recipes(u16 category, u16 *out_ids, int max_count);
```

### 6.9 ポリシー設定

```c
void econ_set_price_policy(econ_price_policy_fn fn);
void econ_set_restock_policy(econ_restock_policy_fn fn);
void econ_set_trade_policy(econ_trade_policy_fn fn);
void econ_set_turn_policy(econ_turn_policy_fn fn);
```

### 6.10 コールバック

```c
typedef void (*econ_trade_callback)(u16 market_id, u16 item_id,
                                     u16 qty, u16 price, u8 is_buy);
typedef void (*econ_price_callback)(u16 market_id, u16 item_id,
                                     i16 old_mod, i16 new_mod);

void econ_set_trade_callback(econ_trade_callback cb);
void econ_set_price_callback(econ_price_callback cb);
```

### 6.11 Chem連携

```c
void econ_on_chem_event(u16 market_id, u32 chem_elem,
                         u8 chem_action, i16 impact);
```

### 6.12 デバッグ

```c
int  econ_item_count(void);
int  econ_market_count(void);
int  econ_active_market_count(void);
void econ_debug_dump(u16 market_id);
```

---

## 7. 需給カーブ (デフォルトポリシー)

### 7.1 価格計算式

```
最終価格 = base_price
         × (price_mod / 100)        需給カーブ
         × (diplomacy_mod / 100)    外交補正
         + season_delta             季節 (isin)
         + transport_cost           輸送コスト
         + tariff                   関税
         + tax                      税
```

### 7.2 需給カーブの実装

LUTカーブ: SQLの制御点からinit時にlerp_int()で64点テーブルを生成。
ランタイムは配列参照のみ (4 cycles)。

季節: libos32mathのisin()を活用。追加メモリなし。

### 7.3 リフレーション

通貨supply が u16 上限に近づくのを防ぐため、
N ターンごとに全通貨を比率保存で正規化:

```c
#define ECON_REFL_INTERVAL  360  /* 1ゲーム年ごと */
```

---

## 8. LUT活用

| LUT名 | サイズ | 用途 |
|--------|--------|------|
| 弾力性カーブ (curve_lut) | 256B | 非線形需給カーブ (SQL制御点から動的生成) |
| 対数減衰 (decay_lut) | 128B | 価格変動の慣性・収束 |
| 季節正弦波 | 0B | libos32math isin() を共用 |
| 距離減衰 | 32B | 対数的輸送コスト |
| **合計** | **~416B** | |

---

## 9. 計算コスト見積もり (i386 16MHz)

| 処理 | 時間 |
|------|------|
| 価格更新 (3市場×16品目) | 0.66 ms |
| 季節補正 (isin方式) | ~0.06 ms |
| 在庫回復 | 0.06 ms |
| 外交補正 | 0.07 ms |
| NPC自律取引 (4人) | 0.24 ms |
| 為替レート | 0.03 ms |
| **1ターン合計** | **~1.1 ms** |
| econ_get_price() 1回 | 10.8 μs |
| リフレーション (年1回) | 1.05 ms |

---

## 10. メモリ使用量

| 構造体 | 合計 |
|--------|------|
| EconItem[64] | 768B |
| EconMarket[8] | 80B |
| EconStock[64] | 768B |
| EconRoute[32] | 192B |
| EconCurrency[4] | 32B |
| EconDiplomacy[16] | 64B |
| curve_lut[4][64] | 256B |
| EconTradeEntry[32] | 448B |
| LUT (減衰+距離) | 160B |
| **合計** | **~2.8KB** |

---

## 11. ディレクトリ構造

```
programs/libos32econ/
├── libos32econ.h          公開ヘッダ
├── econ_internal.h        内部共有ヘッダ
├── econ_core.c            初期化・終了・RAMキャッシュ・LUT生成
├── econ_market.c          市場操作・価格計算
├── econ_trade.c           取引ロジック (buy/sell/haggle)
├── econ_sim.c             シミュレーション (turn_advance/rebalance)
├── econ_currency.c        通貨・為替・リフレーション
├── econ_craft.c           クラフトシステム
└── econ_query.c           クエリ・分析関数

tools/
└── econ_db_init.py        マスタデータ生成 (→ /db/econ.db)

programs/tests/
└── econ_test.c            テストプログラム
```

---

## 12. 実装フェーズ

| Phase | 内容 | 工数 |
|-------|------|------|
| 1 | 商品マスタ + 単一市場 + 需給カーブ (LUT) + buy/sell | 小 |
| 2 | ポリシー注入 + 季節 (isin) | 小 |
| 3 | 複数市場 + 距離 + 輸送コスト | 小 |
| 4 | 通貨 + 為替 + リフレーション | 中 |
| 5 | 外交関係値 + 価格補正 | 小 |
| 6 | NPC商人 + 自律取引 | 中 |
| 7 | 税 + 通貨発行 + クラフト | 中 |
| 8 | ECS/Chem連携 | 大 |

---

## 13. 関連ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [LIBCHEM_DESIGN.md](../libchem/LIBCHEM_DESIGN.md) | libos32chem 設計書 (同層ライブラリ) |
| [LIBMATH_DESIGN.md](../../../docs/tasks/libmath/LIBMATH_DESIGN.md) | libos32math 設計書 (依存先) |
| [KAPI_SPEC.md](../../../docs/KAPI_SPEC.md) | KernelAPI 仕様書 (DB接続) |
