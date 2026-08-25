#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_block.py — ドカポン3・2・1 のフィールド表示に寄せた案

参考画像から読み取った要点:
  - マスは連続した地形ではなく、**厚みのあるブロック(台)**。
    上面 + 側面の陰影で立体に見せ、その上にキャラクターが立つ。
    マスの境界がはっきり見えるのが正しい (地図のように境界を消すのは誤り)。
  - 画面上部に1行のステータス帯 (名前 / Lv / 攻 / 防 / HP / 所持金)
  - 右上に「あと◯◯週 曜日」
  - コマンドは左側の小さな縦メニュー (必要なときだけ出る)
  - 地形 (岩山・木・水) はブロックの上や周囲に置かれるオブジェクト

出力:
  block_3x3.png  1マス128x96 (9マス表示)  — 今までの縮尺
  block_5x4.png  1マス 76x64 (20マス表示) — 原典に近い密度
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

uimock.PALETTE = PAL.to_rgb888(PAL.RETRO_MAP)
from uimock import Screen   # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'docs', 'tasks', 'game', 'ui')

STATUS_H = 34          # 上部ステータス帯
P_COLOR = [PAL_P_RED, PAL_P_BLUE, PAL_P_GREEN, PAL_P_YELLOW]

# マス種別
PL, V, B, CH, EQ, IT, MG, SH, CI, GT, CA, GC, CO, DU = (
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14)


# ---------------------------------------------------------------------------
#  ブロック (マス1つ)
# ---------------------------------------------------------------------------

def block(s, x, y, w, h, thick, top=PAL_LAND_MID, side=PAL_LAND_LOW,
          highlight=0):
    """厚みのある台。(x,y) は上面の左上。h は上面の高さ。
    手前の側面を濃い色で厚く取り、立体に見せる。"""
    # 手前の側面 (土の断面)
    s.fill_rect(x, y + h, w, thick, side)
    # 断面の陰 (下ほど暗く)
    s.fill_rect(x, y + h + thick - thick // 3, w, thick // 3, PAL_BLACK)
    # 上面と側面の境に明るい線を入れて角を立てる
    s.fill_rect(x, y + h, w, 2, PAL_LAND_HIGH if top == PAL_LAND_MID else top)

    # 上面
    s.fill_rect(x, y, w, h, top)
    # 奥側の縁を暗く (奥行き感)
    s.fill_rect(x, y, w, 2, side)

    # 外周
    s.rect(x, y, w, h + thick, PAL_BLACK)
    if highlight:
        s.rect(x - 1, y - 1, w + 2, h + thick + 2, PAL_P_YELLOW)
        s.rect(x - 2, y - 2, w + 4, h + thick + 4, PAL_BLACK)


def grass_speck(s, x, y, w, h, seed=0):
    """上面にまばらな草。ベタ塗りを避ける"""
    n = 0
    for gy in range(4, h - 3, 7):
        for gx in range(5, w - 4, 11):
            n += 1
            if (gx * 3 + gy * 5 + seed) % 4:
                continue
            s.pixel(x + gx, y + gy, PAL_LAND_HIGH)
            s.pixel(x + gx + 1, y + gy - 1, PAL_LAND_HIGH)


# ---------------------------------------------------------------------------
#  ブロックの上に載るもの
# ---------------------------------------------------------------------------

def o_house(s, cx, by, roof, sc=1):
    """cx=中心x, by=接地y"""
    w, h = 22 * sc, 20 * sc
    x, y = cx - w // 2, by - h
    wall = h * 5 // 9
    s.fill_rect(x + 2, y + h - wall, w - 4, wall, PAL_LAND_HIGH)
    s.rect(x + 2, y + h - wall, w - 4, wall, PAL_BLACK)
    for i in range(h - wall):
        s.fill_rect(x + i, y + i, w - i * 2, 1, roof)
    s.fill_rect(x, y + h - wall - 1, w, 1, PAL_BLACK)
    s.fill_rect(cx - 2 * sc, y + h - wall + 3, 4 * sc, wall - 3, PAL_BLACK)


def o_tree(s, cx, by, sc=1):
    w, h = 14 * sc, 18 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(cx - 2, by - h // 3, 4, h // 3, PAL_ROAD)
    s.fill_circle(cx, y + h // 3, h // 3, PAL_FOREST)
    s.fill_circle(cx - 3, y + h // 3 - 2, h // 5, PAL_LAND_MID)


def o_rock(s, cx, by, sc=1):
    w, h = 26 * sc, 22 * sc
    x, y = cx - w // 2, by - h
    for i in range(h):
        ww = int(w * (i + 2) / (h + 2))
        s.fill_rect(cx - ww // 2, y + i, ww, 1, PAL_ROCK)
    for i in range(h // 3):
        ww = int(w * (i + 2) / (h + 2))
        s.fill_rect(cx - ww // 2, y + i, ww // 2, 1, PAL_LAND_LOW)
    s.fill_rect(cx - 2, y + 1, 3, 2, PAL_WHITE)


def o_shop(s, cx, by, accent, sc=1):
    w, h = 28 * sc, 18 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 2, y + 7, w - 4, h - 7, PAL_ROCK)
    s.rect(x + 2, y + 7, w - 4, h - 7, PAL_BLACK)
    for i in range((w - 4) // 5):
        s.fill_rect(x + 2 + i * 5, y + 2, 5, 5,
                    accent if i % 2 == 0 else PAL_LAND_HIGH)
    s.fill_rect(x, y, w, 2, accent)


def o_torii(s, cx, by, sc=1):
    w, h = 24 * sc, 20 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 3, y + 5, 3, h - 5, PAL_P_RED)
    s.fill_rect(x + w - 6, y + 5, 3, h - 5, PAL_P_RED)
    s.fill_rect(x, y + 1, w, 4, PAL_P_RED)
    s.fill_rect(x + 3, y + 9, w - 6, 3, PAL_P_RED)


def o_castle(s, cx, by, sc=1):
    w, h = 34 * sc, 26 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 3, y + 8, w - 6, h - 8, PAL_ROCK)
    s.rect(x + 3, y + 8, w - 6, h - 8, PAL_BLACK)
    for i in range(3):
        bx = x + 3 + i * ((w - 12) // 2)
        s.fill_rect(bx, y, 8, 9, PAL_LAND_HIGH)
        s.rect(bx, y, 8, 9, PAL_BLACK)
    s.fill_rect(cx - 3, by - 9, 7, 9, PAL_BLACK)


def o_chest(s, cx, by, col, sc=1):
    w, h = 18 * sc, 14 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x, y + 5, w, h - 5, col)
    s.rect(x, y + 5, w, h - 5, PAL_BLACK)
    for i in range(5):
        s.fill_rect(x + i, y + i, w - i * 2, 1, col)
    s.fill_rect(x, y + 8, w, 1, PAL_BLACK)


def o_den(s, cx, by, sc=1):
    w, h = 26 * sc, 20 * sc
    x, y = cx - w // 2, by - h
    for i in range(3):
        s.fill_rect(x + 2 + i * 9, by - 9 - (i % 2) * 3, 5, 9, PAL_ROCK)
    s.fill_rect(cx - 1, y, 2, h, PAL_ROAD)
    s.fill_rect(cx + 1, y, w // 2 - 2, 7, PAL_P_RED)


def o_cave(s, cx, by, sc=1):
    w, h = 30 * sc, 22 * sc
    x, y = cx - w // 2, by - h
    for i in range(h):
        ww = int(w * (i + 3) / (h + 3))
        s.fill_rect(cx - ww // 2, y + i, ww, 1, PAL_ROCK)
    s.fill_circle(cx, by - 5, 7, PAL_BLACK)
    s.fill_rect(cx - 7, by - 6, 15, 6, PAL_BLACK)


def o_office(s, cx, by, sc=1):
    w, h = 26 * sc, 18 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 2, y + 5, w - 4, h - 5, PAL_LAND_HIGH)
    s.rect(x + 2, y + 5, w - 4, h - 5, PAL_BLACK)
    s.fill_rect(x, y, w, 5, PAL_P_YELLOW)
    s.text8(cx - 4, y + 8, 'G', PAL_BLACK)


def o_circle(s, cx, by, sc=1):
    for r in (13, 9, 5):
        s.circle(cx, by - 8, r, PAL_MAGIC)


def o_gate(s, cx, by, sc=1):
    w, h = 26 * sc, 22 * sc
    x, y = cx - w // 2, by - h
    s.fill_rect(x + 1, y + 4, 6, h - 4, PAL_P_RED)
    s.fill_rect(x + w - 7, y + 4, 6, h - 4, PAL_P_RED)
    s.fill_rect(x - 2, y, w + 4, 5, PAL_P_RED)


# ---------------------------------------------------------------------------
#  キャラクタ
# ---------------------------------------------------------------------------

def draw_char(s, cx, by, col, w, h, marker=0):
    x, y = cx - w // 2, by - h
    head = max(6, h * 2 // 5)

    s.fill_rect(x + 2, by - 1, w - 4, 2, PAL_BLACK)            # 影
    s.fill_rect(x + 1, y + head, w - 2, h - head - 2, col)     # 体
    s.rect(x + 1, y + head, w - 2, h - head - 2, PAL_BLACK)
    s.fill_rect(x + 2, y, w - 4, head, PAL_LAND_HIGH)          # 顔
    s.rect(x + 2, y, w - 4, head, PAL_BLACK)
    s.fill_rect(x + 2, y, w - 4, head // 3, PAL_BLACK)         # 髪
    if w >= 14:
        s.pixel(x + 5, y + head - 3, PAL_BLACK)
        s.pixel(x + w - 6, y + head - 3, PAL_BLACK)
    s.fill_rect(x + 2, by - 3, 4, 3, PAL_BLACK)                # 足
    s.fill_rect(x + w - 6, by - 3, 4, 3, PAL_BLACK)

    if marker:
        for i in range(7):
            s.fill_rect(cx - 6 + i, y - 13 + i, 13 - i * 2, 1, PAL_BLACK)
        for i in range(5):
            s.fill_rect(cx - 4 + i, y - 12 + i, 9 - i * 2, 1, PAL_P_YELLOW)


# ---------------------------------------------------------------------------
#  マス描画
# ---------------------------------------------------------------------------

def draw_mass(s, x, y, w, h, thick, mtype, owner=None, lv=1,
              current=0, seed=0, sc=1):
    top = PAL_LAND_MID
    side = PAL_LAND_LOW
    if mtype == GT:
        top, side = PAL_ROAD, PAL_LAND_LOW

    block(s, x, y, w, h, thick, top, side, current)
    grass_speck(s, x, y, w, h, seed)

    cx = x + w // 2
    by = y + h - 3

    if mtype == V:
        roof = P_COLOR[owner] if owner is not None else PAL_ROCK
        if lv >= 2:
            o_house(s, cx - 18 * sc, by, roof, sc)
            o_house(s, cx + 14 * sc, by - 4, roof, sc)
        else:
            o_house(s, cx, by, roof, sc)
        if lv >= 3:
            o_house(s, cx, by - 10 * sc, roof, sc)
    elif mtype == B:
        o_den(s, cx, by, sc)
    elif mtype == CH:
        o_chest(s, cx, by, PAL_P_YELLOW, sc)
    elif mtype == GC:
        o_chest(s, cx, by, PAL_MAGIC, sc)
    elif mtype in (IT, EQ, MG):
        o_shop(s, cx, by, {IT: PAL_SKY, EQ: PAL_P_BLUE, MG: PAL_MAGIC}[mtype],
               sc)
    elif mtype == SH:
        o_torii(s, cx, by, sc)
    elif mtype == CI:
        o_circle(s, cx, by, sc)
    elif mtype == CA:
        o_castle(s, cx, by, sc)
    elif mtype == CO:
        o_office(s, cx, by, sc)
    elif mtype == DU:
        o_cave(s, cx, by, sc)
    elif mtype == GT:
        o_gate(s, cx, by, sc)
    else:
        if seed % 3 == 0:
            o_tree(s, cx + 14 * sc, by, sc)
        elif seed % 3 == 1:
            o_rock(s, cx - 12 * sc, by, sc)


# ---------------------------------------------------------------------------
#  画面
# ---------------------------------------------------------------------------

def status_bar(s):
    s.fill_rect(0, 0, 640, STATUS_H, PAL_LAND_LOW)
    s.fill_rect(0, STATUS_H - 1, 640, 1, PAL_BLACK)
    s.fill_rect(6, 5, 10, 22, P_COLOR[0])
    s.rect(6, 5, 10, 22, PAL_BLACK)
    s.text16(22, 3, 'Susanoo', PAL_WHITE)
    s.text8(22, 21, 'Lv 4   ATK 18   DEF 12   HP  60/ 78', PAL_WHITE)
    s.text8(310, 21, 'GOLD  2,158', PAL_P_YELLOW)

    # 右上: 残り週と曜日
    s.fill_rect(450, 3, 186, 15, PAL_ROCK)
    s.rect(450, 3, 186, 15, PAL_BLACK)
    s.text8(456, 5, 'WEEK  6 / 60', PAL_WHITE)
    s.text8(576, 5, 'SAT', PAL_P_YELLOW)


def command_menu(s, x, y, items, sel=0):
    w = 8 * 8 + 16
    h = len(items) * 16 + 8
    s.fill_rect(x + 3, y + 3, w, h, PAL_BLACK)
    s.fill_rect(x, y, w, h, PAL_LAND_LOW)
    s.rect(x, y, w, h, PAL_WHITE)
    s.rect(x + 2, y + 2, w - 4, h - 4, PAL_WHITE)
    for i, it in enumerate(items):
        iy = y + 6 + i * 16
        if i == sel:
            s.fill_rect(x + 5, iy - 1, w - 10, 16, PAL_ROCK)
            s.text8(x + 7, iy + 4, '>', PAL_P_YELLOW)
        s.text16(x + 16, iy, it, PAL_WHITE)


SCENE = [
    [(CH, None, 1), (V, 2, 2),  (PL, None, 1), (SH, None, 1), (PL, None, 1)],
    [(IT, None, 1), (V, 0, 3),  (B,  None, 1), (PL, None, 1), (CO, None, 1)],
    [(PL, None, 1), (PL, None, 1), (CA, None, 1), (GT, None, 1), (CH, None, 1)],
    [(V,  1, 1),    (PL, None, 1), (MG, None, 1), (PL, None, 1), (DU, None, 1)],
]


def build(cols, rows, cw, ch, thick, char_w, char_h, sc, name):
    s = Screen(PAL_SEA_DEEP)
    status_bar(s)

    fw = cols * cw
    fh = rows * (ch + thick + 2)
    ox = (640 - fw) // 2
    oy = STATUS_H + (400 - STATUS_H - fh) // 2

    for r in range(rows):
        for c in range(cols):
            mt, ow, lv = SCENE[r][c]
            x = ox + c * cw
            y = oy + r * (ch + thick + 2)
            cur = (r == rows // 2 and c == cols // 2)
            draw_mass(s, x + 2, y, cw - 4, ch, thick, mt, ow, lv,
                      current=cur, seed=r * 7 + c * 3, sc=sc)

    # キャラクタ: 中央マスに2人、別マスに2人
    ccx = ox + (cols // 2) * cw + cw // 2
    ccy = oy + (rows // 2) * (ch + thick + 2) + ch - 3
    draw_char(s, ccx - 14, ccy, P_COLOR[0], char_w, char_h, marker=1)
    draw_char(s, ccx + 14, ccy, P_COLOR[1], char_w, char_h)
    draw_char(s, ox + cw // 2, oy + ch - 3, P_COLOR[2], char_w, char_h)
    draw_char(s, ox + (cols - 1) * cw + cw // 2,
              oy + (rows - 1) * (ch + thick + 2) + ch - 3,
              P_COLOR[3], char_w, char_h)

    command_menu(s, 8, STATUS_H + 12,
                 ['MOVE', 'CHECK', 'ITEM', 'OTHER', 'MAP'], sel=0)

    s.save(os.path.join(OUT, name))
    return name


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    print('wrote', build(3, 3, 190, 84, 22, 30, 48, 2, 'block_3x3.png'))
    print('wrote', build(5, 4, 122, 58, 14, 18, 30, 1, 'block_5x4.png'))
