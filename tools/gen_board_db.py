#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_board_db.py — libos32board用 サンプルボードDB生成スクリプト

12マスのスゴロク型テストボードを生成する。
マス配置:
  [0]--[1]--[2(分岐)]--[3]--[4(ゴール)]
                 |
                [5]--[6]--[7(ゴール2)]
  区画0: マス0-4  (初期解放)
  区画1: マス5-7  (ボス撃破で解放)

追加テスト用:
  [8]--[9(一方通行)]-->[10]--[11]
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'assets', 'board.db')

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
        cost    INTEGER DEFAULT 0,
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

    # マスデータ (type: 0=通常, 1=イベント, 2=ショップ, 3=ゴール, 4=スタート)
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
        # --- コスト付き経路探索テスト用 ---
        # 並列経路: 20→21→22→25 (コスト: 1+1+1=3) vs 20→23→24→25 (コスト: 5+5+1=11)
        (20, 0, 0, 0, 0, 0,   0, 200),  # 分岐起点
        (21, 0, 0, 0, 1, 0,  50, 200),  # 安い経路 (cost=1)
        (22, 0, 0, 0, 1, 0, 100, 200),  # 安い経路 (cost=1)
        (23, 0, 0, 0, 5, 0,  50, 250),  # 高い経路 (cost=5)
        (24, 0, 0, 0, 5, 0, 100, 250),  # 高い経路 (cost=5)
        (25, 3, 0, 0, 0, 0, 150, 225),  # 合流ゴール
    ]
    c.executemany(
        'INSERT INTO masses VALUES (?,?,?,?,?,?,?,?)', masses)

    # 接続データ
    connections = [
        # from, to, bidirectional
        (0, 1, 1),   # 0 <-> 1
        (1, 2, 1),   # 1 <-> 2
        (2, 3, 1),   # 2 <-> 3 (分岐の一方)
        (2, 5, 1),   # 2 <-> 5 (分岐の他方)
        (3, 4, 1),   # 3 <-> 4
        (5, 6, 1),   # 5 <-> 6
        (6, 7, 1),   # 6 <-> 7
        (8, 9, 1),   # 8 <-> 9
        (9, 10, 0),  # 9 -> 10 (一方通行)
        (10, 11, 1), # 10 <-> 11
        # コスト付き並列経路
        (20, 21, 1), # 20 <-> 21 (安い経路)
        (21, 22, 1), # 21 <-> 22
        (22, 25, 1), # 22 <-> 25
        (20, 23, 1), # 20 <-> 23 (高い経路)
        (23, 24, 1), # 23 <-> 24
        (24, 25, 1), # 24 <-> 25
    ]
    c.executemany(
        'INSERT INTO connections VALUES (?,?,?)', connections)

    # 区画データ
    areas = [
        (0, 0, 0),   # 区画0: 初期解放
        (1, 1, 1),   # 区画1: ボス撃破(ボスID=1)で解放
    ]
    c.executemany(
        'INSERT INTO areas VALUES (?,?,?)', areas)

    conn.commit()
    conn.close()
    print(f"Generated: {DB_PATH}")
    print(f"  {len(masses)} masses, {len(connections)} connections, {len(areas)} areas")

if __name__ == '__main__':
    main()
