#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_map2.py — 退色レトロ地図風の盤面モック

前案 (uimock_map.py) との違い:
  - パレットをカーネル既定の原色から、退色したモスグリーン系へ差し替え
    (PC-98 のパレットは gfx_set_palette で書き換えられる)
  - マップ背景は「海 + 陸3段」の4色だけで組む。
    地形の描き分けは色数ではなくタイルの模様で行う
  - 建物とキャラだけ彩度のある色を使い、地図の上で確実に浮かせる
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import uimock
import uimock_palette as PAL
from uimock_palette import (PAL_BLACK, PAL_SEA_DEEP, PAL_SEA_SHALLOW,
                            PAL_LAND_LOW, PAL_LAND_MID, PAL_LAND_HIGH,
                            PAL_ROAD, PAL_ROCK, PAL_FOREST, PAL_P_BLUE,
                            PAL_P_RED, PAL_MAGIC, PAL_P_GREEN, PAL_SKY,
                            PAL_P_YELLOW, PAL_WHITE)

# uimock のパレットを差し替えてから Screen を使う
uimock.PALETTE = PAL.to_rgb888(PAL.RETRO_MAP)
from uimock import Screen   # noqa: E402  (差し替え後に import する)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'docs', 'tasks', 'game', 'ui')

T = 8
CELL_TW, CELL_TH = 16, 12
CELL_W, CELL_H = CELL_TW * T, CELL_TH * T
FIELD_W, FIELD_H = 400, 304
PANEL_X, PANEL_W, BOTTOM_Y = 400, 240, 304

P_COLOR = [PAL_P_RED, PAL_P_BLUE, PAL_P_GREEN, PAL_P_YELLOW]


# ---------------------------------------------------------------------------
#  地形タイル — 背景4色のみ。模様で描き分ける
# ---------------------------------------------------------------------------

def t_sea(s, x, y, v=0):
    s.fill_rect(x, y, T, T, PAL_SEA_DEEP)
    if v % 5 == 0:
        s.fill_rect(x + 1, y + 3, 4, 1, PAL_SEA_SHALLOW)
    elif v % 5 == 2:
        s.fill_rect(x + 3, y + 6, 4, 1, PAL_SEA_SHALLOW)


def t_coast(s, x, y, v=0):
    """波打ち際。海と陸のあいだに1タイル挟む"""
    s.fill_rect(x, y, T, T, PAL_SEA_SHALLOW)
    s.fill_rect(x, y, T, 3, PAL_LAND_LOW)


def t_plain(s, x, y, v=0):
    """草地。面積が一番広いので模様は最小限に"""
    s.fill_rect(x, y, T, T, PAL_LAND_MID)
    if v % 5 == 0:
        s.pixel(x + 2, y + 5, PAL_LAND_HIGH)
        s.pixel(x + 3, y + 4, PAL_LAND_HIGH)
    elif v % 5 == 3:
        s.pixel(x + 5, y + 2, PAL_LAND_LOW)


def t_hill(s, x, y, v=0):
    """丘。明るい陸色でうねりを描く"""
    s.fill_rect(x, y, T, T, PAL_LAND_MID)
    s.fill_rect(x + 1, y + 2, 6, 2, PAL_LAND_HIGH)
    s.fill_rect(x + 2, y + 4, 4, 1, PAL_LAND_LOW)


def t_forest(s, x, y, v=0):
    """森。全タイルを埋めると壁に見えるので、草地ベースにまばらに木を置く"""
    t_plain(s, x, y, v)
    if v % 3 != 0:
        return
    # 1本の木 (樹冠 + 幹)
    s.fill_rect(x + 2, y + 1, 4, 3, PAL_FOREST)
    s.fill_rect(x + 1, y + 2, 6, 2, PAL_FOREST)
    s.pixel(x + 3, y + 2, PAL_LAND_MID)          # 陰影
    s.fill_rect(x + 3, y + 4, 2, 2, PAL_LAND_LOW)


def t_rock(s, x, y, v=0):
    """山地。稜線を持つ山を、隣り合うタイルにまたがらせずまばらに置く"""
    t_plain(s, x, y, v)
    if v % 4 != 0:
        return
    # 山ひとつ (稜線 + 陰 + 頂の明るみ)
    for i in range(5):
        s.fill_rect(x + 3 - i // 2, y + 2 + i, 1 + i, 1, PAL_ROCK)
    s.fill_rect(x + 3, y + 3, 2, 3, PAL_LAND_HIGH)
    s.pixel(x + 3, y + 2, PAL_WHITE)


def t_road(s, x, y, v=0):
    s.fill_rect(x, y, T, T, PAL_ROAD)
    if v % 3 == 0:
        s.pixel(x + 2, y + 3, PAL_LAND_LOW)
    elif v % 3 == 1:
        s.pixel(x + 5, y + 6, PAL_LAND_LOW)


def t_snow(s, x, y, v=0):
    s.fill_rect(x, y, T, T, PAL_LAND_HIGH)
    s.fill_rect(x + 1, y + 4, 3, 1, PAL_WHITE)
    if v % 2:
        s.fill_rect(x + 4, y + 2, 3, 1, PAL_WHITE)


def t_swamp(s, x, y, v=0):
    s.fill_rect(x, y, T, T, PAL_LAND_LOW)
    if v % 2:
        s.fill_rect(x + 1, y + 4, 3, 1, PAL_MAGIC)
    else:
        s.fill_rect(x + 4, y + 2, 3, 1, PAL_MAGIC)


TERR = {'g': t_plain, 'h': t_hill, 'F': t_forest, 'M': t_rock,
        'w': t_snow, 'p': t_swamp, 's': t_sea, 'c': t_coast, 'r': t_road}


# ---------------------------------------------------------------------------
#  建物 — 少ない色数で、屋根の色だけ所有者を示す
# ---------------------------------------------------------------------------

def house(s, x, y, roof, w=4, h=4):
    pw, ph = w * T, h * T
    wall_h = ph // 2
    s.fill_rect(x + 3, y + ph - wall_h, pw - 6, wall_h - 2, PAL_LAND_HIGH)
    s.rect(x + 3, y + ph - wall_h, pw - 6, wall_h - 2, PAL_BLACK)
    for i in range(wall_h - 2):
        s.fill_rect(x + 1 + i, y + ph - wall_h - (wall_h - 2) + i,
                    pw - 2 - i * 2, 1, roof)
    s.fill_rect(x + pw // 2 - 2, y + ph - wall_h + 3, 4, wall_h - 5, PAL_BLACK)


def shop(s, x, y, accent, w=5, h=4):
    pw, ph = w * T, h * T
    s.fill_rect(x + 3, y + ph - 14, pw - 6, 14, PAL_ROCK)
    s.rect(x + 3, y + ph - 14, pw - 6, 14, PAL_BLACK)
    for i in range((pw - 6) // 5):
        s.fill_rect(x + 3 + i * 5, y + ph - 20, 5, 6,
                    accent if i % 2 == 0 else PAL_LAND_HIGH)
    s.fill_rect(x + 1, y + ph - 22, pw - 2, 2, accent)


def torii(s, x, y, w=5, h=4):
    pw, ph = w * T, h * T
    s.fill_rect(x + 5, y + 8, 4, ph - 10, PAL_P_RED)
    s.fill_rect(x + pw - 9, y + 8, 4, ph - 10, PAL_P_RED)
    s.fill_rect(x + 2, y + 4, pw - 4, 4, PAL_P_RED)
    s.fill_rect(x + 5, y + 12, pw - 10, 3, PAL_P_RED)


def castle(s, x, y, w=6, h=5):
    pw, ph = w * T, h * T
    s.fill_rect(x + 3, y + ph - 20, pw - 6, 20, PAL_ROCK)
    s.rect(x + 3, y + ph - 20, pw - 6, 20, PAL_BLACK)
    for i in range(3):
        bx = x + 4 + i * ((pw - 10) // 2)
        s.fill_rect(bx, y + ph - 28, 7, 8, PAL_LAND_HIGH)
        s.rect(bx, y + ph - 28, 7, 8, PAL_BLACK)
    s.fill_rect(x + pw // 2 - 3, y + ph - 11, 6, 11, PAL_BLACK)


def den(s, x, y, w=4, h=4):
    pw, ph = w * T, h * T
    for i in range(3):
        s.fill_rect(x + 4 + i * 8, y + ph - 8 - i % 2 * 3, 4, 8, PAL_ROCK)
    s.fill_rect(x + pw // 2 - 1, y + 4, 2, ph - 10, PAL_LAND_LOW)
    s.fill_rect(x + pw // 2 + 1, y + 4, pw // 2 - 3, 6, PAL_P_RED)


def chest(s, x, y, col, w=3, h=3):
    pw, ph = w * T, h * T
    s.fill_rect(x + 3, y + ph - 11, pw - 6, 11, col)
    s.rect(x + 3, y + ph - 11, pw - 6, 11, PAL_BLACK)
    for i in range(4):
        s.fill_rect(x + 3 + i, y + ph - 15 + i, pw - 6 - i * 2, 1, col)
    s.fill_rect(x + 3, y + ph - 7, pw - 6, 1, PAL_BLACK)


def cave(s, x, y, w=4, h=4):
    pw, ph = w * T, h * T
    for i in range(5):
        s.fill_rect(x + 2 - i, y + 6 + i * 3, 4 + i * 5, 3, PAL_ROCK)
    s.fill_circle(x + pw // 2, y + ph - 5, 7, PAL_BLACK)
    s.fill_rect(x + pw // 2 - 7, y + ph - 6, 14, 6, PAL_BLACK)


def office(s, x, y, w=5, h=4):
    pw, ph = w * T, h * T
    s.fill_rect(x + 3, y + ph - 16, pw - 6, 16, PAL_LAND_HIGH)
    s.rect(x + 3, y + ph - 16, pw - 6, 16, PAL_BLACK)
    s.fill_rect(x + 1, y + ph - 20, pw - 2, 4, PAL_P_YELLOW)
    s.text8(x + pw // 2 - 4, y + ph - 12, 'G', PAL_BLACK)


def circle_mark(s, x, y, w=5, h=4):
    pw, ph = w * T, h * T
    cx, cy = x + pw // 2, y + ph // 2
    for r in (13, 9, 5):
        s.circle(cx, cy, r, PAL_MAGIC)


# ---------------------------------------------------------------------------
#  キャラクタ 16x24
# ---------------------------------------------------------------------------

def draw_char(s, x, y, col, w=16, h=24, marker=0):
    px, py = x - w // 2, y - h
    head_h = 9

    s.fill_rect(px + 2, y - 1, w - 4, 2, PAL_BLACK)          # 影
    s.fill_rect(px + 1, py + head_h, w - 2, h - head_h - 3, col)
    s.rect(px + 1, py + head_h, w - 2, h - head_h - 3, PAL_BLACK)
    s.fill_rect(px + 3, py, w - 6, head_h, PAL_LAND_HIGH)    # 顔
    s.rect(px + 3, py, w - 6, head_h, PAL_BLACK)
    s.fill_rect(px + 3, py, w - 6, 3, PAL_BLACK)             # 髪
    s.pixel(px + 5, py + 6, PAL_BLACK)
    s.pixel(px + w - 6, py + 6, PAL_BLACK)
    s.fill_rect(px + 2, y - 3, 4, 3, PAL_BLACK)              # 足
    s.fill_rect(px + w - 6, y - 3, 4, 3, PAL_BLACK)

    if marker:   # 手番プレイヤーの頭上に ▼ (地形に紛れないよう縁取る)
        mx = px + w // 2
        for i in range(7):
            s.fill_rect(mx - 6 + i, py - 12 + i, 13 - i * 2, 1, PAL_BLACK)
        for i in range(5):
            s.fill_rect(mx - 4 + i, py - 11 + i, 9 - i * 2, 1, PAL_P_YELLOW)


# ---------------------------------------------------------------------------
#  マス
# ---------------------------------------------------------------------------

(V, B, CH, IT, EQ, MG, SH, CI, GT, CA, GC, CO, DU, PL) = (
    1, 2, 3, 5, 4, 6, 7, 8, 10, 11, 12, 13, 14, 0)


def draw_cell(s, ox, oy, mtype, terr='g', owner=None, lv=1,
              n=0, so=0, w=0, e=0, seed=0):
    base = TERR.get(terr, t_plain)
    for ty in range(CELL_TH):
        for tx in range(CELL_TW):
            base(s, ox + tx * T, oy + ty * T, tx * 3 + ty * 5 + seed)

    cx, cy = CELL_TW // 2, CELL_TH // 2
    for ty in range(CELL_TH):
        for tx in range(CELL_TW):
            on = (abs(tx - cx) <= 1 and abs(ty - cy) <= 1)
            if not on and abs(tx - cx) <= 1:
                on = (ty < cy and n) or (ty > cy and so)
            if not on and abs(ty - cy) <= 1:
                on = (tx < cx and w) or (tx > cx and e)
            if on:
                t_road(s, ox + tx * T, oy + ty * T, tx + ty)

    if mtype == V:
        roof = P_COLOR[owner] if owner is not None else PAL_ROCK
        house(s, ox + 1 * T, oy + 1 * T, roof, 4, 4)
        if lv >= 2: house(s, ox + 11 * T, oy + 1 * T, roof, 4, 4)
        if lv >= 3: house(s, ox + 11 * T, oy + 7 * T, roof, 4, 4)
        t_forest(s, ox + 6 * T, oy + 0 * T)
        t_forest(s, ox + 1 * T, oy + 9 * T)
    elif mtype == B:
        den(s, ox + 11 * T, oy + 1 * T)
        t_rock(s, ox + 1 * T, oy + 1 * T)
        t_rock(s, ox + 2 * T, oy + 9 * T)
    elif mtype == CH:
        chest(s, ox + 12 * T, oy + 2 * T, PAL_P_YELLOW)
        t_forest(s, ox + 1 * T, oy + 1 * T)
    elif mtype == GC:
        chest(s, ox + 12 * T, oy + 2 * T, PAL_MAGIC)
    elif mtype in (IT, EQ, MG):
        shop(s, ox + 10 * T, oy + 1 * T,
             {IT: PAL_SKY, EQ: PAL_P_BLUE, MG: PAL_MAGIC}[mtype])
    elif mtype == SH:
        torii(s, ox + 10 * T, oy + 1 * T)
        t_forest(s, ox + 1 * T, oy + 1 * T)
    elif mtype == CI:
        circle_mark(s, ox + 10 * T, oy + 1 * T)
    elif mtype == CA:
        castle(s, ox + 9 * T, oy + 1 * T)
    elif mtype == CO:
        office(s, ox + 10 * T, oy + 1 * T)
    elif mtype == DU:
        cave(s, ox + 11 * T, oy + 1 * T)
    elif mtype == GT:
        s.fill_rect(ox + 10 * T, oy + 2 * T, 6, 26, PAL_P_RED)
        s.fill_rect(ox + 13 * T, oy + 2 * T, 6, 26, PAL_P_RED)
        s.fill_rect(ox + 10 * T - 3, oy + 1 * T, 32, 6, PAL_P_RED)
    else:
        t_forest(s, ox + 2 * T, oy + 1 * T)
        t_forest(s, ox + 13 * T, oy + 9 * T)


SCENE = [
    [(CH, 'F', None, 1), (V,  'g', 2, 2),    (PL, 'M', None, 1)],
    [(IT, 'g', None, 1), (V,  'g', 0, 3),    (B,  'M', None, 1)],
    [(PL, 's', None, 1), (SH, 'h', None, 1), (CO, 'g', None, 1)],
]
LINK_E = [[1, 1, 0], [1, 1, 0], [0, 1, 0]]
LINK_S = [[0, 1, 0], [0, 1, 1], [0, 0, 0]]


def build():
    s = Screen(PAL_SEA_DEEP)
    ox0, oy0 = 8, 8
    for r in range(3):
        for c in range(3):
            mt, tr, ow, lv = SCENE[r][c]
            draw_cell(s, ox0 + c * CELL_W, oy0 + r * CELL_H, mt, tr, ow, lv,
                      n=LINK_S[r - 1][c] if r > 0 else 0, so=LINK_S[r][c],
                      w=LINK_E[r][c - 1] if c > 0 else 0, e=LINK_E[r][c],
                      seed=r * 7 + c * 3)

    cx, cy = ox0 + CELL_W, oy0 + CELL_H
    draw_char(s, cx + 44, cy + CELL_H - 12, P_COLOR[0], marker=1)
    draw_char(s, cx + 68, cy + CELL_H - 12, P_COLOR[1])
    draw_char(s, ox0 + 2 * CELL_W + 64, oy0 + 2 * CELL_H + CELL_H - 12,
              P_COLOR[2])

    s.rect(0, 0, FIELD_W, FIELD_H, PAL_WHITE)

    # 右パネル
    s.fill_rect(PANEL_X, 0, PANEL_W, BOTTOM_Y, PAL_LAND_LOW)
    s.fill_rect(PANEL_X, 0, 1, BOTTOM_Y, PAL_WHITE)
    s.fill_rect(PANEL_X + 4, 4, PANEL_W - 8, 14, PAL_ROCK)
    s.text8(PANEL_X + 8, 7, 'W6 SAT   TURN 23', PAL_WHITE)
    names = ['Susanoo', 'Y.Takeru', 'Okuninushi', 'Amaterasu']
    y = 24
    for i, nm in enumerate(names):
        if i == 0:
            s.fill_rect(PANEL_X + 3, y - 2, PANEL_W - 6, 66, PAL_LAND_MID)
        s.fill_rect(PANEL_X + 8, y, 8, 10, P_COLOR[i])
        s.rect(PANEL_X + 8, y, 8, 10, PAL_WHITE)
        s.text8(PANEL_X + 22, y + 1, nm, PAL_WHITE)
        s.text8(PANEL_X + 8, y + 16, 'Lv%-2d HP %3d/%3d' % (4 - i, 60, 78),
                PAL_WHITE)
        s.text8(PANEL_X + 8, y + 32, 'G %-6d  VIL %d' % (1113, 3 - i),
                PAL_P_YELLOW)
        s.fill_rect(PANEL_X + 4, y + 60, PANEL_W - 8, 1, PAL_ROCK)
        y += 66

    # 下段
    s.fill_rect(0, BOTTOM_Y, 640, 400 - BOTTOM_Y, PAL_LAND_LOW)
    s.rect(2, BOTTOM_Y + 2, 636, 400 - BOTTOM_Y - 6, PAL_WHITE)
    s.fill_rect(5, BOTTOM_Y + 5, 630, 14, PAL_ROCK)
    s.text8(10, BOTTOM_Y + 8, 'Susanoo', PAL_WHITE)
    s.text8(552, BOTTOM_Y + 8, 'YOUR TURN', PAL_P_YELLOW)
    s.text16(16, BOTTOM_Y + 28, 'Roll the dice to move.', PAL_WHITE)
    s.text8(16, BOTTOM_Y + 56, '[R] ROLL   [W] SAVE   [ESC] QUIT', PAL_ROCK)
    return s


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, 'map_retro.png')
    build().save(p)
    print('wrote', os.path.normpath(p))
