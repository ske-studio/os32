#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_field.py — ドカポン3・2・1 のフィールド表現に寄せた案 (3回目)

参考画像から読み直した要点:
  - マスは「ブロック(台)」ではない。地形は連続していて、
    **すごろくの道は地面に置かれた矢印 (▽ ◁ △ ▷) の列**で示される。
    矢印1つ = 1マス。進行方向を向いている。
  - 地形は草地に草の房、丸い樹、水辺、崖。連続した地図。
  - 村や施設は地形の上に建物として建っていて、名前の看板が添えられる。
  - キャラクタは道の上に立つ。マス間隔の半分強くらいの大きさ。

画面構成は従来どおり (盤面400x304 + 右パネル240 + 下段窓)。

盤面のスケール:
  グリッド1マス = 48x48px  ->  400x304 に 8x6 セル入る。
  そのうち道になっているセルにだけ矢印が出るので、
  実際に見えるマス数は 10〜20 程度。参考画像の密度に近い。
"""

import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import uimock
import uimock_palette as PAL
from uimock_palette import (PAL_BLACK, PAL_SEA_DEEP, PAL_SEA_SHALLOW,
                            PAL_LAND_LOW, PAL_LAND_MID, PAL_LAND_HIGH,
                            PAL_ROAD, PAL_ROCK, PAL_FOREST, PAL_P_BLUE,
                            PAL_P_RED, PAL_MAGIC, PAL_P_GREEN, PAL_SKY,
                            PAL_P_YELLOW, PAL_WHITE)

uimock.PALETTE = PAL.to_rgb888(PAL.RETRO_MAP)
from uimock import Screen, clip_text   # noqa: E402

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(PROJ, 'docs', 'tasks', 'game', 'ui')
BOARD_DB = os.path.join(PROJ, 'assets', 'board.db')

CELL = 48                       # グリッド1マスの画面上のサイズ
VIEW_C, VIEW_R = 9, 7           # 描画するセル数 (画面外にはみ出す分を含む)
FIELD_W, FIELD_H = 400, 304
PANEL_X, PANEL_W, BOTTOM_Y = 400, 240, 304

P_COLOR = [PAL_P_RED, PAL_P_BLUE, PAL_P_GREEN, PAL_P_YELLOW]

MASS_EMPTY, MASS_VILLAGE, MASS_BATTLE, MASS_TREASURE = 0, 1, 2, 3
MASS_EQUIP, MASS_ITEM, MASS_MAGIC = 4, 5, 6
MASS_CHURCH, MASS_CIRCLE = 7, 8
MASS_GATE, MASS_CASTLE, MASS_GOLDCHEST = 10, 11, 12
MASS_COLLECT, MASS_DUNGEON = 13, 14

TERR_PLAIN, TERR_FOREST, TERR_ROCK, TERR_DESERT, TERR_SWAMP, TERR_SNOW = range(6)

MASS_LABEL = {
    MASS_VILLAGE: 'VILLAGE', MASS_BATTLE: 'MONSTER', MASS_TREASURE: 'CHEST',
    MASS_GOLDCHEST: 'GOLD', MASS_EQUIP: 'WEAPON', MASS_ITEM: 'ITEM',
    MASS_MAGIC: 'MAGIC', MASS_CHURCH: 'SHRINE', MASS_CIRCLE: 'CIRCLE',
    MASS_GATE: 'GATE', MASS_CASTLE: 'CASTLE', MASS_COLLECT: 'OFFICE',
    MASS_DUNGEON: 'CAVE',
}


# ---------------------------------------------------------------------------
#  地形 (連続した地面)
# ---------------------------------------------------------------------------

def grass_base(s, x, y, w, h, seed=0):
    """羊皮紙の地。古地図なので草を描き込まず、
    まばらな点描と等高線風のハッチングで起伏を示す。"""
    s.fill_rect(x, y, w, h, PAL_LAND_MID)

    # 紙のムラ (明るい方へ)
    for gy in range(2, h - 2, 7):
        for gx in range(3, w - 3, 9):
            if (gx * 5 + gy * 7 + seed) % 4:
                continue
            s.pixel(x + gx, y + gy, PAL_LAND_HIGH)
            s.pixel(x + gx + 1, y + gy, PAL_LAND_HIGH)

    # 起伏のハッチング (セピアの短い線。古地図の山の描法)
    if (seed // 3) % 3 == 0:
        for k in range(3):
            hy = y + 6 + k * 13
            hx = x + 5 + ((seed + k * 7) % 12)
            for i in range(4):
                s.pixel(hx + i, hy + (i % 2), PAL_LAND_LOW)


def tree(s, cx, by, sc=1):
    """木立。古地図らしく塗りつぶさず、輪郭と最小限の陰で描く"""
    r = 7 * sc
    s.fill_rect(cx - 1, by - r, 3, r, PAL_LAND_LOW)
    s.fill_circle(cx, by - r - r // 2, r, PAL_FOREST)
    s.fill_circle(cx - r // 3, by - r - r // 2 - r // 3, r // 2, PAL_LAND_MID)
    s.circle(cx, by - r - r // 2, r, PAL_BLACK)


def water(s, x, y, w, h, seed=0):
    """海。波を描かず、刷りムラ程度の濃淡だけ入れる"""
    s.fill_rect(x, y, w, h, PAL_SEA_DEEP)
    for gy in range(3, h - 2, 11):
        for gx in range(2, w - 8, 15):
            if (gx + gy + seed) % 3:
                continue
            s.fill_rect(x + gx, y + gy, 7, 2, PAL_SEA_SHALLOW)


def shore(s, x, y, w, h, side='e'):
    """陸と水の境。参考画像のように砂と波で柔らかくする"""
    if side == 'e':
        s.fill_rect(x, y, 4, h, PAL_LAND_HIGH)
        s.fill_rect(x + 4, y, 3, h, PAL_SEA_SHALLOW)
    elif side == 'w':
        s.fill_rect(x + w - 4, y, 4, h, PAL_LAND_HIGH)
        s.fill_rect(x + w - 7, y, 3, h, PAL_SEA_SHALLOW)


def cliff(s, x, y, w, h):
    """崖。岩肌の段"""
    for i in range(0, h, 6):
        s.fill_rect(x, y + i, w, 4, PAL_ROCK)
        s.fill_rect(x, y + i + 4, w, 2, PAL_LAND_LOW)


def rockpile(s, cx, by, sc=1):
    """山。古地図の描法にならい、面は塗らず稜線と片側の陰で立てる"""
    w, h = 24 * sc, 15 * sc
    # 陰 (右側だけ塗る)
    for i in range(h):
        ww = int(w * (i + 3) / (h + 3))
        s.fill_rect(cx, by - h + i, ww // 2, 1, PAL_LAND_LOW)
    # 稜線
    for i in range(h):
        ww = int(w * (i + 3) / (h + 3))
        s.pixel(cx - ww // 2, by - h + i, PAL_BLACK)
        s.pixel(cx + ww // 2, by - h + i, PAL_BLACK)
    s.fill_rect(cx - w // 2, by - 1, w, 1, PAL_LAND_LOW)


# ---------------------------------------------------------------------------
#  道 = 矢印マーカー (これが「マス」)
# ---------------------------------------------------------------------------

DIR_N, DIR_E, DIR_S, DIR_W = 0, 1, 2, 3


def arrow(s, cx, cy, d, col=PAL_LAND_LOW, size=14):
    """進行方向を向いた三角。参考画像の ▽ / ◁ にあたる"""
    edge = PAL_BLACK
    for i in range(size):
        wlen = (size - i) * 2 - 1
        if d == DIR_S:
            s.fill_rect(cx - wlen // 2, cy - size // 2 + i, wlen, 1, col)
        elif d == DIR_N:
            s.fill_rect(cx - wlen // 2, cy + size // 2 - i, wlen, 1, col)
        elif d == DIR_E:
            s.fill_rect(cx - size // 2 + i, cy - wlen // 2, 1, wlen, col)
        else:
            s.fill_rect(cx + size // 2 - i, cy - wlen // 2, 1, wlen, col)
    # 縁取り。地面に沈まないよう2辺を黒で引く
    half = size // 2
    if d == DIR_S:
        s.line(cx - size + 1, cy - half, cx, cy + half, edge)
        s.line(cx + size - 1, cy - half, cx, cy + half, edge)
    elif d == DIR_N:
        s.line(cx - size + 1, cy + half, cx, cy - half, edge)
        s.line(cx + size - 1, cy + half, cx, cy - half, edge)
    elif d == DIR_E:
        s.line(cx - half, cy - size + 1, cx + half, cy, edge)
        s.line(cx - half, cy + size - 1, cx + half, cy, edge)
    else:
        s.line(cx + half, cy - size + 1, cx - half, cy, edge)
        s.line(cx + half, cy + size - 1, cx - half, cy, edge)


def signboard(s, cx, by, text):
    """村名などの立て看板"""
    w = 8 * len(text) + 8
    h = 13
    x, y = cx - w // 2, by - h - 5
    s.fill_rect(cx - 2, by - 6, 3, 6, PAL_LAND_LOW)
    s.fill_rect(x + 2, y + 2, w, h, PAL_BLACK)
    s.fill_rect(x, y, w, h, PAL_LAND_HIGH)
    s.rect(x, y, w, h, PAL_BLACK)
    s.rect(x + 1, y + 1, w - 2, h - 2, PAL_LAND_LOW)
    s.text8(x + 4, y + 3, text, PAL_BLACK)


# ---------------------------------------------------------------------------
#  施設 (地形の上に建つ)
# ---------------------------------------------------------------------------

def house(s, cx, by, roof, sc=1):
    w, h = 26 * sc, 22 * sc
    x, y = cx - w // 2, by - h
    wall = h * 5 // 9
    s.fill_rect(x + 2, y + h - wall, w - 4, wall, PAL_LAND_HIGH)
    s.rect(x + 2, y + h - wall, w - 4, wall, PAL_BLACK)
    for i in range(h - wall):
        s.fill_rect(x + i, y + i, w - i * 2, 1, roof)
    s.fill_rect(x, y + h - wall - 1, w, 1, PAL_BLACK)
    s.fill_rect(cx - 3, y + h - wall + 4, 6, wall - 4, PAL_BLACK)


def shop(s, cx, by, accent, sc=1):
    w, h = 30 * sc, 20 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 2, y + 8, w - 4, h - 8, PAL_ROCK)
    s.rect(x + 2, y + 8, w - 4, h - 8, PAL_BLACK)
    for i in range((w - 4) // 5):
        s.fill_rect(x + 2 + i * 5, y + 3, 5, 5,
                    accent if i % 2 == 0 else PAL_LAND_HIGH)
    s.fill_rect(x, y + 1, w, 2, accent)


def torii(s, cx, by, sc=1):
    w, h = 26 * sc, 22 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 4, y + 6, 4, h - 6, PAL_P_RED)
    s.fill_rect(x + w - 8, y + 6, 4, h - 6, PAL_P_RED)
    s.fill_rect(x, y + 1, w, 5, PAL_P_RED)
    s.fill_rect(x + 4, y + 11, w - 8, 3, PAL_P_RED)


def castle(s, cx, by, sc=1):
    w, h = 38 * sc, 30 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 3, y + 10, w - 6, h - 10, PAL_ROCK)
    s.rect(x + 3, y + 10, w - 6, h - 10, PAL_BLACK)
    for i in range(3):
        bx = x + 3 + i * ((w - 12) // 2)
        s.fill_rect(bx, y, 9, 11, PAL_LAND_HIGH)
        s.rect(bx, y, 9, 11, PAL_BLACK)
    s.fill_rect(cx - 4, by - 11, 8, 11, PAL_BLACK)


def chest(s, cx, by, col, sc=1):
    w, h = 20 * sc, 15 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x, y + 6, w, h - 6, col)
    s.rect(x, y + 6, w, h - 6, PAL_BLACK)
    for i in range(6):
        s.fill_rect(x + i, y + i, w - i * 2, 1, col)
    s.fill_rect(x, y + 9, w, 1, PAL_BLACK)


def den(s, cx, by, sc=1):
    w = 28 * sc
    x = cx - w // 2
    for i in range(3):
        s.fill_rect(x + 2 + i * 10, by - 11 - (i % 2) * 4, 6, 11, PAL_ROCK)
        s.rect(x + 2 + i * 10, by - 11 - (i % 2) * 4, 6, 11, PAL_BLACK)
    s.fill_rect(cx - 1, by - 24, 2, 24, PAL_LAND_LOW)
    s.fill_rect(cx + 1, by - 24, 12, 8, PAL_P_RED)


def cave(s, cx, by, sc=1):
    w, h = 32 * sc, 24 * sc
    for i in range(h):
        ww = int(w * (i + 3) / (h + 3))
        s.fill_rect(cx - ww // 2, by - h + i, ww, 1, PAL_ROCK)
    s.fill_circle(cx, by - 6, 8, PAL_BLACK)
    s.fill_rect(cx - 8, by - 7, 17, 7, PAL_BLACK)


def office(s, cx, by, sc=1):
    w, h = 28 * sc, 20 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 2, y + 6, w - 4, h - 6, PAL_LAND_HIGH)
    s.rect(x + 2, y + 6, w - 4, h - 6, PAL_BLACK)
    s.fill_rect(x, y, w, 6, PAL_P_YELLOW)
    s.text8(cx - 4, y + 10, 'G', PAL_BLACK)


def circle_mark(s, cx, by, sc=1):
    for r in (15, 10, 5):
        s.circle(cx, by - 10, r, PAL_MAGIC)


def gate(s, cx, by, sc=1):
    w, h = 30 * sc, 26 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 1, y + 5, 7, h - 5, PAL_P_RED)
    s.fill_rect(x + w - 8, y + 5, 7, h - 5, PAL_P_RED)
    s.fill_rect(x - 2, y, w + 4, 6, PAL_P_RED)


# ---------------------------------------------------------------------------
#  キャラクタ (道の上に立つ)
# ---------------------------------------------------------------------------

def character(s, cx, by, col, w=22, h=32, marker=0):
    x, y = cx - w // 2, by - h
    head = h * 2 // 5

    s.fill_rect(x + 3, by - 2, w - 6, 3, PAL_BLACK)             # 影
    # 体
    s.fill_rect(x + 2, y + head, w - 4, h - head - 4, col)
    s.rect(x + 2, y + head, w - 4, h - head - 4, PAL_BLACK)
    # 腕
    s.fill_rect(x, y + head + 2, 3, h // 4, col)
    s.fill_rect(x + w - 3, y + head + 2, 3, h // 4, col)
    # 顔
    s.fill_rect(x + 3, y + 2, w - 6, head - 2, PAL_LAND_HIGH)
    s.rect(x + 3, y + 2, w - 6, head - 2, PAL_BLACK)
    # 髪
    s.fill_rect(x + 2, y, w - 4, 5, PAL_BLACK)
    s.fill_rect(x + 1, y + 3, 3, 5, PAL_BLACK)
    s.fill_rect(x + w - 4, y + 3, 3, 5, PAL_BLACK)
    # 目
    s.fill_rect(x + 6, y + head - 5, 2, 3, PAL_BLACK)
    s.fill_rect(x + w - 8, y + head - 5, 2, 3, PAL_BLACK)
    # 足
    s.fill_rect(x + 3, by - 4, 5, 4, PAL_LAND_LOW)
    s.fill_rect(x + w - 8, by - 4, 5, 4, PAL_LAND_LOW)

    if marker:
        for i in range(8):
            s.fill_rect(cx - 7 + i, y - 15 + i, 15 - i * 2, 1, PAL_BLACK)
        for i in range(6):
            s.fill_rect(cx - 5 + i, y - 14 + i, 11 - i * 2, 1, PAL_P_YELLOW)


# ---------------------------------------------------------------------------
#  盤面
# ---------------------------------------------------------------------------

def load_board():
    con = sqlite3.connect(BOARD_DB)
    grid, by_id = {}, {}
    for (mid, mt, gx, gy, terr, param) in con.execute(
            'SELECT id, type, x, y, terrain, param FROM masses'):
        m = dict(id=mid, type=mt, x=gx, y=gy, terrain=terr, param=param)
        grid[(gx, gy)] = m
        by_id[mid] = m
    links = set()
    for (a, b, bd) in con.execute(
            'SELECT from_id, to_id, bidirectional FROM connections'):
        links.add((a, b))
        if bd:
            links.add((b, a))
    con.close()
    return grid, by_id, links


GRID, BY_ID, LINKS = load_board()
VILLAGE_OWNER = {}
VILLAGE_NAME = {}


def facility(s, m, cx, by):
    t = m['type']
    if t == MASS_VILLAGE:
        owner = VILLAGE_OWNER.get(m['id'])
        roof = P_COLOR[owner] if owner is not None else PAL_ROCK
        house(s, cx - 14, by - 6, roof)
        house(s, cx + 12, by, roof)
    elif t == MASS_BATTLE:
        den(s, cx, by)
    elif t == MASS_TREASURE:
        chest(s, cx, by, PAL_P_YELLOW)
    elif t == MASS_GOLDCHEST:
        chest(s, cx, by, PAL_MAGIC)
    elif t in (MASS_ITEM, MASS_EQUIP, MASS_MAGIC):
        shop(s, cx, by, {MASS_ITEM: PAL_SKY, MASS_EQUIP: PAL_P_BLUE,
                         MASS_MAGIC: PAL_MAGIC}[t])
    elif t == MASS_CHURCH:
        torii(s, cx, by)
    elif t == MASS_CIRCLE:
        circle_mark(s, cx, by)
    elif t == MASS_CASTLE:
        castle(s, cx, by)
    elif t == MASS_COLLECT:
        office(s, cx, by)
    elif t == MASS_DUNGEON:
        cave(s, cx, by)
    elif t == MASS_GATE:
        gate(s, cx, by)


def dir_to(m, n):
    if n['x'] > m['x']: return DIR_E
    if n['x'] < m['x']: return DIR_W
    if n['y'] > m['y']: return DIR_S
    return DIR_N


def draw_field(s, center_id, occupants, cur_pid=0):
    c = BY_ID[center_id]
    ox = FIELD_W // 2 - CELL // 2 - (VIEW_C // 2) * CELL
    oy = FIELD_H // 2 - CELL // 2 - (VIEW_R // 2) * CELL

    # 1. 地面 (連続した草地)。マスの無いところは水にする
    for r in range(VIEW_R):
        for col in range(VIEW_C):
            gx, gy = c['x'] - VIEW_C // 2 + col, c['y'] - VIEW_R // 2 + r
            x, y = ox + col * CELL, oy + r * CELL
            m = GRID.get((gx, gy))
            # マスの周囲1セルまでは陸にして、地形が唐突に切れないようにする
            near = any(GRID.get((gx + dx, gy + dy))
                       for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            if m or near:
                grass_base(s, x, y, CELL, CELL, gx * 3 + gy * 5)
            else:
                # 陸から2セル以内なら浅瀬にして、境界の直線的な段差を和らげる
                near2 = any(GRID.get((gx + dx, gy + dy))
                            for dx in (-2, -1, 0, 1, 2)
                            for dy in (-2, -1, 0, 1, 2))
                water(s, x, y, CELL, CELL, gx + gy)
                if near2:
                    s.fill_rect(x, y, CELL, CELL, PAL_SEA_SHALLOW)
                    for k in range(0, CELL, 11):
                        s.fill_rect(x + k, y + (k * 3) % CELL, 5, 1,
                                    PAL_SEA_DEEP)

    # 2. 地形の飾り (道の無いところに樹や岩)
    for r in range(VIEW_R):
        for col in range(VIEW_C):
            gx, gy = c['x'] - VIEW_C // 2 + col, c['y'] - VIEW_R // 2 + r
            if GRID.get((gx, gy)):
                continue
            if not any(GRID.get((gx + dx, gy + dy))
                       for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                continue
            x, y = ox + col * CELL, oy + r * CELL
            h = (gx * 7 + gy * 11) % 5
            if h == 0:
                tree(s, x + CELL // 2, y + CELL - 6)
            elif h == 1:
                tree(s, x + CELL // 3, y + CELL - 10)
                tree(s, x + CELL * 2 // 3, y + CELL - 4)
            elif h == 2:
                rockpile(s, x + CELL // 2, y + CELL - 6)

    # 3. 道 = 矢印マーカー
    for r in range(VIEW_R):
        for col in range(VIEW_C):
            gx, gy = c['x'] - VIEW_C // 2 + col, c['y'] - VIEW_R // 2 + r
            m = GRID.get((gx, gy))
            if not m:
                continue
            x, y = ox + col * CELL, oy + r * CELL
            cx, cy = x + CELL // 2, y + CELL // 2

            # 進行方向 = ID が増える側の隣接マス
            nxt = None
            for (dx, dy) in ((1, 0), (0, 1), (-1, 0), (0, -1)):
                n = GRID.get((gx + dx, gy + dy))
                if n and (m['id'], n['id']) in LINKS and n['id'] > m['id']:
                    nxt = n
                    break
            d = dir_to(m, nxt) if nxt else DIR_E
            arrow(s, cx, cy + 12, d)

    # 4. 施設と看板
    for r in range(VIEW_R):
        for col in range(VIEW_C):
            gx, gy = c['x'] - VIEW_C // 2 + col, c['y'] - VIEW_R // 2 + r
            m = GRID.get((gx, gy))
            if not m or m['type'] == MASS_EMPTY:
                continue
            x, y = ox + col * CELL, oy + r * CELL
            facility(s, m, x + CELL // 2, y + CELL // 2 + 2)
            if m['type'] == MASS_VILLAGE and abs(gx - c['x']) <= 1 \
                    and abs(gy - c['y']) <= 1:
                signboard(s, x + CELL // 2 + 14, y + CELL - 2,
                          VILLAGE_NAME.get(m['id'], 'VIL%d' % m['param']))

    # 5. キャラクタ
    #    描画順: 奥(y小)から手前(y大)へ。同じマスなら手番プレイヤーを最後に
    #    描いて必ず前面に出す。
    def order_key(o):
        pid, mid = o
        mm = BY_ID.get(mid)
        return (mm['y'] if mm else 0, 1 if pid == cur_pid else 0)

    for pid, mid in sorted(occupants, key=order_key):
        m = BY_ID.get(mid)
        if not m:
            continue
        dx, dy = m['x'] - c['x'], m['y'] - c['y']
        if abs(dx) > VIEW_C // 2 or abs(dy) > VIEW_R // 2:
            continue
        x = ox + (dx + VIEW_C // 2) * CELL
        y = oy + (dy + VIEW_R // 2) * CELL
        character(s, x + CELL // 2 - 8 + pid * 6, y + CELL - 4,
                  P_COLOR[pid], marker=(pid == cur_pid))

    s.rect(0, 0, FIELD_W, FIELD_H, PAL_WHITE)


def draw_panel(s):
    """UI は海のスレート色を地にする。
    羊皮紙の地図と同じ色にすると文字が読めなくなる。"""
    s.fill_rect(PANEL_X, 0, PANEL_W, BOTTOM_Y, PAL_SEA_DEEP)
    s.fill_rect(PANEL_X, 0, 2, BOTTOM_Y, PAL_LAND_HIGH)
    s.fill_rect(PANEL_X + 4, 4, PANEL_W - 8, 14, PAL_SEA_SHALLOW)
    s.text8(PANEL_X + 8, 7, 'W6 SAT   TURN 23', PAL_LAND_HIGH)
    names = ['Susanoo', 'Y.Takeru', 'Okuninushi', 'Amaterasu']
    y = 24
    for i, nm in enumerate(names):
        if i == 0:
            s.fill_rect(PANEL_X + 3, y - 2, PANEL_W - 6, 66, PAL_SEA_SHALLOW)
        s.fill_rect(PANEL_X + 8, y, 8, 10, P_COLOR[i])
        s.rect(PANEL_X + 8, y, 8, 10, PAL_LAND_HIGH)
        s.text8(PANEL_X + 22, y + 1, nm, PAL_LAND_HIGH)
        s.text8(PANEL_X + 8, y + 16, 'Lv%-2d HP %3d/%3d' % (4 - i, 60, 78),
                PAL_LAND_HIGH)
        s.text8(PANEL_X + 8, y + 32, 'G %-6d  VIL %d' % (1113, 3 - i),
                PAL_P_YELLOW)
        s.fill_rect(PANEL_X + 4, y + 60, PANEL_W - 8, 1, PAL_SEA_SHALLOW)
        y += 66


def draw_bottom(s):
    s.fill_rect(0, BOTTOM_Y, 640, 400 - BOTTOM_Y, PAL_SEA_DEEP)
    s.fill_rect(0, BOTTOM_Y, 640, 2, PAL_LAND_HIGH)
    s.rect(4, BOTTOM_Y + 6, 632, 400 - BOTTOM_Y - 12, PAL_LAND_HIGH)
    s.fill_rect(7, BOTTOM_Y + 9, 626, 14, PAL_SEA_SHALLOW)
    s.text8(12, BOTTOM_Y + 12, 'Susanoo', PAL_LAND_HIGH)
    s.text8(550, BOTTOM_Y + 12, 'YOUR TURN', PAL_P_YELLOW)
    s.text16(16, BOTTOM_Y + 32, 'Roll the dice to move.', PAL_LAND_HIGH)
    s.text8(16, BOTTOM_Y + 60, '[R] ROLL   [W] SAVE   [ESC] QUIT',
            PAL_SEA_SHALLOW)


def build(center_id, occupants, cur_pid=0):
    s = Screen(PAL_SEA_DEEP)
    draw_field(s, center_id, occupants, cur_pid)
    draw_panel(s)
    draw_bottom(s)
    return s


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    # 村がいくつか見える位置を選ぶ
    center = None
    for mid, m in sorted(BY_ID.items()):
        if m['type'] == MASS_VILLAGE and m['area'] if 'area' in m else True:
            center = mid
            break
    center = center or 0
    VILLAGE_OWNER.update({center: 0})
    occ = [(0, center), (1, center + 1), (2, center + 3)]
    p = os.path.join(OUT, 'field_arrow.png')
    build(center, occ).save(p)
    print('wrote', os.path.normpath(p))
