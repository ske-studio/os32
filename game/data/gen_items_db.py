#!/usr/bin/env python3
"""items.db 生成スクリプト — libos32inv テスト用サンプルデータ (Phase 1+2)"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(__file__), '..', 'build', 'db', 'items.db')

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
        (1,  '薬草',       0, 1, 30, 1, 0, 1, 0,  0, 1, 10,   0, 0),
        (2,  '毒消し',   0, 2,  0, 1, 0, 1, 0,  0, 1, 15,   0, 0),
        (3,  '万能薬',     0, 3, 99, 3, 0, 1, 0,  0, 3, 500,  0, 0),
        (4,  '霊気の水',      0, 4, 50, 2, 0, 1, 0,  0, 2, 200,  0, 0),
        (5,  '不死鳥の羽',    0, 5,  0, 4, 0, 1, 0,  0, 4, 1000, 0, 0),
        # 素材アイテム (スタック可能)
        (6,  '鉄鉱石',    0, 0,  0, 1, 0, 1, 0,  0, 1, 20,   0, 0),
        (7,  '銀鉱石',    0, 0,  0, 2, 0, 1, 0,  0, 2, 80,   0, 0),
        (8,  '龍の牙', 0, 0,  0, 3, 0, 1, 0,  0, 3, 300,  0, 0),
        # 武器 (耐久度あり)
        (10, '小太刀', 1, 0, 0, 1, 0, 0, 10, 0, 1, 100,  0, 30),
        (11, '太刀',  1, 0, 0, 2, 0, 0, 25, 0, 2, 300,  0, 50),
        (12, '呪杖',   1, 0, 0, 3, 0, 0, 15, 3, 2, 250,  0, 40),
        (13, '大斧',   1, 0, 0, 3, 0, 0, 35, 0, 3, 500,  0, 60),
        (14, '神槍',  1, 0, 0, 4, 0, 0, 50, 0, 4, 1200, 0, 255),
        # 盾 (耐久度あり, IronShield + ChainMail = 鉄セット set_id=1)
        (20, '木盾', 2, 0, 0, 1, 1, 0, 5,  1, 1, 50,   0, 20),
        (21, '鉄盾', 2, 0, 0, 2, 1, 0, 15, 1, 2, 200,  1, 40),
        (22, '銀盾', 2, 0, 0, 3, 1, 0, 30, 1, 3, 600,  2, 80),
        # 鎧 (耐久度あり, ChainMail = 鉄セット set_id=1)
        (30, '革鎧',    3, 0, 0, 1, 2, 0, 8,  1, 1, 80,   0, 25),
        (31, '鎖帷子',  3, 0, 0, 2, 2, 0, 20, 1, 2, 350,  1, 50),
        (32, '板金鎧',  3, 0, 0, 3, 2, 0, 40, 1, 3, 800,  2, 100),
        # アクセサリ (装備スロット3=ACCESSORY, ミスリルセット set_id=2)
        # ゲーム側の装備スロットは 0..3 の4つなので、指輪も ACCESSORY に置く
        (40, '疾風の指輪',  4, 0, 0, 2, 3, 0, 10, 2, 1, 150,  0, 255),
        (41, '剛力の指輪',  4, 0, 0, 2, 3, 0, 8,  0, 2, 200,  0, 255),
        (42, '呪符',4, 0, 0, 3, 3, 0, 12, 3, 2, 300,  2, 255),
        # 合成品
        (50, '鋼の太刀', 1, 0, 0, 2, 0, 0, 20, 0, 2, 250,  0, 40),

        # --- デビル (イザナミ) 専用装備 ---
        # 変身中だけ強制装備される。店にも抽選にも出さない (shop_lineup /
        # lottery_tables に載せない)。数値は変身仕様の定数をそのまま移した:
        # ATK+86 / 盾DEF+43 + 鎧DEF+62 = DEF+105。
        # 耐久度は無限 (0xFF=255) — 変身中に壊れては困る。
        (90, '天沼矛',   1, 0, 0, 5, 0, 0, 86, 0, 1, 0, 0, 255),
        (91, '黄泉の盾', 2, 0, 0, 5, 1, 0, 43, 1, 1, 0, 0, 255),
        (92, '黄泉の鎧', 3, 0, 0, 5, 2, 0, 62, 1, 1, 0, 0, 255),

        # --- 言霊 (魔法アイテム) ---
        # 消耗品 (type=0) だが effect が 10 番台。戦闘の「魔法」コマンドで
        # 選んで唱える。param = 威力/回復量、stat_bonus = 付与する状態異常
        # (KOTODAMA_CURSE のみ)、stat_type は未使用。
        #   effect 10 = 攻撃言霊 (param + 術者MAG のダメージ)
        #   effect 11 = 治癒言霊 (param HP 回復)
        #   effect 12 = 呪縛言霊 (stat_bonus の状態異常を付与)
        (60, '火之迦具土', 0, 10, 30, 2, 0, 1, 0,  0, 1, 120, 0, 0),
        (61, '建御雷',  0, 10, 55, 3, 0, 1, 0,  0, 2, 320, 0, 0),
        (62, '天叢雲',  0, 10, 90, 4, 0, 1, 0,  0, 3, 700, 0, 0),
        (63, '少名毘古那',   0, 11, 40, 2, 0, 1, 0,  0, 1, 100, 0, 0),
        (64, '大国主',    0, 11, 90, 3, 0, 1, 0,  0, 3, 400, 0, 0),
        # 呪縛: stat_bonus = BTL_STATUS_* のビット (1=毒, 2=麻痺)
        (65, '黄泉比良坂',0, 12,  0, 3, 0, 1, 1,  0, 2, 260, 0, 0),
        (66, '石凝姥',   0, 12,  0, 4, 0, 1, 2,  0, 3, 500, 0, 0),
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
        (1, 2, 4), (1, 2, 1), (1, 2, 2),      # Stage 2
        (1, 3, 3), (1, 3, 1), (1, 3, 4),      # Stage 3
        (1, 4, 5), (1, 4, 3),                 # Stage 4
        # 言霊屋 (type=2)
        (2, 1, 60), (2, 1, 63),               # Stage 1
        (2, 2, 61), (2, 2, 65), (2, 2, 63),   # Stage 2
        (2, 3, 62), (2, 3, 64), (2, 3, 66),   # Stage 3
        (2, 4, 62), (2, 4, 64), (2, 4, 66),   # Stage 4
        # 装備屋 (type=0) — アクセサリと Stage 4
        (0, 1, 40), (0, 2, 41), (0, 2, 42),
        (0, 4, 14), (0, 4, 22), (0, 4, 32),
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
