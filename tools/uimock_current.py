#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_current.py — 現状のUIを uimock で再現し、実機スクショと突き合わせる

レンダラ (uimock.py) が実機と同じ絵を出せるかの検証用。
ここが一致しないうちは、モックアップ案を信用してはいけない。
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uimock import (Screen, C_BLACK, C_BLUE_D, C_GREEN_D, C_GRAY, C_WHITE,
                    C_RED, C_BLUE, C_GREEN, C_YELLOW, C_MAGENTA, C_CYAN)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'docs', 'tasks', 'game', 'ui')


def mu_window(s, x, y, w, h, title):
    """現状の microUI ウィンドウ (title_height=24, padding=5, spacing=4)"""
    s.fill_rect(x, y, w, 24, C_GREEN_D)          # MU_COLOR_TITLEBG = 4
    s.text8(x + 5, y + 8, title, C_WHITE)
    s.fill_rect(x, y + 24, w, h - 24, C_BLUE_D)  # MU_COLOR_WINDOWBG = 1
    s.text8(x + w - 14, y + 8, 'x', C_WHITE)


def mu_button(s, x, y, w, label):
    """現状の mu_button: 高さ = size.y(10) + padding*2(10) = 20、ラベルは中央揃え。
    ラベルが幅を超えると原点が左へはみ出し、クリップで先頭が欠ける"""
    h = 20
    s.fill_rect(x, y, w, h, C_GRAY)              # MU_COLOR_BUTTON = 7
    tw = len(label) * 8
    tx = x + (w - tw) // 2                       # ← ここが負方向へ出る
    ty = y + (h - 8) // 2
    # クリップ矩形 = ボタン矩形。左に出た文字は「丸ごと描かない」
    cx = tx
    for ch in label:
        if cx + 8 > x and cx < x + w:
            s.text8(cx, ty, ch, C_WHITE)
        cx += 8


def build():
    s = Screen(C_BLACK)

    # --- ステータスバー (kcg_draw_utf8 = 8x16) ---
    s.text16(20, 15, 'Susanoo Lv1 HP 50/50 EXP 27 Gold 1113 Pos 39 W6', C_WHITE)

    # --- 盤面 (6行ぶんだけ雰囲気を再現) ---
    colors = [C_WHITE, C_GRAY, C_YELLOW, C_GRAY, C_BLUE, C_GRAY, C_GRAY,
              C_MAGENTA, C_GRAY, C_RED, C_WHITE, C_GRAY, C_GRAY, C_GRAY,
              C_YELLOW, C_GRAY, C_BLUE, C_GRAY, C_MAGENTA, C_BLUE]
    for row in range(6):
        y = row * 36 + 40
        for col in range(20):
            x = col * 26 + 45
            if col < 19:
                s.line(x, y, x + 26, y, C_GRAY)
            c = colors[(col + row * 3) % len(colors)]
            s.fill_circle(x, y, 6, c)
            s.circle(x, y, 6, C_WHITE)

    # --- microUI ウィンドウ (mu_rect(20, 240, 300, 150)) ---
    mu_window(s, 20, 240, 300, 150, 'OS32 Sugoroku RPG')
    s.text8(25, 277, 'Welcome to Shop!', C_WHITE)
    mu_button(s, 20, 292, 95, 'Buy Herb (10 G)')
    mu_button(s, 20, 316, 95, 'Buy Antidote (15 G)')
    mu_button(s, 20, 340, 95, 'Exit Shop')

    return s


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, 'current.png')
    build().save(path, scale=1)
    print('wrote', os.path.normpath(path))
