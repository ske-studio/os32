#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_hud.py — 盤面HUD + 分岐選択UI の再設計案 (PC-98 RPG 風)

3案をピクセル正確に描き出す。出力は docs/tasks/game/ui/ 。
盤面は assets/board.db の実データを使うので、実機の見え方と一致する。

  案A: 上帯 + 下段コマンド窓   … 盤面を最大限に見せる
  案B: 右サイドパネル常時表示   … 4人の資産を常に比較できる
  案C: 下段オールインワン       … 情報量最大、盤面は上半分

共通の作法 (PC-98 RPG 風):
  - ウィンドウは二重罫線 + 影
  - 本体は濃紺(1)、文字は白(15)、強調は明黄(14)
  - 選択は ▶ カーソル + 数字キー
  - 文字は溢れさせず、必ず幅に収まるよう切り詰める
"""

import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uimock import (Screen, win, gauge, cursor, clip_text,
                    C_BLACK, C_BLUE_D, C_RED_D, C_GREEN_D, C_CYAN_D,
                    C_YELLOW_D, C_GRAY, C_BLUE, C_RED, C_GREEN, C_YELLOW,
                    C_MAGENTA, C_CYAN, C_WHITE)

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)
OUT = os.path.join(PROJ_DIR, 'docs', 'tasks', 'game', 'ui')
BOARD_DB = os.path.join(PROJ_DIR, 'assets', 'board.db')

MASS_EMPTY, MASS_VILLAGE, MASS_BATTLE, MASS_TREASURE = 0, 1, 2, 3
MASS_EQUIP_SHOP, MASS_ITEM_SHOP, MASS_MAGIC_SHOP = 4, 5, 6
MASS_CHURCH, MASS_CIRCLE, MASS_EVENT = 7, 8, 9
MASS_GATE, MASS_CASTLE, MASS_MAGIC_CHEST = 10, 11, 12

# マス種別 -> (色, 1文字ラベル)
MASS_STYLE = {
    MASS_EMPTY:       (C_GRAY,     ''),
    MASS_VILLAGE:     (C_GREEN_D,  'V'),
    MASS_BATTLE:      (C_RED,      '!'),
    MASS_TREASURE:    (C_YELLOW,   '$'),
    MASS_EQUIP_SHOP:  (C_BLUE,     'W'),
    MASS_ITEM_SHOP:   (C_BLUE,     'I'),
    MASS_MAGIC_SHOP:  (C_MAGENTA,  'M'),
    MASS_CHURCH:      (C_CYAN,     '+'),
    MASS_CIRCLE:      (C_MAGENTA,  'O'),
    MASS_EVENT:       (C_YELLOW_D, '?'),
    MASS_GATE:        (C_RED_D,    'G'),
    MASS_CASTLE:      (C_WHITE,    'C'),
    MASS_MAGIC_CHEST: (C_YELLOW_D, '*'),
}

MASS_NAME = {
    MASS_EMPTY: 'Plain', MASS_VILLAGE: 'Village', MASS_BATTLE: 'Monster',
    MASS_TREASURE: 'Treasure', MASS_EQUIP_SHOP: 'Weapon Shop',
    MASS_ITEM_SHOP: 'Item Shop', MASS_MAGIC_SHOP: 'Magic Shop',
    MASS_CHURCH: 'Shrine', MASS_CIRCLE: 'Circle', MASS_EVENT: 'Event',
    MASS_GATE: 'Gate', MASS_CASTLE: 'Castle', MASS_MAGIC_CHEST: 'Gold Chest',
}

# プレイヤー色 (現行と同じ)
P_COLOR = [C_RED, C_BLUE, C_GREEN, C_YELLOW]

# デモ用のゲーム状態
PLAYERS = [
    dict(name='Susanoo',    lv=4, hp=62, mhp=78,  gold=1113, est=3, pos=39, cpu=0, devil=0),
    dict(name='Y.Takeru',   lv=3, hp=41, mhp=66,  gold=820,  est=2, pos=52, cpu=1, devil=0),
    dict(name='Okuninushi', lv=5, hp=88, mhp=90,  gold=2140, est=5, pos=71, cpu=1, devil=0),
    dict(name='Amaterasu',  lv=2, hp=12, mhp=54,  gold=310,  est=0, pos=18, cpu=1, devil=1),
]
CUR = 0
WEEK, TURN = 6, 23


def load_board():
    con = sqlite3.connect(BOARD_DB)
    rows = con.execute('SELECT id, type, x, y FROM masses ORDER BY id').fetchall()
    conns = con.execute('SELECT from_id, to_id FROM connections').fetchall()
    con.close()
    return rows, conns


BOARD, CONNS = load_board()


def draw_board(s, ox, oy, sx, sy, radius=5, label=False, rows_max=99):
    """盤面を (ox, oy) 基準・マス間隔 (sx, sy) で描く"""
    pos = {}
    for (mid, mtype, gx, gy) in BOARD:
        if gy >= rows_max:
            continue
        pos[mid] = (ox + gx * sx, oy + gy * sy)

    for (a, b) in CONNS:
        if a in pos and b in pos:
            s.line(pos[a][0], pos[a][1], pos[b][0], pos[b][1], C_GRAY)

    for (mid, mtype, gx, gy) in BOARD:
        if mid not in pos:
            continue
        x, y = pos[mid]
        color, mark = MASS_STYLE.get(mtype, (C_GRAY, ''))
        s.fill_circle(x, y, radius, color)
        s.circle(x, y, radius, C_WHITE)
        if label and mark:
            s.text8(x - 3, y - 3, mark, C_BLACK if color in
                    (C_YELLOW, C_WHITE, C_CYAN) else C_WHITE)

    # プレイヤー駒
    for i, p in enumerate(PLAYERS):
        if p['pos'] not in pos:
            continue
        x, y = pos[p['pos']]
        ring = C_WHITE if i == CUR else C_BLACK
        s.fill_circle(x, y - radius - 3, 3, P_COLOR[i])
        s.circle(x, y - radius - 3, 3, ring)
    return pos


def assets(p):
    return p['gold'] + p['est'] * 640


def rank_order():
    return sorted(range(len(PLAYERS)), key=lambda i: -assets(PLAYERS[i]))


# ---------------------------------------------------------------------------
#  共通パーツ
# ---------------------------------------------------------------------------

def hp_bar_color(hp, mhp):
    if hp * 4 <= mhp:
        return C_RED
    if hp * 2 <= mhp:
        return C_YELLOW
    return C_GREEN


def branch_window(s, x, y, w, opts, sel=0, steps=3, title='WHICH WAY?'):
    """分岐選択ウィンドウ。opts = [(mass_id, mass_type)]"""
    h = 30 + len(opts) * 18 + 10
    win(s, x, y, w, h)
    s.fill_rect(x + 3, y + 3, w - 6, 14, C_RED_D)
    s.text8(x + 8, y + 6, title, C_WHITE)
    s.text8(x + w - 8 * 10 - 8, y + 6, '%d STEPS' % steps, C_YELLOW)

    for i, (mid, mtype) in enumerate(opts):
        oy = y + 24 + i * 18
        if i == sel:
            s.fill_rect(x + 6, oy - 1, w - 12, 17, C_BLUE)
            cursor(s, x + 8, oy, C_YELLOW)
        s.text8(x + 20, oy + 4, '%d.' % (i + 1), C_YELLOW)
        color, mark = MASS_STYLE.get(mtype, (C_GRAY, ''))
        s.fill_circle(x + 40, oy + 7, 5, color)
        s.circle(x + 40, oy + 7, 5, C_WHITE)
        label = clip_text('%-12s #%d' % (MASS_NAME.get(mtype, '?'), mid),
                          w - 60)
        s.text8(x + 52, oy + 4, label, C_WHITE)


# ---------------------------------------------------------------------------
#  案A: 上帯 + 下段コマンド窓
# ---------------------------------------------------------------------------

def plan_a():
    s = Screen(C_BLACK)
    p = PLAYERS[CUR]

    # --- 上帯 (高さ 26) ---
    s.fill_rect(0, 0, 640, 26, C_BLUE_D)
    s.fill_rect(0, 26, 640, 1, C_WHITE)

    s.fill_rect(6, 5, 8, 16, P_COLOR[CUR])
    s.rect(6, 5, 8, 16, C_WHITE)
    s.text16(20, 5, p['name'], C_WHITE)
    s.text16(20 + 8 * 11, 5, 'Lv%-2d' % p['lv'], C_YELLOW)

    gauge(s, 148, 8, 90, 10, p['hp'], p['mhp'], hp_bar_color(p['hp'], p['mhp']))
    s.text8(244, 10, '%3d/%3d' % (p['hp'], p['mhp']), C_WHITE)

    s.text8(310, 10, 'G', C_YELLOW)
    s.text8(322, 10, '%-6d' % p['gold'], C_WHITE)
    s.text8(382, 10, 'VIL', C_YELLOW)
    s.text8(410, 10, '%-2d' % p['est'], C_WHITE)

    # 4人の順位を上帯の右に凝縮
    x = 452
    for rank, i in enumerate(rank_order()):
        s.fill_rect(x, 8, 6, 10, P_COLOR[i])
        s.rect(x, 8, 6, 10, C_WHITE if i == CUR else C_BLACK)
        s.text8(x + 8, 10, '%d' % (rank + 1), C_WHITE)
        x += 24

    s.text8(556, 10, 'W%-2d T%-3d' % (WEEK, TURN), C_YELLOW)

    # --- 盤面 ---
    draw_board(s, 28, 42, 26, 33, radius=5, label=True)

    # --- 下段: 分岐選択 ---
    branch_window(s, 16, 300, 300,
                  [(40, MASS_VILLAGE), (60, MASS_BATTLE)], sel=0, steps=3)

    # --- 右下: 操作ヒント ---
    win(s, 330, 300, 294, 86)
    s.text8(340, 310, 'DEST', C_YELLOW)
    s.fill_rect(340, 322, 274, 1, C_GRAY)
    s.text8(340, 330, 'Village  Ashihara      Lv2', C_WHITE)
    s.text8(340, 344, 'Owner    Okuninushi', C_WHITE)
    s.text8(340, 358, 'Toll     320 G', C_RED)
    s.text8(340, 372, '[1/2] SELECT   [ENTER] GO', C_GRAY)
    return s


# ---------------------------------------------------------------------------
#  案B: 右サイドパネル常時表示
# ---------------------------------------------------------------------------

def plan_b():
    s = Screen(C_BLACK)

    # --- 盤面 (左 512px に収める: 16 + 19*25 + 5 = 496) ---
    draw_board(s, 16, 26, 25, 42, radius=5, label=True)

    # --- 右パネル (x 512..640) ---
    px, pw = 512, 128
    s.fill_rect(px, 0, pw, 400, C_BLUE_D)
    s.fill_rect(px, 0, 1, 400, C_WHITE)

    s.fill_rect(px + 4, 4, pw - 8, 12, C_RED_D)
    s.text8(px + 8, 6, 'WEEK %-2d T%-3d' % (WEEK, TURN), C_WHITE)

    y = 22
    for rank, i in enumerate(rank_order()):
        p = PLAYERS[i]
        cur_row = (i == CUR)
        if cur_row:
            s.fill_rect(px + 3, y - 2, pw - 6, 62, C_BLUE)
        s.fill_rect(px + 6, y, 6, 10, P_COLOR[i])
        s.rect(px + 6, y, 6, 10, C_WHITE)
        s.text8(px + 16, y + 1, '%d' % (rank + 1), C_YELLOW)
        s.text8(px + 28, y + 1, clip_text(p['name'], 88), C_WHITE)
        if p['devil']:
            s.text8(px + 100, y + 1, 'D', C_RED)

        s.text8(px + 6, y + 14, 'Lv%-2d' % p['lv'], C_YELLOW)
        gauge(s, px + 40, y + 14, 62, 8, p['hp'], p['mhp'],
              hp_bar_color(p['hp'], p['mhp']))

        s.text8(px + 6, y + 26, 'G', C_YELLOW)
        s.text8(px + 16, y + 26, '%-7d' % p['gold'], C_WHITE)
        s.text8(px + 6, y + 38, 'V', C_YELLOW)
        s.text8(px + 16, y + 38, '%-2d' % p['est'], C_WHITE)
        s.text8(px + 44, y + 38, 'A', C_YELLOW)
        s.text8(px + 54, y + 38, '%-7d' % assets(p), C_WHITE)

        s.fill_rect(px + 4, y + 54, pw - 8, 1, C_GRAY)
        y += 62

    s.text8(px + 6, 380, '[R] ROLL', C_GRAY)

    # --- 分岐選択は盤面側にモーダル ---
    branch_window(s, 96, 150, 300,
                  [(40, MASS_VILLAGE), (60, MASS_BATTLE)], sel=1, steps=3)
    return s


# ---------------------------------------------------------------------------
#  案C: 下段オールインワン
# ---------------------------------------------------------------------------

def plan_c():
    s = Screen(C_BLACK)

    # --- 盤面 (上 275px、行間を詰める) ---
    draw_board(s, 28, 20, 26, 29, radius=4, label=False)

    s.fill_rect(0, 278, 640, 1, C_WHITE)

    # --- 下段: 左に現在プレイヤー、中央に4人、右にコマンド ---
    win(s, 4, 284, 200, 112)
    s.fill_rect(7, 287, 194, 14, C_RED_D)
    p = PLAYERS[CUR]
    s.text8(12, 290, clip_text(p['name'] + ('  DEVIL' if p['devil'] else ''),
                               186), C_WHITE)
    s.text16(12, 306, 'Lv%-2d' % p['lv'], C_YELLOW)
    gauge(s, 60, 310, 134, 10, p['hp'], p['mhp'], hp_bar_color(p['hp'], p['mhp']))
    s.text8(12, 328, 'HP', C_YELLOW)
    s.text8(34, 328, '%3d/%3d' % (p['hp'], p['mhp']), C_WHITE)
    s.text8(12, 342, 'GOLD', C_YELLOW)
    s.text8(52, 342, '%-7d' % p['gold'], C_WHITE)
    s.text8(12, 356, 'VILL', C_YELLOW)
    s.text8(52, 356, '%-2d' % p['est'], C_WHITE)
    s.text8(12, 370, 'ASSET', C_YELLOW)
    s.text8(60, 370, '%-7d' % assets(p), C_WHITE)

    # 中央: 4人の順位表
    win(s, 210, 284, 190, 112)
    s.fill_rect(213, 287, 184, 14, C_RED_D)
    s.text8(218, 290, 'RANKING   W%-2d T%-3d' % (WEEK, TURN), C_WHITE)
    y = 306
    for rank, i in enumerate(rank_order()):
        q = PLAYERS[i]
        if i == CUR:
            s.fill_rect(214, y - 2, 182, 18, C_BLUE)
        s.text8(218, y + 2, '%d' % (rank + 1), C_YELLOW)
        s.fill_rect(232, y + 1, 6, 10, P_COLOR[i])
        s.rect(232, y + 1, 6, 10, C_WHITE)
        s.text8(244, y + 2, clip_text(q['name'], 80), C_WHITE)
        s.text8(330, y + 2, '%7d' % assets(q), C_YELLOW if rank == 0 else C_WHITE)
        y += 20

    # 右: 分岐選択
    branch_window(s, 406, 284, 230,
                  [(40, MASS_VILLAGE), (60, MASS_BATTLE)], sel=0, steps=3)
    return s


PLANS = [('hud_a', plan_a, '案A: 上帯 + 下段コマンド窓'),
         ('hud_b', plan_b, '案B: 右サイドパネル'),
         ('hud_c', plan_c, '案C: 下段オールインワン')]


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for name, fn, desc in PLANS:
        path = os.path.join(OUT, name + '.png')
        fn().save(path, scale=1)
        print('%-8s %s -> %s' % (name, desc, os.path.normpath(path)))
