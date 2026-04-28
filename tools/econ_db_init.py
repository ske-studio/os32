#!/usr/bin/env python3
"""
econ_db_init.py — libos32econ マスタデータベース生成ツール

/db/econ.db にテスト用の経済マスタデータを投入する。
ゲーム実装時は各タイトルが独自のデータを投入する想定。
"""

import sqlite3
import os
import sys

DB_PATH = os.path.join(os.path.dirname(__file__), '..', 'assets', 'econ.db')

SCHEMA = """
/* 商品マスタ */
CREATE TABLE IF NOT EXISTS items (
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
CREATE TABLE IF NOT EXISTS markets (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    nation_id   INTEGER NOT NULL,
    tax_rate    INTEGER DEFAULT 10,
    wealth      INTEGER DEFAULT 100,
    pop         INTEGER DEFAULT 100
);

/* 市場在庫 */
CREATE TABLE IF NOT EXISTS market_items (
    market_id   INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    stock       INTEGER DEFAULT 10,
    max_stock   INTEGER DEFAULT 50,
    demand      INTEGER DEFAULT 50,
    restock_rate INTEGER DEFAULT 1,
    PRIMARY KEY (market_id, item_id)
);

/* 交易ルート */
CREATE TABLE IF NOT EXISTS trade_routes (
    from_id   INTEGER NOT NULL,
    to_id     INTEGER NOT NULL,
    distance  INTEGER NOT NULL,
    risk      INTEGER DEFAULT 0,
    tariff    INTEGER DEFAULT 0,
    flags     INTEGER DEFAULT 0,
    PRIMARY KEY (from_id, to_id)
);

/* 通貨 */
CREATE TABLE IF NOT EXISTS currencies (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    nation_id   INTEGER NOT NULL,
    supply      INTEGER NOT NULL,
    base_value  INTEGER DEFAULT 100
);

/* 国家 */
CREATE TABLE IF NOT EXISTS nations (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    currency_id INTEGER NOT NULL
);

/* 外交関係 */
CREATE TABLE IF NOT EXISTS diplomacy (
    nation_a    INTEGER NOT NULL,
    nation_b    INTEGER NOT NULL,
    relation    INTEGER DEFAULT 50,
    trade_pact  INTEGER DEFAULT 0,
    embargo     INTEGER DEFAULT 0,
    PRIMARY KEY (nation_a, nation_b)
);

/* LUTカーブ制御点 */
CREATE TABLE IF NOT EXISTS curve_points (
    curve_id    INTEGER NOT NULL,
    x           INTEGER NOT NULL,
    y           INTEGER NOT NULL,
    PRIMARY KEY (curve_id, x)
);

/* 季節修正 */
CREATE TABLE IF NOT EXISTS season_modifiers (
    item_id     INTEGER NOT NULL,
    season      INTEGER NOT NULL,
    supply_mod  INTEGER DEFAULT 100,
    demand_mod  INTEGER DEFAULT 100,
    PRIMARY KEY (item_id, season)
);

/* NPC商人 */
CREATE TABLE IF NOT EXISTS merchants (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    market_id   INTEGER NOT NULL,
    buy_margin  INTEGER DEFAULT 50,
    sell_margin INTEGER DEFAULT 120,
    specialty   INTEGER DEFAULT 0,
    mood        INTEGER DEFAULT 50
);

/* クラフトレシピ */
CREATE TABLE IF NOT EXISTS recipes (
    id          INTEGER PRIMARY KEY,
    result_id   INTEGER NOT NULL,
    result_qty  INTEGER DEFAULT 1,
    craft_time  INTEGER DEFAULT 1,
    skill_req   INTEGER DEFAULT 0
);

CREATE TABLE IF NOT EXISTS recipe_materials (
    recipe_id   INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    quantity    INTEGER NOT NULL,
    PRIMARY KEY (recipe_id, item_id)
);

/* 不動産マスタ */
CREATE TABLE IF NOT EXISTS estates (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    type        INTEGER NOT NULL DEFAULT 0,
    stage       INTEGER NOT NULL DEFAULT 1,
    base_value  INTEGER NOT NULL
);

/* レベル別パラメータ */
CREATE TABLE IF NOT EXISTS estate_levels (
    level       INTEGER PRIMARY KEY,
    income_mul  INTEGER NOT NULL DEFAULT 100,
    value_mul   INTEGER NOT NULL DEFAULT 100,
    invest_div  INTEGER NOT NULL DEFAULT 256
);

/* 不動産種別定義 */
CREATE TABLE IF NOT EXISTS estate_types (
    type            INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    bonus_pct       INTEGER NOT NULL DEFAULT 0,
    bonus_mode      INTEGER NOT NULL DEFAULT 0,
    bonus_per_route INTEGER NOT NULL DEFAULT 0
);
"""

def insert_test_data(conn):
    """テスト用マスタデータを投入"""
    c = conn.cursor()

    # === 商品 (8品目) ===
    items = [
        # (id, name, category, base_price, weight, rarity, curve_type, season_amp, diplo_weight, elasticity)
        (1,  'Wheat',    0, 10,  2, 1, 0, 30, 80,  50),  # 小麦 (食料、季節変動あり)
        (2,  'Fish',     0, 15,  3, 1, 0, 20, 80,  60),  # 魚
        (3,  'Iron Ore', 1, 25,  5, 2, 0,  0, 100, 40),  # 鉄鉱石 (材料)
        (4,  'Lumber',   1, 12,  4, 1, 0, 10, 90,  45),  # 木材
        (5,  'Sword',    2, 80,  3, 3, 1,  0, 120, 30),  # 剣 (武器)
        (6,  'Shield',   3, 60,  4, 3, 1,  0, 110, 35),  # 盾 (防具)
        (7,  'Potion',   4, 30,  1, 2, 0,  0, 100, 50),  # ポーション
        (8,  'Silk',     5, 100, 1, 4, 0, 15, 150, 25),  # 絹 (贅沢品)
    ]
    c.executemany(
        "INSERT INTO items (id, name, category, base_price, weight, rarity, "
        "curve_type, season_amp, diplo_weight, elasticity) VALUES (?,?,?,?,?,?,?,?,?,?)",
        items
    )

    # === 国家 (3国) ===
    nations = [
        (0, 'Player Kingdom', 0),
        (1, 'Trade Republic',  1),
        (2, 'Mountain Empire', 2),
    ]
    c.executemany("INSERT INTO nations VALUES (?,?,?)", nations)

    # === 市場 (3箇所) ===
    markets = [
        (1, 'Capital Market',  0, 10, 150, 200),  # プレイヤー国首都
        (2, 'Port City',       1,  5, 200, 300),  # 交易共和国 (低税率)
        (3, 'Mountain Bazaar', 2, 15, 100, 100),  # 山岳帝国 (高税率)
    ]
    c.executemany("INSERT INTO markets VALUES (?,?,?,?,?,?)", markets)

    # === 市場在庫 ===
    # 各市場に全商品を配置 (特産品は在庫多め)
    stock_data = [
        # Capital: 小麦豊富、鉄少なめ
        (1, 1, 30, 50, 30, 2),  # Wheat
        (1, 2, 15, 40, 40, 1),  # Fish
        (1, 3,  5, 30, 60, 1),  # Iron Ore
        (1, 4, 20, 50, 35, 2),  # Lumber
        (1, 5,  3, 10, 70, 0),  # Sword
        (1, 6,  3, 10, 65, 0),  # Shield
        (1, 7, 10, 30, 50, 1),  # Potion
        (1, 8,  2, 10, 80, 0),  # Silk

        # Port City: 魚豊富、絹安い
        (2, 1, 15, 40, 40, 1),
        (2, 2, 40, 60, 20, 3),  # Fish特産
        (2, 3, 10, 30, 50, 1),
        (2, 4, 10, 40, 45, 1),
        (2, 5,  5, 15, 60, 0),
        (2, 6,  5, 15, 55, 0),
        (2, 7, 15, 40, 40, 1),
        (2, 8, 10, 20, 30, 1),  # Silk特産

        # Mountain: 鉄豊富、魚少ない
        (3, 1, 10, 30, 50, 1),
        (3, 2,  3, 20, 70, 0),  # 魚不足
        (3, 3, 35, 50, 20, 3),  # Iron特産
        (3, 4, 25, 50, 30, 2),  # 木材豊富
        (3, 5, 10, 20, 40, 1),  # 武器生産地
        (3, 6, 10, 20, 45, 1),  # 防具生産地
        (3, 7,  5, 20, 60, 0),
        (3, 8,  1,  5, 90, 0),  # 絹希少
    ]
    c.executemany(
        "INSERT INTO market_items VALUES (?,?,?,?,?,?)",
        stock_data
    )

    # === 交易ルート ===
    routes = [
        (1, 2, 5,  5, 0, 0),   # Capital ↔ Port: 短距離、低リスク
        (1, 3, 10, 15, 5, 0),  # Capital ↔ Mountain: 中距離、やや危険
        (2, 3, 12, 20, 10, 0), # Port ↔ Mountain: 長距離、危険
    ]
    c.executemany("INSERT INTO trade_routes VALUES (?,?,?,?,?,?)", routes)

    # === 通貨 ===
    currencies = [
        (0, 'Gold Crown',     0, 10000, 100),
        (1, 'Silver Ducat',   1, 15000,  80),
        (2, 'Iron Mark',      2,  8000, 120),
    ]
    c.executemany("INSERT INTO currencies VALUES (?,?,?,?,?)", currencies)

    # === 外交関係 ===
    diplomacy_data = [
        (0, 1,  60, 1, 0),  # Player ↔ Trade Republic: 友好、通商条約あり
        (0, 2, -20, 0, 0),  # Player ↔ Mountain Empire: やや敵対
        (1, 2,  30, 0, 0),  # Trade ↔ Mountain: 中立やや友好
    ]
    c.executemany("INSERT INTO diplomacy VALUES (?,?,?,?,?)", diplomacy_data)

    # === LUTカーブ制御点 ===
    # Curve 0: 線形 (デフォルト需給カーブ)
    curve0 = [(0, 0, 0), (0, 32, 128), (0, 63, 255)]
    # Curve 1: S字カーブ (武器/防具用)
    curve1 = [(1, 0, 10), (1, 16, 40), (1, 32, 128), (1, 48, 210), (1, 63, 245)]
    c.executemany("INSERT INTO curve_points VALUES (?,?,?)", curve0 + curve1)

    # === NPC商人 ===
    merchants_data = [
        (1, 'General Trader',  1, 50, 120, 7, 60),  # Capital汎用商人
        (2, 'Fishmonger',      2, 60, 110, 0, 70),   # Port魚商
        (3, 'Blacksmith',      3, 45, 130, 2, 40),   # Mountain武器商
        (4, 'Silk Merchant',   2, 55, 140, 5, 50),   # Port絹商
    ]
    c.executemany("INSERT INTO merchants VALUES (?,?,?,?,?,?,?)", merchants_data)

    # === 不動産 (4件) ===
    estates = [
        # (id, name, type, stage, base_value)
        (1, 'Greenhill Village',   0, 1, 6400),   # 村 (base_value/640=10)
        (2, 'Harbor Town',         1, 1, 12800),   # 港 (base_value/640=20)
        (3, 'Iron Fortress',       2, 2, 19200),   # 砦 (base_value/640=30)
        (4, 'Gold Mine',           3, 2, 32000),   # 鉱山 (base_value/640=50)
    ]
    c.executemany(
        "INSERT INTO estates (id, name, type, stage, base_value) "
        "VALUES (?,?,?,?,?)",
        estates
    )

    # === レベルテーブル (8段階) ===
    estate_levels = [
        # (level, income_mul%, value_mul%, invest_div)
        (1,  100, 100, 256),   # Lv1: 等倍
        (2,  120, 130, 128),   # Lv2: 収入1.2倍, 価値1.3倍
        (3,  150, 170,  64),   # Lv3: 収入1.5倍, 価値1.7倍
        (4,  180, 210,  32),   # Lv4: 収入1.8倍, 価値2.1倍
        (5,  210, 250,  16),   # Lv5: 収入2.1倍, 価値2.5倍
        (6,  240, 250,   8),   # Lv6: 収入2.4倍, 価値2.5倍
        (7,  250, 250,   4),   # Lv7: 収入2.5倍, 価値2.5倍
        (8,  250, 250,   2),   # Lv8: 収入2.5倍, 価値2.5倍 (最大)
    ]
    c.executemany(
        "INSERT INTO estate_levels (level, income_mul, value_mul, invest_div) "
        "VALUES (?,?,?,?)",
        estate_levels
    )

    # === 不動産種別定義 (4種) ===
    estate_types = [
        # (type, name, bonus_pct, bonus_mode, bonus_per_route)
        (0, 'Village',  0,  0,  0),   # 村: ボーナスなし
        (1, 'Port',     10, 1, 10),   # 港: 基本+10%, ルート1本ごと+10%
        (2, 'Fort',     20, 0,  0),   # 砦: 固定+20%
        (3, 'Mine',     50, 2,  0),   # 鉱山: 固定+50%
    ]
    c.executemany(
        "INSERT INTO estate_types (type, name, bonus_pct, bonus_mode, bonus_per_route) "
        "VALUES (?,?,?,?,?)",
        estate_types
    )

    conn.commit()

def main():
    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)
        print(f"Removed existing: {DB_PATH}")

    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)

    conn = sqlite3.connect(DB_PATH)
    print(f"Creating: {DB_PATH}")

    # スキーマ作成
    conn.executescript(SCHEMA)
    print("  Schema created")

    # テストデータ投入
    insert_test_data(conn)
    print("  Test data inserted")

    # 統計表示
    c = conn.cursor()
    c.execute("SELECT COUNT(*) FROM items")
    print(f"  Items:      {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM markets")
    print(f"  Markets:    {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM market_items")
    print(f"  Stocks:     {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM trade_routes")
    print(f"  Routes:     {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM currencies")
    print(f"  Currencies: {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM diplomacy")
    print(f"  Diplomacy:  {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM curve_points")
    print(f"  Curve pts:  {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM merchants")
    print(f"  Merchants:  {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM estates")
    print(f"  Estates:    {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM estate_levels")
    print(f"  Est.Levels: {c.fetchone()[0]}")
    c.execute("SELECT COUNT(*) FROM estate_types")
    print(f"  Est.Types:  {c.fetchone()[0]}")

    conn.close()
    print("Done!")

if __name__ == '__main__':
    main()
