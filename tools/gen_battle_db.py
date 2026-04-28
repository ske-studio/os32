#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_battle_db.py — libos32battle 用サンプルデータベース生成スクリプト

assets/battle.db に以下のテーブルを作成:
  - command_matrix: コマンドマトリクス (攻撃/防御コマンドの組み合わせ結果)
  - status_effects: 状態異常定義 (毒, 麻痺, 眠り, 混乱, 盲目)
  - element_chart:  属性相性テーブル (火→氷, 氷→火, 火→火, 雷→水 等)
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'assets', 'battle.db')

SCHEMA = """
/* コマンドマトリクス */
CREATE TABLE IF NOT EXISTS command_matrix (
    atk_cmd     INTEGER NOT NULL,
    def_cmd     INTEGER NOT NULL,
    result_type INTEGER NOT NULL,
    PRIMARY KEY (atk_cmd, def_cmd)
);

/* 状態異常定義 */
CREATE TABLE IF NOT EXISTS status_effects (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    bit_flag        INTEGER NOT NULL,
    duration        INTEGER DEFAULT 0,
    tick_damage     INTEGER DEFAULT 0,
    prevents_action INTEGER DEFAULT 0
);

/* 属性相性テーブル */
CREATE TABLE IF NOT EXISTS element_chart (
    elem_atk    INTEGER NOT NULL,
    elem_def    INTEGER NOT NULL,
    multiplier  INTEGER NOT NULL DEFAULT 256,
    PRIMARY KEY (elem_atk, elem_def)
);
"""

# BTL_RES_* 定数
RES_NORMAL  = 0
RES_GUARD   = 1
RES_NOGUARD = 2
RES_COUNTER = 3
RES_MISS    = 4
RES_REFLECT = 5
RES_VULN    = 6
RES_YIELD   = 7

# コマンドID:
#   0 = 攻撃
#   1 = 強攻撃
#   2 = 魔法
#   3 = ためる
#   4 = 逃走
#
# 防御コマンドID:
#   0 = なし (無防備)
#   1 = 防御
#   2 = 回避
#   3 = 反射

COMMAND_MATRIX = [
    # (atk_cmd, def_cmd, result_type)
    # atk=0 (攻撃)
    (0, 0, RES_NORMAL),   # 攻撃 vs 無防備 → 通常
    (0, 1, RES_GUARD),    # 攻撃 vs 防御   → ガード
    (0, 2, RES_NORMAL),   # 攻撃 vs 回避   → 通常 (回避率で判定)
    (0, 3, RES_REFLECT),  # 攻撃 vs 反射   → 反射

    # atk=1 (強攻撃)
    (1, 0, RES_NOGUARD),  # 強攻撃 vs 無防備 → 防御無視
    (1, 1, RES_NORMAL),   # 強攻撃 vs 防御   → ガード貫通→通常
    (1, 2, RES_NORMAL),   # 強攻撃 vs 回避   → 通常
    (1, 3, RES_COUNTER),  # 強攻撃 vs 反射   → カウンター

    # atk=2 (魔法)
    (2, 0, RES_VULN),     # 魔法 vs 無防備 → 弱点
    (2, 1, RES_NORMAL),   # 魔法 vs 防御   → 通常 (物理防御は魔法に効きにくい)
    (2, 2, RES_NORMAL),   # 魔法 vs 回避   → 通常 (魔法は回避しづらい)
    (2, 3, RES_REFLECT),  # 魔法 vs 反射   → 反射

    # atk=3 (ためる)
    (3, 0, RES_MISS),     # ためる vs 無防備 → ミス (ダメージなし)
    (3, 1, RES_MISS),     # ためる vs 防御   → ミス
    (3, 2, RES_MISS),     # ためる vs 回避   → ミス
    (3, 3, RES_MISS),     # ためる vs 反射   → ミス

    # atk=4 (逃走)
    (4, 0, RES_YIELD),    # 逃走 vs 無防備 → 逃走判定
    (4, 1, RES_YIELD),    # 逃走 vs 防御   → 逃走判定
    (4, 2, RES_YIELD),    # 逃走 vs 回避   → 逃走判定
    (4, 3, RES_YIELD),    # 逃走 vs 反射   → 逃走判定
]

# 状態異常定義
# (id, name, bit_flag, duration, tick_damage, prevents_action)
STATUS_EFFECTS = [
    (0, 'Poison',  1,  0, 2, 0),   # 毒: 毎ターン2ダメージ, 永続
    (1, 'Paralyze', 2, 3, 0, 1),   # 麻痺: 3ターン行動不能, 自然回復
    (2, 'Sleep',   4,  2, 0, 1),   # 眠り: 2ターン行動不能, 自然回復
    (3, 'Confuse', 8,  4, 0, 0),   # 混乱: 4ターン, 行動ランダム化
    (4, 'Blind',   16, 5, 0, 0),   # 盲目: 5ターン, 命中率低下
]

# 属性相性テーブル
# 属性ビットフラグ: 1=火, 2=氷, 4=雷, 8=水, 16=風, 32=地
ELEMENT_CHART = [
    # (elem_atk, elem_def, multiplier)
    (1,  2,  512),    # 火 → 氷: 2倍
    (2,  1,  512),    # 氷 → 火: 2倍
    (4,  8,  512),    # 雷 → 水: 2倍
    (8,  4,  128),    # 水 → 雷: 半減
    (16, 32, 384),    # 風 → 地: 1.5倍
    (32, 16, 384),    # 地 → 風: 1.5倍
    (1,  1,  128),    # 火 → 火: 半減 (同属性は半減)
    (2,  2,  128),    # 氷 → 氷: 半減
    (4,  4,  128),    # 雷 → 雷: 半減
    (8,  8,  128),    # 水 → 水: 半減
]


def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)

    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.executescript(SCHEMA)

    cur.executemany(
        "INSERT INTO command_matrix (atk_cmd, def_cmd, result_type)"
        " VALUES (?,?,?)",
        COMMAND_MATRIX
    )

    cur.executemany(
        "INSERT INTO status_effects (id, name, bit_flag, duration,"
        " tick_damage, prevents_action) VALUES (?,?,?,?,?,?)",
        STATUS_EFFECTS
    )

    cur.executemany(
        "INSERT INTO element_chart (elem_atk, elem_def, multiplier)"
        " VALUES (?,?,?)",
        ELEMENT_CHART
    )

    conn.commit()
    conn.close()

    print(f"Generated {DB_PATH}")
    print(f"  command_matrix: {len(COMMAND_MATRIX)} entries")
    print(f"  status_effects: {len(STATUS_EFFECTS)} entries")
    print(f"  element_chart:  {len(ELEMENT_CHART)} entries")


if __name__ == '__main__':
    main()
