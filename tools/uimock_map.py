#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_map.py — 盤面を「地図」として描き直す案のモック

現状: 1マス = 窓のような台座 (UIパーツに見える)
案  : 1マス = 8x8 タイル 16x12 枚 で組んだ小さな風景 (地図に見える)

  画面 640x400 / PC-98 16色
  盤面 400x304 = 50x38 タイル (8px)
  1マス 128x96 = 16x12 タイル
  3x3 マス = 48x36 タイル (余り 2x2 タイルがスクロールの糊しろ)

キャラクタのサイズ比較も出す:
  A 16x16 (2x2 タイル)   B 16x24 (2x3 タイル)   C 24x32 (3x4 タイル)
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uimock import (Screen, clip_text,
                    C_BLACK, C_BLUE_D, C_RED_D, C_GREEN_D, C_CYAN_D,
                    C_YELLOW_D, C_GRAY, C_BLUE, C_RED, C_GREEN, C_YELLOW,
                    C_MAGENTA, C_CYAN, C_WHITE, C_MAGENTA_D)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'docs', 'tasks', 'game', 'ui')

T = 8                       # タイル 8x8
CELL_TW, CELL_TH = 16, 12   # 1マス = 16x12 タイル
CELL_W, CELL_H = CELL_TW * T, CELL_TH * T    # 128 x 96
FIELD_W, FIELD_H = 400, 304
P_COLOR = [C_RED, C_BLUE, C_GREEN, C_YELLOW]


# ---------------------------------------------------------------------------
#  8x8 タイル
#  すべて「地形」。UI の枠は使わない。
# ---------------------------------------------------------------------------

def t_grass(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_GREEN_D)
    if v % 3 == 0:
        s.pixel(x + 2, y + 5, C_GREEN); s.pixel(x + 3, y + 4, C_GREEN)
    elif v % 3 == 1:
        s.pixel(x + 5, y + 2, C_GREEN); s.pixel(x + 6, y + 3, C_GREEN)


def t_road(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_YELLOW_D)
    if v % 2:
        s.pixel(x + 2, y + 3, C_GRAY)
    else:
        s.pixel(x + 5, y + 6, C_GRAY)


def t_sea(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_BLUE_D)
    if v % 2:
        s.fill_rect(x + 1, y + 3, 3, 1, C_BLUE)
    else:
        s.fill_rect(x + 4, y + 5, 3, 1, C_BLUE)


def t_coast(s, x, y, v=0):
    """波打ち際"""
    s.fill_rect(x, y, T, T, C_BLUE_D)
    s.fill_rect(x, y, T, 3, C_YELLOW_D)
    s.fill_rect(x, y + 3, T, 1, C_WHITE)


def t_tree(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_GREEN_D)
    s.fill_rect(x + 2, y + 5, 2, 3, C_YELLOW_D)
    s.fill_rect(x + 1, y + 1, 5, 4, C_GREEN)
    s.fill_rect(x + 2, y, 3, 1, C_GREEN)
    s.pixel(x + 2, y + 2, C_GREEN_D)


def t_rock(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_GREEN_D)
    s.fill_rect(x + 1, y + 3, 6, 4, C_GRAY)
    s.fill_rect(x + 2, y + 2, 4, 2, C_GRAY)
    s.fill_rect(x + 2, y + 5, 2, 2, C_BLACK)


def t_snow(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_WHITE)
    if v % 2:
        s.pixel(x + 2, y + 3, C_CYAN)
    else:
        s.pixel(x + 5, y + 5, C_CYAN)


def t_swamp(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_MAGENTA_D)
    if v % 2:
        s.fill_rect(x + 1, y + 4, 3, 1, C_MAGENTA)
    else:
        s.fill_rect(x + 4, y + 2, 3, 1, C_MAGENTA)


def t_sand(s, x, y, v=0):
    s.fill_rect(x, y, T, T, C_YELLOW_D)
    if v % 2:
        s.fill_rect(x + 1, y + 5, 3, 1, C_YELLOW)


TILES = {'g': t_grass, 'r': t_road, 's': t_sea, 'c': t_coast, 'T': t_tree,
         'R': t_rock, 'w': t_snow, 'p': t_swamp, 'd': t_sand}


# ---------------------------------------------------------------------------
#  建物 (複数タイルにまたがる)
# ---------------------------------------------------------------------------

def house(s, x, y, roof, w=3, h=3):
    """w x h タイルの家。roof は所有者の色"""
    px, py = x, y
    pw, ph = w * T, h * T
    body_h = ph * 2 // 3
    # 壁
    s.fill_rect(px + 2, py + ph - body_h, pw - 4, body_h, C_YELLOW_D)
    s.rect(px + 2, py + ph - body_h, pw - 4, body_h, C_BLACK)
    # 屋根
    for i in range(ph - body_h):
        s.fill_rect(px + i, py + i, pw - i * 2, 1, roof)
    s.line(px, py + ph - body_h - 1, px + pw - 1, py + ph - body_h - 1, C_BLACK)
    # 扉
    s.fill_rect(px + pw // 2 - 2, py + ph - body_h + 2, 4, body_h - 2, C_BLACK)


def shop_stall(s, x, y, accent, w=4, h=3):
    pw, ph = w * T, h * T
    s.fill_rect(x + 2, y + ph - 12, pw - 4, 12, C_GRAY)
    s.rect(x + 2, y + ph - 12, pw - 4, 12, C_BLACK)
    # 縞のオーニング
    for i in range((pw - 4) // 4):
        s.fill_rect(x + 2 + i * 4, y + ph - 18, 4, 6,
                    accent if i % 2 == 0 else C_WHITE)
    s.fill_rect(x, y + ph - 20, pw, 2, accent)


def shrine(s, x, y, w=4, h=3):
    pw, ph = w * T, h * T
    # 鳥居
    s.fill_rect(x + 3, y + 6, 3, ph - 8, C_CYAN)
    s.fill_rect(x + pw - 6, y + 6, 3, ph - 8, C_CYAN)
    s.fill_rect(x + 1, y + 3, pw - 2, 3, C_CYAN)
    s.fill_rect(x + 3, y + 9, pw - 6, 2, C_CYAN)


def castle(s, x, y, w=5, h=4):
    pw, ph = w * T, h * T
    s.fill_rect(x + 2, y + ph - 18, pw - 4, 18, C_GRAY)
    s.rect(x + 2, y + ph - 18, pw - 4, 18, C_BLACK)
    for i in range(4):
        s.fill_rect(x + 2 + i * ((pw - 6) // 3), y + ph - 24, 5, 6, C_WHITE)
        s.rect(x + 2 + i * ((pw - 6) // 3), y + ph - 24, 5, 6, C_BLACK)
    s.fill_rect(x + pw // 2 - 3, y + ph - 10, 6, 10, C_BLACK)


def cave(s, x, y, w=3, h=3):
    pw, ph = w * T, h * T
    s.fill_rect(x + 1, y + 4, pw - 2, ph - 4, C_GRAY)
    s.fill_circle(x + pw // 2, y + ph - 4, 6, C_BLACK)
    s.fill_rect(x + pw // 2 - 6, y + ph - 6, 12, 6, C_BLACK)


def monster_den(s, x, y, w=3, h=3):
    pw, ph = w * T, h * T
    # 骨と旗
    s.fill_rect(x + 2, y + ph - 4, pw - 4, 3, C_GRAY)
    s.fill_rect(x + pw // 2 - 1, y + 2, 2, ph - 6, C_YELLOW_D)
    s.fill_rect(x + pw // 2 + 1, y + 2, pw // 2 - 2, 6, C_RED)


def chest(s, x, y, col=C_YELLOW, w=2, h=2):
    pw, ph = w * T, h * T
    s.fill_rect(x + 1, y + ph // 2 - 2, pw - 2, ph // 2 + 1, col)
    s.rect(x + 1, y + ph // 2 - 2, pw - 2, ph // 2 + 1, C_BLACK)
    for i in range(ph // 2 - 2):
        s.fill_rect(x + 1 + i // 2, y + ph // 2 - 3 - i, pw - 2 - i, 1, col)
    s.fill_rect(x + 1, y + ph // 2 + 1, pw - 2, 1, C_BLACK)


def circle_mark(s, x, y, w=4, h=3):
    pw, ph = w * T, h * T
    cx, cy = x + pw // 2, y + ph // 2
    for r in (12, 8, 4):
        s.circle(cx, cy, r, C_MAGENTA)


def collect_office(s, x, y, w=4, h=3):
    pw, ph = w * T, h * T
    s.fill_rect(x + 2, y + ph - 16, pw - 4, 16, C_YELLOW_D)
    s.rect(x + 2, y + ph - 16, pw - 4, 16, C_BLACK)
    s.fill_rect(x, y + ph - 20, pw, 4, C_YELLOW)
    s.text8(x + pw // 2 - 4, y + ph - 12, "G", C_BLACK)


# ---------------------------------------------------------------------------
#  キャラクタ
# ---------------------------------------------------------------------------

def draw_char(s, x, y, col, w, h, facing=0):
    """下端中央が (x, y) に来るように描く"""
    px = x - w // 2
    py = y - h

    head_h = max(4, h // 3)
    body_h = h - head_h

    # 影
    s.fill_rect(px + 1, y - 1, w - 2, 2, C_BLACK)

    # 体 (マント風に色付き)
    s.fill_rect(px + 1, py + head_h, w - 2, body_h - 1, col)
    s.rect(px + 1, py + head_h, w - 2, body_h - 1, C_BLACK)

    # 頭
    s.fill_rect(px + 2, py, w - 4, head_h, C_YELLOW_D)
    s.rect(px + 2, py, w - 4, head_h, C_BLACK)

    # 髪
    s.fill_rect(px + 2, py, w - 4, max(1, head_h // 3), C_BLACK)

    # 目 (16px 以上のときだけ)
    if w >= 14 and head_h >= 5:
        s.pixel(px + 4, py + head_h - 2, C_BLACK)
        s.pixel(px + w - 5, py + head_h - 2, C_BLACK)

    # 足
    if h >= 20:
        s.fill_rect(px + 2, y - 3, 3, 3, C_BLACK)
        s.fill_rect(px + w - 5, y - 3, 3, 3, C_BLACK)


# ---------------------------------------------------------------------------
#  マスの中身 (16x12 タイル = 128x96)
# ---------------------------------------------------------------------------

MASS_VILLAGE, MASS_BATTLE, MASS_TREASURE = 1, 2, 3
MASS_EQUIP, MASS_ITEM, MASS_MAGIC = 4, 5, 6
MASS_CHURCH, MASS_CIRCLE = 7, 8
MASS_GATE, MASS_CASTLE, MASS_GOLDCHEST = 10, 11, 12
MASS_COLLECT, MASS_DUNGEON, MASS_PLAIN = 13, 14, 0


def draw_cell(s, ox, oy, mtype, terrain='g', owner=None, lv=1,
              open_n=0, open_s=0, open_w=0, open_e=0, seed=0):
    """1マス分の地形と建物を描く。open_* はその方向に道が伸びるか"""
    base = {'g': 'g', 'F': 'T', 'M': 'R', 'w': 'w', 'p': 'p', 'd': 'd'}.get(
        terrain, 'g')

    # 1. 下地
    for ty in range(CELL_TH):
        for tx in range(CELL_TW):
            v = (tx * 3 + ty * 5 + seed)
            TILES[base](s, ox + tx * T, oy + ty * T, v)

    # 2. 十字の道 (接続のある方向にだけ伸ばす)
    cx_t, cy_t = CELL_TW // 2, CELL_TH // 2
    for ty in range(CELL_TH):
        for tx in range(CELL_TW):
            on = False
            if abs(tx - cx_t) <= 1 and abs(ty - cy_t) <= 1:
                on = True
            elif abs(tx - cx_t) <= 1:
                if ty < cy_t and open_n: on = True
                if ty > cy_t and open_s: on = True
            elif abs(ty - cy_t) <= 1:
                if tx < cx_t and open_w: on = True
                if tx > cx_t and open_e: on = True
            if on:
                t_road(s, ox + tx * T, oy + ty * T, tx + ty)

    # 3. 建物
    if mtype == MASS_VILLAGE:
        roof = P_COLOR[owner] if owner is not None else C_GRAY
        house(s, ox + 1 * T, oy + 1 * T, roof, 4, 4)
        house(s, ox + 11 * T, oy + 6 * T, roof, 3, 3)
        t_tree(s, ox + 6 * T, oy + 0 * T)
        t_tree(s, ox + 14 * T, oy + 1 * T)
        if lv >= 2:
            house(s, ox + 12 * T, oy + 1 * T, roof, 3, 3)
        if lv >= 3:
            house(s, ox + 1 * T, oy + 7 * T, roof, 3, 3)
    elif mtype == MASS_BATTLE:
        monster_den(s, ox + 11 * T, oy + 1 * T, 4, 4)
        t_rock(s, ox + 1 * T, oy + 1 * T)
        t_rock(s, ox + 2 * T, oy + 8 * T)
        t_rock(s, ox + 13 * T, oy + 8 * T)
    elif mtype == MASS_TREASURE:
        chest(s, ox + 12 * T, oy + 2 * T, C_YELLOW, 3, 3)
        t_tree(s, ox + 1 * T, oy + 1 * T)
        t_tree(s, ox + 2 * T, oy + 9 * T)
    elif mtype == MASS_GOLDCHEST:
        chest(s, ox + 12 * T, oy + 2 * T, C_MAGENTA, 3, 3)
    elif mtype in (MASS_ITEM, MASS_EQUIP, MASS_MAGIC):
        accent = {MASS_ITEM: C_CYAN, MASS_EQUIP: C_BLUE,
                  MASS_MAGIC: C_MAGENTA}[mtype]
        shop_stall(s, ox + 10 * T, oy + 1 * T, accent, 5, 4)
        t_tree(s, ox + 1 * T, oy + 9 * T)
    elif mtype == MASS_CHURCH:
        shrine(s, ox + 10 * T, oy + 1 * T, 5, 4)
        t_tree(s, ox + 1 * T, oy + 1 * T)
    elif mtype == MASS_CIRCLE:
        circle_mark(s, ox + 10 * T, oy + 1 * T, 5, 4)
    elif mtype == MASS_CASTLE:
        castle(s, ox + 9 * T, oy + 1 * T, 6, 5)
    elif mtype == MASS_COLLECT:
        collect_office(s, ox + 10 * T, oy + 1 * T, 5, 4)
    elif mtype == MASS_DUNGEON:
        cave(s, ox + 11 * T, oy + 1 * T, 4, 4)
        t_rock(s, ox + 1 * T, oy + 2 * T)
    elif mtype == MASS_GATE:
        s.fill_rect(ox + 10 * T, oy + 2 * T, 6, 24, C_RED_D)
        s.fill_rect(ox + 13 * T, oy + 2 * T, 6, 24, C_RED_D)
        s.fill_rect(ox + 10 * T - 2, oy + 1 * T, 30, 6, C_RED)
    else:
        t_tree(s, ox + 2 * T, oy + 1 * T)
        t_tree(s, ox + 13 * T, oy + 9 * T)


# ---------------------------------------------------------------------------
#  シーン
# ---------------------------------------------------------------------------

PANEL_X, PANEL_W = 400, 240
BOTTOM_Y = 304

# 3x3 の内容 (type, terrain, owner, lv)
SCENE = [
    [(MASS_TREASURE, 'g', None, 1), (MASS_VILLAGE, 'g', 2, 2), (MASS_PLAIN, 'F', None, 1)],
    [(MASS_ITEM,     'g', None, 1), (MASS_VILLAGE, 'g', 0, 3), (MASS_BATTLE, 'M', None, 1)],
    [(MASS_PLAIN,    'g', None, 1), (MASS_CHURCH,  'g', None, 1), (MASS_COLLECT, 'g', None, 1)],
]
# 接続 (右/下があるか)
LINK_E = [[1, 1, 0], [1, 1, 0], [1, 1, 0]]
LINK_S = [[0, 1, 0], [0, 1, 1], [0, 0, 0]]


def draw_field(s, char_w, char_h, label=None):
    ox0, oy0 = 8, 8
    for r in range(3):
        for c in range(3):
            mtype, terr, owner, lv = SCENE[r][c]
            ox = ox0 + c * CELL_W
            oy = oy0 + r * CELL_H
            draw_cell(s, ox, oy, mtype, terr, owner, lv,
                      open_n=LINK_S[r - 1][c] if r > 0 else 0,
                      open_s=LINK_S[r][c],
                      open_w=LINK_E[r][c - 1] if c > 0 else 0,
                      open_e=LINK_E[r][c],
                      seed=r * 7 + c * 3)

    # 現在マス (中央) を控えめに示す: 角の4隅マーカー
    cx, cy = ox0 + CELL_W, oy0 + CELL_H
    for (dx, dy, hx, hy) in ((0, 0, 1, 1), (CELL_W - 8, 0, -1, 1),
                             (0, CELL_H - 8, 1, -1),
                             (CELL_W - 8, CELL_H - 8, -1, -1)):
        s.fill_rect(cx + dx + (0 if hx > 0 else 6), cy + dy, 2, 8, C_WHITE)
        s.fill_rect(cx + dx, cy + dy + (0 if hy > 0 else 6), 8, 2, C_WHITE)

    # キャラクタ: 中央マスに2人、右下マスに1人
    draw_char(s, cx + 40, cy + CELL_H - 10, P_COLOR[0], char_w, char_h)
    draw_char(s, cx + 40 + char_w + 4, cy + CELL_H - 10, P_COLOR[1],
              char_w, char_h)
    draw_char(s, ox0 + 2 * CELL_W + 64, oy0 + 2 * CELL_H + CELL_H - 10,
              P_COLOR[2], char_w, char_h)

    s.rect(0, 0, FIELD_W, FIELD_H, C_WHITE)

    if label:
        s.fill_rect(4, FIELD_H - 16, 8 * len(label) + 8, 12, C_BLACK)
        s.text8(8, FIELD_H - 14, label, C_YELLOW)


def draw_panel(s):
    s.fill_rect(PANEL_X, 0, PANEL_W, BOTTOM_Y, C_BLUE_D)
    s.fill_rect(PANEL_X, 0, 1, BOTTOM_Y, C_WHITE)
    s.fill_rect(PANEL_X + 4, 4, PANEL_W - 8, 14, C_RED_D)
    s.text8(PANEL_X + 8, 7, "W6 SAT   TURN 23", C_WHITE)
    names = ['Susanoo', 'Y.Takeru', 'Okuninushi', 'Amaterasu']
    y = 24
    for i, nm in enumerate(names):
        if i == 0:
            s.fill_rect(PANEL_X + 3, y - 2, PANEL_W - 6, 66, C_BLUE)
        s.fill_rect(PANEL_X + 8, y, 8, 10, P_COLOR[i])
        s.rect(PANEL_X + 8, y, 8, 10, C_WHITE)
        s.text8(PANEL_X + 22, y + 1, nm, C_WHITE)
        s.text8(PANEL_X + 8, y + 16, "Lv%-2d HP %3d/%3d" % (4 - i, 60, 78),
                C_WHITE)
        s.text8(PANEL_X + 8, y + 32, "G %-6d  VIL %d" % (1113, 3 - i),
                C_YELLOW)
        s.fill_rect(PANEL_X + 4, y + 60, PANEL_W - 8, 1, C_GRAY)
        y += 66


def draw_bottom(s):
    s.fill_rect(0, BOTTOM_Y, 640, 400 - BOTTOM_Y, C_BLUE_D)
    s.rect(2, BOTTOM_Y + 2, 636, 400 - BOTTOM_Y - 6, C_WHITE)
    s.fill_rect(5, BOTTOM_Y + 5, 630, 14, C_RED_D)
    s.text8(10, BOTTOM_Y + 8, "Susanoo", C_WHITE)
    s.text8(552, BOTTOM_Y + 8, "YOUR TURN", C_YELLOW)
    s.text16(16, BOTTOM_Y + 28, "Roll the dice to move.", C_WHITE)
    s.text8(16, BOTTOM_Y + 56, "[R] ROLL   [W] SAVE   [ESC] QUIT", C_GRAY)


def scene(char_w, char_h, label):
    s = Screen(C_BLACK)
    draw_field(s, char_w, char_h, label)
    draw_panel(s)
    draw_bottom(s)
    return s


CHAR_SIZES = [(16, 16, 'A: 16x16'), (16, 24, 'B: 16x24'), (24, 32, 'C: 24x32')]


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for w, h, label in CHAR_SIZES:
        name = 'map_char_%dx%d.png' % (w, h)
        scene(w, h, label).save(os.path.join(OUT, name))
        print('wrote', name)
