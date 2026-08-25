#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OS32 化学エンジン マスターデータ生成スクリプト

BotW型化学エンジン用のルール辞書 (chem.db) を生成する。
ゲームデザイナーがこのスクリプトを編集してルールを追加・変更可能。

使い方:
    python3 game/data/chem_db_init.py                  # game/build/db/chem.db を生成
    python3 game/data/chem_db_init.py -o /path/to.db   # 出力先を指定

ページサイズは OS32 カーネルの SQLITE_DEFAULT_PAGE_SIZE (1024) に合わせる。
"""

import sqlite3
import argparse
import os
import sys

# OS32 カーネル側の設定と一致させる
PAGE_SIZE = 1024

# ====================================================================
# 属性ビットフラグ定義 (libos32chem.h と一致させること)
# ====================================================================
ELEM_NONE     = 0x00000000
ELEM_FIRE     = 0x00000001
ELEM_WATER    = 0x00000002
ELEM_WOOD     = 0x00000004
ELEM_ICE      = 0x00000008
ELEM_ELECTRIC = 0x00000010
ELEM_STEAM    = 0x00000020
ELEM_GRASS    = 0x00000040
ELEM_METAL    = 0x00000080
ELEM_STONE    = 0x00000100
ELEM_WIND     = 0x00000200

# アクション種別
ACT_NONE       = 0
ACT_IGNITE     = 1
ACT_EXTINGUISH = 2
ACT_FREEZE     = 3
ACT_MELT       = 4
ACT_EVAPORATE  = 5
ACT_ELECTRIFY  = 6
ACT_SPREAD     = 7
ACT_DAMAGE     = 8
ACT_SPAWN      = 9
ACT_DESTROY    = 10

# ターゲット
TGT_A    = 0
TGT_B    = 1
TGT_BOTH = 2
TGT_AREA = 3


def create_schema(conn):
    """テーブルスキーマを作成"""
    cur = conn.cursor()

    cur.execute("""
        CREATE TABLE IF NOT EXISTS elements (
            id    INTEGER PRIMARY KEY,
            name  TEXT NOT NULL,
            flag  INTEGER NOT NULL UNIQUE
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS reactions (
            id          INTEGER PRIMARY KEY,
            elem_a      INTEGER NOT NULL,
            elem_b      INTEGER NOT NULL,
            action      INTEGER NOT NULL,
            target      INTEGER NOT NULL,
            spawn_elem  INTEGER DEFAULT 0,
            temp_delta  INTEGER DEFAULT 0,
            hp_delta    INTEGER DEFAULT 0,
            priority    INTEGER DEFAULT 5
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS phase_transitions (
            id          INTEGER PRIMARY KEY,
            elem_from   INTEGER NOT NULL,
            temp_min    INTEGER NOT NULL,
            temp_max    INTEGER NOT NULL,
            elem_to     INTEGER NOT NULL,
            spawn_elem  INTEGER DEFAULT 0
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS object_types (
            id          INTEGER PRIMARY KEY,
            name        TEXT NOT NULL,
            elements    INTEGER NOT NULL,
            temperature INTEGER DEFAULT 20,
            hp          INTEGER DEFAULT 100,
            flammable   INTEGER DEFAULT 0
        )
    """)

    conn.commit()


def populate_elements(conn):
    """属性マスターデータを投入"""
    elements = [
        (1,  "Fire",     ELEM_FIRE),
        (2,  "Water",    ELEM_WATER),
        (3,  "Wood",     ELEM_WOOD),
        (4,  "Ice",      ELEM_ICE),
        (5,  "Electric", ELEM_ELECTRIC),
        (6,  "Steam",    ELEM_STEAM),
        (7,  "Grass",    ELEM_GRASS),
        (8,  "Metal",    ELEM_METAL),
        (9,  "Stone",    ELEM_STONE),
        (10, "Wind",     ELEM_WIND),
    ]
    conn.executemany(
        "INSERT INTO elements (id, name, flag) VALUES (?, ?, ?)",
        elements
    )
    conn.commit()
    print(f"  elements: {len(elements)} entries")


def populate_reactions(conn):
    """相互作用ルールを投入"""
    # (id, elem_a, elem_b, action, target, spawn_elem, temp_delta, hp_delta, priority)
    reactions = [
        # --- 火の相互作用 ---
        # 火 + 木 → 木に着火 (最優先)
        (1,  ELEM_FIRE, ELEM_WOOD, ACT_IGNITE, TGT_B, 0, 50, 0, 10),
        # 火 + 草 → 草に着火
        (2,  ELEM_FIRE, ELEM_GRASS, ACT_IGNITE, TGT_B, 0, 40, 0, 10),
        # 火 + 木 → 木にダメージ
        (3,  ELEM_FIRE, ELEM_WOOD, ACT_DAMAGE, TGT_B, 0, 0, -25, 5),
        # 火 + 草 → 草にダメージ (大)
        (4,  ELEM_FIRE, ELEM_GRASS, ACT_DAMAGE, TGT_B, 0, 0, -40, 5),

        # --- 水の相互作用 ---
        # 水 + 火 → 火を消火
        (5,  ELEM_WATER, ELEM_FIRE, ACT_EXTINGUISH, TGT_B, 0, -30, 0, 9),
        # 水 + 電気 → 両方に帯電 (水は電気を伝導する)
        (6,  ELEM_WATER, ELEM_ELECTRIC, ACT_ELECTRIFY, TGT_BOTH, 0, 0, -10, 8),

        # --- 氷の相互作用 ---
        # 氷 + 火 → 氷が融解して水になる
        (7,  ELEM_ICE, ELEM_FIRE, ACT_MELT, TGT_A, 0, 30, 0, 8),
        # 水 + 氷 → 水が凍結 (氷が水に触れると凍る)
        (8,  ELEM_ICE, ELEM_WATER, ACT_FREEZE, TGT_B, 0, -40, 0, 7),

        # --- 電気の相互作用 ---
        # 電気 + 金属 → 金属に帯電 (伝導)
        (9,  ELEM_ELECTRIC, ELEM_METAL, ACT_ELECTRIFY, TGT_B, 0, 5, 0, 8),
        # 電気 + 水 → 両方にダメージ
        (10, ELEM_ELECTRIC, ELEM_WATER, ACT_DAMAGE, TGT_BOTH, 0, 0, -15, 6),

        # --- 風の相互作用 ---
        # 風 + 火 → 火が周囲に伝播
        (11, ELEM_WIND, ELEM_FIRE, ACT_SPREAD, TGT_B, 0, 20, 0, 7),

        # --- 蒸気の生成 ---
        # 火 + 水 → 蒸気を生成 (SPAWNで副産物)
        (12, ELEM_FIRE, ELEM_WATER, ACT_SPAWN, TGT_AREA, ELEM_STEAM, 0, 0, 4),

        # --- 石の相互作用 ---
        # 火 + 石 → 石にダメージ (弱い)
        (13, ELEM_FIRE, ELEM_STONE, ACT_DAMAGE, TGT_B, 0, 10, -5, 3),
    ]
    conn.executemany(
        "INSERT INTO reactions "
        "(id, elem_a, elem_b, action, target, spawn_elem, temp_delta, hp_delta, priority) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        reactions
    )
    conn.commit()
    print(f"  reactions: {len(reactions)} rules")


def populate_phase_transitions(conn):
    """温度ベース状態遷移ルールを投入"""
    # (id, elem_from, temp_min, temp_max, elem_to, spawn_elem)
    phases = [
        # 水 + 温度 >= 100 → 蒸気に変化
        (1, ELEM_WATER, 100, 32767, ELEM_STEAM, 0),
        # 水 + 温度 <= 0 → 氷に変化
        (2, ELEM_WATER, -32768, 0, ELEM_ICE, 0),
        # 氷 + 温度 > 0 → 水に変化 (融解)
        (3, ELEM_ICE, 1, 32767, ELEM_WATER, 0),
        # 蒸気 + 温度 <= 80 → 水に戻る (凝結)
        (4, ELEM_STEAM, -32768, 80, ELEM_WATER, 0),
        # 草 + 温度 >= 200 → 自然発火 (火に変化)
        (5, ELEM_GRASS, 200, 32767, ELEM_FIRE, 0),
    ]
    conn.executemany(
        "INSERT INTO phase_transitions "
        "(id, elem_from, temp_min, temp_max, elem_to, spawn_elem) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        phases
    )
    conn.commit()
    print(f"  phase_transitions: {len(phases)} rules")


def populate_object_types(conn):
    """オブジェクト型テンプレートを投入"""
    # (id, name, elements, temperature, hp, flammable)
    types = [
        (1,  "Tree",       ELEM_WOOD,               20, 100, 1),
        (2,  "Barrel",     ELEM_WOOD,               20,  50, 1),
        (3,  "Crate",      ELEM_WOOD,               20,  40, 1),
        (4,  "Bush",       ELEM_GRASS,              20,  30, 1),
        (5,  "Campfire",   ELEM_FIRE | ELEM_WOOD,  200,  60, 0),
        (6,  "Puddle",     ELEM_WATER,              15,  80, 0),
        (7,  "Ice Block",  ELEM_ICE,               -10, 120, 0),
        (8,  "Metal Crate", ELEM_METAL,             20, 200, 0),
        (9,  "Boulder",    ELEM_STONE,              20, 500, 0),
        (10, "Torch",      ELEM_FIRE | ELEM_METAL, 150, 100, 0),
    ]
    conn.executemany(
        "INSERT INTO object_types "
        "(id, name, elements, temperature, hp, flammable) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        types
    )
    conn.commit()
    print(f"  object_types: {len(types)} templates")


def main():
    parser = argparse.ArgumentParser(
        description="OS32 化学エンジン マスターデータ生成")
    # 既定はスクリプト位置基準。cwd に依らず game/build/db/ へ出す。
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "build", "db", "chem.db")
    parser.add_argument("-o", "--output", default=default_out,
                        help="出力DBファイルパス (default: <game>/build/db/chem.db)")
    args = parser.parse_args()

    output = args.output
    print(f"=== chem_db_init: generating {output} ===")

    # 既存ファイルがあれば削除して再生成
    if os.path.exists(output):
        os.remove(output)
        print(f"  removed existing {output}")

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)

    conn = sqlite3.connect(output)

    # OS32 カーネル側と同じページサイズに設定
    conn.execute(f"PRAGMA page_size = {PAGE_SIZE}")
    conn.execute("PRAGMA journal_mode = DELETE")

    create_schema(conn)
    populate_elements(conn)
    populate_reactions(conn)
    populate_phase_transitions(conn)
    populate_object_types(conn)

    # VACUUM でページサイズを確定
    conn.execute("VACUUM")
    conn.commit()
    conn.close()

    size = os.path.getsize(output)
    print(f"  output: {output} ({size} bytes, {size // PAGE_SIZE} pages)")
    print("=== done ===")


if __name__ == "__main__":
    main()
