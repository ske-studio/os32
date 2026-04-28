#!/usr/bin/env python3
"""items.db 生成スクリプト — libos32inv テスト用サンプルデータ (Phase 1+2)"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(__file__), '..', 'assets', 'items.db')

def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    # items テーブル (Phase 2: set_id, max_durability 追加)
    c.execute("""CREATE TABLE items(
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        type INTEGER NOT NULL DEFAULT 0,
        effect INTEGER DEFAULT 0,
        param INTEGER DEFAULT 0,
        rarity INTEGER DEFAULT 1,
        equip_slot INTEGER DEFAULT 0,
        stackable INTEGER DEFAULT 0,
        stat_bonus INTEGER DEFAULT 0,
        stat_type INTEGER DEFAULT 0,
        stage INTEGER DEFAULT 1,
        price INTEGER DEFAULT 0,
        set_id INTEGER DEFAULT 0,
        max_durability INTEGER DEFAULT 255
    )""")

    # shop_lineup テーブル
    c.execute("""CREATE TABLE shop_lineup(
        shop_type INTEGER NOT NULL,
        stage INTEGER NOT NULL,
        item_id INTEGER NOT NULL,
        PRIMARY KEY (shop_type, stage, item_id)
    )""")

    # lottery_tables テーブル
    c.execute("""CREATE TABLE lottery_tables(
        table_type INTEGER NOT NULL,
        item_id INTEGER NOT NULL,
        weight INTEGER NOT NULL DEFAULT 1,
        min_stage INTEGER DEFAULT 1,
        PRIMARY KEY (table_type, item_id)
    )""")

    # recipes テーブル (Phase 2)
    c.execute("""CREATE TABLE recipes(
        id INTEGER PRIMARY KEY,
        result_id INTEGER NOT NULL,
        mat_a INTEGER NOT NULL,
        mat_b INTEGER DEFAULT 0,
        mat_a_count INTEGER DEFAULT 1,
        mat_b_count INTEGER DEFAULT 1,
        result_count INTEGER DEFAULT 1
    )""")

    # set_bonus テーブル (Phase 2)
    c.execute("""CREATE TABLE set_bonus(
        set_id INTEGER NOT NULL,
        piece_count INTEGER NOT NULL,
        stat_type INTEGER NOT NULL,
        bonus INTEGER NOT NULL,
        PRIMARY KEY (set_id, piece_count, stat_type)
    )""")

    # --- アイテムデータ ---
    # (id, name, type, effect, param, rarity, equip_slot, stackable,
    #  stat_bonus, stat_type, stage, price, set_id, max_durability)

    items = [
        # 消耗品 (スタック可能, 耐久度なし)
        (1,  'Herb',       0, 1, 30, 1, 0, 1, 0,  0, 1, 10,   0, 0),
        (2,  'Antidote',   0, 2,  0, 1, 0, 1, 0,  0, 1, 15,   0, 0),
        (3,  'Elixir',     0, 3, 99, 3, 0, 1, 0,  0, 3, 500,  0, 0),
        (4,  'Ether',      0, 4, 50, 2, 0, 1, 0,  0, 2, 200,  0, 0),
        (5,  'Phoenix',    0, 5,  0, 4, 0, 1, 0,  0, 4, 1000, 0, 0),
        # 素材アイテム (スタック可能)
        (6,  'IronOre',    0, 0,  0, 1, 0, 1, 0,  0, 1, 20,   0, 0),
        (7,  'MithOre',    0, 0,  0, 2, 0, 1, 0,  0, 2, 80,   0, 0),
        (8,  'DragonFang', 0, 0,  0, 3, 0, 1, 0,  0, 3, 300,  0, 0),
        # 武器 (耐久度あり)
        (10, 'ShortSword', 1, 0, 0, 1, 0, 0, 10, 0, 1, 100,  0, 30),
        (11, 'LongSword',  1, 0, 0, 2, 0, 0, 25, 0, 2, 300,  0, 50),
        (12, 'MagicRod',   1, 0, 0, 3, 0, 0, 15, 3, 2, 250,  0, 40),
        (13, 'GreatAxe',   1, 0, 0, 3, 0, 0, 35, 0, 3, 500,  0, 60),
        (14, 'HolyLance',  1, 0, 0, 4, 0, 0, 50, 0, 4, 1200, 0, 255),
        # 盾 (耐久度あり, IronShield + ChainMail = 鉄セット set_id=1)
        (20, 'WoodShield', 2, 0, 0, 1, 1, 0, 5,  1, 1, 50,   0, 20),
        (21, 'IronShield', 2, 0, 0, 2, 1, 0, 15, 1, 2, 200,  1, 40),
        (22, 'MithShield', 2, 0, 0, 3, 1, 0, 30, 1, 3, 600,  2, 80),
        # 鎧 (耐久度あり, ChainMail = 鉄セット set_id=1)
        (30, 'Leather',    3, 0, 0, 1, 2, 0, 8,  1, 1, 80,   0, 25),
        (31, 'ChainMail',  3, 0, 0, 2, 2, 0, 20, 1, 2, 350,  1, 50),
        (32, 'PlateMail',  3, 0, 0, 3, 2, 0, 40, 1, 3, 800,  2, 100),
        # アクセサリ (装備スロット7=RING, ミスリルセット set_id=2)
        (40, 'SpeedRing',  4, 0, 0, 2, 7, 0, 10, 2, 1, 150,  0, 255),
        (41, 'PowerRing',  4, 0, 0, 2, 7, 0, 8,  0, 2, 200,  0, 255),
        (42, 'MagicAmulet',4, 0, 0, 3, 3, 0, 12, 3, 2, 300,  2, 255),
        # 合成品
        (50, 'SteelSword', 1, 0, 0, 2, 0, 0, 20, 0, 2, 250,  0, 40),
    ]
    c.executemany(
        "INSERT INTO items VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", items)

    # --- ショップ品揃え ---
    shop = [
        # 装備屋 (type=0)
        (0, 1, 10), (0, 1, 20), (0, 1, 30),  # Stage 1
        (0, 2, 11), (0, 2, 21), (0, 2, 31),  # Stage 2
        (0, 3, 13), (0, 3, 22), (0, 3, 32),  # Stage 3
        # 道具屋 (type=1)
        (1, 1, 1), (1, 1, 2),                # Stage 1
        (1, 2, 4),                            # Stage 2
        (1, 3, 3),                            # Stage 3
    ]
    c.executemany("INSERT INTO shop_lineup VALUES(?,?,?)", shop)

    # --- 抽選テーブル ---
    lottery = [
        # 宝箱 (type=0)
        (0, 1, 50, 1),   # Herb
        (0, 2, 30, 1),   # Antidote
        (0, 4, 20, 2),   # Ether
        (0, 3, 5, 3),    # Elixir
        (0, 10, 15, 1),  # ShortSword
        (0, 20, 10, 1),  # WoodShield
        # 敵ドロップ (type=1)
        (1, 1, 60, 1),   # Herb
        (1, 2, 20, 1),   # Antidote
        (1, 10, 10, 1),  # ShortSword
        (1, 40, 5, 2),   # SpeedRing
    ]
    c.executemany("INSERT INTO lottery_tables VALUES(?,?,?,?)", lottery)

    # --- レシピ (Phase 2) ---
    recipes = [
        # id, result_id, mat_a, mat_b, mat_a_count, mat_b_count, result_count
        (1, 50, 10, 6, 1, 3, 1),  # ShortSword + IronOre x3 → SteelSword
        (2, 3, 1, 4, 5, 2, 1),    # Herb x5 + Ether x2 → Elixir
        (3, 1, 2, 0, 2, 0, 3),    # Antidote x2 → Herb x3
    ]
    c.executemany("INSERT INTO recipes VALUES(?,?,?,?,?,?,?)", recipes)

    # --- セットボーナス (Phase 2) ---
    set_bonuses = [
        # set_id, piece_count, stat_type, bonus
        # 鉄セット (set_id=1): IronShield + ChainMail
        (1, 2, 1, 10),  # 2個装備でDEF+10
        # ミスリルセット (set_id=2): MithShield + PlateMail + MagicAmulet
        (2, 2, 1, 15),  # 2個装備でDEF+15
        (2, 3, 1, 25),  # 3個装備でDEF+25 (2個ボーナスと合算)
        (2, 3, 3, 10),  # 3個装備でMAG+10
    ]
    c.executemany("INSERT INTO set_bonus VALUES(?,?,?,?)", set_bonuses)

    conn.commit()
    conn.close()
    print(f"Created {DB_PATH}")

if __name__ == '__main__':
    main()
