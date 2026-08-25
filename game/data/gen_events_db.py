#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_events_db.py — libos32event 用イベント定義の生成

  --game (既定)  game/build/db/events.db       対戦スゴロクRPG の週次イベント9種
  --test         game/build/db/events_test.db  evt_test が前提とする汎用テストデータ

evt_tick() は毎ターン呼ぶ。libos32event の乱数発火は
「rng % 256 < 無発生カウンタ」で、カウンタは tick ごとに +1 されるため、
週1回しか tick しないと第6週でも 2.3% にしかならず実質発生しない。
そこで min_turn / cooldown / period / duration の単位はすべて「ターン」とし、
1週=7ターンで換算した値を入れている (例: cooldown 8週 = 56)。

効果の中身は game_glue.c の apply_event() が ID で分岐して実装する。
このDBは「いつ・どれくらいの頻度で起きるか」だけを持つ。

原典 (DOS版 sample/game/) が本リポジトリに無いため、9種の内容は
ドカポン系の週次イベントを踏まえた合成データ。
"""

import argparse
import os
import sqlite3
import sys

ASSET_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', 'build', 'db')

TYPE_PERIODIC = 0
TYPE_RANDOM = 1
TYPE_CONDITION = 2

SCOPE_GLOBAL = 0
SCOPE_PLAYER = 1

SCHEMA = """
CREATE TABLE events (
    id           INTEGER PRIMARY KEY,
    type         INTEGER NOT NULL DEFAULT 1,
    weight       INTEGER NOT NULL DEFAULT 5,
    min_turn     INTEGER DEFAULT 0,
    cooldown     INTEGER DEFAULT 10,
    period       INTEGER DEFAULT 0,
    duration     INTEGER DEFAULT 0,
    grp          INTEGER DEFAULT 0,
    chain_id     INTEGER DEFAULT 0,
    chain_chance INTEGER DEFAULT 0,
    scope        INTEGER DEFAULT 0
)
"""

# 排他グループ: 同じグループのイベントは同時に走らない
GRP_ECONOMY = 1     # 経済に効くもの
GRP_HAZARD = 2      # 全員に不利なもの
GRP_SHOP = 3        # 店に効くもの

W = 7   # 1週 = 7ターン

# (id, 名前, type, weight, min_turn, cooldown, period, duration, grp,
#  chain_id, chain_chance, scope)   — ターン単位
GAME_EVENTS = [
    # --- 経済 ---
    (1,  'Harvest',      TYPE_RANDOM, 6, 2*W, 8*W,  0, 0,   GRP_ECONOMY, 0, 0, SCOPE_GLOBAL),
    (2,  'TaxLevy',      TYPE_RANDOM, 4, 4*W, 12*W, 0, 0,   GRP_ECONOMY, 0, 0, SCOPE_GLOBAL),
    (3,  'Bandits',      TYPE_RANDOM, 4, 3*W, 10*W, 0, 0,   GRP_ECONOMY, 0, 0, SCOPE_PLAYER),

    # --- 災厄 ---
    (4,  'Plague',       TYPE_RANDOM, 3, 5*W, 14*W, 0, 0,   GRP_HAZARD, 0, 0, SCOPE_GLOBAL),
    # 魔物大量発生は1週間続く。連鎖で討伐指令が出ることがある
    (5,  'MonsterSurge', TYPE_RANDOM, 4, 4*W, 12*W, 0, 1*W, GRP_HAZARD, 9, 60, SCOPE_GLOBAL),

    # --- 救済 ---
    (6,  'Oracle',       TYPE_RANDOM, 5, 3*W, 9*W,  0, 0,   0, 0, 0, SCOPE_PLAYER),

    # --- 店 ---
    # 大売出しは1週間続く
    (7,  'BigSale',      TYPE_RANDOM, 5, 2*W, 10*W, 0, 1*W, GRP_SHOP, 0, 0, SCOPE_GLOBAL),

    # --- 定期 ---
    # 4週ごとの宝探し (必ず来る)
    (8,  'TreasureHunt', TYPE_PERIODIC, 0, 0, 0, 4*W, 0,    0, 0, 0, SCOPE_PLAYER),

    # --- 連鎖先 ---
    (9,  'BossHunt',     TYPE_RANDOM, 2, 8*W, 16*W, 0, 1*W, 0, 0, 0, SCOPE_GLOBAL),
]

# evt_test.c が前提とする汎用データ (既存 events.db と同じ内容)
TEST_EVENTS = [
    (1,  0, 0, 0, 0, 4,  0, 0, 0, 0, 0),
    (2,  0, 0, 0, 0, 10, 2, 0, 0, 0, 0),
    (10, 1, 5, 5, 10, 0, 0, 0, 0, 0, 0),
    (11, 1, 3, 5, 8,  0, 0, 0, 0, 0, 0),
    (12, 1, 8, 3, 15, 0, 3, 0, 0, 0, 0),
    (20, 1, 4, 5, 10, 0, 5, 1, 0, 0, 0),
    (21, 1, 4, 5, 10, 0, 5, 1, 0, 0, 0),
    (30, 1, 6, 2, 12, 0, 0, 0, 31, 50, 0),
    (31, 1, 0, 0, 5,  0, 2, 0, 0, 0, 0),
    (40, 2, 0, 0, 6,  0, 3, 0, 0, 0, 1),
]


def generate(db_name, rows, named):
    path = os.path.join(ASSET_DIR, db_name)
    os.makedirs(ASSET_DIR, exist_ok=True)
    if os.path.exists(path):
        os.remove(path)

    con = sqlite3.connect(path)
    c = con.cursor()
    c.executescript(SCHEMA)

    if named:
        vals = [(e[0],) + e[2:] for e in rows]
    else:
        vals = rows
    c.executemany('INSERT INTO events VALUES (?,?,?,?,?,?,?,?,?,?,?)', vals)
    con.commit()
    con.close()

    print('Generated: %s' % os.path.normpath(path))
    print('  %d events' % len(vals))
    if named:
        for e in rows:
            kind = ['periodic', 'random', 'condition'][e[2]]
            print('    %-3d %-13s %-9s w=%-2d cd=%-2d dur=%-2d grp=%d'
                  % (e[0], e[1], kind, e[3], e[5], e[7], e[8]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', action='store_true')
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()

    if not (args.game or args.test or args.all):
        args.game = True

    if args.game or args.all:
        generate('events.db', GAME_EVENTS, named=True)
    if args.test or args.all:
        generate('events_test.db', TEST_EVENTS, named=False)
    return 0


if __name__ == '__main__':
    sys.exit(main())
