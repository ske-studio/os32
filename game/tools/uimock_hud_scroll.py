#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_hud_scroll.py — 案C 改: タイル盤面 + 3x3 マス表示 + スクロール

レイアウト (640x400):
  +------------------------------+----------------+
  |                              |                |
  |  タイル盤面 400x304          |  情報パネル    |
  |  25 x 19 タイル (16px)       |  240x304       |
  |  うち可視 24x18 = 3x3 マス   |  4人順位/詳細  |
  |  (1列1行はスクロール余白)    |                |
  +------------------------------+----------------+
  |  下段窓 640x96  メッセージ / コマンド / 分岐選択 |
  +--------------------------------------------------+

1マス = 8x6 タイル = 128x96px。マスに地形と施設を描けるだけの面積がある。
駒が動くと盤面が 1 マス分 (128 or 96px) スクロールする。

libos32tilemap の現状の上限は TILEMAP_COLS/ROWS = 24 なので、
25 列にするには定数を 25 (縦は 19) へ広げる必要がある。
"""

import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uimock import (Screen, win, gauge, cursor, clip_text,
                    C_BLACK, C_BLUE_D, C_RED_D, C_GREEN_D, C_CYAN_D,
                    C_YELLOW_D, C_GRAY, C_BLUE, C_RED, C_GREEN, C_YELLOW,
                    C_MAGENTA, C_CYAN, C_WHITE, C_MAGENTA_D)

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(TOOLS_DIR)
OUT = os.path.join(PROJ_DIR, 'docs', 'tasks', 'game', 'ui')
BOARD_DB = os.path.join(PROJ_DIR, 'assets', 'board.db')

# --- レイアウト定数 (実装時にそのまま C の #define へ移せる) ---
TILE = 16
FIELD_X, FIELD_Y = 0, 0
FIELD_W, FIELD_H = 400, 304          # 25 x 19 タイル
VIEW_COLS, VIEW_ROWS = 24, 18        # 実際に見せる 3x3 マス分
CELL_TW, CELL_TH = 8, 6              # 1マス = 8x6 タイル
CELL_W, CELL_H = CELL_TW * TILE, CELL_TH * TILE   # 128 x 96

PANEL_X, PANEL_W = 400, 240
PANEL_H = 304
BOTTOM_Y, BOTTOM_H = 304, 96

MASS_EMPTY, MASS_VILLAGE, MASS_BATTLE, MASS_TREASURE = 0, 1, 2, 3
MASS_EQUIP_SHOP, MASS_ITEM_SHOP, MASS_MAGIC_SHOP = 4, 5, 6
MASS_CHURCH, MASS_CIRCLE, MASS_EVENT = 7, 8, 9
MASS_GATE, MASS_CASTLE, MASS_MAGIC_CHEST = 10, 11, 12

MASS_NAME = {
    MASS_EMPTY: 'PLAIN', MASS_VILLAGE: 'VILLAGE', MASS_BATTLE: 'MONSTER',
    MASS_TREASURE: 'CHEST', MASS_EQUIP_SHOP: 'WEAPON', MASS_ITEM_SHOP: 'ITEM',
    MASS_MAGIC_SHOP: 'MAGIC', MASS_CHURCH: 'SHRINE', MASS_CIRCLE: 'CIRCLE',
    MASS_EVENT: 'EVENT', MASS_GATE: 'GATE', MASS_CASTLE: 'CASTLE',
    MASS_MAGIC_CHEST: 'GOLD',
}
MASS_ACCENT = {
    MASS_EMPTY: C_GRAY, MASS_VILLAGE: C_GREEN, MASS_BATTLE: C_RED,
    MASS_TREASURE: C_YELLOW, MASS_EQUIP_SHOP: C_BLUE, MASS_ITEM_SHOP: C_BLUE,
    MASS_MAGIC_SHOP: C_MAGENTA, MASS_CHURCH: C_CYAN, MASS_CIRCLE: C_MAGENTA,
    MASS_EVENT: C_YELLOW_D, MASS_GATE: C_RED_D, MASS_CASTLE: C_WHITE,
    MASS_MAGIC_CHEST: C_YELLOW,
}

P_COLOR = [C_RED, C_BLUE, C_GREEN, C_YELLOW]

PLAYERS = [
    dict(name='Susanoo',    lv=4, hp=62, mhp=78, gold=1113, est=3, pos=35, cpu=0, devil=0),
    dict(name='Y.Takeru',   lv=3, hp=41, mhp=66, gold=820,  est=2, pos=34, cpu=1, devil=0),
    dict(name='Okuninushi', lv=5, hp=88, mhp=90, gold=2140, est=5, pos=71, cpu=1, devil=0),
    dict(name='Amaterasu',  lv=2, hp=12, mhp=54, gold=310,  est=0, pos=18, cpu=1, devil=1),
]
CUR = 0
WEEK, TURN = 6, 23
# 村の所有者 (mass_id -> player) デモ用
VILLAGE_OWNER = {15: 2, 55: 0, 35: 1}
VILLAGE_LV = {15: 2, 55: 3, 35: 1}


def load_board():
    con = sqlite3.connect(BOARD_DB)
    masses = {}
    grid = {}
    for (mid, mtype, gx, gy) in con.execute(
            'SELECT id, type, x, y FROM masses'):
        masses[mid] = (mtype, gx, gy)
        grid[(gx, gy)] = mid
    conns = set()
    for (a, b, bidir) in con.execute(
            'SELECT from_id, to_id, bidirectional FROM connections'):
        conns.add((a, b))
        if bidir:
            conns.add((b, a))
    con.close()
    return masses, grid, conns


MASSES, GRID, CONNS = load_board()


# ---------------------------------------------------------------------------
#  タイル地形 (16x16)
# ---------------------------------------------------------------------------

def tile_grass(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_GREEN_D)
    for (dx, dy) in ((3, 4), (11, 6), (6, 11), (13, 13), (1, 9)):
        s.pixel(x + dx, y + dy, C_GREEN)
        s.pixel(x + dx, y + dy - 1, C_GREEN)


def tile_path(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_YELLOW_D)
    for (dx, dy) in ((2, 3), (9, 5), (5, 12), (12, 10)):
        s.pixel(x + dx, y + dy, C_GRAY)


def tile_water(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_BLUE_D)
    for dy in (3, 9, 14):
        s.fill_rect(x + 2, y + dy, 5, 1, C_BLUE)
        s.fill_rect(x + 9, y + dy + 2, 5, 1, C_BLUE)


def tile_rock(s, x, y):
    s.fill_rect(x, y, TILE, TILE, C_GRAY)
    s.fill_rect(x + 3, y + 6, 10, 7, C_BLACK)
    s.fill_rect(x + 4, y + 5, 8, 7, C_GRAY)


TERRAIN = {'g': tile_grass, 'p': tile_path, 'w': tile_water, 'r': tile_rock}


def draw_terrain(s, ox, oy, cols, rows, seed_fn):
    for r in range(rows):
        for c in range(cols):
            TERRAIN[seed_fn(c, r)](s, ox + c * TILE, oy + r * TILE)


# ---------------------------------------------------------------------------
#  マス (8x6 タイル = 128x96)
# ---------------------------------------------------------------------------

def draw_mass_cell(s, x, y, mid, mtype, is_current, occupants):
    """1マスを 128x96 の枠に描く"""
    accent = MASS_ACCENT.get(mtype, C_GRAY)
    name = MASS_NAME.get(mtype, '?')

    # 台座 (影 + 本体 + 二重罫線)
    px, py, pw, ph = x + 14, y + 16, 100, 60
    s.fill_rect(px + 3, py + 3, pw, ph, C_BLACK)
    s.fill_rect(px, py, pw, ph, C_BLUE_D)
    s.rect(px, py, pw, ph, C_WHITE if is_current else C_GRAY)
    if is_current:
        s.rect(px - 2, py - 2, pw + 4, ph + 4, C_YELLOW)

    # 種別の帯
    s.fill_rect(px + 2, py + 2, pw - 4, 10, accent)
    s.text8(px + 5, py + 3, clip_text(name, pw - 10), C_BLACK
            if accent in (C_YELLOW, C_WHITE, C_CYAN, C_GREEN) else C_WHITE)

    # マス番号
    s.text8(px + pw - 26, py + ph - 10, '#%-3d' % mid, C_GRAY)

    # 種別ごとの中身
    cx, cy = px + pw // 2, py + 32
    if mtype == MASS_VILLAGE:
        owner = VILLAGE_OWNER.get(mid)
        lv = VILLAGE_LV.get(mid, 1)
        roof = P_COLOR[owner] if owner is not None else C_GRAY
        # 家
        s.fill_rect(cx - 14, cy - 2, 28, 14, C_YELLOW_D)
        s.rect(cx - 14, cy - 2, 28, 14, C_BLACK)
        for i in range(8):
            s.fill_rect(cx - 14 + i, cy - 2 - i, 28 - i * 2, 1, roof)
        s.fill_rect(cx - 4, cy + 4, 8, 8, C_BLACK)
        s.text8(px + 5, py + ph - 10, 'Lv%d' % lv, C_YELLOW)
    elif mtype == MASS_BATTLE:
        for i in range(5):
            s.fill_rect(cx - 10 + i * 5, cy - 6 + (i % 2) * 4, 3, 12, C_RED)
        s.text8(cx - 8, cy + 14, '!!', C_RED)
    elif mtype in (MASS_TREASURE, MASS_MAGIC_CHEST):
        col = C_YELLOW if mtype == MASS_TREASURE else C_MAGENTA
        s.fill_rect(cx - 12, cy - 4, 24, 16, col)
        s.rect(cx - 12, cy - 4, 24, 16, C_BLACK)
        s.fill_rect(cx - 12, cy + 2, 24, 2, C_BLACK)
        s.fill_rect(cx - 2, cy + 1, 4, 5, C_BLACK)
    elif mtype in (MASS_ITEM_SHOP, MASS_EQUIP_SHOP, MASS_MAGIC_SHOP):
        s.fill_rect(cx - 14, cy - 2, 28, 14, C_GRAY)
        s.rect(cx - 14, cy - 2, 28, 14, C_BLACK)
        for i in range(7):
            s.fill_rect(cx - 16 + i * 5, cy - 6, 3, 5, accent)
        s.fill_rect(cx - 16, cy - 8, 32, 3, accent)
    elif mtype == MASS_CHURCH:
        s.fill_rect(cx - 12, cy + 2, 24, 10, C_CYAN_D)
        s.fill_rect(cx - 14, cy - 4, 28, 3, C_CYAN)
        s.fill_rect(cx - 10, cy - 1, 20, 3, C_CYAN)
        s.fill_rect(cx - 8, cy + 2, 3, 10, C_CYAN)
        s.fill_rect(cx + 5, cy + 2, 3, 10, C_CYAN)
    elif mtype == MASS_CIRCLE:
        s.circle(cx, cy + 4, 12, C_MAGENTA)
        s.circle(cx, cy + 4, 8, C_MAGENTA)
        s.circle(cx, cy + 4, 4, C_MAGENTA)
    elif mtype == MASS_CASTLE:
        s.fill_rect(cx - 16, cy - 2, 32, 14, C_GRAY)
        for i in range(4):
            s.fill_rect(cx - 16 + i * 9, cy - 8, 6, 6, C_WHITE)
        s.fill_rect(cx - 4, cy + 4, 8, 8, C_BLACK)
    elif mtype == MASS_GATE:
        s.fill_rect(cx - 14, cy - 6, 6, 18, C_RED_D)
        s.fill_rect(cx + 8, cy - 6, 6, 18, C_RED_D)
        s.fill_rect(cx - 16, cy - 8, 32, 4, C_RED)
    else:
        s.fill_rect(cx - 10, cy + 6, 20, 3, C_GRAY)

    # 駒
    for n, pid in enumerate(occupants):
        bx = px + 6 + n * 14
        by = py + ph - 20
        s.fill_circle(bx, by, 5, P_COLOR[pid])
        s.circle(bx, by, 5, C_WHITE)
        if pid == CUR:
            s.circle(bx, by, 7, C_YELLOW)


def linked(a, b):
    return (a, b) in CONNS or (b, a) in CONNS


def draw_field(s, center_id):
    """center_id を中心に 3x3 マスを描く。スクロールで 1 マス分ずれる想定。

    地形は「実際の接続」から決める:
      - 接続のある左右の間 = 道
      - 接続のない上下の間 = 海 (別ステージなので渡れないことを地形で示す)
    """
    mtype, gx, gy = MASSES[center_id]

    cells = {}
    for row in range(3):
        for col in range(3):
            cells[(col, row)] = GRID.get((gx - 1 + col, gy - 1 + row))

    def terrain_at(c, r):
        col, row = c // CELL_TW, r // CELL_TH
        lc, lr = c % CELL_TW, r % CELL_TH
        mid = cells.get((col, row))

        # マスの台座まわりは道
        if mid is not None and 1 <= lr <= CELL_TH - 2:
            return 'p'

        # 上下のマスと接続がなければ、境界を海で隔てる
        above = cells.get((col, row - 1))
        if lr == 0 and mid is not None and above is not None \
                and not linked(mid, above):
            return 'w'
        if lr == CELL_TH - 1 and mid is not None:
            below = cells.get((col, row + 1))
            if below is not None and not linked(mid, below):
                return 'w'

        if (c * 7 + r * 13) % 19 == 0:
            return 'r'
        return 'g'

    draw_terrain(s, FIELD_X, FIELD_Y, 25, 19, terrain_at)

    occ = {}
    for i, p in enumerate(PLAYERS):
        occ.setdefault(p['pos'], []).append(i)

    # 接続線を先に敷く (台座の下)
    for (col, row), mid in cells.items():
        if mid is None:
            continue
        cx = FIELD_X + col * CELL_W + CELL_W // 2
        cy = FIELD_Y + row * CELL_H + CELL_H // 2
        for (dc, dr) in ((1, 0), (0, 1)):
            nid = cells.get((col + dc, row + dr))
            if nid is None or not linked(mid, nid):
                continue
            if dc:
                s.fill_rect(cx, cy - 3, CELL_W, 6, C_YELLOW_D)
                s.fill_rect(cx, cy - 4, CELL_W, 1, C_GRAY)
                s.fill_rect(cx, cy + 3, CELL_W, 1, C_GRAY)
            else:
                s.fill_rect(cx - 3, cy, 6, CELL_H, C_YELLOW_D)

    for (col, row), mid in cells.items():
        if mid is None:
            continue
        draw_mass_cell(s, FIELD_X + col * CELL_W, FIELD_Y + row * CELL_H,
                       mid, MASSES[mid][0], mid == center_id,
                       occ.get(mid, []))

    # 画面外へ伸びるショートカット接続を矢印で示す
    off = [n for n in (a for (a, b) in CONNS if b == center_id)
           if n not in cells.values()]
    if off:
        cy = FIELD_Y + CELL_H + CELL_H // 2
        s.fill_rect(FIELD_W - 26, cy - 8, 24, 16, C_RED_D)
        s.rect(FIELD_W - 26, cy - 8, 24, 16, C_YELLOW)
        s.text8(FIELD_W - 22, cy - 4, '>>', C_YELLOW)

    s.rect(FIELD_X, FIELD_Y, FIELD_W, FIELD_H, C_WHITE)


# ---------------------------------------------------------------------------
#  右パネル
# ---------------------------------------------------------------------------

def assets(p):
    return p['gold'] + p['est'] * 640


def hp_color(hp, mhp):
    if hp * 4 <= mhp:
        return C_RED
    if hp * 2 <= mhp:
        return C_YELLOW
    return C_GREEN


def draw_panel(s):
    x, w = PANEL_X, PANEL_W
    s.fill_rect(x, 0, w, PANEL_H, C_BLUE_D)
    s.fill_rect(x, 0, 1, PANEL_H, C_WHITE)

    s.fill_rect(x + 4, 4, w - 8, 14, C_RED_D)
    s.text8(x + 8, 7, 'WEEK %-2d      TURN %-3d' % (WEEK, TURN), C_WHITE)

    order = sorted(range(len(PLAYERS)), key=lambda i: -assets(PLAYERS[i]))
    y = 24
    for rank, i in enumerate(order):
        p = PLAYERS[i]
        if i == CUR:
            s.fill_rect(x + 3, y - 2, w - 6, 66, C_BLUE)
        s.text8(x + 8, y + 1, '%d' % (rank + 1), C_YELLOW)
        s.fill_rect(x + 20, y, 8, 10, P_COLOR[i])
        s.rect(x + 20, y, 8, 10, C_WHITE)
        s.text8(x + 34, y + 1, clip_text(p['name'], 120), C_WHITE)
        if p['devil']:
            s.fill_rect(x + w - 34, y, 26, 10, C_RED)
            s.text8(x + w - 32, y + 1, 'DEV', C_WHITE)

        s.text8(x + 8, y + 16, 'Lv%-2d' % p['lv'], C_YELLOW)
        gauge(s, x + 44, y + 15, 120, 10, p['hp'], p['mhp'],
              hp_color(p['hp'], p['mhp']))
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


# ---------------------------------------------------------------------------
#  下段窓
# ---------------------------------------------------------------------------

def draw_bottom_branch(s, opts, sel=0, steps=3):
    win(s, 2, BOTTOM_Y + 2, 636, BOTTOM_H - 6, shadow=False)
    s.fill_rect(5, BOTTOM_Y + 5, 630, 14, C_RED_D)
    s.text8(10, BOTTOM_Y + 8, 'WHICH WAY?', C_WHITE)
    s.text8(520, BOTTOM_Y + 8, '%d STEPS LEFT' % steps, C_YELLOW)

    for i, mid in enumerate(opts):
        mtype = MASSES[mid][0]
        ox = 12 + i * 210
        oy = BOTTOM_Y + 26
        if i == sel:
            s.fill_rect(ox - 4, oy - 3, 204, 56, C_BLUE)
            s.rect(ox - 4, oy - 3, 204, 56, C_YELLOW)
            cursor(s, ox, oy + 2, C_YELLOW)
        s.text8(ox + 12, oy + 6, '%d.' % (i + 1), C_YELLOW)
        accent = MASS_ACCENT.get(mtype, C_GRAY)
        s.fill_rect(ox + 30, oy + 2, 60, 12, accent)
        s.text8(ox + 33, oy + 4, clip_text(MASS_NAME.get(mtype, '?'), 56),
                C_BLACK if accent in (C_YELLOW, C_WHITE, C_CYAN, C_GREEN)
                else C_WHITE)
        s.text8(ox + 100, oy + 4, '#%-3d' % mid, C_GRAY)

        if mtype == MASS_VILLAGE:
            owner = VILLAGE_OWNER.get(mid)
            oname = PLAYERS[owner]['name'] if owner is not None else '(none)'
            s.text8(ox + 12, oy + 22, 'OWNER', C_YELLOW)
            s.text8(ox + 64, oy + 22, clip_text(oname, 120),
                    P_COLOR[owner] if owner is not None else C_GRAY)
            s.text8(ox + 12, oy + 36, 'TOLL', C_YELLOW)
            s.text8(ox + 64, oy + 36, '320 G' if owner is not None else '-',
                    C_RED if owner is not None else C_GRAY)
        elif mtype == MASS_BATTLE:
            s.text8(ox + 12, oy + 22, 'ENEMY', C_YELLOW)
            s.text8(ox + 64, oy + 22, 'Kotengu', C_WHITE)
            s.text8(ox + 12, oy + 36, 'LEVEL', C_YELLOW)
            s.text8(ox + 64, oy + 36, 'Stage 2', C_WHITE)

    s.text8(416, BOTTOM_Y + 74, '[1/2] SELECT   [ENTER] GO', C_GRAY)


def draw_bottom_idle(s):
    win(s, 2, BOTTOM_Y + 2, 636, BOTTOM_H - 6, shadow=False)
    s.fill_rect(5, BOTTOM_Y + 5, 630, 14, C_RED_D)
    p = PLAYERS[CUR]
    s.text8(10, BOTTOM_Y + 8, '%s' % p['name'], C_WHITE)
    s.text8(552, BOTTOM_Y + 8, 'YOUR TURN', C_YELLOW)
    s.text16(16, BOTTOM_Y + 28, 'Roll the dice to move.', C_WHITE)
    s.text8(16, BOTTOM_Y + 56, '[R] ROLL   [W] SAVE   [ESC] QUIT', C_GRAY)
    # サイコロ
    s.fill_rect(560, BOTTOM_Y + 34, 40, 40, C_WHITE)
    s.rect(560, BOTTOM_Y + 34, 40, 40, C_BLACK)
    for (dx, dy) in ((10, 10), (26, 10), (18, 18), (10, 26), (26, 26)):
        s.fill_circle(560 + dx, BOTTOM_Y + 34 + dy, 3, C_BLACK)


# ---------------------------------------------------------------------------

def scene_branch():
    s = Screen(C_BLACK)
    draw_field(s, PLAYERS[CUR]['pos'])
    draw_panel(s)
    draw_bottom_branch(s, [36, 39], sel=0, steps=3)
    return s


def scene_idle():
    s = Screen(C_BLACK)
    draw_field(s, PLAYERS[CUR]['pos'])
    draw_panel(s)
    draw_bottom_idle(s)
    return s


SCENES = [('hud_scroll_branch', scene_branch, '分岐選択中'),
          ('hud_scroll_idle', scene_idle, 'サイコロ待ち')]


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for name, fn, desc in SCENES:
        path = os.path.join(OUT, name + '.png')
        fn().save(path, scale=1)
        print('%-20s %s -> %s' % (name, desc, os.path.normpath(path)))
