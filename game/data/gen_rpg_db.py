#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_rpg_db.py — libos32rpg 用データベース生成スクリプト

game/build/db/rpg.db に以下のテーブルを作成:
  - exp_curve: 累計必要経験値テーブル (1〜99)
  - level_growth: 職業/氏神別の成長パラメーターおよび配分ポイント数
  - status_field: フィールド上の状態異常定義 (毒, 麻痺, 眠り, 呪い, 臆病等)
  - reborn_table: 順位に応じた復活所要ターン数 (最少・最大)
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'build', 'db', 'rpg.db')

SCHEMA = """
/* 経験値曲線 */
CREATE TABLE IF NOT EXISTS exp_curve (
    level     INTEGER PRIMARY KEY,
    total_exp INTEGER NOT NULL
);

/* レベルアップ成長定義 */
CREATE TABLE IF NOT EXISTS level_growth (
    class_id     INTEGER PRIMARY KEY,
    atk          INTEGER NOT NULL,
    def          INTEGER NOT NULL,
    spd          INTEGER NOT NULL,
    mag          INTEGER NOT NULL,
    hp           INTEGER NOT NULL,
    free_points  INTEGER NOT NULL
);

/* フィールド状態異常定義 */
CREATE TABLE IF NOT EXISTS status_field (
    bit_flag        INTEGER PRIMARY KEY,
    prevents_action INTEGER NOT NULL DEFAULT 0,  /* 1=行動不能 */
    tick_kind       INTEGER NOT NULL DEFAULT 0,  /* 0=なし, 1=固定, 2=Lv比例, 3=最大HP% */
    tick_value      INTEGER NOT NULL DEFAULT 0,  /* ダメージ基本値 */
    recovery_pct    INTEGER NOT NULL DEFAULT 0,  /* 毎ターン自然回復確率(%) */
    lethal          INTEGER NOT NULL DEFAULT 0   /* 0=HP1で停止, 1=死亡あり */
);

/* 順位別リボーン待機ターン定義 */
CREATE TABLE IF NOT EXISTS reborn_table (
    rank_bucket INTEGER PRIMARY KEY,            /* 順位(1起点) */
    min_turns   INTEGER NOT NULL,                /* 最小待機ターン */
    max_turns   INTEGER NOT NULL                 /* 最大待機ターン(確定復活) */
);
"""

# 成長率初期データ (氏神ボーナス+2、および自由配分4pt)
# UJI_SUSANOO=0, UJI_YAMATOTAKERU=1, UJI_OKUNINUSHI=2, UJI_AMATERASU=3, UJI_TSUKUYOMI=4
LEVEL_GROWTH = [
    # (class_id, atk, def, spd, mag, hp, free_points)
    (0, 2, 0, 0, 0,  0, 4),  # スサノオ: ATK+2
    (1, 0, 2, 0, 0,  0, 4),  # ヤマトタケル: DEF+2
    (2, 0, 0, 2, 0,  0, 4),  # オオクニヌシ: SPD+2
    (3, 0, 0, 0, 2,  0, 4),  # アマテラス: MAG+2
    (4, 0, 0, 0, 0,  0, 4),  # ツクヨミ: コード側でランダム+2
]

# 状態異常定義初期データ (bit_flag, prevents_action, tick_kind, tick_value, recovery_pct, lethal)
# 状態異常ビット: 1=毒, 2=麻痺, 4=眠り, 8=混乱, 16=盲目, 32=呪い, 64=臆病
STATUS_FIELD = [
    (1,  0, 2,  1,  0, 0),  # 毒 (POISON): Lv×1ダメージ, 確率回復なし, 非致死
    (2,  1, 0,  0, 50, 0),  # 麻痺 (PARALYZE): 行動不能, 回復50%
    (4,  1, 0,  0, 33, 0),  # 眠り (SLEEP): 行動不能, 回復33%
    (8,  0, 0,  0, 25, 0),  # 混乱 (CONFUSE): 行動ランダム化(フィールドは無関係), 回復25%
    (16, 0, 0,  0, 20, 0),  # 盲目 (BLIND): 命中率低下(フィールドは無関係), 回復20%
    (32, 0, 3, 15,  0, 0),  # 呪い (CURSE): 最大HP15%ダメージ(コード側で1/4化), 非致死
    (64, 0, 0,  0, 33, 0),  # 臆病 (COWARD): 毎ターン33%で回復
]

# リボーン待機ターンデータ
REBORN_TABLE = [
    # (rank_bucket, min_turns, max_turns)
    (1, 6, 10), # 1位: min 6, max 10
    (2, 4,  7), # 2位: min 4, max 7
    (3, 3,  5), # 3位: min 3, max 5
    (4, 2,  4), # 4位/最下位: min 2, max 4
]


def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)

    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.executescript(SCHEMA)

    # 1. exp_curve 生成 (1〜99)
    exp_curve_data = []
    for lv in range(1, 100):
        total_exp = lv * lv * 10
        exp_curve_data.append((lv, total_exp))

    cur.executemany(
        "INSERT INTO exp_curve (level, total_exp) VALUES (?,?)",
        exp_curve_data
    )

    # 2. level_growth 挿入
    cur.executemany(
        "INSERT INTO level_growth (class_id, atk, def, spd, mag, hp, free_points) VALUES (?,?,?,?,?,?,?)",
        LEVEL_GROWTH
    )

    # 3. status_field 挿入
    cur.executemany(
        "INSERT INTO status_field (bit_flag, prevents_action, tick_kind, tick_value, recovery_pct, lethal) VALUES (?,?,?,?,?,?)",
        STATUS_FIELD
    )

    # 4. reborn_table 挿入
    cur.executemany(
        "INSERT INTO reborn_table (rank_bucket, min_turns, max_turns) VALUES (?,?,?)",
        REBORN_TABLE
    )

    conn.commit()
    conn.close()

    print(f"Generated {DB_PATH}")
    print(f"  exp_curve: {len(exp_curve_data)} entries")
    print(f"  level_growth: {len(LEVEL_GROWTH)} entries")
    print(f"  status_field: {len(STATUS_FIELD)} entries")
    print(f"  reborn_table: {len(REBORN_TABLE)} entries")


if __name__ == '__main__':
    main()
