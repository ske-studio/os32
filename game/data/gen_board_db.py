#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_board_db.py — libos32board 用ボードDB生成スクリプト

2種類のDBを出し分ける:

  --game (既定)  assets/board.db      対戦スゴロクRPG の実マップ
                 オノコロ島(10マス) + 全8ステージ(各20マス) = 170マス。
                 ゲスト側 /db/board.db として game が読む。

  --test         assets/board_test.db libos32board のテスト用固定盤面
                 board_test.c が前提とする12マス構成 + 経路探索用の並列経路。
                 ゲスト側 /db/board_test.db として board_test が読む。

実マップは区画が9個あり board_test の期待する固定トポロジと両立しないため、
テスト用の盤面は別DBに分離している (実マップを board.db に入れると
board_test が成立しなくなる)。
"""

import sqlite3
import os
import sys

ASSET_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', 'assets')

# ==========================================================================
#  実マップ定義 (--game)
# ==========================================================================

# マス種別定数 (game.h 準拠)
MASS_EMPTY = 0
MASS_VILLAGE = 1
MASS_BATTLE = 2
MASS_TREASURE = 3
MASS_EQUIP_SHOP = 4
MASS_ITEM_SHOP = 5
MASS_MAGIC_SHOP = 6
MASS_CHURCH = 7
MASS_CIRCLE = 8
MASS_EVENT = 9
MASS_GATE = 10
MASS_CASTLE = 11
MASS_MAGIC_CHEST = 12

ONOKORO_SIZE = 10
ST_SIZE = 20

# 各ステージの村param開始値とステージごとの村数。
# param は econ.db の estates.id (村マスタ) を指す 1 始まりの村ID で、
# param=0 は「村マスタなし」を意味する。全ステージ合計で 1〜59 が連番になる。
VILLAGE_OFFSETS = [1, 8, 15, 22, 30, 38, 46, 53]
VILLAGES_PER_STAGE = [7, 7, 7, 8, 8, 8, 7, 7]

# 村が 8 個のステージで、追加の 1 村に転用する空地マスの位置
EXTRA_VILLAGE_SLOT = 3

ONOKORO_PATTERN = [
    MASS_CASTLE,      # 0: スタート地点
    MASS_EMPTY,       # 1: 空地
    MASS_TREASURE,    # 2: 宝箱
    MASS_EMPTY,       # 3: 空地
    MASS_ITEM_SHOP,   # 4: 道具屋
    MASS_EMPTY,       # 5: 空地
    MASS_TREASURE,    # 6: 宝箱
    MASS_CHURCH,      # 7: 神社
    MASS_EMPTY,       # 8: 空地
    MASS_GATE         # 9: 脱出ゲート
]

STAGE_PATTERN = [
    MASS_CASTLE,      # 0: 起点
    MASS_BATTLE,      # 1: モンスター遭遇
    MASS_VILLAGE,     # 2: 村0
    MASS_BATTLE,      # 3: モンスター遭遇 (村8個のステージでは村に転用される)
    MASS_TREASURE,    # 4
    MASS_VILLAGE,     # 5: 村1
    MASS_ITEM_SHOP,   # 6: 道具屋
    MASS_CHURCH,      # 7: 神社
    MASS_VILLAGE,     # 8: 村2
    MASS_EQUIP_SHOP,  # 9: 装備屋
    MASS_TREASURE,    # 10
    MASS_VILLAGE,     # 11: 村3
    MASS_GATE,        # 12: ゲートキーパー
    MASS_VILLAGE,     # 13: 村4
    MASS_CHURCH,      # 14: 神社
    MASS_MAGIC_SHOP,  # 15: 言霊屋
    MASS_VILLAGE,     # 16: 村5
    MASS_CIRCLE,      # 17: 魔法陣
    MASS_VILLAGE,     # 18: 村6
    MASS_MAGIC_CHEST  # 19: 黄宝箱
]


def create_schema(c, default_cost):
    c.execute('''CREATE TABLE masses (
        id      INTEGER PRIMARY KEY,
        type    INTEGER NOT NULL,
        area    INTEGER NOT NULL DEFAULT 0,
        param   INTEGER DEFAULT 0,
        cost    INTEGER DEFAULT {},
        flags   INTEGER DEFAULT 0,
        x       INTEGER DEFAULT 0,
        y       INTEGER DEFAULT 0,
        terrain INTEGER DEFAULT 0
    )'''.format(default_cost))

    c.execute('''CREATE TABLE connections (
        from_id       INTEGER NOT NULL,
        to_id         INTEGER NOT NULL,
        bidirectional INTEGER NOT NULL DEFAULT 1,
        PRIMARY KEY (from_id, to_id)
    )''')

    c.execute('''CREATE TABLE areas (
        id           INTEGER PRIMARY KEY,
        unlock_type  INTEGER NOT NULL DEFAULT 0,
        unlock_param INTEGER DEFAULT 0
    )''')


def build_game():
    """実マップ (170マス / 9区画) を組み立てて返す"""
    masses = []
    connections = []

    # === 1. マスデータ ===
    # (A) オノコロ島 (area 0)
    for i in range(ONOKORO_SIZE):
        masses.append((i, ONOKORO_PATTERN[i], 0, 0, 1, 0, i % 20, i // 20))

    # (B) ステージ 1-8 (area 1-8)
    for stage in range(8):
        base = ONOKORO_SIZE + stage * ST_SIZE
        v_base = VILLAGE_OFFSETS[stage]
        v_count = 0

        for i in range(ST_SIZE):
            idx = base + i
            mtype = STAGE_PATTERN[i]

            # ステージ2以降のマス0は空地に変更 (起点はステージ1のみ)
            if i == 0 and stage > 0:
                mtype = MASS_EMPTY

            # STAGE_PATTERN の村は 7 個しかないので、村 8 個のステージでは
            # 空地を 1 マス村に転用する
            if i == EXTRA_VILLAGE_SLOT and VILLAGES_PER_STAGE[stage] > 7:
                mtype = MASS_VILLAGE

            param = 0
            if mtype == MASS_VILLAGE and v_count < VILLAGES_PER_STAGE[stage]:
                param = v_base + v_count
                v_count += 1

            masses.append((idx, mtype, stage + 1, param, 1, 0,
                           idx % 20, idx // 20))

    # === 2. 接続データ ===
    # (A) オノコロ島内
    for i in range(ONOKORO_SIZE - 1):
        connections.append((i, i + 1, 1))

    # オノコロ島最終マス(9: GATE) -> 高天原(10) は一方通行
    connections.append((ONOKORO_SIZE - 1, ONOKORO_SIZE, 0))

    # (B) ステージ内およびステージ間
    for stage in range(8):
        base = ONOKORO_SIZE + stage * ST_SIZE

        for i in range(ST_SIZE):
            idx = base + i
            if i < ST_SIZE - 1:
                connections.append((idx, idx + 1, 1))
            elif stage < 7:
                connections.append((idx, base + ST_SIZE, 1))   # 次ステージへ
            else:
                connections.append((idx, ONOKORO_SIZE, 1))     # 高天原へ戻る

        # 分岐接続: 各ステージのマス5 -> マス9
        connections.append((base + 5, base + 9, 1))

    # === 3. 区画データ ===
    # 区画0 (オノコロ島) と 区画1 (ステージ1) は初期解放、
    # 区画2-8 はボス撃破で解放 (unlock_type=1, unlock_param=ボスID)
    areas = [(0, 0, 0), (1, 0, 0)]
    for stage in range(1, 8):
        areas.append((stage + 1, 1, stage))

    return masses, connections, areas


def build_test():
    """board_test.c が前提とする固定盤面を組み立てて返す

      [0]--[1]--[2(分岐)]--[3]--[4(ゴール)]
                     |
                    [5]--[6]--[7(ゴール2)]
      区画0: マス0-4 (初期解放) / 区画1: マス5-7 (ボス撃破で解放)

    追加テスト用:
      [8]--[9(一方通行)]-->[10]--[11]
      コスト付き並列経路: 20→21→22→25 (計3) vs 20→23→24→25 (計11)
    """
    # type: 0=通常, 1=イベント, 2=ショップ, 3=ゴール, 4=スタート
    masses = [
        # id, type, area, param, cost, flags, x, y
        (0,  4, 0, 0, 0, 0,   0,  0),   # スタート
        (1,  0, 0, 0, 0, 0,  50,  0),   # 通常
        (2,  1, 0, 0, 0, 0, 100,  0),   # イベント (分岐)
        (3,  2, 0, 0, 0, 0, 150,  0),   # ショップ
        (4,  3, 0, 0, 0, 0, 200,  0),   # ゴール
        (5,  0, 1, 0, 0, 0, 100, 50),   # 通常 (区画1)
        (6,  1, 1, 0, 0, 0, 150, 50),   # イベント (区画1)
        (7,  3, 1, 0, 0, 0, 200, 50),   # ゴール2 (区画1)
        (8,  0, 0, 0, 0, 0,   0, 100),  # 孤立経路
        (9,  0, 0, 0, 1, 0,  50, 100),  # コスト1 (一方通行テスト用)
        (10, 0, 0, 0, 0, 0, 100, 100),  # 通常
        (11, 0, 0, 0, 0, 0, 150, 100),  # 通常 (末端)
        (20, 0, 0, 0, 0, 0,   0, 200),  # 分岐起点
        (21, 0, 0, 0, 1, 0,  50, 200),  # 安い経路 (cost=1)
        (22, 0, 0, 0, 1, 0, 100, 200),  # 安い経路 (cost=1)
        (23, 0, 0, 0, 5, 0,  50, 250),  # 高い経路 (cost=5)
        (24, 0, 0, 0, 5, 0, 100, 250),  # 高い経路 (cost=5)
        (25, 3, 0, 0, 0, 0, 150, 225),  # 合流ゴール
    ]

    connections = [
        # from, to, bidirectional
        (0, 1, 1),
        (1, 2, 1),
        (2, 3, 1),   # 分岐の一方
        (2, 5, 1),   # 分岐の他方
        (3, 4, 1),
        (5, 6, 1),
        (6, 7, 1),
        (8, 9, 1),
        (9, 10, 0),  # 一方通行
        (10, 11, 1),
        (20, 21, 1), (21, 22, 1), (22, 25, 1),   # 安い経路
        (20, 23, 1), (23, 24, 1), (24, 25, 1),   # 高い経路
    ]

    areas = [
        (0, 0, 0),   # 区画0: 初期解放
        (1, 1, 1),   # 区画1: ボス撃破(ボスID=1)で解放
    ]

    return masses, connections, areas


def generate(db_name, builder, default_cost):
    db_path = os.path.join(ASSET_DIR, db_name)
    os.makedirs(ASSET_DIR, exist_ok=True)
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    c = conn.cursor()
    create_schema(c, default_cost)

    masses, connections, areas = builder()
    c.executemany('INSERT INTO masses (id,type,area,param,cost,flags,x,y)'
                  ' VALUES (?,?,?,?,?,?,?,?)', masses)
    c.executemany('INSERT INTO connections VALUES (?,?,?)', connections)
    c.executemany('INSERT INTO areas VALUES (?,?,?)', areas)

    conn.commit()
    conn.close()

    print("Generated: {}".format(db_path))
    print("  {} masses, {} connections, {} areas".format(
        len(masses), len(connections), len(areas)))


def main():
    mode = 'game'
    for arg in sys.argv[1:]:
        if arg == '--test':
            mode = 'test'
        elif arg == '--game':
            mode = 'game'
        elif arg == '--all':
            mode = 'all'
        else:
            print("Usage: gen_board_db.py [--game | --test | --all]",
                  file=sys.stderr)
            return 1

    if mode in ('game', 'all'):
        generate('board.db', build_game, 1)
    if mode in ('test', 'all'):
        generate('board_test.db', build_test, 0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
