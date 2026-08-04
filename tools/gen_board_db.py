#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_board_db.py — 対戦スゴロクRPG 実ゲーム用ボードDB生成スクリプト

オノコロ島（10マス）および全8ステージ（各20マス）の計170マスのマップ情報をSQLite DBに投入する。
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'assets', 'board.db')

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

# 各ステージの村param開始値とステージごとの村数
VILLAGE_OFFSETS = [0, 7, 14, 21, 29, 37, 45, 52]
VILLAGES_PER_STAGE = [7, 7, 7, 8, 8, 8, 7, 7]

# テンプレートパターン
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
    MASS_EMPTY,       # 1
    MASS_VILLAGE,     # 2: 村0
    MASS_EMPTY,       # 3
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

def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    # テーブル作成
    c.execute('''CREATE TABLE masses (
        id      INTEGER PRIMARY KEY,
        type    INTEGER NOT NULL,
        area    INTEGER NOT NULL DEFAULT 0,
        param   INTEGER DEFAULT 0,
        cost    INTEGER DEFAULT 1,
        flags   INTEGER DEFAULT 0,
        x       INTEGER DEFAULT 0,
        y       INTEGER DEFAULT 0
    )''')

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

    masses = []
    connections = []

    # === 1. マスデータの生成 ===
    # (A) オノコロ島
    for i in range(ONOKORO_SIZE):
        x = i % 20
        y = i // 20
        masses.append((i, ONOKORO_PATTERN[i], 0, 0, 1, 0, x, y))

    # (B) ステージ 1-8 (area 1-8)
    for stage in range(8):
        base = ONOKORO_SIZE + stage * ST_SIZE
        v_base = VILLAGE_OFFSETS[stage]
        v_count = 0

        for i in range(ST_SIZE):
            idx = base + i
            mtype = STAGE_PATTERN[i]
            
            # ステージ2以降のマス0は空地に変更
            if i == 0 and stage > 0:
                mtype = MASS_EMPTY

            param = 0
            if mtype == MASS_VILLAGE:
                if v_count < VILLAGES_PER_STAGE[stage]:
                    param = v_base + v_count
                    v_count += 1

            x = idx % 20
            y = idx // 20
            # area = stage + 1
            masses.append((idx, mtype, stage + 1, param, 1, 0, x, y))

    c.executemany('INSERT INTO masses VALUES (?,?,?,?,?,?,?,?)', masses)

    # === 2. 接続データの生成 ===
    # (A) オノコロ島内
    for i in range(ONOKORO_SIZE - 1):
        connections.append((i, i + 1, 1)) # 双方向

    # オノコロ島最終マス(9: GATE) -> 高天原(10) (一方通行)
    connections.append((ONOKORO_SIZE - 1, ONOKORO_SIZE, 0))

    # (B) ステージ 1-8 内およびステージ間
    for stage in range(8):
        base = ONOKORO_SIZE + stage * ST_SIZE

        for i in range(ST_SIZE):
            idx = base + i

            # 基本的な次のマスへの接続
            if i < ST_SIZE - 1:
                connections.append((idx, idx + 1, 1))
            else:
                # ステージの最終マス
                if stage < 7:
                    # 次のステージの最初へ
                    connections.append((idx, base + ST_SIZE, 1))
                else:
                    # ステージ8の最終マス -> 高天原(10)へ戻る
                    connections.append((idx, ONOKORO_SIZE, 1))

        # 分岐接続: 各ステージのマス5 -> マス9
        if base + 5 < ONOKORO_SIZE + ST_SIZE * 8:
            connections.append((base + 5, base + 9, 1))

    c.executemany('INSERT INTO connections VALUES (?,?,?)', connections)

    # === 3. エリア(区画)データの生成 ===
    # エリア 0 (オノコロ島): 初期解放
    # エリア 1 (ステージ1): 初期解放
    # エリア 2-8 (ステージ2-8): ボス撃破で解放 (unlock_type=1: ボス撃破, unlock_param=ボスID)
    areas = [
        (0, 0, 0), # エリア0
        (1, 0, 0), # エリア1
    ]
    for stage in range(1, 8):
        areas.append((stage + 1, 1, stage)) # unlock_paramはボスID (暫定でstage値: 1-7)

    c.executemany('INSERT INTO areas VALUES (?,?,?)', areas)

    conn.commit()
    conn.close()

    print(f"Generated: {DB_PATH}")
    print(f"  {len(masses)} masses, {len(connections)} connections, {len(areas)} areas")

if __name__ == '__main__':
    main()
