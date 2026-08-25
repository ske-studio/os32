#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_world.py — ワールドマップ試作の可視化

  render_overview() : 全 252 マスの俯瞰図 (設計確認用。ゲーム内画面ではない)
  --ingame          : 新マップ上での 3x3 ゲーム画面モック
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageDraw

import gen_worldmap as W
from uimock import (Screen, win, gauge, cursor, clip_text,
                    PALETTE, C_BLACK, C_BLUE_D, C_RED_D, C_GREEN_D, C_CYAN_D,
                    C_YELLOW_D, C_GRAY, C_BLUE, C_RED, C_GREEN, C_YELLOW,
                    C_MAGENTA, C_CYAN, C_WHITE)

PROJ_DIR = W.PROJ_DIR
OUT = os.path.join(PROJ_DIR, 'docs', 'tasks', 'game', 'ui')

# マス種別 -> (色, 1文字)
STYLE = {
    W.MASS_EMPTY:       (C_GRAY,     ''),
    W.MASS_VILLAGE:     (C_GREEN,    'V'),
    W.MASS_BATTLE:      (C_RED,      '!'),
    W.MASS_TREASURE:    (C_YELLOW,   'B'),
    W.MASS_MAGIC_CHEST: (C_MAGENTA,  'Y'),
    W.MASS_EQUIP_SHOP:  (C_BLUE,     'E'),
    W.MASS_ITEM_SHOP:   (C_CYAN,     'I'),
    W.MASS_MAGIC_SHOP:  (C_MAGENTA,  'A'),
    W.MASS_CHURCH:      (C_CYAN_D,   'C'),
    W.MASS_CIRCLE:      (C_MAGENTA,  'O'),
    W.MASS_GATE:        (C_YELLOW_D, 'G'),
    W.MASS_CASTLE:      (C_WHITE,    'K'),
    W.MASS_COLLECT:     (C_YELLOW,   '$'),
    W.MASS_DUNGEON:     (C_RED_D,    '^'),
}
TERR_COLOR = {
    W.TERR_PLAIN:  C_GREEN_D,
    W.TERR_FOREST: C_GREEN_D,
    W.TERR_ROCK:   C_GRAY,
    W.TERR_DESERT: C_YELLOW_D,
    W.TERR_SWAMP:  C_MAGENTA_D if hasattr(W, 'x') else 3,
    W.TERR_SNOW:   C_WHITE,
}


# ---------------------------------------------------------------------------
#  俯瞰図 (設計確認用。画面サイズの制約は無視して大きく描く)
# ---------------------------------------------------------------------------

def render_overview(masses, conns, gate_conns, path, cell=18):
    xs = [m['x'] for m in masses]
    ys = [m['y'] for m in masses]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    pad = 2
    W_px = (x1 - x0 + 1 + pad * 2) * cell
    H_px = (y1 - y0 + 1 + pad * 2) * cell + 40

    im = Image.new('RGB', (W_px, H_px), (10, 10, 20))
    d = ImageDraw.Draw(im)

    def px(m):
        return ((m['x'] - x0 + pad) * cell + cell // 2,
                (m['y'] - y0 + pad) * cell + cell // 2 + 30)

    by_id = {m['id']: m for m in masses}

    # 隣接接続
    for (a, b, _) in conns:
        d.line([px(by_id[a]), px(by_id[b])], fill=(90, 90, 90), width=2)
    # ゲート接続 (離れた場所どうし)
    for (a, b, _) in gate_conns:
        d.line([px(by_id[a]), px(by_id[b])], fill=(200, 170, 40), width=1)

    for m in masses:
        cx, cy = px(m)
        color, mark = STYLE.get(m['type'], (C_GRAY, ''))
        r = cell // 2 - 2
        d.ellipse([cx - r, cy - r, cx + r, cy + r],
                  fill=PALETTE[color], outline=(255, 255, 255))
        if mark:
            d.text((cx - 3, cy - 5), mark, fill=(0, 0, 0))

    # 区画名
    for reg in W.REGIONS:
        ox, oy = reg['origin']
        tx = (ox - x0 + pad) * cell
        ty = (oy - y0 + pad) * cell + 30 - 14
        d.text((tx, ty), '%d %s' % (reg['area'], reg['name']),
               fill=(255, 255, 120))

    d.text((8, 8), 'OS32 SUGOROKU RPG - WORLD MAP (draft)  '
                   '%d masses / %d villages / 9 regions'
           % (len(masses), sum(1 for m in masses
                               if m['type'] == W.MASS_VILLAGE)),
           fill=(255, 255, 255))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    return path


# ---------------------------------------------------------------------------
#  ゲーム内 3x3 ビュー (640x400 / 16色。実機と同じ制約)
# ---------------------------------------------------------------------------

TILE = 16
FIELD_W, FIELD_H = 400, 304
CELL_TW, CELL_TH = 8, 6
CELL_W, CELL_H = CELL_TW * TILE, CELL_TH * TILE
PANEL_X, PANEL_W, PANEL_H = 400, 240, 304
BOTTOM_Y, BOTTOM_H = 304, 96

P_COLOR = [C_RED, C_BLUE, C_GREEN, C_YELLOW]
PLAYERS = [
    dict(name='Susanoo',    lv=4, hp=62, mhp=78, gold=1113, est=3, devil=0),
    dict(name='Y.Takeru',   lv=3, hp=41, mhp=66, gold=820,  est=2, devil=0),
    dict(name='Okuninushi', lv=5, hp=88, mhp=90, gold=2140, est=5, devil=0),
    dict(name='Amaterasu',  lv=2, hp=12, mhp=54, gold=310,  est=0, devil=1),
]
WEEK, TURN = 6, 23

NAME = {
    W.MASS_EMPTY: 'PLAIN', W.MASS_VILLAGE: 'VILLAGE', W.MASS_BATTLE: 'MONSTER',
    W.MASS_TREASURE: 'CHEST', W.MASS_MAGIC_CHEST: 'GOLD CHEST',
    W.MASS_EQUIP_SHOP: 'WEAPON', W.MASS_ITEM_SHOP: 'ITEM',
    W.MASS_MAGIC_SHOP: 'MAGIC', W.MASS_CHURCH: 'SHRINE',
    W.MASS_CIRCLE: 'CIRCLE', W.MASS_GATE: 'GATE', W.MASS_CASTLE: 'CASTLE',
    W.MASS_COLLECT: 'COLLECT', W.MASS_DUNGEON: 'CAVE',
}
TERR_LABEL = {W.TERR_PLAIN: '', W.TERR_FOREST: 'FOREST', W.TERR_ROCK: 'ROCK',
              W.TERR_DESERT: 'DESERT', W.TERR_SWAMP: 'POISON SWAMP',
              W.TERR_SNOW: 'SNOW'}


def t_grass(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_GREEN_D)
    for (dx, dy) in ((3, 4), (11, 6), (6, 11), (13, 13)):
        s.pixel(x + dx, y + dy, C_GREEN)
        s.pixel(x + dx, y + dy - 1, C_GREEN)


def t_forest(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_GREEN_D)
    for (bx, by) in ((3, 4), (10, 8)):
        for i in range(4):
            s.fill_rect(x + bx - i, y + by + i, 2 * i + 2, 1, C_GREEN)
        s.fill_rect(x + bx, y + by + 4, 2, 3, C_YELLOW_D)


def t_rock(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_GRAY)
    s.fill_rect(x + 3, y + 7, 10, 6, C_BLACK)
    s.fill_rect(x + 4, y + 6, 8, 6, C_GRAY)


def t_desert(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_YELLOW_D)
    for dy in (4, 10):
        s.fill_rect(x + 2, y + dy, 6, 1, C_YELLOW)
        s.fill_rect(x + 9, y + dy + 3, 5, 1, C_YELLOW)


def t_swamp(s, x, y):
    s.fill_rect(x, y, TILE, TILE, 3)          # 暗紫
    for (dx, dy) in ((3, 5), (9, 9), (6, 12)):
        s.fill_circle(x + dx, y + dy, 2, C_MAGENTA)


def t_snow(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_WHITE)
    for (dx, dy) in ((4, 4), (11, 7), (7, 12)):
        s.pixel(x + dx, y + dy, C_CYAN)
        s.pixel(x + dx + 1, y + dy, C_CYAN)


def t_sea(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_BLUE_D)
    for dy in (4, 11):
        s.fill_rect(x + 2, y + dy, 5, 1, C_BLUE)
        s.fill_rect(x + 9, y + dy + 2, 5, 1, C_BLUE)


def t_road(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_YELLOW_D)
    for (dx, dy) in ((2, 3), (9, 5), (5, 12), (12, 10)):
        s.pixel(x + dx, y + dy, C_GRAY)


TERR_TILE = {W.TERR_PLAIN: t_grass, W.TERR_FOREST: t_forest,
             W.TERR_ROCK: t_rock, W.TERR_DESERT: t_desert,
             W.TERR_SWAMP: t_swamp, W.TERR_SNOW: t_snow}


def draw_mass(s, x, y, m, is_cur, occ, owner=None, lv=1):
    color, _ = STYLE.get(m['type'], (C_GRAY, ''))
    px, py, pw, ph = x + 14, y + 16, 100, 60

    s.fill_rect(px + 3, py + 3, pw, ph, C_BLACK)
    s.fill_rect(px, py, pw, ph, C_BLUE_D)
    s.rect(px, py, pw, ph, C_WHITE if is_cur else C_GRAY)
    if is_cur:
        s.rect(px - 2, py - 2, pw + 4, ph + 4, C_YELLOW)

    s.fill_rect(px + 2, py + 2, pw - 4, 10, color)
    dark = color in (C_YELLOW, C_WHITE, C_CYAN, C_GREEN)
    s.text8(px + 5, py + 3, clip_text(NAME.get(m['type'], '?'), pw - 10),
            C_BLACK if dark else C_WHITE)
    s.text8(px + pw - 26, py + ph - 10, '#%-3d' % m['id'], C_GRAY)

    cx, cy = px + pw // 2, py + 34
    t = m['type']
    if t == W.MASS_VILLAGE:
        roof = P_COLOR[owner] if owner is not None else C_GRAY
        s.fill_rect(cx - 14, cy - 2, 28, 13, C_YELLOW_D)
        s.rect(cx - 14, cy - 2, 28, 13, C_BLACK)
        for i in range(8):
            s.fill_rect(cx - 14 + i, cy - 2 - i, 28 - i * 2, 1, roof)
        s.fill_rect(cx - 4, cy + 4, 8, 7, C_BLACK)
        s.text8(px + 5, py + ph - 10, 'Lv%d' % lv, C_YELLOW)
    elif t == W.MASS_BATTLE:
        for i in range(5):
            s.fill_rect(cx - 10 + i * 5, cy - 6 + (i % 2) * 4, 3, 12, C_RED)
    elif t in (W.MASS_TREASURE, W.MASS_MAGIC_CHEST):
        col = C_YELLOW if t == W.MASS_TREASURE else C_MAGENTA
        s.fill_rect(cx - 12, cy - 4, 24, 15, col)
        s.rect(cx - 12, cy - 4, 24, 15, C_BLACK)
        s.fill_rect(cx - 12, cy + 2, 24, 2, C_BLACK)
    elif t in (W.MASS_ITEM_SHOP, W.MASS_EQUIP_SHOP, W.MASS_MAGIC_SHOP):
        s.fill_rect(cx - 14, cy - 2, 28, 13, C_GRAY)
        s.rect(cx - 14, cy - 2, 28, 13, C_BLACK)
        s.fill_rect(cx - 16, cy - 8, 32, 3, color)
        for i in range(7):
            s.fill_rect(cx - 16 + i * 5, cy - 5, 3, 4, color)
    elif t == W.MASS_CHURCH:
        s.fill_rect(cx - 12, cy + 1, 24, 10, C_CYAN_D)
        s.fill_rect(cx - 14, cy - 5, 28, 3, C_CYAN)
        s.fill_rect(cx - 3, cy - 10, 6, 8, C_CYAN)
        s.fill_rect(cx - 8, cy - 8, 16, 3, C_CYAN)
    elif t == W.MASS_CASTLE:
        s.fill_rect(cx - 16, cy - 2, 32, 13, C_GRAY)
        for i in range(4):
            s.fill_rect(cx - 16 + i * 9, cy - 8, 6, 6, C_WHITE)
        s.fill_rect(cx - 4, cy + 4, 8, 7, C_BLACK)
    elif t == W.MASS_COLLECT:
        s.fill_rect(cx - 12, cy - 2, 24, 13, C_YELLOW_D)
        s.rect(cx - 12, cy - 2, 24, 13, C_BLACK)
        s.text8(cx - 4, cy + 1, 'G', C_YELLOW)
        s.fill_rect(cx - 14, cy - 6, 28, 4, C_YELLOW)
    elif t == W.MASS_GATE:
        s.fill_rect(cx - 14, cy - 6, 6, 17, C_RED_D)
        s.fill_rect(cx + 8, cy - 6, 6, 17, C_RED_D)
        s.fill_rect(cx - 16, cy - 9, 32, 4, C_RED)
    elif t == W.MASS_DUNGEON:
        s.fill_rect(cx - 14, cy - 2, 28, 13, C_GRAY)
        for i in range(7):
            s.fill_rect(cx - 7 + 0, cy - 2, 14, 13, C_BLACK)
        s.fill_circle(cx, cy + 2, 7, C_BLACK)
    elif t == W.MASS_CIRCLE:
        for r in (12, 8, 4):
            s.circle(cx, cy + 3, r, C_MAGENTA)

    # 地形ラベル (効果のある地形だけ)
    lbl = TERR_LABEL.get(m['terrain'], '')
    if lbl and m['terrain'] in (W.TERR_SWAMP, W.TERR_SNOW):
        s.fill_rect(px + 2, py + ph - 12, pw - 4, 10, C_RED_D)
        s.text8(px + 5, py + ph - 11, clip_text(lbl, pw - 10), C_WHITE)

    for n, pid in enumerate(occ):
        bx, by = px + 8 + n * 14, py + ph - 22
        s.fill_circle(bx, by, 5, P_COLOR[pid])
        s.circle(bx, by, 5, C_WHITE)
        if pid == 0:
            s.circle(bx, by, 7, C_YELLOW)


def render_ingame(masses, conns, center_id, occ_map, path):
    by_id = {m['id']: m for m in masses}
    grid = {(m['x'], m['y']): m for m in masses}
    linked = set()
    for (a, b, _) in conns:
        linked.add((a, b))
        linked.add((b, a))

    c = by_id[center_id]
    s = Screen(C_BLACK)

    cells = {}
    for row in range(3):
        for col in range(3):
            cells[(col, row)] = grid.get((c['x'] - 1 + col, c['y'] - 1 + row))

    # 地形 (25x19 タイル)
    for r in range(19):
        for cc in range(25):
            col, row = cc // CELL_TW, r // CELL_TH
            lc, lr = cc % CELL_TW, r % CELL_TH
            m = cells.get((col, row))
            x, y = cc * TILE, r * TILE
            if m is None:
                t_sea(s, x, y)
            elif 1 <= lr <= CELL_TH - 2 and 1 <= lc <= CELL_TW - 2:
                t_road(s, x, y)
            else:
                TERR_TILE.get(m['terrain'], t_grass)(s, x, y)

    # 接続 (実データ)
    for (col, row), m in cells.items():
        if m is None:
            continue
        cx = col * CELL_W + CELL_W // 2
        cy = row * CELL_H + CELL_H // 2
        for (dc, dr) in ((1, 0), (0, 1)):
            n = cells.get((col + dc, row + dr))
            if n is None or (m['id'], n['id']) not in linked:
                continue
            if dc:
                s.fill_rect(cx, cy - 3, CELL_W, 7, C_YELLOW_D)
            else:
                s.fill_rect(cx - 3, cy, 7, CELL_H, C_YELLOW_D)

    for (col, row), m in cells.items():
        if m is None:
            continue
        draw_mass(s, col * CELL_W, row * CELL_H, m, m['id'] == center_id,
                  occ_map.get(m['id'], []),
                  owner={0: 2, 1: 0}.get(m['id'] % 7),
                  lv=1 + m['id'] % 3)

    s.rect(0, 0, FIELD_W, FIELD_H, C_WHITE)

    # 右パネル
    x, w = PANEL_X, PANEL_W
    s.fill_rect(x, 0, w, PANEL_H, C_BLUE_D)
    s.fill_rect(x, 0, 1, PANEL_H, C_WHITE)
    s.fill_rect(x + 4, 4, w - 8, 14, C_RED_D)
    s.text8(x + 8, 7, 'WEEK %-2d      TURN %-3d' % (WEEK, TURN), C_WHITE)

    def assets(p):
        return p['gold'] + p['est'] * 640

    order = sorted(range(4), key=lambda i: -assets(PLAYERS[i]))
    y = 24
    for rank, i in enumerate(order):
        p = PLAYERS[i]
        if i == 0:
            s.fill_rect(x + 3, y - 2, w - 6, 66, C_BLUE)
        s.text8(x + 8, y + 1, '%d' % (rank + 1), C_YELLOW)
        s.fill_rect(x + 20, y, 8, 10, P_COLOR[i])
        s.rect(x + 20, y, 8, 10, C_WHITE)
        s.text8(x + 34, y + 1, clip_text(p['name'], 120), C_WHITE)
        if p['devil']:
            s.fill_rect(x + w - 34, y, 26, 10, C_RED)
            s.text8(x + w - 32, y + 1, 'DEV', C_WHITE)
        s.text8(x + 8, y + 16, 'Lv%-2d' % p['lv'], C_YELLOW)
        hpc = C_RED if p['hp'] * 4 <= p['mhp'] else (
            C_YELLOW if p['hp'] * 2 <= p['mhp'] else C_GREEN)
        gauge(s, x + 44, y + 15, 120, 10, p['hp'], p['mhp'], hpc)
        s.text8(x + 170, y + 16, '%3d' % p['hp'], C_WHITE)
        s.text8(x + 8, y + 30, 'GOLD', C_YELLOW)
        s.text8(x + 50, y + 30, '%-7d' % p['gold'], C_WHITE)
        s.text8(x + 130, y + 30, 'VIL', C_YELLOW)
        s.text8(x + 162, y + 30, '%-2d' % p['est'], C_WHITE)
        s.text8(x + 8, y + 44, 'ASSET', C_YELLOW)
        s.text8(x + 58, y + 44, '%-8d' % assets(p),
                C_YELLOW if rank == 0 else C_WHITE)
        s.fill_rect(x + 4, y + 60, w - 8, 1, C_GRAY)
        y += 66

    # 下段: 分岐選択 (実データの接続先から)
    opts = [n for n in (b for (a, b, _) in conns if a == center_id)]
    opts += [a for (a, b, _) in conns if b == center_id]
    opts = sorted(set(opts))[:3]

    win(s, 2, BOTTOM_Y + 2, 636, BOTTOM_H - 6, shadow=False)
    s.fill_rect(5, BOTTOM_Y + 5, 630, 14, C_RED_D)
    s.text8(10, BOTTOM_Y + 8, 'WHICH WAY?', C_WHITE)
    s.text8(520, BOTTOM_Y + 8, '3 STEPS LEFT', C_YELLOW)
    for i, oid in enumerate(opts):
        m = by_id[oid]
        ox, oy = 12 + i * 208, BOTTOM_Y + 26
        if i == 0:
            s.fill_rect(ox - 4, oy - 3, 202, 56, C_BLUE)
            s.rect(ox - 4, oy - 3, 202, 56, C_YELLOW)
            cursor(s, ox, oy + 2, C_YELLOW)
        s.text8(ox + 12, oy + 6, '%d.' % (i + 1), C_YELLOW)
        col, _ = STYLE.get(m['type'], (C_GRAY, ''))
        s.fill_rect(ox + 30, oy + 2, 78, 12, col)
        s.text8(ox + 33, oy + 4, clip_text(NAME.get(m['type'], '?'), 74),
                C_BLACK if col in (C_YELLOW, C_WHITE, C_CYAN, C_GREEN)
                else C_WHITE)
        s.text8(ox + 116, oy + 4, '#%-3d' % m['id'], C_GRAY)
        s.text8(ox + 12, oy + 22, 'AREA', C_YELLOW)
        s.text8(ox + 60, oy + 22, clip_text(m['region'], 130), C_WHITE)
        tl = TERR_LABEL.get(m['terrain'], '')
        if tl:
            s.text8(ox + 12, oy + 36, 'TERR', C_YELLOW)
            s.text8(ox + 60, oy + 36, clip_text(tl, 130),
                    C_RED if m['terrain'] in (W.TERR_SWAMP, W.TERR_SNOW)
                    else C_WHITE)
    s.text8(416, BOTTOM_Y + 74, '[1/2/3] SELECT  [ENTER] GO', C_GRAY)

    s.save(path)
    return path


if __name__ == '__main__':
    masses, conns, gate_conns, ids, cells = W.build()
    os.makedirs(OUT, exist_ok=True)

    p = render_overview(masses, conns, gate_conns,
                        os.path.join(OUT, 'worldmap.png'))
    print('overview ->', os.path.normpath(p))

    # 分岐のあるマスを中心に選ぶ
    from collections import Counter
    deg = Counter()
    for (a, b, _) in conns:
        deg[a] += 1
        deg[b] += 1
    by_id = {m['id']: m for m in masses}
    center = next(mid for mid, d in sorted(deg.items())
                  if d >= 3 and by_id[mid]['area'] == 1)
    occ = {center: [0], center + 1: [1]}
    p = render_ingame(masses, conns, center, occ,
                      os.path.join(OUT, 'worldmap_ingame.png'))
    print('ingame   ->', os.path.normpath(p), '(center mass #%d)' % center)
