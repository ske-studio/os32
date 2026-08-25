#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_battle_db.py — libos32battle 用サンプルデータベース生成スクリプト

game/build/db/battle.db に以下のテーブルを作成:
  - command_matrix: コマンドマトリクス (攻撃/防御コマンドの組み合わせ結果)
  - status_effects: 状態異常定義 (毒, 麻痺, 眠り, 混乱, 盲目)
  - element_chart:  属性相性テーブル (火→氷, 氷→火, 火→火, 雷→水 等)
  - enemies:        敵マスタ (対戦スゴロクRPG 用: 野生44体 + ボス8体)

前3テーブルは libos32battle が btl_init() で読む汎用データ。
enemies はゲーム固有なので libos32battle は関知せず、
programs/apps/game の game_glue.c が直接読む。
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'build', 'db', 'battle.db')

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

/* 敵マスタ (ゲーム固有。libos32battle は読まない) */
CREATE TABLE IF NOT EXISTS enemies (
    id       INTEGER PRIMARY KEY,
    name     TEXT NOT NULL,
    stage    INTEGER NOT NULL DEFAULT 1,   /* 出現ステージ 1-8 (board の area に対応) */
    kind     INTEGER NOT NULL DEFAULT 0,   /* 0=野生, 1=ボス */
    max_hp   INTEGER NOT NULL,
    atk      INTEGER NOT NULL,
    def      INTEGER NOT NULL,
    spd      INTEGER NOT NULL,
    mag      INTEGER NOT NULL,
    elements INTEGER NOT NULL DEFAULT 0,   /* 属性ビット (element_chart と同じ割当) */
    exp      INTEGER NOT NULL,
    gold     INTEGER NOT NULL,
    class_id INTEGER NOT NULL DEFAULT 1
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


# ==========================================================================
#  敵マスタ
#
#  DOS 版のオリジナル敵データ (sample/game/enemy_data.c) は本リポジトリに
#  存在しないため、以下は記紀・妖怪の名を借りた合成データ。
#  ステージ 1-8 の野生敵 44 体 + 各ステージのボス 8 体 = 52 体。
#
#  ID 割り当て:
#    1〜44   野生敵 (ステージ順に連番)
#    101〜108 ボス (101 = ステージ1 のボス)
#  ボスIDを別レンジにしておくと、game 側が stage から boss を
#  100 + stage で直に引ける。
# ==========================================================================

# ステージごとの野生敵名。ステージ 1-4 が 6 体、5-8 が 5 体で計 44 体。
WILD_NAMES = [
    ['ねずみ',     'こだま',     'きつね',     'たぬき',     'からす',   'むかで'],
    ['鬼火',       '河童',       '小天狗',     '山犬',       '牛鬼',     'なまず'],
    ['雪女',       'ろくろ首',   '鵺',         '海坊主',     '犬神',     '人魂'],
    ['小鬼',       'がしゃ髑髏', '絡新婦',     'かまいたち', 'ばさん',   '雷獣'],
    ['ぬらりひょん', '大天狗',   '山童',       '鬼女',       '天邪鬼'],
    ['土蜘蛛',     '輪入道',     'おとろし',   '般若',       'なまはげ'],
    ['山姥',       '髪切り',     '煙々羅',     '船幽霊',     '怨霊'],
    ['龍神',       '風神',       '雷神',       '迦具土',     '八咫烏'],
]

# ステージごとのボス名 (index 0 = ステージ1)
BOSS_NAMES = [
    '赤鬼',
    '青鬼',
    '鵺の君',
    '酒呑童子',
    '茨木童子',
    '土蜘蛛王',
    '八岐大蛇',
    '伊邪那美',
]

# ステージごとの野生敵の属性ビット (1=火, 2=氷, 4=雷, 8=水, 16=風, 32=地)
# 名前と同じ並び。0 = 無属性。
WILD_ELEMENTS = [
    [0,  32, 1,  0,  16, 32],
    [1,  8,  16, 0,  32, 8],
    [2,  0,  16, 8,  32, 1],
    [32, 0,  2,  16, 16, 4],
    [0,  16, 32, 1,  0],
    [32, 1,  32, 2,  1],
    [2,  16, 1,  8,  0],
    [8,  16, 4,  1,  16],
]

# ステージごとのボス属性
BOSS_ELEMENTS = [1, 2, 16, 1, 32, 32, 8, 4]


def enemy_stats(stage, slot, is_boss):
    """ステージとスロットからステータスを算出する。

    ステージが上がるごとに直線的に強くなり、同一ステージ内では
    slot が後ろほど少しだけ強い (雑魚の中の当たり外れ)。
    ボスはその係数を上乗せする。
    """
    max_hp = 24 + stage * 14 + slot * 4
    atk    = 6 + stage * 3 + slot
    dfn    = 4 + stage * 3 + slot
    spd    = 6 + stage * 2 + slot
    mag    = 3 + stage * 2 + slot

    exp    = stage * stage * 8 + slot * 3 + 4
    gold   = stage * 40 + slot * 10 + 20

    if is_boss:
        max_hp = max_hp * 5 // 2
        atk    = atk * 8 // 5
        dfn    = dfn * 8 // 5
        spd    = spd * 6 // 5
        mag    = mag * 8 // 5
        exp    = stage * stage * 40 + 100
        gold   = stage * 300 + 200

    return max_hp, atk, dfn, spd, mag, exp, gold


def build_enemies():
    """敵マスタ行を (id, name, stage, kind, max_hp, atk, def, spd, mag,
    elements, exp, gold, class_id) のタプルで返す"""
    rows = []

    eid = 1
    for stage_idx, names in enumerate(WILD_NAMES):
        stage = stage_idx + 1
        elems = WILD_ELEMENTS[stage_idx]
        if len(elems) != len(names):
            raise ValueError(
                "stage {}: 名前 {} 件に対し属性 {} 件".format(
                    stage, len(names), len(elems)))
        for slot, name in enumerate(names):
            hp, atk, dfn, spd, mag, exp, gold = enemy_stats(stage, slot, False)
            rows.append((eid, name, stage, 0, hp, atk, dfn, spd, mag,
                         elems[slot], exp, gold, 1))
            eid += 1

    for stage_idx, name in enumerate(BOSS_NAMES):
        stage = stage_idx + 1
        hp, atk, dfn, spd, mag, exp, gold = enemy_stats(stage, 5, True)
        rows.append((100 + stage, name, stage, 1, hp, atk, dfn, spd, mag,
                     BOSS_ELEMENTS[stage_idx], exp, gold, 2))

    return rows


ENEMIES = build_enemies()


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

    cur.executemany(
        "INSERT INTO enemies (id, name, stage, kind, max_hp, atk, def,"
        " spd, mag, elements, exp, gold, class_id)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
        ENEMIES
    )

    conn.commit()
    conn.close()

    wild = sum(1 for e in ENEMIES if e[3] == 0)
    boss = len(ENEMIES) - wild

    print(f"Generated {DB_PATH}")
    print(f"  command_matrix: {len(COMMAND_MATRIX)} entries")
    print(f"  status_effects: {len(STATUS_EFFECTS)} entries")
    print(f"  element_chart:  {len(ELEMENT_CHART)} entries")
    print(f"  enemies:        {len(ENEMIES)} entries "
          f"(wild {wild} / boss {boss})")


if __name__ == '__main__':
    main()
