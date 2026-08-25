#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_ai_db.py — libos32ai 用サンプルデータベース生成スクリプト

game/build/db/ai.db に profiles テーブルを作成し、
4種類のAIプロファイル (慎重・攻撃的・ランダム・バランス) を挿入する。
"""

import sqlite3
import os
import sys

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'build', 'db', 'ai.db')

SCHEMA = """
CREATE TABLE IF NOT EXISTS profiles (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    p0_miss     INTEGER NOT NULL DEFAULT 10,
    p1_noise    INTEGER NOT NULL DEFAULT 3,
    p2          INTEGER NOT NULL DEFAULT 0,
    p3          INTEGER NOT NULL DEFAULT 0,
    p4          INTEGER NOT NULL DEFAULT 0,
    p5          INTEGER NOT NULL DEFAULT 0,
    p6          INTEGER NOT NULL DEFAULT 0,
    p7          INTEGER NOT NULL DEFAULT 0,
    p8          INTEGER NOT NULL DEFAULT 0,
    p9          INTEGER NOT NULL DEFAULT 0,
    p10         INTEGER NOT NULL DEFAULT 0,
    p11         INTEGER NOT NULL DEFAULT 0,
    p12         INTEGER NOT NULL DEFAULT 0,
    p13         INTEGER NOT NULL DEFAULT 0,
    p14         INTEGER NOT NULL DEFAULT 0,
    p15         INTEGER NOT NULL DEFAULT 0
);
"""

# プロファイルデータ
# (id, name, miss, noise, p2_aggro, p3_caution, p4_greed, p5..p15)
PROFILES = [
    # id=0: 慎重型 — ミス率低、ノイズ小、攻撃性低、慎重さ高
    (0, 'Careful',    5,  2,  20, 80, 30, 0,0,0,0,0,0,0,0,0,0,0),
    # id=1: 攻撃型 — ミス率低、ノイズ小、攻撃性高
    (1, 'Aggressive', 5,  3,  90, 20, 50, 0,0,0,0,0,0,0,0,0,0,0),
    # id=2: ランダム型 — ミス率高、ノイズ大
    (2, 'Random',    40, 10,  50, 50, 50, 0,0,0,0,0,0,0,0,0,0,0),
    # id=3: バランス型 — すべて中程度
    (3, 'Balanced',  10,  5,  50, 50, 50, 0,0,0,0,0,0,0,0,0,0,0),
]

def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)

    # 既存のDBがあれば削除して再生成
    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.executescript(SCHEMA)

    cur.executemany(
        "INSERT INTO profiles (id, name, p0_miss, p1_noise,"
        " p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        PROFILES
    )

    conn.commit()
    conn.close()

    print(f"Generated {DB_PATH} with {len(PROFILES)} profiles.")

if __name__ == '__main__':
    main()
